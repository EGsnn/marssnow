#include "apps.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "box3_board.h"
#include "box3_gfx.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mars_app.h"
#include "mars_boot.h"
#include "mars_clock.h"
#include "settings.h"
#ifdef CONFIG_BOARD_TYPE_MARS_ATK_BOX
#include "mars_xiaozhi_bridge.h"
#endif

#define RGB565(r, g, b) (uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3))

enum {
    COLOR_BG = RGB565(13, 18, 31),
    COLOR_PANEL = RGB565(28, 36, 55),
    COLOR_PANEL_HI = RGB565(44, 56, 84),
    COLOR_TEXT = RGB565(235, 240, 250),
    COLOR_MUTED = RGB565(145, 157, 180),
    COLOR_BLUE = RGB565(70, 130, 246),
    COLOR_PURPLE = RGB565(145, 92, 246),
    COLOR_ORANGE = RGB565(245, 151, 64),
    COLOR_GREEN = RGB565(57, 192, 131),
    COLOR_RED = RGB565(236, 86, 91),
    COLOR_CYAN = RGB565(55, 190, 210),
};

static bool s_timer_running;
static int64_t s_timer_base_ms;
static int64_t s_timer_started_ms;
static int s_counter;

static void center_text(int y, const char *text, int scale, uint16_t color, uint16_t background)
{
    gfx_text((BOX3_LCD_WIDTH - gfx_text_width(text, scale)) / 2, y, text, scale, color, background);
}

static void draw_header(const char *title, uint16_t accent, const char *hints)
{
    gfx_clear(COLOR_BG);
    gfx_rect(0, 0, BOX3_LCD_WIDTH, 42, COLOR_PANEL);
    gfx_rect(0, 0, 8, 42, accent);
    gfx_text(18, 10, title, 3, COLOR_TEXT, COLOR_PANEL);
    gfx_text(246, 16, "< BACK", 1, COLOR_MUTED, COLOR_PANEL);
    if (hints) center_text(225, hints, 1, COLOR_MUTED, COLOR_BG);
}

/* ── 绘制函数：只画 framebuffer，不刷屏（present 由调用方/动画控制）── */

static void draw_xiaozhi(bool first_draw)
{
#ifdef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    mars_xiaozhi_render(first_draw);
#else
    (void)first_draw;
    if (mars_boot_check("xiaozhi")) {
        draw_header("XIAOZHI", COLOR_BLUE, "LAUNCHING VOICE CORE...");
        gfx_rect(64, 72, 192, 112, COLOR_PANEL);
        gfx_rect(136, 88, 48, 48, COLOR_BLUE);
        center_text(149, "BOOT SWITCH", 2, COLOR_TEXT, COLOR_PANEL);
        center_text(174, "REBOOTING TO OTA SLOT", 1, COLOR_MUTED, COLOR_PANEL);
    } else {
        draw_header("XIAOZHI", COLOR_BLUE, "VOICE CORE NOT INSTALLED");
        gfx_rect(64, 72, 192, 112, COLOR_PANEL);
        gfx_rect(136, 88, 48, 48, COLOR_PANEL);
        center_text(149, "NO OTA FIRMWARE", 2, COLOR_MUTED, COLOR_PANEL);
        center_text(174, "UPDATE VIA SETTINGS", 1, COLOR_MUTED, COLOR_PANEL);
    }
#endif
}

/* ── 时间页面：两种模式（指针钟表 / 时分秒数字），时间 = IP 所在时区 ── */

enum {
    CLOCK_CX = 160, /* 表盘中心 */
    CLOCK_CY = 102,
    CLOCK_R  = 50,  /* 表盘半径 */
};

/* sin(i°)*1024 查找表（0-89°），避免浮点与 libm 依赖 */
static const int16_t SIN_TAB[90] = {
    0, 18, 36, 54, 71, 89, 107, 125, 143, 160,
    178, 195, 213, 230, 248, 265, 282, 299, 316, 333,
    350, 367, 384, 400, 416, 433, 449, 465, 481, 496,
    512, 527, 543, 558, 573, 587, 602, 616, 630, 644,
    658, 672, 685, 698, 711, 724, 737, 749, 761, 773,
    784, 796, 807, 818, 828, 839, 849, 859, 868, 878,
    887, 896, 904, 912, 920, 928, 935, 943, 949, 956,
    962, 968, 974, 979, 984, 989, 994, 998, 1002, 1005,
    1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024,
};

