if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(MARS_JSON_COMPONENT espressif__cjson)
else()
    set(MARS_JSON_COMPONENT json)
endif()

idf_component_register(
    SRCS
        "ui_runtime.c"
        "line_disc.c"
        "desktop_apps_json.c"
    INCLUDE_DIRS "include"
    REQUIRES mars_bsp mars_common mars_core driver esp_driver_usb_serial_jtag esp_timer ${MARS_JSON_COMPONENT} app_update spiffs
)
