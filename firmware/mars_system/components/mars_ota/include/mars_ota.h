#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 火星系统统一 OTA（mars_ota，L1 系统能力）
 * 一个 manifest JSON 描述所有固件（火星系统 / 小智等外部 App）的版本、
 * 目标分区、下载地址与 SHA256；任何固件走同一套升级链路。
 * 用法（调用方需先保证网络已连接，可配合 mars_net_wait_connected）：
 *   mars_ota_check("xiaozhi");
 *   if (mars_ota_has_update()) mars_ota_update(progress_cb, NULL);
 *   esp_restart();               // 重启后新固件生效
 *   启动正常后 mars_ota_confirm() 确认；异常回滚见 mars_ota_rollback()
 *
 * manifest 结构：
 * { "mars_system": { "version":"0.2.0", "part":"ota_0", "url":"...", "size":123456, "sha256":"64hex" },
 *   "xiaozhi":     { "version":"0.9.6", "part":"ota_1", "url":"...", "size":...,   "sha256":"..." } } */

typedef enum {
    MARS_OTA_IDLE = 0,
    MARS_OTA_CHECKING, /* 拉取 manifest 中 */
    MARS_OTA_READY,    /* 检查到新版本，可 update */
    MARS_OTA_UPDATING, /* 下载写入中 */
    MARS_OTA_DONE,     /* 写入完成，重启后生效 */
    MARS_OTA_FAILED,   /* 出错，mars_ota_error() 查看 */
} mars_ota_state_t;

/* 初始化（懒加载，可省略）；url 默认取 CONFIG_MARS_OTA_MANIFEST_URL，运行时覆盖 */
void mars_ota_init(void);
void mars_ota_set_manifest_url(const char *url);

/* 拉取 manifest 并检查指定 app 是否有新版本（阻塞）。
 * app_id 示例："mars_system" / "xiaozhi"。返回是否有更新。 */
bool mars_ota_check(const char *app_id);

mars_ota_state_t mars_ota_state(void);
const char *mars_ota_error(void);
const char *mars_ota_latest_version(void); /* 最新版本号 */
bool mars_ota_has_update(void);
const char *mars_ota_update_url(void);     /* 待下载固件 URL */
const char *mars_ota_target_partition(void); /* 目标分区 label，如 ota_1 */

/* 下载并写入目标分区（流式，含 SHA256 校验）。
 * progress 可选：回调 percent(0-100)。返回 true 表示写入完成（重启后生效）。 */
bool mars_ota_update(void (*progress)(int percent, void *ctx), void *ctx);

/* 回滚机制：新固件启动正常后调用 confirm；启动异常调用 rollback 回退并重启 */
void mars_ota_confirm(void);
void mars_ota_rollback(void);
