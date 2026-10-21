set(MARS_ROUND_SOURCES)
if(MARS_BOARD STREQUAL "waveshare-esp32-s3-touch-lcd-1-46")
    list(APPEND MARS_ROUND_SOURCES "apps/round_ui.c" "apps/round_view.c" "apps/round_app_view.c")
    set_source_files_properties(${MARS_ROUND_SOURCES} PROPERTIES COMPILE_OPTIONS "-fstack-usage")
endif()
idf_component_register(
    SRCS
        ${MARS_ROUND_SOURCES}
        "main.c"
        "apps/apps.c"
        "apps/launcher.c"
        "apps/app_drawer.c"
        "apps/mars_dynamic_bridge.c"
        "settings.c"
        "settings_wifi.c"
        "settings_bt.c"
    INCLUDE_DIRS "." "apps"
    REQUIRES driver esp_driver_usb_serial_jtag esp_lcd esp_timer nvs_flash espressif__cjson spiffs app_update esp_partition
             esp_wifi esp_netif esp_event bt
             mars_bsp mars_clock mars_common mars_core mars_media mars_net mars_ota mars_ui
             mars_services mars_app_loader mars_audio_stream mars_ui_adapt mars_canvas
)
