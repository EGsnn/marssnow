idf_component_register(
    SRCS "mars_ui_adapt.c" "mars_layout.c" "mars_gesture.c"
    INCLUDE_DIRS "include"
    REQUIRES mars_bsp mars_common)
