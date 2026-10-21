cmake_minimum_required(VERSION 3.16)
if(DEFINED MARS_BOARD AND NOT MARS_BOARD STREQUAL "alientek-atk-box"
        AND NOT MARS_BOARD STREQUAL "waveshare-esp32-s3-touch-lcd-1-46")
    message(FATAL_ERROR "This board has no Mars desktop BSP. Use board_probe for new boards.")
endif()
if(MARS_BOARD STREQUAL "waveshare-esp32-s3-touch-lcd-1-46")
    set(PROJECT_VER "2.5.23-community")
else()
    set(PROJECT_VER "2.5.16-community")
endif()

# 小智的 EAF 文件留在动态应用包；系统只提供通用解码/合成能力。
# 复用仓库内已锁定版本的 ESP Emote GFX 及其编译依赖，避免联网拉包。
set(XIAOZHI_MANAGED "${CMAKE_CURRENT_LIST_DIR}/../../third_party/components")
list(APPEND EXTRA_COMPONENT_DIRS
    "${XIAOZHI_MANAGED}/espressif__esp_websocket_client"
    "${XIAOZHI_MANAGED}/espressif__cjson"
    "${XIAOZHI_MANAGED}/espressif__esp_audio_codec"
    "${XIAOZHI_MANAGED}/espressif__esp_codec_dev"
    "${XIAOZHI_MANAGED}/espressif__esp_lcd_touch"
    "${XIAOZHI_MANAGED}/espressif__esp_new_jpeg"
    "${XIAOZHI_MANAGED}/espressif__freetype"
    "${XIAOZHI_MANAGED}/espressif__cmake_utilities"
    "${XIAOZHI_MANAGED}/laride__heatshrink"
    "${XIAOZHI_MANAGED}/lvgl__lvgl"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(box3_launcher)
