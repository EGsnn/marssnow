if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(MARS_JSON_COMPONENT espressif__cjson)
else()
    set(MARS_JSON_COMPONENT json)
endif()

idf_component_register(
    SRCS
        "mars_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_timer ${MARS_JSON_COMPONENT} mbedtls app_update esp_app_format esp_partition
             esp_system mars_core
)
