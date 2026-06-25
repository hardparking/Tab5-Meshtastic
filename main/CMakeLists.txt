idf_component_register(
    SRCS
        "main.cpp"
        "board/lcd_tools.cpp"
        "ui/ui_shell.cpp"
    INCLUDE_DIRS
        "."
        "board"
        "ui"
    # M0 brings up board + display + LVGL shell + NVS only. The pinned
    # esp_hosted / esp_wifi_remote / bt stack is fetched via idf_component.yml
    # but not required here yet — the BLE transport (M1) will add it.
    REQUIRES
        nvs_flash
        m5_tab5_component
)
