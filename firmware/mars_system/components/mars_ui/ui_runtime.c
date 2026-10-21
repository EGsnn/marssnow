#include "ui_runtime.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "box3_board.h"
#include "mars_board.h"
#include "box3_gfx.h"
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "font_cn.h"
#include "line_disc.h"
#include "desktop_apps_json.h"
#include "desktop_dock.h"
#include "mars_boot.h"
#include "sdkconfig.h"

#ifdef CONFIG_MARS_LAYOUT_V2
#include "mars_assets.h"
#endif

#define UI_LINE_CAPACITY (16 * 1024)
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
#define UI_APP_DRAWER_ACTIONS "{\"id\":\"app.apps.open\",\"label\":\"APP\"},{\"id\":\"app.apps.close\",\"label\":\"CLOSE APPS\"},{\"id\":\"app.fluid_clock.open\",\"label\":\"FLUID CLOCK\"},"
#else
#define UI_APP_DRAWER_ACTIONS ""
#endif

#define UI_TAP_SLOP 8
#define UI_FILE_PATH "/spiffs/ui.json"
#define RGB565(r, g, b) (uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3))

/* 旧布局内置壁纸；v2 从共享 assets mmap 资源包读取。 */
#ifndef CONFIG_MARS_LAYOUT_V2
extern const uint16_t bg_landscape[320 * 240];
extern const uint16_t bg_sunrise[320 * 240];
extern const uint16_t bg_galaxy[320 * 240];
extern const uint16_t bg_neon[320 * 240];
#endif

static const char *TAG = "ui_runtime";
static const char *DEFAULT_DOCUMENT =
    "{\"version\":2,\"entryPage\":\"home\",\"pages\":["
    "{\"id\":\"home\",\"background\":\"#0d121f\",\"elements\":["
    "{\"type\":\"image\",\"x\":0,\"y\":0,\"width\":320,\"height\":240,\"src\":\"landscape\"},"
#ifndef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    MARS_NATIVE_DOCK_ELEMENTS
#else
    "{\"type\":\"block\",\"x\":12,\"y\":198,\"width\":296,\"height\":36,\"radius\":18,\"background\":\"#0d121f\",\"borderWidth\":1,\"borderColor\":\"#23304a\"},"
    "{\"type\":\"button\",\"x\":12,\"y\":203,\"width\":28,\"height\":28,\"radius\":8,\"background\":\"#2a9d6e\",\"icon\":\"home\",\"color\":\"#ffffff\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.xiaozhi.open\"}},"
    "{\"type\":\"button\",\"x\":79,\"y\":203,\"width\":28,\"height\":28,\"radius\":8,\"background\":\"#2f6df6\",\"icon\":\"square\",\"color\":\"#ffffff\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.clock.open\"}},"
    "{\"type\":\"button\",\"x\":146,\"y\":203,\"width\":28,\"height\":28,\"radius\":8,\"background\":\"#f28c3a\",\"icon\":\"play\",\"color\":\"#ffffff\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.timer.open\"}},"
    "{\"type\":\"button\",\"x\":213,\"y\":203,\"width\":28,\"height\":28,\"radius\":8,\"background\":\"#6b7a90\",\"icon\":\"settings\",\"color\":\"#ffffff\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.system.open\"}},"
    "{\"type\":\"button\",\"x\":280,\"y\":203,\"width\":28,\"height\":28,\"radius\":8,\"background\":\"#66ccff\",\"icon\":\"square\",\"color\":\"#ffffff\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.dyn.0.open\"}}"
#endif
    "]},"
    "{\"id\":\"about\",\"background\":\"#101827\",\"elements\":["
    "{\"type\":\"text\",\"x\":20,\"y\":32,\"width\":280,\"height\":36,\"text\":\"JSON PAGE ROUTING\",\"fontSize\":18,\"color\":\"#39c083\",\"align\":\"center\"},"
    "{\"type\":\"text\",\"x\":20,\"y\":82,\"width\":280,\"height\":30,\"text\":\"UI AND ACTIONS DECOUPLED\",\"fontSize\":12,\"color\":\"#eef2f7\",\"align\":\"center\"},"
    "{\"type\":\"button\",\"x\":45,\"y\":154,\"width\":230,\"height\":54,\"radius\":12,\"background\":\"#39c083\",\"text\":\"BACK HOME\",\"fontSize\":18,\"color\":\"#0d121f\",\"onTap\":{\"type\":\"navigate\",\"pageId\":\"home\"}}]}]}";

static char s_line[UI_LINE_CAPACITY];
static mars_line_disc_t s_line_disc;
static bool s_line_disc_ready;
static int (*s_selftest_handler)(char *, size_t);
static cJSON *s_document;
static cJSON *s_current_page;
static bool s_active;
static bool s_start_suspended;
static bool s_native_shell;
static ui_runtime_action_handler_t s_action_handler;

typedef struct {
    bool down;
    bool moved;
    int start_y;
    int last_x;
    int last_y;
    int start_offset;
    cJSON *container;
    cJSON *slider;
} ui_gesture_t;

static ui_gesture_t s_gesture;

