#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "box3_board.h"
#include "box3_gfx.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "settings_bt.h"
#include "settings_wifi.h"
#include "settings_keyboard.h"

#define RGB565(r, g, b) (uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3))

enum {
    COLOR_BG = RGB565(13, 18, 31),
    COLOR_PANEL = RGB565(28, 36, 55),
    COLOR_PANEL_HI = RGB565(44, 56, 84),
    COLOR_TEXT = RGB565(235, 240, 250),
    COLOR_MUTED = RGB565(145, 157, 180),
    COLOR_BLUE = RGB565(70, 130, 246),
    COLOR_PURPLE = RGB565(145, 92, 246),
    COLOR_GREEN = RGB565(57, 192, 131),
    COLOR_RED = RGB565(236, 86, 91),
    COLOR_CYAN = RGB565(55, 190, 210),
};

#define HEADER_H 42
#define BACK_HIT_Y HEADER_H + 4   /* 与 launcher 的返回区一致：y<46 且 x>=230 */
#define WIFI_LIST_TOP 80
#define WIFI_LIST_ROWS 5
#define WIFI_ROW_H 25
#define WIFI_BTN_Y 210
#define WIFI_BTN_H 26
#define BT_LIST_TOP 110
#define BT_LIST_ROWS 4
#define BT_ROW_H 24
#define BT_BTN_Y 210
#define BT_BTN_H 26

typedef enum {
    PAGE_MENU = 0,
    PAGE_WIFI,
    PAGE_WIFI_KEYBOARD,
    PAGE_BT,
    PAGE_ABOUT,
} settings_page_t;

static settings_page_t s_page = PAGE_MENU;
static int s_menu_selected;
static int s_wifi_selected;
static int s_wifi_scroll;
static int s_bt_selected;
static int s_bt_scroll;
static char s_pending_ssid[33];
static char s_password[64];
static bool s_keyboard_uppercase;
static wifi_keys_mode_t s_keyboard_mode;
static unsigned s_keyboard_page;
static uint32_t s_last_sig = UINT32_MAX;
static int s_bt_scroll_sig = -1; /* 滚动条跳转只改 s_bt_scroll，需单独触发重绘 */

/* UI 层扫描请求标志：用于区分"从未扫描"与"扫描完成" */
static bool s_wifi_scan_requested;
static bool s_bt_scan_requested;

static void set_page(settings_page_t page)
{
    s_page = page;
    s_last_sig = UINT32_MAX; /* 强制重绘 */
}

static void draw_header(const char *title, uint16_t accent)
{
    gfx_clear(COLOR_BG);
    gfx_rect(0, 0, BOX3_LCD_WIDTH, HEADER_H, COLOR_PANEL);
    gfx_rect(0, 0, 8, HEADER_H, accent);
    gfx_text(18, 8, title, 3, COLOR_TEXT, COLOR_PANEL);
    if (s_page != PAGE_MENU) {
        gfx_text(246, 16, "< BACK", 1, COLOR_MUTED, COLOR_PANEL);
    }
}

static void truncate_to_width(const char *in, char *out, size_t out_size, int max_px)
{
    if (!in || out_size == 0) return;
    int w = 0;
    const unsigned char *p = (const unsigned char *)in;
    size_t n = 0;
    while (*p && n + 4 < out_size) {
        int cw;
        if (*p >= 0xE0 && p[1] && p[2]) {
            cw = 16;
        } else if (*p < 0x80) {
            cw = 6;
        } else {
            p++;
            continue;
        }
        if (w + cw > max_px) break;
        w += cw;
        if (*p >= 0xE0 && p[1] && p[2]) {
            out[n++] = (char)p[0];
            out[n++] = (char)p[1];
            out[n++] = (char)p[2];
            p += 3;
        } else {
            out[n++] = (char)*p++;
        }
    }
    out[n] = 0;
}

/* ---------- 菜单页 ---------- */

