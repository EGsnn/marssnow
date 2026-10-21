if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(MARS_JSON_COMPONENT espressif__cjson)
else()
    set(MARS_JSON_COMPONENT json)
endif()

idf_component_register(
    SRCS
        "mars_clock.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_netif ${MARS_JSON_COMPONENT} nvs_flash esp_timer mars_core mars_net
)
