#include "mars_ui_adapt.h"
#include "mars_board_config.h"
#include <stddef.h>
#include <string.h>

static const mars_ui_profile_t s_profile = {
    .layout = MARS_UI_LAYOUT_FAMILY,
    .motion_tier = MARS_UI_MOTION_TIER,
    .sha256 = MARS_UI_PROFILE_SHA256,
    .native_surface = {MARS_BOARD_DISPLAY_WIDTH, MARS_BOARD_DISPLAY_HEIGHT,
                       MARS_UI_LAYOUT_SAFEINSET, MARS_BOARD_DISPLAY_ROUND},
    .legacy_viewport = {MARS_BOARD_UI_X, MARS_BOARD_UI_Y, MARS_BOARD_UI_WIDTH, MARS_BOARD_UI_HEIGHT},
    .background = MARS_UI_TOKENS_BACKGROUND, .surface = MARS_UI_TOKENS_SURFACE,
    .text = MARS_UI_TOKENS_TEXT, .accent = MARS_UI_TOKENS_ACCENT,
    .gap = MARS_UI_TOKENS_GAP, .radius = MARS_UI_TOKENS_RADIUS,
    .touch_min = MARS_UI_TOKENS_TOUCHMIN, .font_scale = MARS_UI_TOKENS_FONTSCALE,
};
const mars_ui_profile_t *mars_ui_profile(void) { return &s_profile; }

static bool valid(const mars_ui_surface_t *s)
{
    return s && s->width > 0 && s->height > 0 && s->width <= 1024 && s->height <= 1024 &&
           s->inset >= 0 && s->inset < (s->width + 1) / 2 &&
           s->inset < (s->height + 1) / 2 && (!s->round || s->width == s->height);
}
static bool point(const mars_ui_surface_t *s, int x, int y)
{
    if (x < s->inset || y < s->inset || x >= s->width - s->inset || y >= s->height - s->inset)
        return false;
    if (!s->round) return true;
    const int64_t dx = 2 * x - (s->width - 1), dy = 2 * y - (s->height - 1);
    const int64_t radius = s->width - 1 - 2 * s->inset;
    return dx * dx + dy * dy <= radius * radius;
}
bool mars_ui_row_span(const mars_ui_surface_t *s, int y, int *left, int *right)
{
    if (!left || !right) return false;
    *left = *right = 0;
    if (!valid(s) || y < 0 || y >= s->height) return false;
    int l = s->inset, r = s->width - s->inset;
    while (l < r && !point(s, l, y)) ++l;
    if (l == r) return false;
    while (r > l && !point(s, r - 1, y)) --r;
    *left = l; *right = r;
    return true;
}
bool mars_ui_rect_visible(const mars_ui_surface_t *s, mars_ui_rect_t r)
{
    if (!valid(s) || r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 ||
        r.width > s->width || r.height > s->height ||
        r.x > s->width - r.width || r.y > s->height - r.height) return false;
    return point(s, r.x, r.y) && point(s, r.x + r.width - 1, r.y) &&
           point(s, r.x, r.y + r.height - 1) && point(s, r.x + r.width - 1, r.y + r.height - 1);
}
bool mars_ui_hit(const mars_ui_surface_t *s, mars_ui_rect_t r, int x, int y)
{
    return mars_ui_rect_visible(s, r) && x >= r.x && y >= r.y &&
           x < r.x + r.width && y < r.y + r.height;
}
mars_ui_rect_t mars_ui_safe_rect(const mars_ui_surface_t *s)
{
    if (!valid(s)) return (mars_ui_rect_t){0};
    int inset = s->inset;
    mars_ui_rect_t r = {inset, inset, s->width - 2 * inset, s->height - 2 * inset};
    while (!mars_ui_rect_visible(s, r) && r.width > 0 && r.height > 0) {
        ++r.x; ++r.y; r.width -= 2; r.height -= 2;
    }
    return mars_ui_rect_visible(s, r) ? r : (mars_ui_rect_t){0};
}

static unsigned s_cap = MARS_UI_MOTION_FPS, s_samples, s_slow;
static bool s_audio, s_memory, s_reduced;
static mars_ease_t parse_ease(const char *name)
{
    if (!strcmp(name, "in-out")) return MARS_EASE_IN_OUT;
    return !strcmp(name, "linear") ? MARS_EASE_LINEAR : MARS_EASE_OUT;
}
mars_ui_motion_t mars_ui_motion(void)
{
    unsigned fps = s_cap;
    if (s_audio && fps > 30) fps = 30;
    if (s_memory && fps > 20) fps = 20;
    return (mars_ui_motion_t){fps, s_reduced ? 0 : MARS_UI_MOTION_TRANSITIONMS,
                             s_reduced ? 0 : MARS_UI_MOTION_MODALMS,
                             parse_ease(MARS_UI_MOTION_EASE), parse_ease(MARS_UI_MOTION_MODALEASE)};
}
void mars_ui_motion_constraints(bool audio, bool memory, bool reduced)
{
    s_audio = audio; s_memory = memory; s_reduced = reduced;
    s_samples = s_slow = 0;
}
void mars_ui_motion_sample(uint32_t draw_us)
{
    const unsigned fps = mars_ui_motion().fps;
    /* Eight samples, at least six >110% budget. Never upgrade automatically. */
    s_slow += (uint64_t)draw_us * fps > 1100000;
    if (++s_samples < 8) return;
    if (s_slow >= 6) {
        unsigned next = fps > 50 ? 40 : fps > 30 ? 30 : fps > 20 ? 20 : fps > 15 ? 15 : fps;
        if (next < s_cap) s_cap = next;
    }
    s_samples = s_slow = 0;
}
void mars_ui_motion_reset(void)
{
    s_cap = MARS_UI_MOTION_FPS; s_samples = s_slow = 0;
}
