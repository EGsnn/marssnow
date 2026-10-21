#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 蓝牙业务层：ESP32-S3 仅支持 BLE（低功耗蓝牙），不支持经典蓝牙。
 * 实现 GAP 扫描 + GATT 连接的最小功能，供设置 UI 调用。 */

typedef enum {
    BT_SET_OFF = 0,    /* 蓝牙关闭 */
    BT_SET_IDLE,       /* 已开启，空闲 */
    BT_SET_SCANNING,   /* 扫描中 */
    BT_SET_CONNECTING, /* 连接中 */
    BT_SET_CONNECTED,  /* 已连接 */
} settings_bt_state_t;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type; /* BLE_ADDR_PUBLIC / BLE_ADDR_RANDOM */
    char name[33];
    int8_t rssi;
} settings_bt_dev_t;

/* 初始化 NimBLE host（后台任务），默认处于关闭状态 */
void settings_bt_init(void);

bool settings_bt_enabled(void);
void settings_bt_set_enabled(bool on);

settings_bt_state_t settings_bt_state(void);
const char *settings_bt_connected_name(void);

/* 异步扫描 10 秒；完成前 settings_bt_scan_done() 返回 false */
void settings_bt_scan_start(void);
bool settings_bt_scan_done(void);
int settings_bt_scan_count(void);
uint32_t settings_bt_scan_revision(void);
const settings_bt_dev_t *settings_bt_scan_dev(int index);

void settings_bt_connect(int index);
void settings_bt_disconnect(void);
