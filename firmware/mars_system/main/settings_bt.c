#include "settings_bt.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#define BT_MAX_DEVICES 24
#define BT_SCAN_DURATION_MS 10000

static const char *TAG = "settings_bt";

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    char name[33];
    int8_t rssi;
} bt_dev_t;

static bt_dev_t s_devices[BT_MAX_DEVICES];
static int s_device_count;
static uint32_t s_scan_revision;
static uint32_t s_scan_gen;
static uint32_t s_scan_done_gen;
static bool s_scanning;

static volatile settings_bt_state_t s_state = BT_SET_OFF;
static volatile bool s_enabled;
static char s_connected_name[33];
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* 从广播数据中解析设备名（AD type 0x09 完整名 / 0x08 短名） */
static void parse_adv_name(const uint8_t *adv, uint8_t len, char *out, size_t out_size)
{
    out[0] = 0;
    if (!adv || out_size < 2) return;

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, adv, len) != 0 || !fields.name || fields.name_len == 0) {
        return;
    }

    size_t name_len = fields.name_len;
    if (name_len >= out_size) name_len = out_size - 1;
    memcpy(out, fields.name, name_len);
    out[name_len] = 0;
}

static int find_device_by_addr(const ble_addr_t *addr)
{
    for (int i = 0; i < s_device_count; ++i) {
        /* 广播包与扫描响应有时会把同一地址标成 public/random 的普通或
         * identity 类型。设备身份以 48 位地址合并，连接仍保留广播包地址类型。 */
        if (memcmp(s_devices[i].addr, addr->val, sizeof(s_devices[i].addr)) == 0) {
            return i;
        }
    }
    return -1;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &event->disc;
        /* 有效设备 = 可连接的广播（ADV_IND / 定向广播）；
         * 纯信标（SCAN_IND / NONCONN_IND）连不上，不加入列表。
         * 扫描响应（SCAN_RSP）只用于补充已有条目的名字与信号。 */
        const bool connectable = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                                  d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);
        if (!connectable && d->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            break;
        }
        char name[33];
        parse_adv_name(d->data, d->length_data, name, sizeof(name));
        portENTER_CRITICAL(&s_lock);
        /* 同一设备会先后上报广播包与扫描响应（设备名常在扫描响应里），
         * 按地址去重并更新，避免列表出现重复项、名字被遗漏。 */
        int idx = find_device_by_addr(&d->addr);
        bool list_changed = false;
        if (idx < 0) {
            /* 扫描响应没有对应的可连接条目时不新建条目 */
            if (!connectable) {
                portEXIT_CRITICAL(&s_lock);
                break;
            }
            if (s_device_count >= BT_MAX_DEVICES) {
                portEXIT_CRITICAL(&s_lock);
                break;
            }
            idx = s_device_count++;
            memcpy(s_devices[idx].addr, d->addr.val, 6);
            s_devices[idx].addr_type = d->addr.type;
            s_devices[idx].name[0] = 0;
            list_changed = true;
        } else if (connectable) {
            /* 连接必须使用可连接广播携带的地址类型，不能被 SCAN_RSP 覆盖。 */
            s_devices[idx].addr_type = d->addr.type;
        }
        if (name[0] && strcmp(s_devices[idx].name, name) != 0) {
            strlcpy(s_devices[idx].name, name, sizeof(s_devices[idx].name));
            list_changed = true;
        }
        s_devices[idx].rssi = d->rssi;
        if (list_changed) ++s_scan_revision;
        portEXIT_CRITICAL(&s_lock);
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        s_scan_done_gen = s_scan_gen;
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_state = BT_SET_CONNECTED;
        } else {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_state = BT_SET_IDLE;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_state = BT_SET_IDLE;
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    ESP_LOGI(TAG, "NimBLE host synced");
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void settings_bt_init(void)
{
    const esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }
    /* 纯 BLE 中心端：无需初始化 GATT/GAP 服务端 */
    ble_gap_set_event_cb(BLE_HS_CONN_HANDLE_NONE, gap_event, NULL);
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "NimBLE host started (BLE only on ESP32-S3)");
}

bool settings_bt_enabled(void)
{
    return s_enabled;
}

void settings_bt_set_enabled(bool on)
{
    if (on == s_enabled) return;
    if (!on) {
        if (s_state == BT_SET_CONNECTED) settings_bt_disconnect();
        if (s_scanning) ble_gap_disc_cancel();
        s_scanning = false;
        s_scan_done_gen = s_scan_gen;
        s_enabled = false;
        s_state = BT_SET_OFF;
    } else {
        s_enabled = true;
        s_state = BT_SET_IDLE;
    }
}

settings_bt_state_t settings_bt_state(void)
{
    return s_state;
}

const char *settings_bt_connected_name(void)
{
    return s_connected_name;
}

void settings_bt_scan_start(void)
{
    if (!s_enabled) return;
    if (s_scanning) ble_gap_disc_cancel();
    portENTER_CRITICAL(&s_lock);
    s_device_count = 0;
    ++s_scan_revision;
    portEXIT_CRITICAL(&s_lock);
    ++s_scan_gen;
    s_scanning = true;

    struct ble_gap_disc_params params = {
        /* 名称经常只在 SCAN_RSP 中。关闭控制器去重，由上面的地址表合并，
         * 确保先收到广播后仍能收到并补上设备名。 */
        .filter_duplicates = 0,
        .passive = 0,
        .itvl = 0x50,
        .window = 0x50,
        .filter_policy = 0,
        .limited = 0,
    };
    const int rc = ble_gap_disc(s_own_addr_type, BT_SCAN_DURATION_MS, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
        s_scanning = false;
        s_scan_done_gen = s_scan_gen;
    }
}

bool settings_bt_scan_done(void)
{
    return s_scan_gen == s_scan_done_gen && s_scan_gen > 0;
}

int settings_bt_scan_count(void)
{
    return s_device_count;
}

uint32_t settings_bt_scan_revision(void)
{
    portENTER_CRITICAL(&s_lock);
    const uint32_t revision = s_scan_revision;
    portEXIT_CRITICAL(&s_lock);
    return revision;
}

const settings_bt_dev_t *settings_bt_scan_dev(int index)
{
    if (index < 0 || index >= s_device_count) return NULL;
    return (const settings_bt_dev_t *)&s_devices[index];
}

void settings_bt_connect(int index)
{
    if (!s_enabled || index < 0 || index >= s_device_count) return;
    if (s_scanning) ble_gap_disc_cancel();
    s_scanning = false;

    const bt_dev_t *dev = &s_devices[index];
    strlcpy(s_connected_name, dev->name[0] ? dev->name : "(unnamed)", sizeof(s_connected_name));
    ble_addr_t addr = {0};
    addr.type = dev->addr_type;
    memcpy(addr.val, dev->addr, 6);
    s_state = BT_SET_CONNECTING;
    const int rc = ble_gap_connect(s_own_addr_type, &addr, 30000, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect rc=%d", rc);
        s_state = BT_SET_IDLE;
    }
}

void settings_bt_disconnect(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}
