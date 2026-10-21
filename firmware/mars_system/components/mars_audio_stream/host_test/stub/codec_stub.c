/* host 桩：esp_opus enc/dec 仿真
 *  - enc：open 记录 cfg；get_frame_size in=1920B/out=400B；process 产出恒定输出
 *  - dec：按 cfg.sample_rate 产出对应长度 PCM（16000→960 样本等）；可注入失败/延迟
 */
#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint32_t sample_rate;
    int channel;
    int frame_duration;
    int self_delimited;
    int opened;
} dec_ctx_t;

static atomic_int g_enc_fail_next;
static atomic_int g_enc_open_c;
static atomic_int g_enc_close_c;
static atomic_int g_dec_fail_next;
static atomic_int g_dec_delay_ms;
static atomic_int g_dec_open_c;
static atomic_int g_dec_close_c;

static bool consume_one(atomic_int *counter)
{
    int current = atomic_load_explicit(counter, memory_order_relaxed);
    while (current > 0) {
        if (atomic_compare_exchange_weak_explicit(counter, &current, current - 1,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

/* ── encoder ── */
int esp_opus_enc_open(void *cfg, uint32_t cfg_sz, void **enc_hd)
{
    (void)cfg;
    (void)cfg_sz;
    atomic_fetch_add_explicit(&g_enc_open_c, 1, memory_order_relaxed);
    *enc_hd = calloc(1, 8);
    return *enc_hd == NULL ? -1 : ESP_AUDIO_ERR_OK;
}

int esp_opus_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size)
{
    (void)enc_hd;
    /* 真实 esp_audio_codec 返回 PCM 输入字节数，不是 int16 样本数。 */
    *in_size = 960 * (int)sizeof(int16_t);
    *out_size = 400;
    return ESP_AUDIO_ERR_OK;
}

int esp_opus_enc_process(void *enc_hd, esp_audio_enc_in_frame_t *in_frame,
                         esp_audio_enc_out_frame_t *out_frame)
{
    (void)enc_hd;
    if (consume_one(&g_enc_fail_next)) {
        return -1;
    }
    /* 恒定输出 120 字节 Opus（语音帧典型量级） */
    uint32_t n = in_frame->len < 120 ? in_frame->len : 120;
    if (out_frame->len < n) {
        return -1;
    }
    memset(out_frame->buffer, 0xAA, n);
    out_frame->encoded_bytes = n;
    return ESP_AUDIO_ERR_OK;
}

void esp_opus_enc_close(void *enc_hd)
{
    atomic_fetch_add_explicit(&g_enc_close_c, 1, memory_order_relaxed);
    free(enc_hd);
}

/* ── decoder ── */
int esp_opus_dec_open(void *cfg, uint32_t cfg_sz, void **dec_handle)
{
    (void)cfg_sz;
    atomic_fetch_add_explicit(&g_dec_open_c, 1, memory_order_relaxed);
    dec_ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return -1;
    }
    esp_opus_dec_cfg_t *cfg_ = cfg;
    c->sample_rate = cfg_->sample_rate;
    c->channel = cfg_->channel;
    c->frame_duration = (int)cfg_->frame_duration;
    c->self_delimited = cfg_->self_delimited;
    c->opened = 1;
    *dec_handle = c;
    return ESP_AUDIO_ERR_OK;
}

int esp_opus_dec_decode(void *dec_handle, esp_audio_dec_in_raw_t *in,
                        esp_audio_dec_out_frame_t *out, esp_audio_dec_info_t *info)
{
    dec_ctx_t *c = dec_handle;
    if (consume_one(&g_dec_fail_next)) {
        return -1;
    }
    int delay_ms = atomic_load_explicit(&g_dec_delay_ms, memory_order_relaxed);
    if (delay_ms > 0) {
        usleep((useconds_t)delay_ms * 1000);
    }
    uint32_t samples;
    switch (c->sample_rate) {
    case 8000:  samples = 480;  break;
    case 12000: samples = 720;  break;
    case 16000: samples = 960;  break;
    case 24000: samples = 1440; break;
    case 48000: samples = 2880; break;
    default:    samples = 960;  break;
    }
    uint32_t need = samples * 2;
    if (out->len < need) {
        out->needed_size = need;
        out->decoded_size = 0;
        return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;
    }
    memset(out->buffer, 0x55, need);
    out->decoded_size = need;
    in->consumed = in->len;
    if (info != NULL) {
        info->sample_rate = c->sample_rate;
        info->bits_per_sample = 16;
        info->channel = 1;
        info->frame_size = (int)samples;
    }
    return ESP_AUDIO_ERR_OK;
}

void esp_opus_dec_close(void *dec_handle)
{
    atomic_fetch_add_explicit(&g_dec_close_c, 1, memory_order_relaxed);
    free(dec_handle);
}

/* ── 测试辅助 ── */
void stub_enc_set_fail_next(int fail) { atomic_store_explicit(&g_enc_fail_next, fail, memory_order_relaxed); }
int stub_enc_open_count(void) { return atomic_load_explicit(&g_enc_open_c, memory_order_relaxed); }
int stub_enc_close_count(void) { return atomic_load_explicit(&g_enc_close_c, memory_order_relaxed); }
void stub_dec_set_fail_next(int fail) { atomic_store_explicit(&g_dec_fail_next, fail, memory_order_relaxed); }
void stub_dec_set_delay_ms(int ms) { atomic_store_explicit(&g_dec_delay_ms, ms, memory_order_relaxed); }
int stub_dec_open_count(void) { return atomic_load_explicit(&g_dec_open_c, memory_order_relaxed); }
int stub_dec_close_count(void) { return atomic_load_explicit(&g_dec_close_c, memory_order_relaxed); }
