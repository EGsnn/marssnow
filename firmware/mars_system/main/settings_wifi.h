#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"

/* Wi-Fi 业务层：与设置 UI 解耦，供 settings.c 调用。 */

typedef enum {
    WIFI_SET_NONE = 0,   /* 空闲，未连接 */
    WIFI_SET_CONNECTING, /* 连接中 */
    WIFI_SET_CONNECTED,  /* 已连接 */
    WIFI_SET_FAILED,     /* 连接失败 / 断开 */
} settings_wifi_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure; /* 是否加密 */
} settings_wifi_ap_t;

/* 初始化 esp_wifi（STA 模式），并自动连接 NVS 中保存的网络 */
void settings_wifi_init(void);

settings_wifi_state_t settings_wifi_state(void);
const char *settings_wifi_ssid(void); /* 当前连接/连接中的 SSID */
esp_ip4_addr_t settings_wifi_ip(void);
int settings_wifi_rssi(void);
bool settings_wifi_has_saved(void);

/* 异步扫描；扫描完成前 settings_wifi_scan_done() 返回 false */
void settings_wifi_scan_start(void);
bool settings_wifi_scan_done(void);
int settings_wifi_scan_count(void);
const settings_wifi_ap_t *settings_wifi_scan_ap(int index);

/* 连接并保存到 NVS（password 可为空字符串表示开放网络） */
void settings_wifi_connect(const char *ssid, const char *password);
void settings_wifi_disconnect(void);
/* 断开并清除已保存的凭据 */
void settings_wifi_forget(void);
