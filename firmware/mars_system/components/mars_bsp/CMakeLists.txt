if(CMAKE_BUILD_EARLY_EXPANSION)
    # ESP-IDF's requirements scan runs without custom cache variables.
    idf_component_register(REQUIRES driver esp_lcd esp_timer esp_adc
        esp_driver_i2c esp_driver_gpio esp_driver_spi esp_driver_ledc)
    return()
endif()
if(NOT MARS_BOARD)
    # Compatibility default for existing BOX build commands.
    set(MARS_BOARD "alientek-atk-box")
endif()
get_filename_component(MARS_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(MARS_BOARD_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${MARS_REPO_ROOT}/firmware/boards/${MARS_BOARD}.json"
    "${MARS_REPO_ROOT}/scripts/board_profiles.py"
    "${MARS_REPO_ROOT}/scripts/ui_profiles.py"
    "${MARS_REPO_ROOT}/firmware/ui_profiles/catalog.json")
execute_process(
    COMMAND "${PYTHON}" "${MARS_REPO_ROOT}/scripts/board_profiles.py" "${MARS_BOARD}"
        --target "${IDF_TARGET}" --output "${MARS_BOARD_GENERATED}"
    RESULT_VARIABLE MARS_BOARD_RESULT
    ERROR_VARIABLE MARS_BOARD_ERROR)
if(NOT MARS_BOARD_RESULT EQUAL 0)
    message(FATAL_ERROR "Invalid Mars board: ${MARS_BOARD_ERROR}")
endif()
if(MARS_BOARD STREQUAL "alientek-atk-box")
    set(MARS_BOARD_SOURCES "box3_board.c" "atk_board_adapter.c")
elseif(MARS_BOARD STREQUAL "folotoy-ai-passport")
    set(MARS_BOARD_SOURCES "passport_board.c")
elseif(MARS_BOARD STREQUAL "waveshare-esp32-s3-touch-lcd-1-46")
    set(MARS_BOARD_SOURCES "waveshare146_board.c" "waveshare146_touch.c" "vendor/spd2010/esp_lcd_spd2010.c")
else()
    message(FATAL_ERROR "No BSP implementation for ${MARS_BOARD}")
endif()
idf_component_register(
    SRCS ${MARS_BOARD_SOURCES} "mars_board.c" "mars_input.c"
    INCLUDE_DIRS "include" "${MARS_BOARD_GENERATED}"
    PRIV_INCLUDE_DIRS "vendor/spd2010"
    REQUIRES driver esp_lcd esp_timer esp_adc
        esp_driver_i2c esp_driver_gpio esp_driver_spi esp_driver_ledc)

if(MARS_BOARD STREQUAL "waveshare-esp32-s3-touch-lcd-1-46")
    # Replaces the upstream package_manager version macros without modifying its source.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        ESP_LCD_SPD2010_VER_MAJOR=2 ESP_LCD_SPD2010_VER_MINOR=0 ESP_LCD_SPD2010_VER_PATCH=0)
endif()
