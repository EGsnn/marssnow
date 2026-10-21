#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 火星系统事件总线
 * 发布/订阅模型：任何系统服务或 App 都可发布事件，订阅者回调同步触发。
 * 事件数据指针由发布者保证生命周期（回调内只读、不持有）。 */

/* 系统保留事件段：0x1000-0x1FFF；App 自定义事件从 0x2000 起 */
typedef enum {
    MARS_EVENT_NONE = 0,

    MARS_EVENT_WIFI_STATE_CHANGED = 0x1000,
    MARS_EVENT_WIFI_CONNECTED,
    MARS_EVENT_WIFI_DISCONNECTED,
    MARS_EVENT_WIFI_SCAN_DONE,
    MARS_EVENT_WIFI_RSSI_CHANGED,
    MARS_EVENT_WIFI_CONFIG_ENTER,
    MARS_EVENT_WIFI_CONFIG_EXIT,

    MARS_EVENT_APP_OPEN = 0x1100,
    MARS_EVENT_APP_CLOSE,

    MARS_EVENT_OTA_STATE_CHANGED = 0x1200, /* data = mars_ota_state_t */
    MARS_EVENT_OTA_PROGRESS,               /* data = int percent (0-100) */

    MARS_EVENT_MEDIA_STATE_CHANGED = 0x1300, /* data = mars_media_state_t */

    MARS_EVENT_BOOT_SWITCH = 0x1400, /* data = const char* 目标 app_id */

    MARS_EVENT_USER_BASE = 0x2000, /* App 自定义事件起点 */
} mars_event_id_t;

typedef struct {
    mars_event_id_t id;
    const void *data;     /* 发布者数据，回调内只读 */
    size_t data_size;
} mars_event_t;

typedef void (*mars_event_callback_t)(const mars_event_t *event, void *ctx);

/* 系统启动时调用一次 */
void mars_event_init(void);

/* 订阅/退订；同一 (id, cb, ctx) 只注册一次 */
bool mars_event_subscribe(mars_event_id_t id, mars_event_callback_t cb, void *ctx);
bool mars_event_unsubscribe(mars_event_id_t id, mars_event_callback_t cb, void *ctx);

/* 同步发布事件（调用者保证 data 在回调期间有效；data 可为 NULL） */
void mars_event_post(mars_event_id_t id, const void *data, size_t data_size);