static int number_or(const cJSON *object, const char *name, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static const char *string_or(const cJSON *object, const char *name, const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
}

static bool bool_or(const cJSON *object, const char *name, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static uint16_t parse_color(const char *value, uint16_t fallback)
{
    if (!value || value[0] != '#' || strlen(value) != 7) return fallback;
    unsigned int rgb = 0;
    if (sscanf(value + 1, "%06x", &rgb) != 1) return fallback;
    return RGB565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

static void set_number(cJSON *object, const char *name, int value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsNumber(item)) cJSON_SetNumberValue(item, value);
    else cJSON_AddNumberToObject(object, name, value);
}

static void draw_border_box(int x, int y, int width, int height, int radius,
                            int border_width, uint16_t background, uint16_t border)
{
    if (width <= 0 || height <= 0) return;
    if (border_width > 0) {
        gfx_round_rect(x, y, width, height, radius, border);
        const int inset = border_width * 2 >= width || border_width * 2 >= height ? 0 : border_width;
        if (inset > 0) gfx_round_rect(x + inset, y + inset, width - inset * 2,
                                     height - inset * 2, radius > inset ? radius - inset : 0, background);
    } else gfx_round_rect(x, y, width, height, radius, background);
}

static void draw_icon(const char *name, int x, int y, int size, uint16_t color)
{
    if (size < 8) size = 8;
    const int c = size / 2;
    if (strcmp(name, "play") == 0) {
        for (int row = 0; row < size; ++row) {
            int half = row <= c ? row : size - 1 - row;
            gfx_rect(x, y + row, half + 1, 1, color);
        }
    } else if (strcmp(name, "apps") == 0) {
        const int gap = size / 4;
        const int tile = (size - gap) / 2;
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 2; ++col)
                gfx_round_rect(x + col * (tile + gap), y + row * (tile + gap),
                               tile, tile, tile > 4 ? 2 : 1, color);
    } else if (strcmp(name, "clock") == 0) {
        static const uint8_t points[8][2]={{6,0},{13,0},{19,6},{19,13},{13,19},{6,19},{0,13},{0,6}};
        for(int i=0;i<8;++i) {
            const int j=(i+1)%8;
            gfx_line(x+points[i][0]*(size-1)/19,y+points[i][1]*(size-1)/19,
                     x+points[j][0]*(size-1)/19,y+points[j][1]*(size-1)/19,2,color);
        }
        gfx_line(x+c,y+size/4,x+c,y+c,2,color);
        gfx_line(x+c,y+c,x+size*3/4,y+c+size/8,2,color);
        gfx_rect(x+c-1,y+c-1,3,3,color);
    } else if (strcmp(name, "square") == 0) gfx_rect(x, y, size, size, color);
    else if (strcmp(name, "plus") == 0) {
        gfx_rect(x + c - 1, y, 3, size, color);
        gfx_rect(x, y + c - 1, size, 3, color);
    } else if (strcmp(name, "home") == 0) {
        gfx_line(x, y + c, x + c, y, 2, color);
        gfx_line(x + c, y, x + size - 1, y + c, 2, color);
        gfx_frame(x + 2, y + c, size - 4, size - c, 2, color);
    } else if (strcmp(name, "settings") == 0) {
        gfx_frame(x + 2, y + 2, size - 4, size - 4, 2, color);
        gfx_rect(x + c - 1, y + c - 1, 3, 3, color);
    } else if (strcmp(name, "heart") == 0) {
        gfx_rect(x + 1, y + 2, c - 1, c, color);
        gfx_rect(x + c, y + 2, c - 1, c, color);
        gfx_line(x + 1, y + c, x + c, y + size - 1, 2, color);
        gfx_line(x + size - 2, y + c, x + c, y + size - 1, 2, color);
    }
}

static int font_scale(int pixels)
{
    if (pixels >= 25) return 4;
    if (pixels >= 18) return 3;
    if (pixels >= 12) return 2;
    return 1;
}

/* 内置背景图案：image 组件 src 指定后全屏铺满绘制 */
static void draw_background(const char *name, int x, int y, int width, int height)
{
    const bool valid = strcmp(name, "gradient") == 0 || strcmp(name, "aurora") == 0 ||
                       strcmp(name, "grid") == 0 || strcmp(name, "landscape") == 0 ||
                       strcmp(name, "sunrise") == 0 || strcmp(name, "galaxy") == 0 ||
                       strcmp(name, "neon") == 0;
    if (!valid) return;
    if (strcmp(name, "landscape") == 0
#ifdef CONFIG_MARS_LAYOUT_V2
        || strcmp(name, "sunrise") == 0 || strcmp(name, "galaxy") == 0 ||
            strcmp(name, "neon") == 0
#endif
    ) {
        /* 内置风景壁纸：画入离屏画布，随 gfx_present 整幅上屏 */
        if (width == 320 && height == 240) {
#ifdef CONFIG_MARS_LAYOUT_V2
            const uint16_t *landscape = mars_assets_landscape();
            if (landscape) {
                gfx_bitmap(0, 0, 320, 240, landscape);
                return;
            }
#else
            gfx_bitmap(0, 0, 320, 240, bg_landscape);
            return;
#endif
        }
#ifndef CONFIG_MARS_LAYOUT_V2
        return;
#endif
    }
#ifndef CONFIG_MARS_LAYOUT_V2
    if (strcmp(name, "sunrise") == 0) {
        if (width == 320 && height == 240) gfx_bitmap(0, 0, 320, 240, bg_sunrise);
        return;
    }
    if (strcmp(name, "galaxy") == 0) {
        if (width == 320 && height == 240) gfx_bitmap(0, 0, 320, 240, bg_galaxy);
        return;
    }
    if (strcmp(name, "neon") == 0) {
        if (width == 320 && height == 240) gfx_bitmap(0, 0, 320, 240, bg_neon);
        return;
    }
#endif
    /* 垂直渐变 #0d121f -> #1d2a4a */
    const int r0 = 13, g0 = 18, b0 = 31, r1 = 29, g1 = 42, b1 = 74;
    for (int row = 0; row < height; ++row) {
        const int t = row * 255 / (height > 1 ? height - 1 : 1);
        const uint16_t c = RGB565((uint8_t)(r0 + (r1 - r0) * t / 255),
                                  (uint8_t)(g0 + (g1 - g0) * t / 255),
                                  (uint8_t)(b0 + (b1 - b0) * t / 255));
        gfx_rect(x, y + row, width, 1, c);
    }
    if (strcmp(name, "aurora") == 0) {
        /* 明亮壁纸：高饱和多层光晕 + 星星点缀，铺满全屏 */
        gfx_round_rect(x + width - 160, y + 14, 160, 130, 65, RGB565(50, 120, 130));
        gfx_round_rect(x + width - 110, y + 60, 110, 100, 50, RGB565(70, 110, 160));
        gfx_round_rect(x + 12, y + height - 140, 170, 120, 60, RGB565(70, 80, 170));
        gfx_round_rect(x + 80, y + height - 95, 130, 80, 40, RGB565(120, 110, 180));
        gfx_round_rect(x + width - 90, y + height - 60, 90, 60, 30, RGB565(120, 70, 60));
        gfx_rect(x + 42, y + 30, 2, 2, RGB565(220, 228, 245));
        gfx_rect(x + 96, y + 64, 2, 2, RGB565(220, 228, 245));
        gfx_rect(x + 182, y + 24, 2, 2, RGB565(220, 228, 245));
        gfx_rect(x + 248, y + 84, 2, 2, RGB565(220, 228, 245));
        gfx_rect(x + 286, y + 40, 2, 2, RGB565(220, 228, 245));
        gfx_rect(x + 60, y + 140, 2, 2, RGB565(200, 210, 235));
        gfx_rect(x + 160, y + 100, 2, 2, RGB565(200, 210, 235));
        gfx_rect(x + 210, y + 120, 2, 2, RGB565(200, 210, 235));
    } else if (strcmp(name, "grid") == 0) {
        for (int gx = 24; gx < width; gx += 24) gfx_rect(x + gx, y, 1, height, RGB565(28, 36, 52));
        for (int gy = 24; gy < height; gy += 24) gfx_rect(x, y + gy, width, 1, RGB565(28, 36, 52));
    }
}

static void render_elements(const cJSON *elements, int origin_x, int origin_y, uint16_t page_background);

static void render_element(const cJSON *element, int origin_x, int origin_y, uint16_t page_background)
{
    if (!bool_or(element, "visible", true)) return;
    const char *type = string_or(element, "type", "");
    const int x = origin_x + number_or(element, "x", 0);
    const int y = origin_y + number_or(element, "y", 0);
    const int width = number_or(element, "width", 1);
    const int height = number_or(element, "height", 1);
    const int radius = number_or(element, "radius", 0);
    const int border_width = number_or(element, "borderWidth", 0);
    const uint16_t background = parse_color(string_or(element, "background", NULL), page_background);
    const uint16_t border = parse_color(string_or(element, "borderColor", NULL), background);
    const uint16_t color = parse_color(string_or(element, "color", NULL), RGB565(238, 242, 247));

    if (strcmp(type, "container") == 0) {
        draw_border_box(x, y, width, height, radius, border_width, background, border);
        const cJSON *children = cJSON_GetObjectItemCaseSensitive(element, "children");
        const cJSON *scroll = cJSON_GetObjectItemCaseSensitive(element, "scroll");
        const bool vertical = cJSON_IsObject(scroll) && strcmp(string_or(scroll, "direction", "vertical"), "vertical") == 0;
        int offset = vertical ? number_or(scroll, "offset", 0) : 0;
        int content_height = vertical ? number_or(scroll, "contentHeight", height) : height;
        int max_offset = content_height > height ? content_height - height : 0;
        if (offset < 0) offset = 0;
        if (offset > max_offset) offset = max_offset;
        gfx_clip_push(x, y, width, height);
        render_elements(children, x, y - offset, page_background);
        gfx_clip_pop();
        if (vertical && max_offset > 0) {
            const cJSON *bar = cJSON_GetObjectItemCaseSensitive(scroll, "bar");
            const int bar_width = number_or(bar, "width", 4);
            const int track_x = x + width - bar_width - 3;
            const int track_y = y + 4;
            const int track_height = height - 8;
            int thumb_height = track_height * height / content_height;
            if (thumb_height < 18) thumb_height = 18;
            const int thumb_y = track_y + (track_height - thumb_height) * offset / max_offset;
            gfx_round_rect(track_x, track_y, bar_width, track_height, bar_width / 2,
                           parse_color(string_or(bar, "trackColor", NULL), RGB565(55, 65, 81)));
            gfx_round_rect(track_x, thumb_y, bar_width, thumb_height, bar_width / 2,
                           parse_color(string_or(bar, "color", NULL), RGB565(57, 192, 131)));
        }
        return;
    }
    if (strcmp(type, "line") == 0) {
        gfx_line(x, y, x + width - 1, y, number_or(element, "thickness", height), color);
        return;
    }
    if (strcmp(type, "block") == 0) {
        draw_border_box(x, y, width, height, radius, border_width, background, border);
        return;
    }
    if (strcmp(type, "progress") == 0) {
        draw_border_box(x, y, width, height, radius, border_width, background, border);
        int value = number_or(element, "value", 0);
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        const int inset = border_width + 2;
        const int fill_width = (width - inset * 2) * value / 100;
        if (fill_width > 0) gfx_round_rect(x + inset, y + inset, fill_width,
                                           height - inset * 2, radius > inset ? radius - inset : 0, color);
        return;
    }
    if (strcmp(type, "switch") == 0) {
        draw_border_box(x, y, width, height, radius, border_width, background, border);
        const int inset = border_width + 2;
        const int track_w = width - inset * 2;
        const int track_h = height - inset * 2;
        if (track_w <= 0 || track_h <= 0) return;
        const bool on = bool_or(element, "value", false);
        const uint16_t track_color = on ? color : RGB565(48, 54, 64);
        gfx_round_rect(x + inset, y + inset, track_w, track_h, track_h / 2, track_color);
        const int thumb = track_h - 4;
        if (thumb >= 4) {
            const int thumb_x = on ? x + inset + track_w - thumb - 2 : x + inset + 2;
            gfx_round_rect(thumb_x, y + inset + 2, thumb, thumb, thumb / 2, RGB565(238, 242, 247));
        }
        return;
    }
    if (strcmp(type, "list") == 0) {
        draw_border_box(x, y, width, height, radius, border_width, background, border);
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(element, "items");
        const int row_height = number_or(element, "rowHeight", 40);
        const int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
        const cJSON *scroll = cJSON_GetObjectItemCaseSensitive(element, "scroll");
        int offset = cJSON_IsObject(scroll) ? number_or(scroll, "offset", 0) : 0;
        int content_height = count * row_height;
        if (content_height < height) content_height = height;
        int max_offset = content_height > height ? content_height - height : 0;
        if (offset < 0) offset = 0;
        if (offset > max_offset) offset = max_offset;
        const uint16_t divider = parse_color(string_or(element, "lineColor", NULL), RGB565(32, 37, 44));
        if (cJSON_IsArray(items)) {
            gfx_clip_push(x, y, width, height);
            int index = 0;
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, items) {
                const int ry = y - offset + index * row_height;
                if (ry + row_height <= y) { ++index; continue; }
                if (ry >= y + height) break;
                const char *icon = string_or(item, "icon", "none");
                const bool has_icon = strcmp(icon, "none") != 0;
                const char *text = string_or(item, "text", "");
                const int scale = font_scale(number_or(item, "fontSize", number_or(element, "fontSize", 12)));
                if (has_icon) draw_icon(icon, x + 12, ry + (row_height - 12) / 2, 12, color);
                const int text_x = x + (has_icon ? 30 : 12);
                const int text_y = ry + (row_height - gfx_text_height(text, scale)) / 2;
                gfx_text_overlay(text_x, text_y, text, scale, color);
                const int arrow_y = ry + (row_height - gfx_text_height(">", scale)) / 2;
                gfx_text_overlay(x + width - 14 - gfx_text_width(">", scale), arrow_y, ">", scale, RGB565(86, 96, 113));
                if (index < count - 1 && ry + row_height < y + height)
                    gfx_rect(x + 12, ry + row_height, width - 24, 1, divider);
                ++index;
            }
            gfx_clip_pop();
        }
        if (max_offset > 0) {
            const cJSON *bar = cJSON_IsObject(scroll) ? cJSON_GetObjectItemCaseSensitive(scroll, "bar") : NULL;
            const int bar_width = number_or(bar, "width", 4);
            const int track_x = x + width - bar_width - 3;
            const int track_y = y + 4;
            const int track_height = height - 8;
            int thumb_height = track_height * height / content_height;
            if (thumb_height < 18) thumb_height = 18;
            const int thumb_y = track_y + (track_height - thumb_height) * offset / max_offset;
            gfx_round_rect(track_x, track_y, bar_width, track_height, bar_width / 2,
                           parse_color(string_or(bar, "trackColor", NULL), RGB565(55, 65, 81)));
            gfx_round_rect(track_x, thumb_y, bar_width, thumb_height, bar_width / 2,
                           parse_color(string_or(bar, "color", NULL), RGB565(57, 192, 131)));
        }
        return;
    }
    if (strcmp(type, "slider") == 0) {
        const int inset = 2;
        const int track_w = width - inset * 2;
        const int track_h = height - inset * 2;
        if (track_w <= 0 || track_h <= 0) return;
        const int ty = y + inset;
        const uint16_t track = parse_color(string_or(element, "trackColor", NULL), RGB565(48, 54, 64));
        gfx_round_rect(x + inset, ty, track_w, track_h, track_h / 2, track);
        int value = number_or(element, "value", 0);
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        const int fill_w = track_w * value / 100;
        if (fill_w > 0) gfx_round_rect(x + inset, ty, fill_w, track_h, track_h / 2, color);
        const int thumb_d = track_h + 4;
        const int thumb_cx = x + inset + thumb_d / 2 + (track_w - thumb_d) * value / 100;
        const int thumb_y = y + (height - thumb_d) / 2;
        gfx_round_rect(thumb_cx - thumb_d / 2, thumb_y, thumb_d, thumb_d, thumb_d / 2, RGB565(238, 242, 247));
        if (thumb_d >= 8)
            gfx_round_rect(thumb_cx - (thumb_d - 4) / 2, thumb_y + 2, thumb_d - 4, thumb_d - 4,
                           (thumb_d - 4) / 2, color);
        return;
    }
    if (strcmp(type, "image") == 0) {
        const char *src = string_or(element, "src", "");
        draw_background(src, x, y, width, height);
        if (strcmp(src, "gradient") == 0 || strcmp(src, "aurora") == 0 || strcmp(src, "grid") == 0 || strcmp(src, "landscape") == 0 || strcmp(src, "sunrise") == 0 || strcmp(src, "galaxy") == 0 || strcmp(src, "neon") == 0) return;
        draw_border_box(x, y, width, height, radius, border_width, background, border);
        int size = (width < height ? width : height) - 12;
        if (size > 32) size = 32;
        draw_icon(string_or(element, "icon", "home"), x + (width - size) / 2,
                  y + (height - size) / 2, size, color);
        return;
    }

    const char *label = string_or(element, "text", "");
    const int scale = font_scale(number_or(element, "fontSize", 12));
    int text_x = x;
    const int text_y = y + (height - gfx_text_height(label, scale)) / 2;
    const int text_width = gfx_text_width(label, scale);
    const char *align = string_or(element, "align", strcmp(type, "button") == 0 ? "center" : "left");
    if (strcmp(type, "button") == 0) {
        draw_border_box(x, y, width, height, radius, border_width, background, border);
        const char *icon = string_or(element, "icon", "none");
        int icon_size = strcmp(icon, "none") == 0 ? 0 : number_or(element, "iconSize", 12);
        if(icon_size) {
            const int max_size=(width<height?width:height)-8;
            if(icon_size<8)icon_size=8;
            if(icon_size>32)icon_size=32;
            if(icon_size>max_size)icon_size=max_size<8?0:max_size;
        }
        if (icon_size && text_width == 0) {
            /* 只有图标：图标在按钮内水平垂直居中 */
            draw_icon(icon, x + (width - icon_size) / 2, y + (height - icon_size) / 2, icon_size, color);
            return;
        }
        const int content_width = text_width + (icon_size ? icon_size + 5 : 0);
        text_x = x + (width - content_width) / 2 + (icon_size ? icon_size + 5 : 0);
        if (icon_size) draw_icon(icon, text_x - icon_size - 5, y + (height - icon_size) / 2, icon_size, color);
        gfx_text_overlay(text_x, text_y, label, scale, color);
        return;
    }
    if (strcmp(type, "text") != 0) return;
    if (strcmp(align, "center") == 0) text_x = x + (width - text_width) / 2;
    else if (strcmp(align, "right") == 0) text_x = x + width - text_width;
    gfx_text_overlay(text_x, text_y, label, scale, color);
}

