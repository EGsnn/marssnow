#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 火星系统引导切换（L1 系统能力）
 * 同一时刻只能跑一个固件，App 之间通过"切引导分区 + 重启"切换：
 *   - "xiaozhi" → 小智 A/B 槽（ota_0 / ota_1，自动选有效槽）
 *   - "box"     → 本系统（当前运行的 factory / ota 槽）
 * 切换前校验目标分区是否有有效固件，无效则不重启、返回错误。 */

/* 目标分区是否可引导（存在且有有效固件，不重启、无副作用） */
bool mars_boot_check(const char *app_id);

/* 切到目标分区并重启；无效分区返回 ESP_ERR_NOT_FOUND 且不重启 */
esp_err_t mars_boot_switch(const char *app_id);

#ifdef __cplusplus
}
#endif
