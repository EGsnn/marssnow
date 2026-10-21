/* host 桩：esp_opus_enc 声明（接口与 esp_audio_codec 2.5.0 一致） */
#ifndef STUB_ESP_OPUS_ENC_H
#define STUB_ESP_OPUS_ENC_H

#include <stdint.h>

#define ESP_OPUS_BITRATE_AUTO (-1000)

typedef enum {
    ESP_OPUS_ENC_FRAME_DURATION_60_MS = 5,
} esp_opus_enc_frame_duration_t;

typedef enum {
    ESP_OPUS_ENC_APPLICATION_VOIP = 0,
} esp_opus_enc_application_mode_t;

typedef struct {
    uint32_t sample_rate;
    int channel;
    int bits_per_sample;
    int bitrate;
    esp_opus_enc_frame_duration_t frame_duration;
    esp_opus_enc_application_mode_t application_mode;
    int complexity;
    int enable_fec;
    int enable_dtx;
    int enable_vbr;
} esp_opus_enc_config_t;

typedef struct {
    uint8_t *buffer;
    uint32_t len;
} esp_audio_enc_in_frame_t;

typedef struct {
    uint8_t *buffer;
    uint32_t len;
    uint32_t encoded_bytes;
} esp_audio_enc_out_frame_t;

typedef int esp_audio_err_t;
#define ESP_AUDIO_ERR_OK 0

int esp_opus_enc_open(void *cfg, uint32_t cfg_sz, void **enc_hd);
int esp_opus_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size);
int esp_opus_enc_process(void *enc_hd, esp_audio_enc_in_frame_t *in_frame,
                         esp_audio_enc_out_frame_t *out_frame);
void esp_opus_enc_close(void *enc_hd);

/* 测试辅助（codec_stub.c） */
void stub_enc_set_fail_next(int fail);
int stub_enc_open_count(void);
int stub_enc_close_count(void);

#endif
