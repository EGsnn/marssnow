#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 火星系统 L1 音频能力层（mars_media）
 * 业务层复用输出所有权与录音生命周期，构建时选择板级音频后端：
 *   - BOX：ES8311 + I2S duplex；微雪 1.46：PCM5101 + 独立 I2S 麦克风
 *   - I2S 播放/录音原语（16kHz int16 单声道）
 *   - 音量控制（硬件增益或软件缩放 0-100）
 *   - 内置提示音（正弦波）
 * 状态变化通过事件总线发布：MARS_EVENT_MEDIA_STATE_CHANGED */

/* 音频设备状态（随 MARS_EVENT_MEDIA_STATE_CHANGED 发布） */
typedef enum {
    MARS_MEDIA_STATE_READY = 0,   /* 所选音频后端初始化完成 */
    MARS_MEDIA_STATE_NO_CODEC,    /* 音频后端初始化失败 */
    MARS_MEDIA_STATE_OUTPUT_ON,   /* 输出通路打开 */
    MARS_MEDIA_STATE_OUTPUT_OFF,  /* 输出通路关闭 */
    MARS_MEDIA_STATE_INPUT_ON,    /* 输入通路打开 */
    MARS_MEDIA_STATE_INPUT_OFF,   /* 输入通路关闭 */
} mars_media_state_t;

typedef struct {
    int input_sample_rate;   /* 录音采样率，默认 16000 */
    int output_sample_rate;  /* 播放采样率，默认 16000 */
} mars_media_cfg_t;

/* 录音回调：pcm 为 int16 单声道帧，samples 为采样点数（帧长由内部固定） */
typedef void (*mars_media_record_cb_t)(const int16_t *pcm, size_t samples, void *ctx);

/* 初始化/反初始化；cfg 为 NULL 时用默认 16kHz。
 * I2C 总线复用 mars_bsp（mars_board_i2c_bus_get），必须等 mars_board_init 之后调用。
 * 音频后端初始化失败不阻塞桌面，发 MARS_MEDIA_STATE_NO_CODEC。 */
esp_err_t mars_media_init(const mars_media_cfg_t *cfg);
esp_err_t mars_media_deinit(void);

/* 所选音频后端就绪；不代表物理录放音已验收。 */
bool mars_media_is_ready(void);

/* 输出/输入通路开关（codec 增益 + I2S 通道启停），状态变化发事件。
 * output 已被 claim 时，外部 enable_output 返回 ESP_ERR_INVALID_STATE；
 * owner 应通过 mars_media_play() 按需开启，并由 release 恢复原状态。 */
esp_err_t mars_media_enable_output(bool enable);
esp_err_t mars_media_enable_input(bool enable);

/* 输出通路独占所有权（P1 单音频所有者模型）。
 *
 * 问题：mars_media_play() 会按需自行打开输出通路，调用方无法区分
 * "输出本来就开着" 与 "我这次才打开的"，停止时容易误关别人的通路。
 *
 * 正确用法：需要独占使用输出的服务（如 mars_audio_stream 下行解码播放）在
 * 启动流水线前调用 mars_media_output_claim()，它会记录 claim 时刻的原输出状态；
 * 停止时调用 mars_media_output_release()，只把输出恢复到 claim 之前的状态
 * （claim 前是关的才关，claim 前已开的保持开）。
 *
 * - claim：返回 ESP_OK 获得使用权；ESP_ERR_INVALID_STATE 表示音频底层未就绪，
 *   ESP_ERR_INVALID_STATE(已被占用) 表示已有其他所有者 claim 未 release（P1 单所有者）。
 * - release：幂等；无 claim 时为空操作。
 */
esp_err_t mars_media_output_claim(void);
esp_err_t mars_media_output_release(void);

/* 查询输出通路当前是否开启（不改变状态）。 */
bool mars_media_output_is_enabled(void);


/* 音量 0-100（ES8311 DAC 增益或 PCM5101 软件缩放） */
esp_err_t mars_media_set_volume(int percent);
int mars_media_get_volume(void);

/* 阻塞播放 PCM（int16 单声道，内部展开为 I2S stereo 帧）。
 * 调用方必须先成功 mars_media_output_claim()；没有 owner 时返回
 * ESP_ERR_INVALID_STATE。timeout_ms=0 表示无限等待写完成。 */
esp_err_t mars_media_play(const int16_t *pcm, size_t samples, uint32_t timeout_ms);

/* Synchronous capture for a single audio owner such as XiaoZhi's input task.
 * Returns captured samples, or a negative value on failure. */
int mars_media_read(int16_t *pcm, size_t samples);

/* 录音（后台 I2S 读任务 + 回调）；重复 start 幂等，stop 停止并退任务 */
esp_err_t mars_media_record_start(mars_media_record_cb_t cb, void *ctx);
esp_err_t mars_media_record_stop(void);

/* 是否正在录音（用于多调用方互斥：mars_audio_stream 与旧 API 二选一） */
bool mars_media_record_is_active(void);

/* 内置提示音：正弦波发生器（按键反馈/通知音），音量 0-100 */
esp_err_t mars_media_play_tone(uint16_t freq_hz, uint32_t duration_ms, int volume_percent);

#ifdef __cplusplus
}
#endif
