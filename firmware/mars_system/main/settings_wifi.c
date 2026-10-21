#include "settings_wifi.h"

#include "mars_net.h"

/* 兼容层：Wi-Fi 能力已下放 mars_net，settings UI 继续走原接口。
 * 枚举/结构布局与 mars_net 一一对应，直接透传。 */

void settings_wifi_init(void)
{
    mars_net_init();
}

settings_wifi_state_t settings_wifi_state(void)
{
    return (settings_wifi_state_t)mars_net_state();
}

const char *settings_wifi_ssid(void)
{
    return mars_net_ssid();
}

esp_ip4_addr_t settings_wifi_ip(void)
{
    return mars_net_ip();
}

int settings_wifi_rssi(void)
{
    return mars_net_rssi();
}

bool settings_wifi_has_saved(void)
{
    return mars_net_has_saved();
}

void settings_wifi_scan_start(void)
{
    mars_net_scan_start();
}

bool settings_wifi_scan_done(void)
{
    return mars_net_scan_done();
}

int settings_wifi_scan_count(void)
{
    return mars_net_scan_count();
}

const settings_wifi_ap_t *settings_wifi_scan_ap(int index)
{
    return (const settings_wifi_ap_t *)mars_net_scan_ap(index);
}

void settings_wifi_connect(const char *ssid, const char *password)
{
    mars_net_connect(ssid, password);
}

void settings_wifi_disconnect(void)
{
    mars_net_disconnect();
}

void settings_wifi_forget(void)
{
    mars_net_forget();
}
