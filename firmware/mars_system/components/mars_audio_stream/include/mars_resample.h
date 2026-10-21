#pragma once

/* mars_resample：定点线性插值重采样（单声道 int16 PCM）
 *
 * 用途：下行 Opus 解码后若采样率为 24 kHz（或 48 kHz），
 *       在系统层重采样为 16 kHz 再交给 ES8311 播放。
 * 纯 C、无 FreeRTOS/ESP-IDF 依赖，可主机侧单元测试。
 *
 * 实现：相位累加器 + 相邻样本线性插值。输出样本由输入相位比例
 *       (out/in) 驱动，每输出 1 个样本在输入上推进 in/out 个样本。
 * 跨缓冲调用：每次调用相位从输入 0 起算，上一缓冲末样本参与首样本
 *       插值，保证包边界波形连续；整包重采样（如 24k 包 → 16k 包）
 *       输出样本数精确（960×16/24=640，整除），无时间漂移。 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MARS_RESAMPLE_FRAC_BITS 16
#define MARS_RESAMPLE_ONE       (1u << MARS_RESAMPLE_FRAC_BITS)

typedef struct {
    uint32_t in_rate;          /* 输入采样率 */
    uint32_t out_rate;         /* 输出采样率 */
    uint64_t phase;            /* 定点相位：当前输入采样位置 << 16 */
    uint64_t step;             /* 每个输出样本推进的输入样本数 << 16 */
    int16_t  last_sample;      /* 上一次输出的最后一个输入样本（跨缓冲连续） */
    uint8_t  initialized;
} mars_resample_t;

/* 初始化重采样器。in_rate/out_rate 需 > 0，单声道。 */
void mars_resample_init(mars_resample_t *r, uint32_t in_rate, uint32_t out_rate);

/* 重采样一段 int16 单声道 PCM。
 * @param in       输入缓冲
 * @param in_n     输入样本数
 * @param out      输出缓冲（建议容量 >= in_n * out_rate / in_rate + 4）
 * @param out_cap  输出缓冲容量（样本数）
 * @return 实际输出样本数；出错返回 0。
 * 跨缓冲连续：上一缓冲末样本用于当前缓冲首样本插值（波形无跳变）。 */
size_t mars_resample_process(mars_resample_t *r, const int16_t *in,
                             size_t in_n, int16_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