static int sin1024(int deg)
{
    deg %= 360;
    if (deg < 0) deg += 360;
    const int quad = deg / 90;
    const int rem = deg % 90;
    if (rem == 0) {
        return quad == 1 ? 1024 : (quad == 3 ? -1024 : 0);
    }
    const int base = (quad == 0 || quad == 2) ? SIN_TAB[rem] : SIN_TAB[90 - rem];
    return (quad == 0 || quad == 1) ? base : -base;
}

static int cos1024(int deg) { return sin1024(deg + 90); }

static void clock_draw_circle(int cx, int cy, int r, int thickness, uint16_t color)
{
    if (r <= 0) return;
    if (thickness < 1) thickness = 1;
    const int off = thickness / 2;
    int x = 0;
    int y = r;
    int d = 1 - r;
    while (x <= y) {
        gfx_rect(cx + x - off, cy + y - off, thickness, thickness, color);
        gfx_rect(cx - x - off, cy + y - off, thickness, thickness, color);
        gfx_rect(cx + x - off, cy - y - off, thickness, thickness, color);
        gfx_rect(cx - x - off, cy - y - off, thickness, thickness, color);
        gfx_rect(cx + y - off, cy + x - off, thickness, thickness, color);
        gfx_rect(cx - y - off, cy + x - off, thickness, thickness, color);
        gfx_rect(cx + y - off, cy - x - off, thickness, thickness, color);
        gfx_rect(cx - y - off, cy - x - off, thickness, thickness, color);
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            --y;
        }
        ++x;
    }
}

static void clock_fill_circle(int cx, int cy, int r, uint16_t color)
{
    for (int yy = -r; yy <= r; ++yy) {
        int x = 0;
        while (x * x + yy * yy <= r * r) ++x;
        const int half = x - 1;
        gfx_rect(cx - half, cy + yy, half * 2 + 1, 1, color);
    }
}

/* 指针：0°=12 点方向，顺时针；tail 为过中心的反向尾巴长度 */
static void clock_hand(int deg, int len, int tail, int thickness, uint16_t color)
{
    const int ex = CLOCK_CX + (len * sin1024(deg)) / 1024;
    const int ey = CLOCK_CY - (len * cos1024(deg)) / 1024;
    const int tx = CLOCK_CX - (tail * sin1024(deg)) / 1024;
    const int ty = CLOCK_CY + (tail * cos1024(deg)) / 1024;
    gfx_line(tx, ty, ex, ey, thickness, color);
}

/* 表盘：外圈 + 60 刻度；t 为空时只画盘面（未同步占位） */
static void clock_draw_face(const struct tm *t)
{
    clock_draw_circle(CLOCK_CX, CLOCK_CY, CLOCK_R, 2, COLOR_PANEL_HI);
    for (int i = 0; i < 60; ++i) {
        const int deg = i * 6;
        const bool major = (i % 5) == 0;
        const int r0 = CLOCK_R - (major ? 13 : 8);
        const int r1 = CLOCK_R - (major ? 4 : 5);
        gfx_line(CLOCK_CX + (r0 * sin1024(deg)) / 1024,
                 CLOCK_CY - (r0 * cos1024(deg)) / 1024,
                 CLOCK_CX + (r1 * sin1024(deg)) / 1024,
                 CLOCK_CY - (r1 * cos1024(deg)) / 1024,
                 major ? 2 : 1, major ? COLOR_TEXT : COLOR_MUTED);
    }
    if (!t) return;
    clock_hand((t->tm_hour % 12) * 30 + t->tm_min / 2, 28, 6, 4, COLOR_TEXT);
    clock_hand(t->tm_min * 6 + t->tm_sec / 10, 40, 8, 3, COLOR_TEXT);
    clock_hand(t->tm_sec * 6, 46, 10, 1, COLOR_RED);
    clock_fill_circle(CLOCK_CX, CLOCK_CY, 4, COLOR_TEXT);
    clock_fill_circle(CLOCK_CX, CLOCK_CY, 2, COLOR_RED);
}

static const char *clock_state_text(mars_clock_state_t st)
{
    switch (st) {
    case MARS_CLOCK_SYNCED:  return "NTP SYNCED";
    case MARS_CLOCK_SYNCING: return "SYNCING...";
    case MARS_CLOCK_FAILED:  return "SYNC FAILED";
    default:                 return "NO NETWORK";
    }
}

