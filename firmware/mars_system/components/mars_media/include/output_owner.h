#pragma once

/* output_owner：输出通路独占所有权状态机（纯 C，无 OS 依赖）
 *
 * 为什么需要：mars_media_play() 会按需自行打开输出通路，调用方无法区分
 * "输出本来就开着" 与 "我这次才打开的"。本状态机记录 claim 时刻的原状态，
 * release 时只恢复 claim 前的状态（claim 前是关的才关，claim 前已开则保持）。
 *
 * 本文件只实现无锁的状态判定，不加锁——并发互斥由调用方在 production 用
 * FreeRTOS mutex、host 用 pthread mutex 包裹同一份逻辑，保证两边语义一致。
 *
 * P1 单音频所有者：重复 claim 返回失败。
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool claimed;       /* 是否已有所有者 */
    bool state_before;  /* claim 时刻输出是否已开启 */
} output_owner_t;

/* 初始化到"无所有者"状态。 */
void output_owner_init(output_owner_t *o);

/* 尝试 claim。current_enabled 为当前输出通路是否已开启。
 * 返回 true 表示成功获得所有权（已记录原状态）；false 表示已被占用。 */
bool output_owner_claim(output_owner_t *o, bool current_enabled);

/* 释放所有权。返回值表示调用方是否需要关闭输出通路：
 * true  = claim 前是关的，本所有者打开了它，应关闭以恢复原状态；
 * false = claim 前已开，保持开启（不动别人的通路）。 */
bool output_owner_release(output_owner_t *o);

/* 当前是否已有所有者。 */
bool output_owner_is_claimed(const output_owner_t *o);

#ifdef __cplusplus
}
#endif
