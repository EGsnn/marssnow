#include "app_drawer.h"
#include "box3_gfx.h"
#include "mars_anim.h"
#include "mars_ui_adapt.h"
#include "mars_dynamic_bridge.h"
#include "mars_app_loader.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PANEL_X = 24, PANEL_Y = 22, PANEL_W = 272, PANEL_H = 196,
       CARD_X = 36, CARD_Y = 78, CARD_W = 80, CARD_H = 94, CARD_STEP = 84,
       ICON_Y = 82, ICON_SIZE = 48, LABEL_Y = 142 };
static bool s_open, s_closing, s_wait_up, s_down, s_moved;
static uint16_t s_press_x, s_press_y, s_last_x, s_last_y;
static int s_slots[MARS_APP_SLOTS], s_count, s_page, s_launch = -1;
static uint8_t s_opacity;
static mars_anim_t s_fade;
static void draw_page(void);

static bool inside_panel(int x, int y)
{
    return x >= PANEL_X && x < PANEL_X + PANEL_W && y >= PANEL_Y && y < PANEL_Y + PANEL_H;
}

static int hit_card(int x, int y)
{
    if (x < CARD_X || y < CARD_Y || y >= CARD_Y + CARD_H) return -1;
    int column = (x - CARD_X) / CARD_STEP;
    return column < 3 && s_page * 3 + column < s_count &&
           (x - CARD_X) % CARD_STEP < CARD_W ? column : -1;
}

static void draw_app_icon(int x, int slot, const char *name)
{
    if (mars_dynamic_bridge_draw_icon(slot, x, ICON_Y)) return;
    const bool xiaozhi = strcmp(name, "xiaozhi") == 0;
    const bool companion = strcmp(name, "mars_companion") == 0;
    const bool fluid = strcmp(name, "fluid_clock") == 0;
    const bool dialog = strcmp(name, "fluid_dialog") == 0;
    static const uint16_t colors[] = {0x2d73, 0x447f, 0x83df};
    const uint16_t accent = xiaozhi ? colors[0] : colors[slot % 3];
    gfx_round_rect(x, ICON_Y, ICON_SIZE, ICON_SIZE, 12, accent);
    if (xiaozhi) {
        /* Built-in robot icon; no extra bitmap or application ABI needed. */
        gfx_line(x + 24, ICON_Y + 7, x + 24, ICON_Y + 14, 2, 0xffff);
        gfx_round_rect(x + 22, ICON_Y + 4, 4, 4, 2, 0xffff);
        gfx_round_rect(x + 9, ICON_Y + 14, 30, 24, 7, 0xffff);
        gfx_round_rect(x + 16, ICON_Y + 21, 4, 4, 2, accent);
        gfx_round_rect(x + 28, ICON_Y + 21, 4, 4, 2, accent);
        gfx_line(x + 18, ICON_Y + 31, x + 30, ICON_Y + 31, 2, accent);
    } else if (companion) {
        gfx_round_rect(x + 16, ICON_Y + 29, 17, 13, 5, 0xef7b);
        gfx_round_rect(x + 8, ICON_Y + 6, 32, 30, 13, 0xffff);
        gfx_round_rect(x + 12, ICON_Y + 11, 24, 20, 8, 0x194b);
        gfx_rect(x + 18, ICON_Y + 19, 3, 4, 0x97fc);
        gfx_rect(x + 28, ICON_Y + 19, 3, 4, 0x97fc);
        gfx_rect(x + 22, ICON_Y + 26, 6, 1, 0x97fc);
        gfx_rect(x + 22, ICON_Y + 35, 5, 3, 0xfbae);
    } else if (dialog) {
        gfx_round_rect(x + 7, ICON_Y + 9, 34, 27, 10, 0xffff);
        gfx_round_rect(x + 10, ICON_Y + 12, 28, 21, 8, accent);
        gfx_line(x + 15, ICON_Y + 33, x + 11, ICON_Y + 40, 3, 0xffff);
        for(int i=0;i<3;++i)gfx_round_rect(x+15+i*8,ICON_Y+20,4,6,2,0xffff);
    } else if (fluid) {
        gfx_round_rect(x + 9, ICON_Y + 8, 30, 31, 13, 0xffff);
        gfx_round_rect(x + 12, ICON_Y + 11, 24, 25, 10, accent);
        gfx_line(x + 24, ICON_Y + 15, x + 24, ICON_Y + 24, 2, 0xffff);
        gfx_line(x + 24, ICON_Y + 24, x + 31, ICON_Y + 28, 2, 0xffff);
        gfx_rect(x + 10, ICON_Y + 35, 5, 5, 0xffff);
        gfx_rect(x + 34, ICON_Y + 32, 4, 5, 0xffff);
    } else {
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 2; ++col)
                gfx_round_rect(x + 14 + col * 12, ICON_Y + 14 + row * 12,
                               8, 8, 2, 0xffff);
    }
}

static bool hit_close(int x, int y)
{
    return x >= 254 && x < 290 && y >= 28 && y < 64;
}

static void change_page(int direction)
{
    const int next = s_page + direction;
    if (next < 0 || next >= (s_count + 2) / 3) return;
    s_page = next;
    draw_page();
    gfx_present_fade(255);
}

static void fade_step(float value, void *ctx)
{
    (void)ctx;
    s_opacity = (uint8_t)value;
    gfx_present_fade(s_opacity);
}