static uint16_t clock_state_color(mars_clock_state_t st)
{
    switch (st) {
    case MARS_CLOCK_SYNCED: return COLOR_GREEN;
    case MARS_CLOCK_FAILED: return COLOR_RED;
    default:                return COLOR_MUTED;
    }
}

/* 时区行：如 "Asia/Shanghai UTC+8" */
static void clock_draw_tz_line(int y)
{
    char line[48];
    const int off = mars_clock_tz_offset_sec();
    if (off == 0) {
        snprintf(line, sizeof(line), "%s", mars_clock_tz_name());
    } else {
        const int h = off / 3600;
        const int m = (off % 3600) / 60;
        if (m == 0) {
            snprintf(line, sizeof(line), "%s UTC%+d", mars_clock_tz_name(), h);
        } else {
            snprintf(line, sizeof(line), "%s UTC%+d:%02d", mars_clock_tz_name(), h, m < 0 ? -m : m);
        }
    }
    center_text(y, line, 1, COLOR_MUTED, COLOR_BG);
}

/* 状态行：SYNCED 时高亮显示时区，否则显示同步状态 */
static void clock_draw_info_line(int y)
{
    const mars_clock_state_t st = mars_clock_state();
    char line[48];
    uint16_t color;
    if (st == MARS_CLOCK_SYNCED) {
        const int off = mars_clock_tz_offset_sec();
        if (off == 0) {
            snprintf(line, sizeof(line), "%s", mars_clock_tz_name());
        } else {
            const int h = off / 3600;
            const int m = (off % 3600) / 60;
            if (m == 0) {
                snprintf(line, sizeof(line), "%s UTC%+d", mars_clock_tz_name(), h);
            } else {
                snprintf(line, sizeof(line), "%s UTC%+d:%02d", mars_clock_tz_name(), h, m < 0 ? -m : m);
            }
        }
        color = COLOR_GREEN;
    } else {
        strlcpy(line, clock_state_text(st), sizeof(line));
        color = clock_state_color(st);
    }
    center_text(y, line, 1, color, COLOR_BG);
}

static bool s_clock_analog = true; /* true=指针钟表，false=时分秒数字 */
static mars_clock_state_t s_clock_last_state = (mars_clock_state_t)-1;

