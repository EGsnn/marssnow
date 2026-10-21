#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-width polling ABI. No hardware handles or app callbacks escape. */
#define MARS_CAP_CLOCK_READ (1ull << 6)
#define MARS_CAP_TOUCH_STATE (1ull << 7)
#define MARS_CAP_BACK_VISIBILITY (1ull << 8)
#define MARS_CAP_FONT_GLYPH (1ull << 9)
typedef struct {
    uint32_t valid, year, month, day, hour, minute, second;
    int32_t utc_offset_sec;
    uint32_t sync_state;
} mars_time_snapshot_t;
typedef struct { uint32_t down, x, y; } mars_touch_snapshot_t;

/* 0: local system time available; -2: not synced; -1: null output.
 * Does not start SNTP, Wi-Fi or touch drivers. Output is always initialized. */
int mars_time_read(mars_time_snapshot_t *out);
int mars_input_read_touch(mars_touch_snapshot_t *out);

/* App may hide the shared top-right return hot area with its controls.
 * Navigation resets it on open/close. Hardware back remains available. */
void mars_input_set_back_visible(uint32_t visible);
uint32_t mars_input_back_visible(void); /* navigation only; not exported */

/* System navigation publisher only; intentionally not exported to apps.
 * Publish an up state whenever navigation owns the screen. */
void mars_input_publish_touch(uint32_t down, uint16_t x, uint16_t y);
/* Kernel only; same read ABI, supports native coordinates through 511. */
void mars_input_publish_native_touch(uint32_t down,uint16_t x,uint16_t y);

#ifdef __cplusplus
}
#endif
