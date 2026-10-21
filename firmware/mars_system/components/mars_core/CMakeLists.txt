idf_component_register(
    SRCS
        "mars_event.c"
        "mars_app.c"
        "mars_boot.c"
        "app_slots.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition esp_app_format app_update esp_system esp_timer
)
