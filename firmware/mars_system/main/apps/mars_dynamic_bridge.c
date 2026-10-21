/**
 * @file mars_dynamic_bridge.c
 * @brief 动态应用桥：把 app_0/1/2 分区中的 MARSAPP 应用注册到桌面
 *
 * 桌面扫描到 app_N 分区有有效应用包时，注册一个"动态应用"入口：
 *  - open  → mars_app_launch(slot) 加载并启动
 *  - close → mars_app_unload(slot)  停止并释放运行内存（Flash 安装包保留）
 *
 * 注意：mars_app_t 回调无 ctx 参数，回调通过注册名（"APP0".."APP2"）定位槽位。
 */

#include "mars_dynamic_bridge.h"
#include "mars_app_loader.h"
#include "mars_app.h"
#include "app_slots.h"
#include "box3_gfx.h"
#include "esp_partition.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"

static const char* kTag = "dyn_bridge";

#define DYN_NAME_PREFIX "APP"

/* 动态应用槽位元信息 */
typedef struct {
    bool        registered;   /* 是否已注册到桌面 */
    int         slot;
    char        name[24];     /* 注册名，如 "APP0" */
    char        package_name[APP_NAME_MAX];
    char        display_name[64];
    const char* partition_label;
    uint32_t    icon_offset;
    mars_app_t  app;          /* 桌面注册项 */
} dyn_slot_t;

static dyn_slot_t s_dyn[MARS_APP_SLOTS];
static bool s_scanned = false;

/* 通过注册名找槽位 */
static int slot_from_name(const char* name)
{
    if (!name || strncmp(name, DYN_NAME_PREFIX, 3) != 0) return -1;
    int slot = name[3] - '0';
    if (slot < 0 || slot >= MARS_APP_SLOTS) return -1;
    return slot;
}

/* ── 回调 ── */

/* 当前活动应用名（由 mars_core 的 mars_app_open 设置） */
extern const char* g_mars_active_app_name;

static void dyn_open(void)
{
    int slot = slot_from_name(g_mars_active_app_name);
    if (slot < 0) return;
    ESP_LOGI(kTag, "open dynamic app slot %d", slot);
    int rc = mars_app_launch(slot);
    if (rc != 0) {
        ESP_LOGE(kTag, "launch slot %d failed (%d)", slot, rc);
    }
}

static void dyn_close(void)
{
    int slot = slot_from_name(g_mars_active_app_name);
    if (slot < 0) return;
    ESP_LOGI(kTag, "close dynamic app slot %d", slot);
    mars_app_unload(slot);
}

static void dyn_key(int key)
{
    int slot = slot_from_name(g_mars_active_app_name);
    if (slot >= 0) mars_app_handle_key(slot, key);
}

static void dyn_touch(uint16_t x, uint16_t y)
{
    int slot = slot_from_name(g_mars_active_app_name);
    if (slot >= 0) mars_app_handle_touch(slot, x, y);
}

static void dyn_tick(int64_t now_ms)
{
    (void)now_ms;
    /* 动态应用内部自行驱动；这里保留为空（看门狗扩展点） */
}

/* ── 扫描与注册 ── */

void mars_dynamic_bridge_init(void)
{
    if (s_scanned) return;
    s_scanned = true;
    memset(s_dyn, 0, sizeof(s_dyn));

    app_slots_scan();

    for (int i = 0; i < APP_SLOT_COUNT && i < MARS_APP_SLOTS; i++) {
        app_slot_info_t info;
        if (!app_slots_get_info(i, &info)) continue;
        if (!info.is_valid_app) continue;

        dyn_slot_t* d = &s_dyn[i];
        d->slot = i;
        d->registered = true;
        snprintf(d->name, sizeof(d->name), "%s%d", DYN_NAME_PREFIX, i);
        memcpy(d->package_name, info.app_name, sizeof(d->package_name) - 1);
        d->package_name[sizeof(d->package_name) - 1] = '\0';
        memcpy(d->display_name, info.display_name, sizeof(d->display_name));
        d->partition_label = info.label;
        d->icon_offset = info.icon_offset;

        /* 组装桌面注册项 */
        d->app.name = d->name;
        d->app.accent = 0x5AEB;  /* 天蓝 */
        d->app.open = dyn_open;
        d->app.close = dyn_close;
        d->app.key = dyn_key;
        d->app.touch = dyn_touch;
        d->app.tick = dyn_tick;
        mars_app_register(&d->app);

        ESP_LOGI(kTag, "registered slot %d as '%s' (pkg %s)", i, d->name, info.app_name);
    }
}

int mars_dynamic_bridge_count(void)
{
    int n = 0;
    for (int i = 0; i < MARS_APP_SLOTS; i++) {
        if (s_dyn[i].registered) n++;
    }
    return n;
}

bool mars_dynamic_bridge_is_slot(int slot)
{
    if (slot < 0 || slot >= MARS_APP_SLOTS) return false;
    return s_dyn[slot].registered;
}

const char* mars_dynamic_bridge_package_name(int slot)
{
    return mars_dynamic_bridge_is_slot(slot) ? s_dyn[slot].package_name : NULL;
}

const char* mars_dynamic_bridge_display_name(int slot)
{
    return mars_dynamic_bridge_is_slot(slot) && s_dyn[slot].display_name[0]
        ? s_dyn[slot].display_name : NULL;
}

bool mars_dynamic_bridge_draw_icon(int slot, int x, int y)
{
    if (!mars_dynamic_bridge_is_slot(slot) || s_dyn[slot].icon_offset == 0) return false;
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, s_dyn[slot].partition_label);
    if (part == NULL) return false;
    const void *pixels = NULL;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, s_dyn[slot].icon_offset, 48u * 48u * 2u,
                           ESP_PARTITION_MMAP_DATA, &pixels, &handle) != ESP_OK) return false;
    gfx_bitmap(x, y, 48, 48, (const uint16_t *)pixels);
    esp_partition_munmap(handle);
    return true;
}

const char* mars_dynamic_bridge_registry_name(const char* package_name)
{
    if (package_name == NULL) return NULL;
    for (int i = 0; i < MARS_APP_SLOTS; ++i) {
        if (s_dyn[i].registered && strcmp(s_dyn[i].package_name, package_name) == 0) {
            return s_dyn[i].name;
        }
    }
    return NULL;
}