static void menu_subtitle(int index, char *out, size_t size)
{
    out[0] = 0;
    if (index == 0) {
        const settings_wifi_state_t st = settings_wifi_state();
        if (st == WIFI_SET_CONNECTED) {
            snprintf(out, size, "已连接 %s", settings_wifi_ssid());
        } else if (st == WIFI_SET_CONNECTING) {
            snprintf(out, size, "连接中 %s", settings_wifi_ssid());
        } else if (settings_wifi_has_saved()) {
            snprintf(out, size, "未连接 已保存");
        } else {
            snprintf(out, size, "未连接");
        }
    } else if (index == 1) {
        if (!settings_bt_enabled()) {
            snprintf(out, size, "已关闭");
        } else if (settings_bt_state() == BT_SET_CONNECTED) {
            snprintf(out, size, "已连接 %s", settings_bt_connected_name());
        } else {
            snprintf(out, size, "已开启");
        }
    } else {
        snprintf(out, size, "芯片 固件 网络状态");
    }
}

static void draw_menu(void)
{
    draw_header("系统设置", COLOR_CYAN);
    static const char *const titles[] = {"Wi-Fi 设置", "蓝牙设置", "关于"};
    static const uint16_t accents[] = {COLOR_BLUE, COLOR_PURPLE, COLOR_RED};
    for (int i = 0; i < 3; ++i) {
        const int y = 52 + i * 58;
        const bool sel = i == s_menu_selected;
        gfx_round_rect(10, y, 300, 52, 6, sel ? COLOR_PANEL_HI : COLOR_PANEL);
        if (sel) gfx_frame(10, y, 300, 52, 2, accents[i]);
        gfx_rect(10, y, 6, 52, accents[i]);
        gfx_text(28, y + 8, titles[i], 1, COLOR_TEXT, sel ? COLOR_PANEL_HI : COLOR_PANEL);
        char sub[40];
        menu_subtitle(i, sub, sizeof(sub));
        gfx_text(28, y + 28, sub, 1, COLOR_MUTED, sel ? COLOR_PANEL_HI : COLOR_PANEL);
    }
}

/* ---------- Wi-Fi 页 ---------- */

static void wifi_state_line(char *out, size_t size, bool *color_ok)
{
    const settings_wifi_state_t st = settings_wifi_state();
    *color_ok = false;
    switch (st) {
    case WIFI_SET_CONNECTED:
        snprintf(out, size, "已连接 %s", settings_wifi_ssid());
        *color_ok = true;
        break;
    case WIFI_SET_CONNECTING:
        snprintf(out, size, "连接中 %s", settings_wifi_ssid());
        break;
    case WIFI_SET_FAILED:
        snprintf(out, size, "连接失败 请重试");
        break;
    default:
        if (s_wifi_scan_requested && !settings_wifi_scan_done()) {
            static const char dots[] = "...";
            snprintf(out, size, "扫描中%.*s", (int)((esp_timer_get_time() / 100000 / 4) % 3) + 1, dots);
        } else if (settings_wifi_has_saved()) {
            snprintf(out, size, "未连接 已保存 %s", settings_wifi_ssid());
        } else {
            snprintf(out, size, "未连接 点击扫描");
        }
        break;
    }
}