static void render_elements(const cJSON *elements, int origin_x, int origin_y, uint16_t page_background)
{
    if (!cJSON_IsArray(elements)) return;
    const cJSON *element = NULL;
    cJSON_ArrayForEach(element, elements) render_element(element, origin_x, origin_y, page_background);
}

static cJSON *find_page(const char *page_id)
{
    if (!s_document || !page_id) return NULL;
    cJSON *pages = cJSON_GetObjectItemCaseSensitive(s_document, "pages");
    cJSON *page = NULL;
    cJSON_ArrayForEach(page, pages) {
        if (strcmp(string_or(page, "id", ""), page_id) == 0) return page;
    }
    return NULL;
}

static void render_current_page(void)
{
    if (!s_current_page) return;
    const uint16_t background = parse_color(string_or(s_current_page, "background", "#0d121f"), RGB565(13, 18, 31));
    gfx_clip_reset();
    gfx_clear(background);
    render_elements(cJSON_GetObjectItemCaseSensitive(s_current_page, "elements"), 0, 0, background);
    gfx_present();
}

static bool adopt_document(cJSON *root)
{
    if (!cJSON_IsObject(root)) return false;
    const bool suspended = s_start_suspended || (s_document != NULL && !s_active);
    const int version = number_or(root, "version", 0);
    if (version == 1) {
        cJSON *screen = cJSON_GetObjectItemCaseSensitive(root, "screen");
        cJSON *elements = cJSON_GetObjectItemCaseSensitive(root, "elements");
        if (!cJSON_IsObject(screen) || !cJSON_IsArray(elements)) return false;
        if (s_document) cJSON_Delete(s_document);
        s_document = root;
        s_current_page = root;
        cJSON_AddStringToObject(root, "background", string_or(screen, "background", "#0d121f"));
    } else if (version == 2) {
        cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
        if (!cJSON_IsArray(pages) || cJSON_GetArraySize(pages) == 0) return false;
        const char *entry = string_or(root, "entryPage", string_or(cJSON_GetArrayItem(pages, 0), "id", ""));
        if (s_document) cJSON_Delete(s_document);
        s_document = root;
        s_current_page = find_page(entry);
        if (!s_current_page) s_current_page = cJSON_GetArrayItem(pages, 0);
    } else return false;
    s_active = !suspended;
    memset(&s_gesture, 0, sizeof(s_gesture));
    if (s_active) render_current_page();
    return true;
}

