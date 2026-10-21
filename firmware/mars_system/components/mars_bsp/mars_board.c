#include "mars_board.h"

static const mars_board_info_t info = {
    .id = MARS_BOARD_ID,
    .chip = MARS_BOARD_CHIP,
    .width = MARS_BOARD_DISPLAY_WIDTH,
    .height = MARS_BOARD_DISPLAY_HEIGHT,
    .flash_bytes = MARS_BOARD_FLASH_BYTES,
    .psram_bytes = MARS_BOARD_PSRAM_BYTES,
};

const mars_board_info_t *mars_board_info(void) { return &info; }
const char *mars_board_profile_json(void) { return MARS_BOARD_PROFILE_JSON; }

__attribute__((weak)) esp_err_t mars_board_brightness_set(unsigned percent)
{(void)percent;return ESP_ERR_NOT_SUPPORTED;}
