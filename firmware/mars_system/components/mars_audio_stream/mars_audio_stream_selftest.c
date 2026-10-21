/* mars_audio_stream_selftest：固件内 Opus 编解码往返自测（按命令运行） */

#include "mars_audio_stream_selftest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"

#include "mars_audio_stream.h"

static const char *TAG = "mars_stream_selftest";

/* Espressif 的 codec chain 测试使用 40 KiB 编码栈；Opus/SILK 不能直接跑在
 * 8 KiB 的 app_main 栈上，否则会破坏 FreeRTOS 相邻内存。 */
#define SELFTEST_TASK_STACK  (40 * 1024)
#define SELFTEST_TASK_PRIO   5
#define SELFTEST_REASON_LEN  64
#define SELFTEST_ENC_MAX     2048
#define SELFTEST_DEC_SAMPLES 4096

typedef struct {
    SemaphoreHandle_t done;
    int rc;
    UBaseType_t stack_hwm;
    char reason[SELFTEST_REASON_LEN];
} selftest_job_t;

static void set_reason(char *reason, size_t reason_len, const char *message)
{
    if (reason != NULL && reason_len > 0) {
        snprintf(reason, reason_len, "%s", message);
    }
}

static int selftest_run(char *reason, size_t reason_len)
{
#define SELFTEST_FAIL(message)                       \
    do {                                             \
        set_reason(reason, reason_len, message);      \
        ESP_LOGE(TAG, "selftest: FAIL %s", message); \
        goto out;                                    \
    } while (0)

    const int sample_count = MARS_AUDIO_STREAM_FRAME_SAMPLES;
    const int expected_in_bytes = sample_count * (int)sizeof(int16_t);
    int rc = -1;
    int16_t *pcm = NULL;
    uint8_t *encoded = NULL;
    int16_t *decoded = NULL;
    void *enc_handle = NULL;
    void *dec_handle = NULL;

    pcm = malloc((size_t)expected_in_bytes);
    decoded = malloc(SELFTEST_DEC_SAMPLES * sizeof(int16_t));
    if (pcm == NULL || decoded == NULL) {
        SELFTEST_FAIL("alloc failed");
    }

    /* 固定 440 Hz 正弦（60 ms @ 16 kHz，幅度 12000）。 */
    for (int i = 0; i < sample_count; ++i) {
        pcm[i] = (int16_t)(12000.0f * sinf(2.0f * (float)M_PI * 440.0f * (float)i /
                                           16000.0f));
    }

    esp_opus_enc_config_t enc_cfg = {
        .sample_rate      = 16000,
        .channel          = 1,
        .bits_per_sample  = 16,
        .bitrate          = ESP_OPUS_BITRATE_AUTO,
        .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity       = 5,
        .enable_fec       = false,
        .enable_dtx       = false,
        .enable_vbr       = true,
    };
    esp_audio_err_t err = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &enc_handle);
    if (err != ESP_AUDIO_ERR_OK || enc_handle == NULL) {
        SELFTEST_FAIL("enc_open failed");
    }

    int in_bytes = 0;
    int out_bytes = 0;
    err = esp_opus_enc_get_frame_size(enc_handle, &in_bytes, &out_bytes);
    if (err != ESP_AUDIO_ERR_OK || in_bytes != expected_in_bytes ||
        out_bytes <= 0 || out_bytes > SELFTEST_ENC_MAX) {
        ESP_LOGE(TAG, "selftest: frame size ret=%d in=%d out=%d expected=%d",
                 (int)err, in_bytes, out_bytes, expected_in_bytes);
        SELFTEST_FAIL("frame size mismatch");
    }

    encoded = malloc((size_t)out_bytes);
    if (encoded == NULL) {
        SELFTEST_FAIL("encoded alloc failed");
    }
    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)pcm,
        .len = (uint32_t)in_bytes,
    };
    esp_audio_enc_out_frame_t out = {
        .buffer = encoded,
        .len = (uint32_t)out_bytes,
        .encoded_bytes = 0,
    };
    err = esp_opus_enc_process(enc_handle, &in, &out);
    if (err != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0 ||
        out.encoded_bytes > (uint32_t)out_bytes) {
        ESP_LOGE(TAG, "selftest: enc_process ret=%d encoded=%u cap=%d",
                 (int)err, (unsigned)out.encoded_bytes, out_bytes);
        SELFTEST_FAIL("enc_process failed");
    }
    const uint32_t encoded_len = out.encoded_bytes;
    esp_opus_enc_close(enc_handle);
    enc_handle = NULL;

    esp_opus_dec_cfg_t dec_cfg = {
        .sample_rate    = 16000,
        .channel        = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
        .self_delimited = false,
    };
    err = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &dec_handle);
    if (err != ESP_AUDIO_ERR_OK || dec_handle == NULL) {
        SELFTEST_FAIL("dec_open failed");
    }
    esp_audio_dec_in_raw_t raw = {
        .buffer = encoded,
        .len = encoded_len,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
    };
    esp_audio_dec_out_frame_t dec_out = {
        .buffer = (uint8_t *)decoded,
        .len = SELFTEST_DEC_SAMPLES * sizeof(int16_t),
        .needed_size = 0,
        .decoded_size = 0,
    };
    esp_audio_dec_info_t dec_info = {0};
    err = esp_opus_dec_decode(dec_handle, &raw, &dec_out, &dec_info);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "selftest: dec_decode ret=%d needed=%u", (int)err,
                 (unsigned)dec_out.needed_size);
        SELFTEST_FAIL("dec_decode failed");
    }

    const uint32_t decoded_samples = dec_out.decoded_size / sizeof(int16_t);
    double input_energy = 0.0;
    double output_energy = 0.0;
    for (int i = 0; i < sample_count; ++i) {
        input_energy += (double)pcm[i] * (double)pcm[i];
    }
    for (uint32_t i = 0; i < decoded_samples; ++i) {
        output_energy += (double)decoded[i] * (double)decoded[i];
    }
    input_energy /= sample_count;
    output_energy = decoded_samples > 0 ? output_energy / decoded_samples : 0.0;

    ESP_LOGI(TAG, "selftest: enc %uB -> dec %u samples; energy in=%.0f out=%.0f; "
                  "rate=%uHz ch=%u",
             (unsigned)encoded_len, (unsigned)decoded_samples,
             input_energy, output_energy,
             (unsigned)dec_info.sample_rate, (unsigned)dec_info.channel);
    if (decoded_samples == 0 ||
        decoded_samples > (uint32_t)(sample_count + sample_count / 4)) {
        SELFTEST_FAIL("decoded length abnormal");
    }
    if (output_energy < input_energy * 0.2) {
        SELFTEST_FAIL("decoded energy too low");
    }

    rc = 0;
    ESP_LOGI(TAG, "selftest: PASS");