static bool parse_and_adopt(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
#if defined(CONFIG_MARS_LAYOUT_V2) && !defined(CONFIG_BOARD_TYPE_MARS_ATK_BOX)
    desktop_apps_json_normalize(root);
#endif
    if (!adopt_document(root)) {
        cJSON_Delete(root);
        return false;
    }
    return true;
}

static void save_document(const char *json)
{
    FILE *file = fopen(UI_FILE_PATH, "w");
    if (!file) {
        ESP_LOGW(TAG, "Cannot open %s for UI save", UI_FILE_PATH);
        return;
    }
    const size_t length = strlen(json);
    if (fwrite(json, 1, length, file) != length)
        ESP_LOGW(TAG, "Short write while saving UI to %s", UI_FILE_PATH);
    fclose(file);
    ESP_LOGI(TAG, "UI saved to %s (%u bytes)", UI_FILE_PATH, (unsigned)length);
}

static bool load_saved_document(void)
{
    FILE *file = fopen(UI_FILE_PATH, "r");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length < 2 || length > UI_LINE_CAPACITY) {
        fclose(file);
        return false;
    }
    char *json = malloc((size_t)length + 1);
    if (!json) {
        fclose(file);
        return false;
    }
    const size_t got = fread(json, 1, (size_t)length, file);
    fclose(file);
    json[got] = '\0';
    const bool loaded = got == (size_t)length && parse_and_adopt(json);
    free(json);
    return loaded;
}

