/* host 桩：mars_media 接口（行为仿真，实现见 media_stub.c） */
#ifndef STUB_MARS_MEDIA_H
#define STUB_MARS_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"   /* esp_err_t, ESP_OK */

typedef void (*mars_media_record_cb_t)(const int16_t *pcm, size_t samples, void *ctx);

esp_err_t mars_media_init(const void *cfg);
esp_err_t mars_media_deinit(void);
bool mars_media_is_ready(void);
esp_err_t mars_media_enable_output(bool enable);
esp_err_t mars_media_enable_input(bool enable);
esp_err_t mars_media_output_claim(void);
esp_err_t mars_media_output_release(void);
bool mars_media_output_is_enabled(void);
esp_err_t mars_media_set_volume(int percent);
esp_err_t mars_media_play(const int16_t *pcm, size_t samples, uint32_t timeout_ms);
esp_err_t mars_media_record_start(mars_media_record_cb_t cb, void *ctx);
esp_err_t mars_media_record_stop(void);
bool mars_media_record_is_active(void);
esp_err_t mars_media_play_tone(uint16_t freq_hz, uint32_t duration_ms, int volume_percent);

/* 测试辅助（media_stub.c） */
void stub_media_set_record_interval_ms(int ms);
void stub_media_set_play_fail_next(int fail);
unsigned long stub_media_play_calls(void);
unsigned long stub_media_play_total_samples(void);
int stub_media_output_enabled(void);
unsigned long stub_media_record_calls(void);
/* 模拟"其他所有者已 claim 输出"：返回 EBUSY */
void stub_media_foreign_claim(int held);
/* 模拟"输出原本已开启"（不经过本服务 claim/release） */
void stub_media_set_output_enabled(int on);
void stub_media_reset(void);

#endif

int stub_media_first_sample(unsigned call);
