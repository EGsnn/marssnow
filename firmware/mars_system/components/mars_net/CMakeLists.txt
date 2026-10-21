idf_component_register(
    SRCS
        "mars_net.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer mars_core
)