static cJSON *find_scroll_at(const cJSON *elements, int px, int py)
{
    if (!cJSON_IsArray(elements)) return NULL;
    cJSON *element = NULL;
    cJSON_ArrayForEach(element, elements) {
        const char *type = string_or(element, "type", "");
        if (strcmp(type, "container") != 0 && strcmp(type, "list") != 0) continue;
        const int x = number_or(element, "x", 0);
        const int y = number_or(element, "y", 0);
        const int width = number_or(element, "width", 0);
        const int height = number_or(element, "height", 0);
        if (px < x || px >= x + width || py < y || py >= y + height) continue;
        int content_height = height;
        if (strcmp(type, "list") == 0) {
            const cJSON *items = cJSON_GetObjectItemCaseSensitive(element, "items");
            const int row_height = number_or(element, "rowHeight", 40);
            content_height = cJSON_IsArray(items) ? cJSON_GetArraySize(items) * row_height : height;
        } else {
            const cJSON *scroll = cJSON_GetObjectItemCaseSensitive(element, "scroll");
            if (!cJSON_IsObject(scroll)) continue;
            content_height = number_or(scroll, "contentHeight", height);
        }
        if (content_height > height) return element;
    }
    return NULL;
}

static cJSON *hit_test(const cJSON *elements, int origin_x, int origin_y, int px, int py, int *hit_row)
{
    if (!cJSON_IsArray(elements)) return NULL;
    if (hit_row) *hit_row = -1;
    for (int i = cJSON_GetArraySize(elements) - 1; i >= 0; --i) {
        cJSON *element = cJSON_GetArrayItem(elements, i);
        if (!bool_or(element, "visible", true)) continue;
        const int x = origin_x + number_or(element, "x", 0);
        const int y = origin_y + number_or(element, "y", 0);
        const int width = number_or(element, "width", 0);
        const int height = number_or(element, "height", 0);
        if (px < x || px >= x + width || py < y || py >= y + height) continue;
        if (strcmp(string_or(element, "type", ""), "container") == 0) {
            cJSON *scroll = cJSON_GetObjectItemCaseSensitive(element, "scroll");
            const int offset = cJSON_IsObject(scroll) ? number_or(scroll, "offset", 0) : 0;
            cJSON *child = hit_test(cJSON_GetObjectItemCaseSensitive(element, "children"), x, y - offset, px, py, hit_row);
            if (child) return child;
        }
        if (strcmp(string_or(element, "type", ""), "list") == 0) {
            const int row_height = number_or(element, "rowHeight", 40);
            const cJSON *scroll = cJSON_GetObjectItemCaseSensitive(element, "scroll");
            const int offset = cJSON_IsObject(scroll) ? number_or(scroll, "offset", 0) : 0;
            const int row = row_height > 0 ? (py - y + offset) / row_height : -1;
            const cJSON *items = cJSON_GetObjectItemCaseSensitive(element, "items");
            if (cJSON_IsArray(items) && row >= 0 && row < cJSON_GetArraySize(items)) {
                const cJSON *item = cJSON_GetArrayItem(items, row);
                if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(item, "onTap"))) {
                    if (hit_row) *hit_row = row;
                    return element;
                }
            }
            continue;
        }
        if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(element, "onTap"))) return element;
    }
    return NULL;
}

