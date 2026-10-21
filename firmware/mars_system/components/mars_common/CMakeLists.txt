if(IDF_TARGET STREQUAL "esp32c3")
    # Flash-mapped fonts and pure geometry only. Never link the BOX PSRAM canvases.
    idf_component_register(SRCS "anim.c" "font/font_cn.c" "font/mars_glyph.c"
        INCLUDE_DIRS "include" "font")
    return()
endif()
set(MARS_COMMON_SOURCES
    "box3_gfx.c"
    "anim.c"
    "font/font_cn.c"
    "font/mars_glyph.c")
if(CONFIG_MARS_LAYOUT_V2)
    list(APPEND MARS_COMMON_SOURCES "mars_assets.c")
    set(MARS_COMMON_EMBED_FILES "font/font_cn.bin")
endif()

idf_component_register(
    SRCS
        ${MARS_COMMON_SOURCES}
    INCLUDE_DIRS "include" "font"
    REQUIRES mars_bsp
    PRIV_REQUIRES esp_partition
    EMBED_FILES ${MARS_COMMON_EMBED_FILES}
)
