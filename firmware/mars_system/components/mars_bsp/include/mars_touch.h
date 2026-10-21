#pragma once
#include <stdbool.h>
#include <stdint.h>
/* NONE means no new report, never a release. down is the retained physical
 * state; coordinates remain valid on release. CANCEL is transport/reset loss. */
typedef enum {MARS_TOUCH_NONE,MARS_TOUCH_POINT,MARS_TOUCH_UP,MARS_TOUCH_CANCEL} mars_touch_phase_t;
typedef struct {
 mars_touch_phase_t phase;
 bool down;
 uint8_t fingers;
 uint16_t x,y;
 int64_t time_ms;
} mars_touch_sample_t;