static void draw_wifi_page(void)
{
    draw_header("Wi-Fi 设置", COLOR_BLUE);

    /* 状态行 */
    char line[48];
    bool ok = false;
    wifi_state_line(line, sizeof(line), &ok);
    const uint16_t color = ok ? COLOR_GREEN : (settings_wifi_state() == WIFI_SET_FAILED ? COLOR_RED : COLOR_TEXT);
    gfx_text(10, 48, line, 1, color, COLOR_BG);
    if (settings_wifi_state() == WIFI_SET_CONNECTED) {
        char ip[24];
        const esp_ip4_addr_t ipaddr = settings_wifi_ip();
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ipaddr));
        gfx_text(10, 62, ip, 1, COLOR_MUTED, COLOR_BG);
    }

    /* 列表区 */
    gfx_rect(4, 78, 312, WIFI_LIST_ROWS * WIFI_ROW_H, COLOR_PANEL);
    const int count = settings_wifi_scan_count();
    if (count == 0) {
        if (s_wifi_scan_requested && !settings_wifi_scan_done()) {
            gfx_text(16, 130, "正在扫描附近网络...", 1, COLOR_MUTED, COLOR_PANEL);
        } else {
            gfx_text(16, 130, "点\"扫描\"查找 Wi-Fi", 1, COLOR_MUTED, COLOR_PANEL);
        }
    } else {
        for (int i = 0; i < WIFI_LIST_ROWS; ++i) {
            const int idx = s_wifi_scroll + i;
            if (idx >= count) break;
            const settings_wifi_ap_t *ap = settings_wifi_scan_ap(idx);
            const int y = 78 + i * WIFI_ROW_H;
            if (idx == s_wifi_selected) {
                gfx_rect(4, y, 312, WIFI_ROW_H, COLOR_PANEL_HI);
                gfx_rect(4, y, 4, WIFI_ROW_H, COLOR_BLUE);
            }
            char ssid[40];
            truncate_to_width(ap->ssid, ssid, sizeof(ssid), 200);
            gfx_text(16, y + 6, ssid, 1, COLOR_TEXT, idx == s_wifi_selected ? COLOR_PANEL_HI : COLOR_PANEL);
            /* 加密标记 + 信号强度 */
            if (ap->secure) gfx_text(222, y + 6, "*", 1, COLOR_MUTED, idx == s_wifi_selected ? COLOR_PANEL_HI : COLOR_PANEL);
            char rssi[12];
            snprintf(rssi, sizeof(rssi), "%ddBm", ap->rssi);
            gfx_text(236, y + 6, rssi, 1, COLOR_MUTED, idx == s_wifi_selected ? COLOR_PANEL_HI : COLOR_PANEL);
        }
    }

    /* 按钮行 */
    static const char *const btns[] = {"扫描", "连接", "断开", "忘记"};
    for (int i = 0; i < 4; ++i) {
        const int x = 5 + i * 79;
        const bool active = (i == 1 && count > 0) || (i == 2 && settings_wifi_state() != WIFI_SET_NONE);
        gfx_round_rect(x, WIFI_BTN_Y, 77, WIFI_BTN_H, 4,
                       active ? COLOR_PANEL_HI : COLOR_PANEL);
        gfx_text(x + (77 - (int)strlen(btns[i]) * 6) / 2, WIFI_BTN_Y + 9, btns[i], 1,
                 active ? COLOR_TEXT : COLOR_MUTED, active ? COLOR_PANEL_HI : COLOR_PANEL);
    }
}

static void wifi_action(int action)
{
    switch (action) {
    case 0: /* 扫描 */
        s_wifi_scan_requested = true;
        settings_wifi_scan_start();
        break;
    case 1: { /* 连接 */
        const settings_wifi_ap_t *ap = settings_wifi_scan_ap(s_wifi_selected);
        if (!ap) return;
        if (ap->secure) {
            strlcpy(s_pending_ssid, ap->ssid, sizeof(s_pending_ssid));
            s_password[0] = 0;
            s_keyboard_uppercase = false;
            s_keyboard_mode = WIFI_KEYS_LETTERS;
            s_keyboard_page = 0;
            set_page(PAGE_WIFI_KEYBOARD);
        } else {
            settings_wifi_connect(ap->ssid, "");
        }
        break;
    }
    case 2: /* 断开 */
        settings_wifi_disconnect();
        break;
    case 3: /* 忘记 */
        settings_wifi_forget();
        break;
    default:
        break;
    }
}

/* ---------- 密码键盘页 ---------- */



static void keyboard_button(int x,int y,int width,int height,const char *label,int scale,uint16_t color)
{
    gfx_round_rect(x,y,width,height,5,color);
    gfx_text(x+(width-gfx_text_width(label,scale))/2,
             y+(height-gfx_text_height(label,scale))/2,label,scale,
             color==COLOR_GREEN ? COLOR_BG : COLOR_TEXT,color);
}

