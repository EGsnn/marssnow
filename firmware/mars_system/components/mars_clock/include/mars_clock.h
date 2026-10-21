#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* 火星系统时钟能力（mars_clock，L1 系统能力）
 *
 * 时间 = SNTP 同步的 UTC + IP 定位的时区偏移：
 *  - 开机先读取 NVS 缓存的时区偏移，Wi-Fi 连接成功后自动后台同步；
 *  - SNTP 从 ntp.aliyun.com 同步 UTC 基准；
 *  - HTTP 请求 ip-api.com 按公网 IP 定位时区（失败回退 worldtimeapi.org）；
 *  - 时区偏移写入 NVS，离线重启后仍能显示上次定位的当地时间。
 *
 * 任何 App / 系统服务通过本 API 获取"IP 所在时区"的本地时间。 */

typedef enum {
    MARS_CLOCK_NO_NET = 0, /* 未联网：无 SNTP 时间，可能仅有缓存时区 */
    MARS_CLOCK_SYNCING,    /* 正在同步（SNTP 或时区定位中） */
    MARS_CLOCK_SYNCED,     /* 已同步：SNTP 有效 + 时区偏移可用 */
    MARS_CLOCK_FAILED,     /* 联网但同步失败（SNTP 超时 / 时区服务不可达） */
} mars_clock_state_t;

/* 读取时区缓存、订阅 Wi-Fi 事件；重复调用无副作用 */
void mars_clock_init(void);

/* 网络就绪后调用：启动 SNTP 并后台获取 IP 时区；已启动则无操作 */
void mars_clock_start(void);

mars_clock_state_t mars_clock_state(void);

/* 获取"IP 所在时区"的本地时间（UTC + 时区偏移）。
 * SNTP 尚未同步时返回 false。 */
bool mars_clock_get_local_time(struct tm *out);

/* 当前时区偏移（秒，东八区 = 28800）；未定位时返回 0（UTC） */
int mars_clock_tz_offset_sec(void);

/* 时区名，如 "Asia/Shanghai"；未定位时返回 "UTC" */
const char *mars_clock_tz_name(void);