out:
    if (enc_handle != NULL) {
        esp_opus_enc_close(enc_handle);
    }
    if (dec_handle != NULL) {
        esp_opus_dec_close(dec_handle);
    }
    free(pcm);
    free(encoded);
    free(decoded);
    return rc;
#undef SELFTEST_FAIL
}

static void selftest_task(void *arg)
{
    selftest_job_t *job = arg;
    job->rc = selftest_run(job->reason, sizeof(job->reason));
    job->stack_hwm = uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreGive(job->done);
    vTaskDeleteWithCaps(NULL);
}

int mars_audio_stream_selftest(char *reason, size_t reason_len)
{
    if (reason != NULL && reason_len > 0) {
        reason[0] = '\0';
    }

    selftest_job_t job = {
        .done = xSemaphoreCreateBinary(),
        .rc = -1,
        .stack_hwm = 0,
        .reason = {0},
    };
    if (job.done == NULL) {
        set_reason(reason, reason_len, "sync alloc failed");
        return -1;
    }
    if (xTaskCreateWithCaps(selftest_task, "opus_selftest", SELFTEST_TASK_STACK, &job,
                            SELFTEST_TASK_PRIO, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vSemaphoreDelete(job.done);
        set_reason(reason, reason_len, "task alloc failed");
        return -1;
    }

    /* 显式诊断命令允许同步等待；等待期间 app_main 阻塞，两个 idle task可运行。 */
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    set_reason(reason, reason_len, job.reason);
    ESP_LOGI(TAG, "selftest worker stack_hwm=%u", (unsigned)job.stack_hwm);
    return job.rc;
}
