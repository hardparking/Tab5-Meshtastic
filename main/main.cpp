/*
 * Tab5-Meshtastic v2 — Milestone 1: BLE transport + sync engine.
 * -------------------------------------------------------------
 * Boot path: NVS + AppState → board bring-up → power the C6 (wlan_power) →
 * display + LVGL shell → esp_hosted (P4<->C6 SDIO) → NimBLE host → BLE
 * transport (scan/bond/sync to the fixed test radio).
 *
 * Ordering is load-bearing (PRD §4): board.begin() + wlan_power(true) must
 * precede esp_hosted_init(), which must precede nimble_port_init(). The shell
 * is built before the radio comes up so the user always sees chrome immediately.
 *
 * Cold-boot discipline (PRD §9.5): any change to this boot path / peripherals /
 * power rails is validated on a REAL cold battery boot — serial resets the P4.
 */

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "m5_tab5_component.h"
#include "esp_hosted.h"
#include "nimble/nimble_port.h"

#include "lcd_tools.h"
#include "ui_shell.h"
#include "app_state.h"
#include "ble_transport.h"

static const char* TAG = "tab5-mesh-v2";

static m5::tab5::m5tab5_component s_board;

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Tab5-Meshtastic v2 — M1 BLE transport + sync engine");

    app_state_init();

    /* NVS: settings + the NimBLE bond store. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Board bring-up, then power the C6 coprocessor BEFORE esp_hosted. */
    m5::tab5::m5tab5_component_config_t board_cfg = {};
    ESP_ERROR_CHECK(s_board.begin(board_cfg));
    ESP_LOGI(TAG, "powering C6 (wlan_power=ON)");
    ESP_ERROR_CHECK(s_board.wlan_power(true));
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Display + LVGL shell first, so chrome is visible while the radio connects. */
    ESP_ERROR_CHECK(app_lcd_lvgl_init(s_board));
    ui_start();

    /* P4<->C6 transport, then the NimBLE host, then our BLE transport. */
    ESP_LOGI(TAG, "esp_hosted_init()");
    ESP_ERROR_CHECK(esp_hosted_init());

    ret = nimble_port_init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(ret)); return; }

    ble_transport_start();

    ESP_LOGI(TAG, "M1 init done; waiting for BLE sync...");
}