static void activate_element(cJSON *element, int row)
{
    const char *type = string_or(element, "type", "");
    if (strcmp(type, "switch") == 0) {
        const bool next = !bool_or(element, "value", false);
        cJSON *value_item = cJSON_GetObjectItemCaseSensitive(element, "value");
        if (value_item) cJSON_SetBoolValue(value_item, next);
        else cJSON_AddBoolToObject(element, "value", next);
        render_current_page();
        return;
    }
    cJSON *action = NULL;
    if (strcmp(type, "list") == 0 && row >= 0) {
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(element, "items");
        const cJSON *item = cJSON_GetArrayItem(items, row);
        if (item) action = cJSON_GetObjectItemCaseSensitive(item, "onTap");
    }
    if (!action) action = cJSON_GetObjectItemCaseSensitive(element, "onTap");
    const char *action_type = string_or(action, "type", "");
    if (strcmp(action_type, "navigate") == 0) {
        cJSON *target = find_page(string_or(action, "pageId", ""));
        if (target) {
            s_current_page = target;
            render_current_page();
        }
    } else if (strcmp(action_type, "invoke") == 0 && s_action_handler) {
        s_action_handler(string_or(action, "actionId", ""));
    }
}

static void print_capabilities(void)
{
    printf("@CAPS:{\"protocolVersion\":2,\"board\":%s,\"jsonUi\":%s,\"nativeSystemUi\":%s,\"systemDisplay\":{\"width\":%d,\"height\":%d},\"display\":{\"width\":%d,\"height\":%d,\"colorDepth\":16,\"touch\":%s},\"widgets\":[\"button\",\"text\",\"block\",\"image\",\"line\",\"progress\",\"container\",\"switch\",\"list\",\"slider\"],\"layouts\":[\"absolute\",\"vertical-scroll\"],\"maxJsonBytes\":%d,\"actions\":[" UI_APP_DRAWER_ACTIONS "{\"id\":\"app.xiaozhi.open\",\"label\":\"XIAOZHI\"},{\"id\":\"app.clock.open\",\"label\":\"CLOCK\"},{\"id\":\"app.timer.open\",\"label\":\"TIMER\"},{\"id\":\"app.counter.open\",\"label\":\"COUNTER\"},{\"id\":\"app.system.open\",\"label\":\"SYSTEM\"},{\"id\":\"app.about.open\",\"label\":\"ABOUT\"},{\"id\":\"app.back\",\"label\":\"BACK\"}],\"routes\":[\"native:launcher\"]}\n",
           mars_board_profile_json(), s_native_shell ? "false" : "true", s_native_shell ? "true" : "false",
           s_native_shell ? MARS_BOARD_DISPLAY_WIDTH : BOX3_LCD_WIDTH,
           s_native_shell ? MARS_BOARD_DISPLAY_HEIGHT : BOX3_LCD_HEIGHT,
           BOX3_LCD_WIDTH, BOX3_LCD_HEIGHT,
           mars_board_touch_available() ? "true" : "false", s_native_shell ? 0 : UI_LINE_CAPACITY - 1);
    fflush(stdout);
}

static void print_boot_state(void)
{
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();
    printf("@BOOT:%s running:%s\n", boot ? boot->label : "?", running ? running->label : "?");
    fflush(stdout);
}

/* @SWITCH:box|factory|ota0|ota1 -> 跑火星系统槽（ota_0/1 是火星 A/B）；
 * @SWITCH:xiaozhi|ota2|ota3 -> 跑小智槽；@SWITCH:? -> 查询当前引导分区。
 * 切换前统一走 mars_boot 校验，无效分区不重启 */