static void draw_keyboard(void)
{
    /* Dedicated compact header leaves room for 58x36 keys in the app viewport. */
    gfx_clear(COLOR_BG);
    gfx_rect(0,0,BOX3_LCD_WIDTH,30,COLOR_PANEL);
    gfx_text(10,7,"输入密码",1,COLOR_TEXT,COLOR_PANEL);
    gfx_text(246,11,"< BACK",1,COLOR_MUTED,COLOR_PANEL);
    char ssid[44];
    truncate_to_width(s_pending_ssid,ssid,sizeof(ssid),260);
    gfx_text(8,34,"WiFi:",1,COLOR_MUTED,COLOR_BG);
    gfx_text(44,34,ssid,1,COLOR_TEXT,COLOR_BG);
    gfx_round_rect(7,48,306,28,4,COLOR_PANEL);
    const size_t length=strlen(s_password);
    const char *shown=s_password+(length>24 ? length-24 : 0);
    gfx_text(14,55,shown,2,COLOR_TEXT,COLOR_PANEL);

    const char *page_label=s_keyboard_mode==WIFI_KEYS_LETTERS ?
        (s_keyboard_page ? "左半" : "右半") : s_keyboard_mode==WIFI_KEYS_SYMBOLS ? "翻页" : "空格";
    const char *controls[]={"a/A","123","#+=",page_label,"删除","连接"};
    for (int i=0;i<WIFI_KEYS_CONTROL_COUNT;++i) {
        uint16_t color=i==5 ? COLOR_GREEN : i==(int)s_keyboard_mode ? COLOR_BLUE : COLOR_PANEL_HI;
        keyboard_button(WIFI_KEYS_X+i*WIFI_KEYS_CONTROL_PITCH,WIFI_KEYS_CONTROLS_Y,
                        WIFI_KEYS_CONTROL_WIDTH,WIFI_KEYS_CONTROLS_HEIGHT,controls[i],i<3 ? 2 : 1,color);
    }
    for (int cell=0;cell<15;++cell) {
        wifi_key_t key=wifi_keys_key(s_keyboard_mode,s_keyboard_page,s_keyboard_uppercase,cell);
        if (key.kind==WIFI_KEY_NONE) continue;
        char letter[]={key.character,0};
        const char *label=key.label ? key.label : letter;
        const int x=WIFI_KEYS_X+(cell%5)*WIFI_KEYS_PITCH_X;
        const int y=WIFI_KEYS_TOP+(cell/5)*WIFI_KEYS_PITCH_Y;
        keyboard_button(x,y,WIFI_KEYS_WIDTH,WIFI_KEYS_HEIGHT,label,key.character==' ' ? 1 : 2,
                        key.kind==WIFI_KEY_NEXT_PAGE ? COLOR_PANEL_HI : COLOR_PANEL);
    }
}

static void keyboard_input(char ch)
{
    if (!ch) {
        const size_t len = strlen(s_password);
        if (len > 0) s_password[len - 1] = 0;
        return;
    }
    const size_t len = strlen(s_password);
    if (len < sizeof(s_password) - 1) {
        s_password[len] = ch;
        s_password[len + 1] = 0;
    }
}

static void keyboard_action(int action)
{
    switch (action) {
    case 0: /* CLR */
        s_password[0] = 0;
        break;
    case 1: /* OK：连接 */
        settings_wifi_connect(s_pending_ssid, s_password);
        set_page(PAGE_WIFI);
        break;
    default:
        break;
    }
}

/* ---------- 蓝牙页 ---------- */

