#pragma once

/* mars_audio_stream：火星系统 Opus 流式音频服务（P1）
 *
 * 动态应用通过本接口获取/播放 Opus 音频，不直接访问硬件：
 *   - 上行：16 kHz / 单声道 / 60 ms 一帧 Opus 编码，应用轮询读取；
 *   - 下行：Opus 解码播放，24 kHz（及 48 kHz）在系统层重采样到 16 kHz。
 * 所有队列有界，stop/close 为同步屏障：返回后系统任务不再访问
 * 应用内存（严禁系统任务异步回调已卸载应用代码）。
 *
 * ABI 约束：只传定长 C 结构 / 整数 / 缓冲+长度，不跨 C++ 对象。
 */

#include <stddef.h>
#include <stdint.h>
#include "mars_voice_effect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 系统侧模块初始化（由 app_main 在启动早期、任何动态应用/并发调用之前调用一次）。
 * 创建生命周期锁；不在动态应用 ABI 符号表内。幂等。 */
void mars_audio_stream_init(void);

/* 服务能力位（mars_services_capabilities()） */
#define MARS_CAP_OPUS_ENCODE      (1ull << 0)
#define MARS_CAP_OPUS_DECODE      (1ull << 1)
#define MARS_CAP_WSS              (1ull << 2)   /* Verified TLS WebSocket transport */
#define MARS_CAP_SECURE_CONFIG    (1ull << 3)   /* Read-only persisted service configuration */
#define MARS_CAP_AUDIO_LEVELS     (1ull << 4)   /* Polling PCM amplitude, no app callbacks */

#define MARS_CAP_VOICE_EFFECTS (1ull << 5) /* Per-packet effect, legacy playback always original */

/* Extended playback: effect is copied into each queue packet, never global. */
int mars_audio_play_opus_effect(const uint8_t *data, size_t len,
    uint32_t sample_rate, uint16_t frame_ms, uint8_t effect);

#define MARS_AUDIO_STREAM_SAMPLE_RATE 16000
#define MARS_AUDIO_STREAM_CHANNELS    1
#define MARS_AUDIO_STREAM_FRAME_MS    60
#define MARS_AUDIO_STREAM_FRAME_SAMPLES (MARS_AUDIO_STREAM_SAMPLE_RATE * MARS_AUDIO_STREAM_FRAME_MS / 1000) /* 960 */

/* 上行配置（固定 16k/单声道/60ms，字段保留扩展） */
typedef struct {
    uint32_t sample_rate;   /* 输入采样率，当前必须为 16000 */
    uint16_t frame_ms;      /* 编码帧时长，当前必须为 60 */
    uint8_t  channels;      /* 当前必须为 1 */
    uint8_t  reserved;
    uint32_t queue_depth;   /* 上行 Opus 包队列深度（默认 8） */
    uint32_t bitrate;       /* Opus 编码比特率 bps；0 = 自动 */
    int      complexity;    /* Opus 编码复杂度 0-10；<0 = 默认 */
} mars_audio_stream_config_t;

/* 上行包信息（mars_audio_stream_read 输出） */
typedef struct {
    uint32_t sample_rate;   /* 编码采样率 = 16000 */
    uint16_t frame_ms;      /* 编码帧时长 = 60 */
    uint8_t  channels;      /* = 1 */
    uint8_t  flags;         /* 保留 */
    uint64_t seq;           /* 全局包序号（从 1 起） */
    uint32_t payload_len;   /* 本次返回的 Opus 包字节数 */
} mars_audio_packet_info_t;

/* 统计（mars_audio_stream_stats）。
 * 统计自上一次对应方向启动起累计（上行 start 不清下行、下行首次播放不清上行）。 */
typedef struct {
    uint32_t enc_packets;     /* 已编码包数（上行） */
    uint32_t dec_packets;     /* 已解码包数（下行） */
    uint32_t dropped_packets; /* 上下行有界队列满时丢最旧的【包】数（单位：包） */
    uint32_t dropped_samples; /* 上行拼帧器缓冲溢出丢弃的 PCM【样本】数（单位：样本，非包） */
    uint32_t play_failures;   /* 播放（写 codec）失败次数 */
    uint32_t encode_failures; /* Opus 编码失败次数 */
    uint32_t decode_failures; /* Opus 解码失败次数 */
    uint32_t resample_failures;/* 重采样失败次数 */
} mars_audio_stream_stats_t;

