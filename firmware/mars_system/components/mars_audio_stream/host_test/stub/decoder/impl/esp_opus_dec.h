/* host 桩：esp_opus_dec 声明（接口与 esp_audio_codec 2.5.0 一致） */
#ifndef STUB_ESP_OPUS_DEC_H
#define STUB_ESP_OPUS_DEC_H

#include <stdint.h>

typedef enum {
    ESP_OPUS_DEC_FRAME_DURATION_INVALID = -1,
    ESP_OPUS_DEC_FRAME_DURATION_20_MS   = 3,
    ESP_OPUS_DEC_FRAME_DURATION_40_MS   = 4,
    ESP_OPUS_DEC_FRAME_DURATION_60_MS   = 5,
} esp_opus_dec_frame_duration_t;

typedef struct {
    uint32_t sample_rate;
    int channel;
    esp_opus_dec_frame_duration_t frame_duration;
    int self_delimited;
} esp_opus_dec_cfg_t;

typedef struct {
    uint8_t *buffer;
    uint32_t len;
    uint32_t consumed;
    int frame_recover;
} esp_audio_dec_in_raw_t;

typedef struct {
    uint8_t *buffer;
    uint32_t len;
    uint32_t needed_size;
    uint32_t decoded_size;
} esp_audio_dec_out_frame_t;

typedef struct {
    uint32_t sample_rate;
    int bits_per_sample;
    int channel;
    int bitrate;
    int frame_size;
} esp_audio_dec_info_t;

typedef int esp_audio_err_t;
#define ESP_AUDIO_ERR_OK 0
#define ESP_AUDIO_ERR_BUFF_NOT_ENOUGH (-2)
#define ESP_AUDIO_DEC_RECOVERY_NONE 0

int esp_opus_dec_open(void *cfg, uint32_t cfg_sz, void **dec_handle);
int esp_opus_dec_decode(void *dec_handle, esp_audio_dec_in_raw_t *in,
                        esp_audio_dec_out_frame_t *out, esp_audio_dec_info_t *info);
void esp_opus_dec_close(void *dec_handle);

/* 测试辅助（codec_stub.c） */
void stub_dec_set_fail_next(int fail);
void stub_dec_set_delay_ms(int ms);
int stub_dec_open_count(void);
int stub_dec_close_count(void);

#endif