static void draw_bt_page(void)
{
    draw_header("蓝牙设置", COLOR_PURPLE);

    /* 开关行 */
    gfx_text(12, 48, "蓝牙:", 1, COLOR_MUTED, COLOR_BG);
    const bool on = settings_bt_enabled();
    gfx_text(64, 48, on ? "开启" : "关闭", 1, on ? COLOR_GREEN : COLOR_MUTED, COLOR_BG);
    gfx_round_rect(236, 46, 38, 26, 4, on ? COLOR_GREEN : COLOR_PANEL_HI);
    gfx_text(242, 53, "开", 1, COLOR_BG, on ? COLOR_GREEN : COLOR_PANEL_HI);
    gfx_round_rect(278, 46, 38, 26, 4, on ? COLOR_PANEL_HI : COLOR_RED);
    gfx_text(284, 53, "关", 1, COLOR_TEXT, on ? COLOR_PANEL_HI : COLOR_RED);

    /* 状态行 */
    const settings_bt_state_t st = settings_bt_state();
    if (!on) {
        gfx_text(12, 80, "蓝牙已关闭", 1, COLOR_MUTED, COLOR_BG);
    } else if (st == BT_SET_CONNECTED) {
        char line[48];
        snprintf(line, sizeof(line), "已连接 %s", settings_bt_connected_name());
        gfx_text(12, 80, line, 1, COLOR_GREEN, COLOR_BG);
    } else if (st == BT_SET_CONNECTING) {
        char line[48];
        snprintf(line, sizeof(line), "连接中 %s", settings_bt_connected_name());
        gfx_text(12, 80, line, 1, COLOR_TEXT, COLOR_BG);
    } else if (s_bt_scan_requested && !settings_bt_scan_done()) {
        static const char dots[] = "...";
        char line[24];
        snprintf(line, sizeof(line), "扫描中%.*s", (int)((esp_timer_get_time() / 100000 / 4) % 3) + 1, dots);
        gfx_text(12, 80, line, 1, COLOR_TEXT, COLOR_BG);
    } else {
        gfx_text(12, 80, "空闲 点\"扫描\"找设备", 1, COLOR_MUTED, COLOR_BG);
    }

    /* 设备列表 */
    gfx_rect(4, 106, 312, BT_LIST_ROWS * BT_ROW_H, COLOR_PANEL);
    const int count = settings_bt_scan_count();
    if (count == 0) {
        if (!on) {
            gfx_text(16, 148, "先开启蓝牙", 1, COLOR_MUTED, COLOR_PANEL);
        } else if (s_bt_scan_requested && !settings_bt_scan_done()) {
            gfx_text(16, 148, "正在扫描 BLE 设备...", 1, COLOR_MUTED, COLOR_PANEL);
        } else {
            gfx_text(16, 148, "点\"扫描\"查找设备", 1, COLOR_MUTED, COLOR_PANEL);
        }
    } else {
        for (int i = 0; i < BT_LIST_ROWS; ++i) {
            const int idx = s_bt_scroll + i;
            if (idx >= count) break;
            const settings_bt_dev_t *dev = settings_bt_scan_dev(idx);
            const int y = 106 + i * BT_ROW_H;
            if (idx == s_bt_selected) {
                gfx_rect(4, y, 312, BT_ROW_H, COLOR_PANEL_HI);
                gfx_rect(4, y, 4, BT_ROW_H, COLOR_PURPLE);
            }
            char name[44];
            if (dev->name[0]) {
                truncate_to_width(dev->name, name, sizeof(name), 190);
            } else {
                snprintf(name, sizeof(name), "%02X:%02X:%02X",
                         dev->addr[3], dev->addr[4], dev->addr[5]);
            }
            gfx_text(16, y + 5, name, 1, COLOR_TEXT, idx == s_bt_selected ? COLOR_PANEL_HI : COLOR_PANEL);
            char rssi[12];
            snprintf(rssi, sizeof(rssi), "%ddBm", dev->rssi);
            gfx_text(236, y + 5, rssi, 1, COLOR_MUTED, idx == s_bt_selected ? COLOR_PANEL_HI : COLOR_PANEL);
        }

        /* 滚动条：设备数超过可视行数时显示，指示当前滚动位置 */
        if (count > BT_LIST_ROWS) {
            const int list_h = BT_LIST_ROWS * BT_ROW_H;
            const int track_x = 308;
            const int track_w = 4;
            gfx_rect(track_x, 106, track_w, list_h, COLOR_PANEL_HI);
            const int thumb_h = (list_h * BT_LIST_ROWS + count - 1) / count;
            const int range = count - BT_LIST_ROWS;
            int sc = s_bt_scroll;
            if (sc < 0) sc = 0;
            if (sc > range) sc = range;
            const int thumb_y = 106 + (list_h - thumb_h) * sc / range;
            gfx_rect(track_x, thumb_y, track_w, thumb_h, COLOR_PURPLE);
        }
    }

    /* 按钮行 */
    static const char *const btns[] = {"扫描", "连接", "断开"};
    for (int i = 0; i < 3; ++i) {
        const int x = 8 + i * 104;
        const bool active = (i == 0 && on) || (i == 1 && count > 0) ||
                            (i == 2 && st == BT_SET_CONNECTED);
        gfx_round_rect(x, BT_BTN_Y, 100, BT_BTN_H, 4,
                       active ? COLOR_PANEL_HI : COLOR_PANEL);
        gfx_text(x + (100 - (int)strlen(btns[i]) * 6) / 2, BT_BTN_Y + 9, btns[i], 1,
                 active ? COLOR_TEXT : COLOR_MUTED, active ? COLOR_PANEL_HI : COLOR_PANEL);
    }
}

