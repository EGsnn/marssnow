#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "mars_touch.h"
#include "esp_err.h"
#include "mars_board_config.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;
    const char *chip;
    uint16_t width;
    uint16_t height;
    uint32_t flash_bytes;
    uint32_t psram_bytes;
} mars_board_info_t;

typedef enum {
    MARS_INPUT_NONE, MARS_INPUT_PREVIOUS, MARS_INPUT_NEXT,
    MARS_INPUT_SELECT, MARS_INPUT_BACK
} mars_input_action_t;

typedef struct {
    int candidate;
    int stable;
    int64_t candidate_since_ms;
    int64_t pressed_since_ms;
    bool long_sent;
} mars_input_state_t;

/* Hardware profile describes expected parts. Presence is a runtime result. */
const mars_board_info_t *mars_board_info(void);
const char *mars_board_profile_json(void);
esp_err_t mars_board_init(void);
/* Share one bus with audio and optional peripherals; callers must not recreate it. */
esp_err_t mars_board_i2c_bus_get(i2c_master_bus_handle_t *bus);
bool mars_board_touch_available(void);
bool mars_board_codec_present(void);
esp_err_t mars_board_battery_read(int *percent, int *millivolts);

/* RGB565 pixels passed to blit are big endian; return only after DMA completes.
 * A single display owner must serialize all drawing. Boards may keep a native
 * shadow framebuffer when the panel imposes alignment constraints. */
esp_err_t mars_board_fill_rect(int x, int y, int width, int height, uint16_t color);
esp_err_t mars_board_blit(int x, int y, int width, int height, const uint16_t *pixels);
#if MARS_BOARD_DISPLAY_ROUND
/* Fresh event plus retained contact state; one UI consumer per poll. */
mars_touch_sample_t mars_board_touch_sample(void);
/* CPU endian full native app frame; no legacy partial drawing may interleave. */
esp_err_t mars_board_present_cpu_frame(const uint16_t *pixels);
/* Panel byte order (big endian), full native canvas. Synchronous; offset=0 is the new page.
 * Previous/new may not alias the BSP shadow. Single display owner only. */
esp_err_t mars_board_present_slide(const uint16_t *next, const uint16_t *previous, int offset);
#endif
esp_err_t mars_board_brightness_set(unsigned percent);
int mars_board_key_read(void); /* 0=none, 1=previous/up, 2=next/down, 3=select */
bool mars_board_touch_read(uint16_t *x, uint16_t *y);
/* Pure state machine. Poll every ~10 ms. Short select fires on release;
 * hold select for 800 ms emits BACK once and suppresses SELECT. */
mars_input_action_t mars_input_update(mars_input_state_t *state, int raw_key, int64_t now_ms);

#ifdef __cplusplus
}
#endif