static void handle_switch(const char *target)
{
    if (strcmp(target, "?") == 0) {
        print_boot_state();
        return;
    }
    const char *app_id = NULL;
    if (strcmp(target, "box") == 0 || strcmp(target, "factory") == 0) {
        app_id = "box";
    } else if (strcmp(target, "xiaozhi") == 0 || strcmp(target, "ota2") == 0 || strcmp(target, "ota3") == 0) {
        app_id = "xiaozhi";
    } else if (strcmp(target, "ota0") == 0 || strcmp(target, "ota1") == 0) {
        app_id = "box"; /* 新布局下 ota_0/ota_1 是火星 A/B 槽 */
    }
    if (app_id == NULL) {
        printf("@SWITCH:ERROR:no-target\n");
        fflush(stdout);
        return;
    }
    if (!mars_boot_check(app_id)) {
        printf("@SWITCH:ERROR:no-valid-firmware\n");
        fflush(stdout);
        return;
    }
    const esp_err_t err = mars_boot_switch(app_id);
    if (err != ESP_OK) {
        printf("@SWITCH:ERROR:%s\n", esp_err_to_name(err));
        fflush(stdout);
    }
    /* mars_boot_switch 成功后内部 esp_restart，不返回 */
}

static void load_font(void)
{
#ifdef CONFIG_MARS_LAYOUT_V2
    if (font_cn_init_embedded()) ESP_LOGI(TAG, "Chinese font loaded from system image");
    else ESP_LOGE(TAG, "embedded Chinese font invalid");
#else
    FILE *file = fopen("/spiffs/font_cn.bin", "rb");
    if (!file) {
        ESP_LOGW(TAG, "font_cn.bin not found in uidata; Chinese glyphs unavailable");
        return;
    }
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length < 16) {
        fclose(file);
        ESP_LOGE(TAG, "font_cn.bin too small (%ld bytes)", length);
        return;
    }
    uint8_t *data = malloc((size_t)length);
    if (!data) {
        fclose(file);
        ESP_LOGE(TAG, "OOM loading font (%ld bytes)", length);
        return;
    }
    const size_t got = fread(data, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) {
        free(data);
        ESP_LOGE(TAG, "short read of font_cn.bin (%u/%ld)", (unsigned)got, length);
        return;
    }
    if (font_cn_init(data, (size_t)length))
        ESP_LOGI(TAG, "Chinese font loaded from uidata (%ld bytes)", length);
    else {
        free(data);
        ESP_LOGE(TAG, "font_cn.bin invalid header");
    }
#endif
}

void ui_runtime_init(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {
            .tx_buffer_size = 2048,
            .rx_buffer_size = 16384,
        };
        const esp_err_t result = usb_serial_jtag_driver_install(&config);
        if (result == ESP_OK) usb_serial_jtag_vfs_use_driver();
        else ESP_LOGW(TAG, "USB-JTAG buffered driver unavailable: %s", esp_err_to_name(result));
    }
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = "uidata",
        .max_files = 4,
        /* Never erase the user's desktop because of a failed mount. */
        .format_if_mount_failed = false,
    };
    esp_err_t mount = esp_vfs_spiffs_register(&spiffs_conf);
    if (mount == ESP_OK) {
        ESP_LOGI(TAG, "uidata partition mounted at /spiffs");
    } else {
        /* V2 首次烧录后 uidata 分区是空白（全 0xFF），挂载必然失败。
         * 区分两种情况：
         *   - 空白分区（新芯片/首次迁移）：允许格式化一次，建立可写 SPIFFS。
         *   - 非空白（文件系统损坏）：不自动格式化，避免静默丢失用户数据。
         */
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "uidata");
        bool blank = false;
        if (part != NULL) {
            uint8_t buf[16];
            if (esp_partition_read(part, 0, buf, sizeof(buf)) == ESP_OK) {
                blank = true;
                for (size_t i = 0; i < sizeof(buf); ++i) {
                    if (buf[i] != 0xFF) { blank = false; break; }
                }
            }
        }
        if (blank) {
            ESP_LOGW(TAG, "uidata is blank (fresh V2 partition), formatting once");
            spiffs_conf.format_if_mount_failed = true;
            mount = esp_vfs_spiffs_register(&spiffs_conf);
            if (mount == ESP_OK) ESP_LOGI(TAG, "uidata formatted and mounted at /spiffs");
            else ESP_LOGE(TAG, "uidata format+mount failed: %s", esp_err_to_name(mount));
        } else {
            ESP_LOGW(TAG, "uidata SPIFFS mount failed (not blank, will not auto-format): %s",
                     esp_err_to_name(mount));
        }
    }

    load_font();

    if (load_saved_document()) ESP_LOGI(TAG, "Loaded saved JSON UI v%d", number_or(s_document, "version", 0));
    else {
        parse_and_adopt(DEFAULT_DOCUMENT);
        ESP_LOGI(TAG, "Loaded built-in desktop (no saved ui.json)");
    }
    ESP_LOGI(TAG, "JSON UI v2 ready (@UI:<json>, @CAPS?)");
}

void ui_runtime_init_suspended(void)
{
    s_native_shell = true;
    s_start_suspended = true;
    ui_runtime_init();
    s_start_suspended = false;
}

void ui_runtime_set_action_handler(ui_runtime_action_handler_t handler)
{
    s_action_handler = handler;
}

void ui_runtime_set_selftest_handler(int (*handler)(char *, size_t))
{
    s_selftest_handler = handler;
}

/* 一条完整串口行到达（由 line_disc 在收到 CR/LF 时回调）。
 * 保持原有 @CAPS?/@ACTION/@UI/@SWITCH 行为不变；新增 @AUDIO:SELFTEST。 */