static void fade_done(void *ctx)
{
    (void)ctx;
    if (s_closing) {
        gfx_restore_background();
        s_open = false;
    }
}

static void draw_page(void)
{
    gfx_clip_reset();
    gfx_round_rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 14, 0x10c3);
    gfx_text(40, 38, "应用", 1, 0xef9f, 0x10c3);
    gfx_text(267, 42, "X", 2, 0x9d12, 0x10c3);
    gfx_line(36, 63, 284, 63, 1, 0x2947);
    for (int item = s_page * 3; item < s_count && item < s_page * 3 + 3; ++item) {
        const int slot = s_slots[item];
        const char *name = mars_dynamic_bridge_package_name(slot);
        if (name == NULL) continue;
        int x = CARD_X + (item - s_page * 3) * CARD_STEP;
        const bool xiaozhi = strcmp(name, "xiaozhi") == 0;
        const char *packaged_name = mars_dynamic_bridge_display_name(slot);
        const char *label = packaged_name ? packaged_name : xiaozhi ? "小智" : strcmp(name, "mars_companion") == 0 ? "火星伙伴" :
                            strcmp(name, "fluid_clock") == 0 ? "时钟" :
                            strcmp(name, "fluid_dialog") == 0 ? "流体对话" : name;
        draw_app_icon(x + (CARD_W - ICON_SIZE) / 2, slot, name);
        const int label_width = gfx_text_width(label, 1);
        const int label_x = label_width <= CARD_W - 4 ? x + (CARD_W - label_width) / 2 : x + 2;
        gfx_clip_push(x + 2, LABEL_Y, CARD_W - 4, 20);
        gfx_text(label_x, LABEL_Y, label, 1, 0xef9f, 0x10c3);
        gfx_clip_pop();
    }
    if (s_count == 0) gfx_text(88, 113, "暂无已安装应用", 1, 0x9d12, 0x10c3);
    if (s_count > 3) {
        char page_text[32];
        snprintf(page_text, sizeof(page_text), "%d/%d", s_page + 1, (s_count + 2) / 3);
        gfx_text(43, 188, "<", 2, 0xef9f, 0x10c3);
        gfx_text(245, 188, ">", 2, 0xef9f, 0x10c3);
        gfx_text(147, 190, page_text, 1, 0xef9f, 0x10c3);
    }
}

void app_drawer_open(void)
{
    if (s_open) return;
    s_open = true; s_closing = false; s_wait_up = true; s_down = false;
    s_opacity = 0; s_count = 0; s_page = 0; s_launch = -1;
    for (int slot = 0; slot < MARS_APP_SLOTS; ++slot)
        if (mars_dynamic_bridge_package_name(slot) != NULL) s_slots[s_count++] = slot;
    gfx_capture_background();
    gfx_shade_background(150);
    draw_page();
    const mars_ui_motion_t motion = mars_ui_motion();
    mars_anim_start(&s_fade, 0, 255, motion.modal_ms, motion.modal_ease, fade_step, fade_done, NULL);
    gfx_present_fade(0);
}

bool app_drawer_is_open(void) { return s_open; }

void app_drawer_close(void)
{
    if (!s_open || s_closing) return;
    s_closing = true;
    const mars_ui_motion_t motion = mars_ui_motion();
    mars_anim_start(&s_fade, s_opacity, 0, motion.modal_ms, motion.modal_ease, fade_step, fade_done, NULL);
}

void app_drawer_tick(int64_t now_ms) { mars_anim_tick(&s_fade, now_ms); }

void app_drawer_touch(bool down, uint16_t x, uint16_t y)
{
    if (!s_open) return;
    if (s_wait_up || mars_anim_running(&s_fade)) {
        if (!down) s_wait_up = false;
        else s_wait_up = true;
        s_down = false;
        return;
    }
    if (down && !s_down) {
        s_press_x = s_last_x = x; s_press_y = s_last_y = y; s_moved = false;
    } else if (down) {
        s_last_x = x; s_last_y = y;
        if (abs((int)x - s_press_x) > 8 || abs((int)y - s_press_y) > 8) s_moved = true;
    } else if (s_down && s_moved) {
        const int dx = (int)s_last_x - s_press_x;
        const int dy = (int)s_last_y - s_press_y;
        if (inside_panel(s_press_x, s_press_y) && abs(dx) >= 36 && abs(dx) > abs(dy) * 2)
            change_page(dx < 0 ? 1 : -1);
    } else if (s_down) {
        /* Release coordinates may be unset by the touch controller. */
        if (!inside_panel(s_press_x, s_press_y) || hit_close(s_press_x, s_press_y)) {
            app_drawer_close();
        } else if (s_count > 3 && s_press_y >= 174 && s_press_y < 218 && s_press_x < 120) {
            change_page(-1);
        } else if (s_count > 3 && s_press_y >= 174 && s_press_y < 218 && s_press_x >= 200) {
            change_page(1);
        } else {
            int card = hit_card(s_press_x, s_press_y);
            if (card >= 0) { s_launch = s_slots[s_page * 3 + card]; app_drawer_close(); }
        }
    }
    s_down = down;
}

int app_drawer_take_launch(void)
{
    int slot = s_launch;
    s_launch = -1;
    return slot;
}