static void bt_action(int action)
{
    switch (action) {
    case 0: /* 扫描 */
        if (!settings_bt_enabled()) settings_bt_set_enabled(true);
        s_bt_scan_requested = true;
        settings_bt_scan_start();
        break;
    case 1: /* 连接 */
        settings_bt_connect(s_bt_selected);
        break;
    case 2: /* 断开 */
        settings_bt_disconnect();
        break;
    default:
        break;
    }
}

/* ---------- 关于页 ---------- */

static void draw_about_page(void)
{
    draw_header("关于", COLOR_RED);
    gfx_rect(10, 52, 300, 178, COLOR_PANEL);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char line[40];
    int y = 66;
    gfx_text(28, y, "芯片 ESP32-S3", 1, COLOR_TEXT, COLOR_PANEL);
    y += 22;
    snprintf(line, sizeof(line), "核心 %u 主频 240MHz", chip.cores);
    gfx_text(28, y, line, 1, COLOR_TEXT, COLOR_PANEL);
    y += 22;
    snprintf(line, sizeof(line), "内存 %u KB 可用", (unsigned)(esp_get_free_heap_size() / 1024));
    gfx_text(28, y, line, 1, COLOR_GREEN, COLOR_PANEL);
    y += 22;
    snprintf(line, sizeof(line), "最小 %u KB", (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    gfx_text(28, y, line, 1, COLOR_MUTED, COLOR_PANEL);
    y += 22;
    gfx_text(28, y, "固件 BOX UI v0.5", 1, COLOR_MUTED, COLOR_PANEL);
    y += 22;
    if (settings_wifi_state() == WIFI_SET_CONNECTED) {
        const esp_ip4_addr_t ipaddr = settings_wifi_ip();
        snprintf(line, sizeof(line), "IP " IPSTR, IP2STR(&ipaddr));
        gfx_text(28, y, line, 1, COLOR_CYAN, COLOR_PANEL);
    } else {
        gfx_text(28, y, "IP 未连接", 1, COLOR_MUTED, COLOR_PANEL);
    }
    y += 22;
    gfx_text(28, y, "Wi-Fi 2.4G  蓝牙 BLE", 1, COLOR_MUTED, COLOR_PANEL);
}

/* ---------- 触摸分发 ---------- */

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

void settings_touch(uint16_t x, uint16_t y)
{
    /* 返回区（与 launcher 一致） */
    if (y < BACK_HIT_Y && x >= 230) {
        if (s_page == PAGE_WIFI_KEYBOARD) set_page(PAGE_WIFI);
        else if (s_page != PAGE_MENU) set_page(PAGE_MENU);
        return;
    }

    switch (s_page) {
    case PAGE_MENU:
        for (int i = 0; i < 3; ++i) {
            if (in_rect(x, y, 10, 52 + i * 58, 300, 52)) {
                s_menu_selected = i;
                if (i == 0) set_page(PAGE_WIFI);
                else if (i == 1) set_page(PAGE_BT);
                else set_page(PAGE_ABOUT);
                return;
            }
        }
        break;

    case PAGE_WIFI: {
        /* 列表选中 */
        if (in_rect(x, y, 4, 78, 312, WIFI_LIST_ROWS * WIFI_ROW_H)) {
            const int idx = s_wifi_scroll + (y - 78) / WIFI_ROW_H;
            if (idx >= 0 && idx < settings_wifi_scan_count()) {
                s_wifi_selected = idx;
            }
            return;
        }
        /* 按钮 */
        for (int i = 0; i < 4; ++i) {
            if (in_rect(x, y, 5 + i * 79, WIFI_BTN_Y, 77, WIFI_BTN_H)) {
                wifi_action(i);
                return;
            }
        }
        break;
    }

    case PAGE_WIFI_KEYBOARD: {
        const int control=wifi_keys_control_hit(x,y);
        if (control>=0) {
            if (control==0) {
                if (s_keyboard_mode==WIFI_KEYS_LETTERS) s_keyboard_uppercase=!s_keyboard_uppercase;
                else { s_keyboard_mode=WIFI_KEYS_LETTERS; s_keyboard_page=0; }
            } else if (control==1 || control==2) {
                if ((int)s_keyboard_mode!=control) { s_keyboard_mode=(wifi_keys_mode_t)control; s_keyboard_page=0; }
            } else if (control==3) {
                if (s_keyboard_mode==WIFI_KEYS_NUMBERS) keyboard_input(' ');
                else s_keyboard_page=(s_keyboard_page+1)%(s_keyboard_mode==WIFI_KEYS_LETTERS ? 2 : 3);
            } else if (control==4) keyboard_input(0);
            else keyboard_action(1);
        } else {
            const int cell=wifi_keys_cell_hit(x,y);
            wifi_key_t key=wifi_keys_key(s_keyboard_mode,s_keyboard_page,s_keyboard_uppercase,cell);
            if (key.kind==WIFI_KEY_CHARACTER) keyboard_input(key.character);
            else if (key.kind==WIFI_KEY_NEXT_PAGE)
                s_keyboard_page=(s_keyboard_page+1)%(s_keyboard_mode==WIFI_KEYS_LETTERS ? 2 : 3);
        }
        s_last_sig=UINT32_MAX;
        break;
    }

    case PAGE_BT: {
        if (in_rect(x, y, 236, 46, 38, 26)) {
            settings_bt_set_enabled(true);
            return;
        }
        if (in_rect(x, y, 278, 46, 38, 26)) {
            settings_bt_set_enabled(false);
            s_bt_scan_requested = false;
            return;
        }
        /* 点按滚动条跳转 */
        if (in_rect(x, y, 306, 106, 8, BT_LIST_ROWS * BT_ROW_H) &&
            settings_bt_scan_count() > BT_LIST_ROWS) {
            const int range = settings_bt_scan_count() - BT_LIST_ROWS;
            const int list_h = BT_LIST_ROWS * BT_ROW_H;
            int sc = (y - 106) * range / list_h;
            if (sc < 0) sc = 0;
            if (sc > range) sc = range;
            s_bt_scroll = sc;
            return;
        }
        if (in_rect(x, y, 4, 106, 312, BT_LIST_ROWS * BT_ROW_H)) {
            const int idx = s_bt_scroll + (y - 106) / BT_ROW_H;
            if (idx >= 0 && idx < settings_bt_scan_count()) {
                s_bt_selected = idx;
            }
            return;
        }
        for (int i = 0; i < 3; ++i) {
            if (in_rect(x, y, 8 + i * 104, BT_BTN_Y, 100, BT_BTN_H)) {
                bt_action(i);
                return;
            }
        }
        break;
    }

    case PAGE_ABOUT:
        break;
    }
}

/* ---------- 按键分发 ---------- */

void settings_key(int key)
{
    switch (s_page) {
    case PAGE_MENU:
        if (key == 1 && s_menu_selected > 0) --s_menu_selected;
        else if (key == 2 && s_menu_selected < 2) ++s_menu_selected;
        else if (key == 3) {
            if (s_menu_selected == 0) set_page(PAGE_WIFI);
            else if (s_menu_selected == 1) set_page(PAGE_BT);
            else set_page(PAGE_ABOUT);
        }
        break;

    case PAGE_WIFI:
        if (key == 1 && s_wifi_selected > 0) {
            --s_wifi_selected;
            if (s_wifi_selected < s_wifi_scroll) s_wifi_scroll = s_wifi_selected;
        } else if (key == 2) {
            if (s_wifi_selected + 1 < settings_wifi_scan_count()) {
                ++s_wifi_selected;
                if (s_wifi_selected >= s_wifi_scroll + WIFI_LIST_ROWS) {
                    s_wifi_scroll = s_wifi_selected - WIFI_LIST_ROWS + 1;
                }
            }
        } else if (key == 3) {
            set_page(PAGE_MENU);
        }
        break;

    case PAGE_WIFI_KEYBOARD:
        if (key == 1) keyboard_input(0); /* 退格 */
        else if (key == 3) keyboard_action(1); /* 连接 */
        break;

    case PAGE_BT:
        if (key == 1 && s_bt_selected > 0) {
            --s_bt_selected;
            if (s_bt_selected < s_bt_scroll) s_bt_scroll = s_bt_selected;
        } else if (key == 2) {
            if (s_bt_selected + 1 < settings_bt_scan_count()) {
                ++s_bt_selected;
                if (s_bt_selected >= s_bt_scroll + BT_LIST_ROWS) {
                    s_bt_scroll = s_bt_selected - BT_LIST_ROWS + 1;
                }
            }
        } else if (key == 3) {
            set_page(PAGE_MENU);
        }
        break;

    case PAGE_ABOUT:
        if (key == 3) set_page(PAGE_MENU);
        break;
    }
}

/* ---------- 绘制入口 ---------- */

static uint32_t state_signature(void)
{
    uint32_t sig = (uint32_t)s_page * 1000003u;
    sig ^= (uint32_t)settings_wifi_state() << 8;
    sig ^= (uint32_t)settings_bt_state() << 12;
    sig ^= settings_bt_enabled() ? 0x10000u : 0;
    sig ^= settings_wifi_scan_done() ? 0x20000u : 0;
    sig ^= settings_bt_scan_done() ? 0x40000u : 0;
    sig ^= (uint32_t)s_wifi_selected << 16;
    sig ^= (uint32_t)s_bt_selected << 20;
    sig ^= (uint32_t)strlen(s_password) << 24;
    sig ^= s_keyboard_uppercase ? 0x80000000u : 0;
    sig ^= (uint32_t)s_menu_selected;
    sig ^= s_wifi_scan_requested ? 0x80000u : 0;
    sig ^= s_bt_scan_requested ? 0x100000u : 0;
    /* 设备名常由后到的 SCAN_RSP 补全；数量不变也必须刷新列表。 */
    sig ^= settings_bt_scan_revision() * 2654435761u;
    /* 扫描中的动画刷新 */
    if ((s_page == PAGE_WIFI && s_wifi_scan_requested && !settings_wifi_scan_done()) ||
        (s_page == PAGE_BT && s_bt_scan_requested && !settings_bt_scan_done())) {
        sig ^= (uint32_t)(esp_timer_get_time() / 400000);
    }
    return sig;
}

void settings_draw(bool first_draw)
{
    const uint32_t sig = state_signature();
    const bool scroll_changed = s_bt_scroll != s_bt_scroll_sig;
    if (scroll_changed) s_bt_scroll_sig = s_bt_scroll;
    if (!first_draw && !scroll_changed && sig == s_last_sig) return;
    s_last_sig = sig;

    switch (s_page) {
    case PAGE_MENU:
        draw_menu();
        break;
    case PAGE_WIFI:
        draw_wifi_page();
        break;
    case PAGE_WIFI_KEYBOARD:
        draw_keyboard();
        break;
    case PAGE_BT:
        draw_bt_page();
        break;
    case PAGE_ABOUT:
        draw_about_page();
        break;
    }
    /* Opening prepares the foreground for the launcher slide transition.
     * Presenting it now would flash the full page before the desktop slides. */
    if (!first_draw) gfx_present();
}

void settings_tick(int64_t now_ms)
{
    (void)now_ms;
    settings_draw(false);
}

void settings_reset(void)
{
    s_page = PAGE_MENU;
    s_menu_selected = 0;
    s_wifi_selected = 0;
    s_wifi_scroll = 0;
    s_bt_selected = 0;
    s_bt_scroll = 0;
    s_pending_ssid[0] = 0;
    s_password[0] = 0;
    s_keyboard_uppercase = false;
    s_keyboard_mode = WIFI_KEYS_LETTERS;
    s_keyboard_page = 0;
    s_wifi_scan_requested = false;
    s_bt_scan_requested = false;
    s_last_sig = UINT32_MAX;  /* 强制重绘 */
    s_bt_scroll_sig = -1;
}