static void on_serial_line(const char *line, void *ctx)
{
    bool *updated = ctx;
    if (strcmp(line, "@CAPS?") == 0) {
        print_capabilities();
    } else if (strncmp(line, "@ACTION:", 8) == 0) {
        const bool handled = s_action_handler && s_action_handler(line + 8);
        printf("@ACTION:%s\n", handled ? "OK" : "ERROR");
        fflush(stdout);
    } else if (strncmp(line, "@SWITCH:", 8) == 0) {
        handle_switch(line + 8);
    } else if (strcmp(line, "@AUDIO:SELFTEST") == 0) {
        /* 唯一串口消费者行解析器内的音频自测命令（P0-1 / P1-3）。
         * 自测返回明确状态：PASS 或 FAIL:<reason>，不再用 DONE 当通过证据。 */
        if (s_selftest_handler != NULL) {
            char reason[64] = {0};
            printf("@AUDIO:SELFTEST:RUNNING\n");
            fflush(stdout);
            int rc = s_selftest_handler(reason, sizeof(reason));
            if (rc == 0) {
                printf("@AUDIO:SELFTEST:PASS\n");
            } else {
                printf("@AUDIO:SELFTEST:FAIL:%s\n", reason[0] ? reason : "unknown");
            }
            fflush(stdout);
        } else {
            printf("@AUDIO:SELFTEST:UNAVAILABLE\n");
            fflush(stdout);
        }
    } else if (strncmp(line, "@UI:", 4) == 0) {
        if (s_native_shell) {
            printf("@UI:ERROR:native-system-ui\n");
            fflush(stdout);
            return;
        }
        if (parse_and_adopt(line + 4)) {
            save_document(line + 4);
            *updated = true;
            printf("@UI:OK\n");
        } else printf("@UI:ERROR:invalid-json\n");
        fflush(stdout);
    }
    /* 未知命令静默丢弃（不污染下一行） */
}

static void on_serial_overflow(void *ctx)
{
    (void)ctx;
    printf("@UI:ERROR:payload-too-large\n");
    fflush(stdout);
}

bool ui_runtime_poll(void)
{
    if (!s_line_disc_ready) {
        mars_line_disc_init(&s_line_disc, s_line, sizeof(s_line));
        s_line_disc_ready = true;
    }
    char incoming[256];
    const ssize_t count = read(STDIN_FILENO, incoming, sizeof(incoming));
    if (count <= 0) return false;
    bool updated = false;
    mars_line_disc_feed(&s_line_disc, incoming, (size_t)count,
                        on_serial_line, &updated, on_serial_overflow);
    return updated;
}

bool ui_runtime_is_active(void) { return s_active; }

void ui_runtime_suspend(void)
{
    s_active = false;
    memset(&s_gesture, 0, sizeof(s_gesture));
}

void ui_runtime_resume(void)
{
    if (!s_document) return;
    s_active = true;
    render_current_page();
}

void ui_runtime_handle_touch(bool down, uint16_t x, uint16_t y)
{
    if (!s_active || !s_current_page) return;
    cJSON *elements = cJSON_GetObjectItemCaseSensitive(s_current_page, "elements");
    if (down && !s_gesture.down) {
        s_gesture.down = true;
        s_gesture.start_y = y;
        s_gesture.last_x = x;
        s_gesture.last_y = y;
        s_gesture.container = find_scroll_at(elements, x, y);
        if (s_gesture.container) {
            cJSON *scroll = cJSON_GetObjectItemCaseSensitive(s_gesture.container, "scroll");
            if (!cJSON_IsObject(scroll)) {
                scroll = cJSON_AddObjectToObject(s_gesture.container, "scroll");
                cJSON_AddNumberToObject(scroll, "offset", 0);
            }
            s_gesture.start_offset = number_or(scroll, "offset", 0);
        } else {
            int row = -1;
            cJSON *hit = hit_test(elements, 0, 0, x, y, &row);
            if (hit && strcmp(string_or(hit, "type", ""), "slider") == 0) s_gesture.slider = hit;
        }
        return;
    }
    if (down && s_gesture.down) {
        const int delta = s_gesture.start_y - (int)y;
        s_gesture.last_x = x;
        if (delta > UI_TAP_SLOP || delta < -UI_TAP_SLOP) s_gesture.moved = true;
        if (s_gesture.slider) {
            cJSON *s = s_gesture.slider;
            const int sx = number_or(s, "x", 0);
            const int sw = number_or(s, "width", 1);
            const int usable = sw > 4 ? sw - 4 : 1;
            int value = ((int)x - sx - 2) * 100 / usable;
            if (value < 0) value = 0;
            if (value > 100) value = 100;
            if (value != number_or(s, "value", 0)) {
                set_number(s, "value", value);
                render_current_page();
            }
            return;
        }
        if (s_gesture.container && y != s_gesture.last_y) {
            cJSON *scroll = cJSON_GetObjectItemCaseSensitive(s_gesture.container, "scroll");
            const int height = number_or(s_gesture.container, "height", 0);
            int content_height = height;
            if (strcmp(string_or(s_gesture.container, "type", ""), "list") == 0) {
                const cJSON *items = cJSON_GetObjectItemCaseSensitive(s_gesture.container, "items");
                const int row_height = number_or(s_gesture.container, "rowHeight", 40);
                content_height = cJSON_IsArray(items) ? cJSON_GetArraySize(items) * row_height : height;
            } else {
                content_height = number_or(scroll, "contentHeight", height);
            }
            int max_offset = content_height - height;
            if (max_offset < 0) max_offset = 0;
            int offset = s_gesture.start_offset + delta;
            if (offset < 0) offset = 0;
            if (offset > max_offset) offset = max_offset;
            if (offset != number_or(scroll, "offset", 0)) {
                set_number(scroll, "offset", offset);
                render_current_page();
            }
            s_gesture.last_y = y;
        }
        return;
    }
    if (!down && s_gesture.down) {
        if (!s_gesture.moved) {
            int row = -1;
            cJSON *element = hit_test(elements, 0, 0, s_gesture.last_x, s_gesture.last_y, &row);
            if (element) activate_element(element, row);
        }
        memset(&s_gesture, 0, sizeof(s_gesture));
    }
}