static void draw_clock(bool first_draw)
{
    if (first_draw) draw_header("CLOCK", COLOR_PURPLE, "TAP TO SWITCH MODE");

    const mars_clock_state_t st = mars_clock_state();
    struct tm t;
    const bool have_time = mars_clock_get_local_time(&t);

    /* 未同步且状态未变：无需重绘 */
    if (!have_time && !first_draw && st == s_clock_last_state) return;
    s_clock_last_state = st;

    /* 每次刷新先清除内容区，避免旧指针和模式内容残留。保留顶部标题与底部提示。 */
    if (!first_draw) gfx_rect(0, 42, BOX3_LCD_WIDTH, 178, COLOR_BG);

    if (s_clock_analog) {
        /* 模式一：指针钟表 */
        clock_draw_face(have_time ? &t : NULL);
        if (have_time) {
            char line[16];
            snprintf(line, sizeof(line), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
            center_text(166, line, 2, COLOR_TEXT, COLOR_BG);
        } else {
            center_text(166, "--:--:--", 2, COLOR_MUTED, COLOR_BG);
        }
        clock_draw_info_line(190);
    } else {
        /* 模式二：时分秒数字 */
        if (have_time) {
            char big[12];
            const bool colon = (t.tm_sec & 1) == 0; /* 冒号每秒闪烁 */
            snprintf(big, sizeof(big), "%02d%c%02d%c%02d",
                     t.tm_hour, colon ? ':' : ' ', t.tm_min, colon ? ':' : ' ', t.tm_sec);
            gfx_rect(40, 80, 240, 40, COLOR_PANEL);
            center_text(87, big, 4, COLOR_TEXT, COLOR_PANEL);

            static const char *const wd[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
            char date[64];
            snprintf(date, sizeof(date), "%04d-%02d-%02d %s",
                     1900 + t.tm_year, t.tm_mon + 1, t.tm_mday, wd[t.tm_wday % 7]);
            center_text(132, date, 2, COLOR_TEXT, COLOR_BG);

            clock_draw_tz_line(156);
            clock_draw_info_line(178);
        } else {
            gfx_rect(40, 80, 240, 40, COLOR_PANEL);
            center_text(87, "--:--:--", 4, COLOR_MUTED, COLOR_PANEL);
            center_text(132, "WAITING FOR TIME", 2, COLOR_MUTED, COLOR_BG);
            clock_draw_info_line(178);
        }
    }
}

static void clock_switch_mode(void)
{
    s_clock_analog = !s_clock_analog;
    s_clock_last_state = (mars_clock_state_t)-1; /* 强制重绘 */
    draw_clock(false);
    gfx_present();
}

static int64_t timer_elapsed_ms(void)
{
    if (!s_timer_running) return s_timer_base_ms;
    return s_timer_base_ms + (esp_timer_get_time() / 1000LL - s_timer_started_ms);
}

static void draw_timer(bool first_draw)
{
    if (first_draw) draw_header("TIMER", COLOR_ORANGE, "TOUCH LEFT: START   RIGHT: RESET");
    const int64_t elapsed = timer_elapsed_ms();
    const unsigned minutes = (unsigned)((elapsed / 60000LL) % 100ULL);
    const unsigned seconds = (unsigned)((elapsed / 1000LL) % 60ULL);
    const unsigned tenths = (unsigned)((elapsed / 100LL) % 10ULL);
    char text[12];
    snprintf(text, sizeof(text), "%02u:%02u:%u", minutes, seconds, tenths);
    gfx_rect(40, 78, 240, 78, COLOR_PANEL);
    center_text(102, text, 4, s_timer_running ? COLOR_ORANGE : COLOR_TEXT, COLOR_PANEL);
    center_text(176, s_timer_running ? "RUNNING" : "PAUSED", 2,
                s_timer_running ? COLOR_GREEN : COLOR_MUTED, COLOR_BG);
}

static void timer_key(int key)
{
    if (key == 1) {
        if (s_timer_running) {
            s_timer_base_ms = timer_elapsed_ms();
            s_timer_running = false;
        } else {
            s_timer_started_ms = esp_timer_get_time() / 1000LL;
            s_timer_running = true;
        }
        draw_timer(false);
    } else if (key == 2) {
        s_timer_running = false;
        s_timer_base_ms = 0;
        draw_timer(false);
    }
}

static void draw_counter(bool first_draw)
{
    if (first_draw) draw_header("COUNTER", COLOR_GREEN, "TOUCH LEFT: MINUS   RIGHT: PLUS");
    char text[8];
    snprintf(text, sizeof(text), "%04d", s_counter);
    gfx_rect(82, 75, 156, 90, COLOR_PANEL);
    center_text(105, text, 4, COLOR_TEXT, COLOR_PANEL);
    center_text(184, "BUTTON COUNTER", 2, COLOR_MUTED, COLOR_BG);
}

static void counter_key(int key)
{
    if (key == 1 && s_counter > 0) --s_counter;
    if (key == 2 && s_counter < 9999) ++s_counter;
    draw_counter(false);
}

static void draw_about(bool first_draw)
{
    (void)first_draw;
    draw_header("ABOUT", COLOR_RED, "LIGHTWEIGHT MULTI APP DESKTOP");
    gfx_rect(48, 68, 224, 128, COLOR_PANEL);
    center_text(84, "ATK BOX", 3, COLOR_TEXT, COLOR_PANEL);
    center_text(121, "DESKTOP V0-2", 2, COLOR_RED, COLOR_PANEL);
    center_text(153, "ESP-IDF 5-5-1", 2, COLOR_MUTED, COLOR_PANEL);
    center_text(179, "I80 DISPLAY", 1, COLOR_MUTED, COLOR_PANEL);
}

/* ── 生命周期入口：open 渲染初始页；交互/tick 后显式刷屏 ── */

/* Standalone Mars switches firmware; the unified variant starts an in-process App. */
static void open_xiaozhi(void)
{
#ifdef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    /* Make the Emote canvas visible and clear its stale frame before the
     * launcher captures XiaoZhi's first frame for the slide transition. */
    mars_xiaozhi_open();
    draw_xiaozhi(true);
#else
    draw_xiaozhi(true);
    gfx_present();
    if (!mars_boot_check("xiaozhi")) {
        return; /* 未安装：已显示提示页 */
    }
    vTaskDelay(pdMS_TO_TICKS(400)); /* 让启动页可见后重启 */
    mars_boot_switch("xiaozhi");
    draw_xiaozhi(true); /* switch 异常返回时回落 */
#endif
}
#ifdef CONFIG_BOARD_TYPE_MARS_ATK_BOX
static void close_xiaozhi(void) { mars_xiaozhi_close(); }
static void xiaozhi_key_cb(int key) { if (key == 1 || key == 2) mars_xiaozhi_tap(); }
static void xiaozhi_touch_cb(uint16_t x, uint16_t y)
{
    (void)x;
    if (y >= 46) mars_xiaozhi_tap();
}
static void xiaozhi_tick_cb(int64_t now_ms)
{
    (void)now_ms;
    if (mars_xiaozhi_render(false)) gfx_present();
}
#endif
static void open_clock(void)
{
    mars_clock_init();
    draw_clock(true);
}
static void open_timer(void) { draw_timer(true); }
static void open_counter(void) { draw_counter(true); }
static void open_settings(void) { settings_draw(true); }
static void open_about(void) { draw_about(true); }

static void timer_key_cb(int key)
{
    timer_key(key);
    gfx_present();
}

static void counter_key_cb(int key)
{
    counter_key(key);
    gfx_present();
}

static void settings_key_cb(int key)
{
    settings_key(key);
    settings_draw(false);
}

static void timer_touch_cb(uint16_t x, uint16_t y)
{
    if (y >= 170) timer_key(x < BOX3_LCD_WIDTH / 2 ? 1 : 2);
    gfx_present();
}

static void counter_touch_cb(uint16_t x, uint16_t y)
{
    if (y >= 170) counter_key(x < BOX3_LCD_WIDTH / 2 ? 1 : 2);
    gfx_present();
}

static void settings_touch_cb(uint16_t x, uint16_t y)
{
    settings_touch(x, y);
    settings_draw(false);
}

static void clock_tick_cb(int64_t now_ms)
{
    static int64_t last_bucket = -1;
    const int64_t bucket = now_ms / 1000;
    if (bucket != last_bucket) {
        last_bucket = bucket;
        draw_clock(false);
        gfx_present();
    }
}

/* 点击页面或按左/右键切换 指针/数字 模式 */
static void clock_key_cb(int key)
{
    if (key == 1 || key == 2) clock_switch_mode();
}

static void clock_touch_cb(uint16_t x, uint16_t y)
{
    (void)x;
    (void)y;
    clock_switch_mode();
}

static void timer_tick_cb(int64_t now_ms)
{
    static int64_t last_bucket = -1;
    if (s_timer_running) {
        const int64_t bucket = now_ms / 100;
        if (bucket != last_bucket) {
            last_bucket = bucket;
            draw_timer(false);
            gfx_present();
        }
    }
}

static void settings_tick_cb(int64_t now_ms)
{
    static int64_t last_bucket = -1;
    const int64_t bucket = now_ms / 300;
    if (bucket != last_bucket) {
        last_bucket = bucket;
        settings_draw(false);
    }
}

/* ── App 注册表 ── */

static const mars_app_t APPS[] = {
#ifdef CONFIG_BOARD_TYPE_MARS_ATK_BOX
    {"XIAOZHI", COLOR_BLUE, open_xiaozhi, close_xiaozhi,
     xiaozhi_key_cb, xiaozhi_touch_cb, xiaozhi_tick_cb},
#else
    {"XIAOZHI", COLOR_BLUE, open_xiaozhi, NULL, NULL, NULL, NULL},
#endif
    {"CLOCK", COLOR_PURPLE, open_clock, NULL, clock_key_cb, clock_touch_cb, clock_tick_cb},
    {"TIMER", COLOR_ORANGE, open_timer, NULL, timer_key_cb, timer_touch_cb, timer_tick_cb},
    {"COUNTER", COLOR_GREEN, open_counter, NULL, counter_key_cb, counter_touch_cb, NULL},
    {"SETTINGS", COLOR_CYAN, open_settings, settings_reset, settings_key_cb, settings_touch_cb, settings_tick_cb},
    {"ABOUT", COLOR_RED, open_about, NULL, NULL, NULL, NULL},
};

void box_apps_register_all(void)
{
    for (size_t i = 0; i < sizeof(APPS) / sizeof(APPS[0]); ++i) {
        mars_app_register(&APPS[i]);
    }
}
