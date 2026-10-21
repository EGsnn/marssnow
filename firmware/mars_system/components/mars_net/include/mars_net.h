#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"

/* 火星系统 Wi-Fi 能力（mars_net，L1 系统能力）
 * 所有事件经 mars_event 总线发布（见 mars_event.h MARS_EVENT_WIFI_*）。
 * 任何 App / 系统服务都可通过本 API 复用 Wi-Fi，无需各自初始化。 */

typedef enum {
    MARS_WIFI_NONE = 0,   /* 空闲，未连接 */
    MARS_WIFI_CONNECTING, /* 连接中 */
    MARS_WIFI_CONNECTED,  /* 已连接（含 IP） */
    MARS_WIFI_FAILED,     /* 连接失败 / 断开 */
} mars_wifi_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure; /* 是否加密 */
} mars_wifi_ap_t;

/* ── 初始化与状态（连接管理组） ── */

/* 初始化 esp_wifi（STA 模式），自动连接 NVS 中保存的网络；重复调用无副作用 */
void mars_net_init(void);

mars_wifi_state_t mars_net_state(void);
const char *mars_net_ssid(void); /* 当前连接/连接中的 SSID */
esp_ip4_addr_t mars_net_ip(void);
int mars_net_rssi(void);         /* 已连接时返回实时信号，否则 0 */
bool mars_net_has_saved(void);

/* ── 连接控制（连接管理组） ── */

/* 连接并保存到 NVS（password 可为空表示开放网络） */
void mars_net_connect(const char *ssid, const char *password);
/* Checked path for provisioning; when staged, persist only after obtaining IP. */
esp_err_t mars_net_connect_checked(const char *ssid,const char *password,bool staged);
void mars_net_disconnect(void);
/* 断开并清除已保存的凭据 */
void mars_net_forget(void);

/* ── 扫描（扫描组） ── */

/* 异步扫描；完成前 mars_net_scan_done() 返回 false，完成后发布
 * MARS_EVENT_WIFI_SCAN_DONE（data=count） */
void mars_net_scan_start(void);
bool mars_net_scan_done(void);
int mars_net_scan_count(void);
const mars_wifi_ap_t *mars_net_scan_ap(int index);

/* ── 系统联动组 ── */

/* 阻塞等待连接成功，最多 timeout_ms；成功返回 true。
 * OTA / NTP / 云端 App 启动前调用 */
bool mars_net_wait_connected(uint32_t timeout_ms);
/* 低功耗模式（WIFI_PS_MIN_MODEM）；默认关闭 */
void mars_net_set_power_save(bool enable);
/* 设置 STA 主机名（DHCP 上报用） */
void mars_net_set_hostname(const char *hostname);

/* ── 配网组（AP 模式骨架） ── */

/* 进入配网模式：停 STA 连接 → 启 softAP（ap_pass 为空则开放网络）。
 * 进入/退出发布 MARS_EVENT_WIFI_CONFIG_ENTER/EXIT。HTTP 配网页后续由 UI 层接入 */
void mars_net_provision_start(const char *ap_ssid, const char *ap_pass);
esp_err_t mars_net_provision_start_checked(const char *ap_ssid,const char *ap_pass);
/* 退出配网：恢复 STA 模式并自动重连已保存网络 */
void mars_net_provision_stop(void);
bool mars_net_in_provision(void);
const char *mars_net_provision_ssid(void);

bool mars_net_credentials_saved(void);
