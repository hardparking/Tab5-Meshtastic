/*
 * Tab5-Meshtastic v2 — Milestone 0: skeleton & boot.
 * ---------------------------------------------------
 * Brings up the board, the display (PPA landscape rotation), the LVGL app
 * shell + status bar, and NVS — and nothing else. This is deliberately the
 * minimal boot path: the M0 gate is 20/20 cold boots from battery with the
 * shell rendered and no corruption (PRD §11, NFR-1). It is also the first probe
 * for PRD R3 — if a clean skeleton is stable, past instability was self-
 * inflicted by later changes.
 *
 * The pinned esp_hosted / esp_wifi_remote / NimBLE stack is established at M0
 * (idf_component.yml + sdkconfig.defaults) but NOT activated here. The BLE
 * transport, sync engine, and AppState arrive in M1+.
 *
 * Cold-boot discipline (PRD §9.5): any change touching this boot path,
 * peripherals, or power rails must be validated on a REAL cold battery boot —
 * attaching serial resets the P4 and masks cold-boot-only regressions.
 */

#include "esp_log.h"
#include "nvs_flash.h"

#include "m5_tab5_component.h"

#include "lcd_tools.h"
#include "ui_shell.h"

static const char* TAG = "tab5-mesh-v2";

static m5::tab5::m5tab5_component s_board;

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Tab5-Meshtastic v2 — M0 skeleton & boot");

    /* NVS: settings + (later) the NimBLE bond store live here. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Board bring-up: SYS I2C, expanders, power rails, LCD + touch. */
    m5::tab5::m5tab5_component_config_t board_cfg = {};
    ESP_ERROR_CHECK(s_board.begin(board_cfg));

    /* Display + LVGL shell. Built before any radio work so the user always sees
     * the app chrome immediately on boot. */
    ESP_ERROR_CHECK(app_lcd_lvgl_init(s_board));
    ui_start();

    ESP_LOGI(TAG, "M0 boot complete; shell rendered");
}
