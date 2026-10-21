if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(MARS_JSON_COMPONENT espressif__cjson)
else()
    set(MARS_JSON_COMPONENT json)
endif()

set(MARS_DYNAMIC_REQUIRES)
if(NOT CONFIG_BOARD_TYPE_MARS_ATK_BOX)
    list(APPEND MARS_DYNAMIC_REQUIRES mars_services)
endif()

idf_component_register(
    SRCS
        "mars_desktop.c"
        "../../main/apps/apps.c"
        "../../main/apps/launcher.c"
        "../../main/settings.c"
        "../../main/settings_wifi.c"
        "../../main/settings_bt.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../main" "../../main/apps"
    REQUIRES driver esp_driver_usb_serial_jtag esp_lcd esp_timer nvs_flash
             ${MARS_JSON_COMPONENT} spiffs app_update esp_wifi esp_netif esp_event bt
             mars_bsp mars_clock mars_common mars_core mars_media mars_net mars_ota mars_ui mars_ui_adapt
             ${MARS_DYNAMIC_REQUIRES}
)
