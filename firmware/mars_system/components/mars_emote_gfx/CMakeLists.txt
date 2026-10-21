# Offline adapter for the repository-pinned ESP Emote GFX sources.  The vendor
# component normally receives these public dependencies from the online IDF
# component registry; Mars builds with the manager disabled, so list them here.
set(GFX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../third_party/components/espressif2022__esp_emote_gfx")
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE GFX_SOURCES "${GFX_DIR}/src/*.c")
else()
    file(GLOB_RECURSE GFX_SOURCES CONFIGURE_DEPENDS "${GFX_DIR}/src/*.c")
endif()

idf_component_register(
    SRCS ${GFX_SOURCES}
    INCLUDE_DIRS "${GFX_DIR}/include"
    PRIV_INCLUDE_DIRS "${GFX_DIR}/src"
    REQUIRES esp_timer espressif__esp_lcd_touch espressif__esp_new_jpeg
             espressif__freetype laride__heatshrink lvgl__lvgl
)
