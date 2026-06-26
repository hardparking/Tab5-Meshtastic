/*
 * Tab5-Meshtastic v2 — physical keyboard driver glue. See keyboard.h.
 *
 * The M5Tab5Keyboard driver's poll task calls key_cb() off the UI thread, so we
 * hand events to a queue and let kbd_task() forward them to ui_kbd_feed(), which
 * takes the LVGL lock. We use POLLING (not the hardware INT) deliberately: it
 * needs no GPIO ISR and so can't perturb the esp_hosted/SDIO interrupt path that
 * the cold-boot gate guards (PRD §4). A 20 ms poll is imperceptible for typing.
 */

#include "keyboard.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "m5_tab5_keyboard.h"
#include "ui_shell.h"

static const char* TAG = "kbd";

static m5::M5Tab5Keyboard s_kb;
static QueueHandle_t s_queue = nullptr;

/* Driver poll-task context. Keep it short: just enqueue the decoded string. */
static void key_cb(m5_tab5_key_event_t ev, void* /*arg*/)
{
    if (ev.type != M5_TAB5_KB_MODE_STRING || ev.str_len == 0) return;
    if (s_queue) xQueueSend(s_queue, &ev, 0);
}

static void kbd_task(void* /*arg*/)
{
    m5_tab5_key_event_t ev;
    for (;;) {
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) == pdTRUE) {
            ui_kbd_feed(ev.str_data, ev.str_len, ev.str_modifier);
        }
    }
}

void kbd_start(void)
{
    s_queue = xQueueCreate(16, sizeof(m5_tab5_key_event_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "event queue alloc failed; physical keyboard disabled");
        return;
    }

    /* Self-created master bus on I2C_NUM_1 (the keyboard's dedicated pins),
     * polling mode — independent of the board's system I2C. */
    m5_tab5_kb_err_t err =
        s_kb.begin(I2C_NUM_1, M5_TAB5_KB_DEFAULT_ADDR, M5_TAB5_KB_DEFAULT_SDA, M5_TAB5_KB_DEFAULT_SCL,
                   M5_TAB5_KB_I2C_FREQ_400K, M5_TAB5_KB_INT_MODE_POLLING);
    if (err != M5_TAB5_KB_OK) {
        ESP_LOGW(TAG, "keyboard not detected (err=%d); using on-screen input only", err);
        return;  /* non-fatal — touch OSK still works */
    }

    s_kb.setInterruptMode(M5_TAB5_KB_INT_MODE_POLLING, 20);  /* 20 ms poll */
    s_kb.enableStringMode(key_cb, nullptr);                  /* chars + modifier, no keymap */

    uint8_t ver = 0;
    s_kb.getVersion(&ver);
    ESP_LOGI(TAG, "physical keyboard ready (FW 0x%02X)", ver);

    xTaskCreate(kbd_task, "kbd_ui", 4096, nullptr, 5, nullptr);
}