/* 默认上行配置 */
#define MARS_AUDIO_STREAM_CONFIG_DEFAULT() \
    { .sample_rate = MARS_AUDIO_STREAM_SAMPLE_RATE, \
      .frame_ms    = MARS_AUDIO_STREAM_FRAME_MS, \
      .channels    = MARS_AUDIO_STREAM_CHANNELS, \
      .queue_depth = 8, \
      .bitrate     = 0, \
      .complexity  = -1 }

/* ═══════════ 上行（采集 → 60ms Opus → 应用轮询） ═══════════ */

/* 开始上行流：启动录音与 Opus 编码任务。
 * @param cfg 可为 NULL（用默认配置）；非法参数返回负错误码。
 * @return 0 成功（幂等：已运行直接返回 0）；-22(EINVAL) 参数错误；
 *         -16(EBUSY) 底层录音被占用；-19(ENODEV) 音频底层未就绪；
 *         -12(ENOMEM) 内存不足。启动上行不会清除仍在运行的下行统计。 */
int mars_audio_stream_start(const mars_audio_stream_config_t *cfg);

/* 轮询读取一个 Opus 包（阻塞至多 timeout_ms）。
 * @param dst      目的缓冲，容量 capacity
 * @param info     非空时返回包信息（seq/sample_rate/...）
 * @param timeout_ms 等待毫秒；0 = 立即返回
 * @return 0 成功取包；-11(EAGAIN) 超时无包；-110(ETIMEDOUT) 上行未运行；
 *         -12(ENOMEM) 目的缓冲过小（包未被消费）。 */
int mars_audio_stream_read(uint8_t *dst, size_t capacity,
                           mars_audio_packet_info_t *info, uint32_t timeout_ms);

/* 停止上行流（同步屏障）：停止录音 → 编码任务经退出信号量确认退出 →
 * 在上行队列锁保护下清空队列并释放队列内存。返回后系统任务不再触碰应用内存。
 * @return 0 成功；-110(ETIMEDOUT) 任务未在限时内退出（保持运行）；
 *         幂等：未运行时返回 0。 */
int mars_audio_stream_stop(void);
/* Stop capture, encode/pad the tail and retain queued packets for read().
 * Idempotent; start() returns EBUSY until stop() releases retained resources. */
int mars_audio_stream_finish(void);

/* ═══════════ 下行（应用提交 Opus → 系统解码播放） ═══════════ */

/* 提交一个 Opus 包到播放队列（系统解码后经 16 kHz 播放）。
 * @param data Opus 包负载
 * @param len  包长度
 * @param sample_rate 编码采样率（8000/12000/16000/24000/48000）
 * @param frame_ms    帧时长；仅允许 0（由解码器推导）/ 20 / 40 / 60
 * @return 0 已入队（队列满时系统丢最旧包腾出位置，新包仍入队并返回 0，
 *         丢包计入 stats.dropped_packets）；
 *         -22(EINVAL) 参数错误或包过长；
 *         -16(EBUSY) 输出通路已被其它所有者占用；
 *         -12(ENOMEM) 初始化/分配失败。
 * 注意：入队成功不代表已播放；播放失败体现在 stats.play_failures。 */
int mars_audio_play_opus(const uint8_t *data, size_t len,
                         uint32_t sample_rate, uint16_t frame_ms);

/* 停止播放并清空播放队列（同步屏障：播放任务退出后返回）。 */
int mars_audio_playback_stop(void);
/* Number of queued packets plus the packet currently decoded/played. */
int mars_audio_playback_pending(void);

/* ═══════════ 统计 / 能力 ═══════════ */

/* 读取累计统计（编码/解码/丢包/播放失败）。 */
void mars_audio_stream_stats(mars_audio_stream_stats_t *out);

/* 当前上行是否运行。 */
int mars_audio_stream_is_running(void);

/* Mean absolute PCM amplitude mapped to 0..1000. Independent directions;
 * samples older than 250ms read as zero. No audio data leaves the system. */
void mars_audio_levels(uint16_t *recording, uint16_t *playback);

#ifdef __cplusplus
}
#endif
