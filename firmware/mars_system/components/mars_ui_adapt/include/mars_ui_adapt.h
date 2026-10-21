#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mars_anim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int x, y, width, height; } mars_ui_rect_t;
typedef struct { int width, height, inset; bool round; } mars_ui_surface_t;
typedef struct {
    const char *layout;
    const char *motion_tier;
    const char *sha256;
    mars_ui_surface_t native_surface;
    mars_ui_rect_t legacy_viewport;
    uint16_t background, surface, text, accent;
    uint16_t gap, radius, touch_min;
    uint8_t font_scale;
} mars_ui_profile_t;
typedef struct {
    uint16_t fps, transition_ms, modal_ms;
    mars_ease_t ease, modal_ease;
} mars_ui_motion_t;

const mars_ui_profile_t *mars_ui_profile(void);
/* Inclusive left, exclusive right. Native pixel coordinates, including inset.
 * Drawing and hit testing must consume the same resulting geometry. */
bool mars_ui_row_span(const mars_ui_surface_t *surface, int y, int *left, int *right);
bool mars_ui_rect_visible(const mars_ui_surface_t *surface, mars_ui_rect_t rect);
bool mars_ui_hit(const mars_ui_surface_t *surface, mars_ui_rect_t rect, int x, int y);
mars_ui_rect_t mars_ui_safe_rect(const mars_ui_surface_t *surface);

/* All motion calls belong to the single UI owner. Returned value is a snapshot.
 * Constraints are transient; governor recovers only on explicit reset to avoid oscillation.
 * These APIs do not read audio state, allocate memory, write NVS, or draw. */
mars_ui_motion_t mars_ui_motion(void);
void mars_ui_motion_constraints(bool audio_busy, bool memory_pressure, bool reduced_motion);
void mars_ui_motion_sample(uint32_t draw_us);
void mars_ui_motion_reset(void);

#ifdef __cplusplus
}
#endif
