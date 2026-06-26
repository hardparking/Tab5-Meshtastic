idf_component_register(
    SRCS
        "main.cpp"
        "board/lcd_tools.cpp"
        "ui/ui_shell.cpp"
        "app/app_state.cpp"
        "app/settings.cpp"
        "mesh/mesh_proto.cpp"
        "ble/ble_transport.cpp"
    INCLUDE_DIRS
        "."
        "board"
        "ui"
        "app"
        "mesh"
        "ble"
    # esp_driver_sdmmc: esp_hosted.h transitively includes driver/sdmmc_host.h
    # but esp_hosted doesn't propagate it, so we pull it in here.
    REQUIRES
        nvs_flash
        bt
        esp_hosted
        esp_wifi_remote
        esp_driver_sdmmc
        esp_timer
        m5_tab5_component
        meshtastic_protos
)
