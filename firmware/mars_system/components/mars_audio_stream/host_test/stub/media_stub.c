/* host 桩：mars_media 行为仿真
 *  - record_start 启动一个按可配间隔回调 1280 样本的录音线程（仿真 80ms 帧）
 *  - play 记录播放样本数；enable_output 记录输出开关状态
 */
#include "mars_media.h"
#include "output_owner.h"

#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#define STUB_FRAME_SAMPLES 1280

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_ready = 1;
static int g_out_enabled = 0;
static int g_play_fail_next = 0;
static unsigned long g_play_calls = 0;
static unsigned long g_play_samples = 0;
static int g_first_samples[4];
/* 输出所有权：与 production mars_media 共用同一份 output_owner 状态机，
 * 仅把互斥原语换成 pthread mutex（生产是 FreeRTOS mutex），语义等价。 */
static output_owner_t g_owner;
static int g_foreign_held = 0;   /* 仿真"其他所有者已 claim" */

static mars_media_record_cb_t g_cb = NULL;
static void *g_cb_ctx = NULL;
static atomic_int g_record_run;
static atomic_int g_interval_ms = 80;
static atomic_ulong g_record_calls;
static pthread_t g_record_tid;
static int16_t g_frame[STUB_FRAME_SAMPLES];

static void *record_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < STUB_FRAME_SAMPLES; ++i) {
        g_frame[i] = (int16_t)(i & 0xFF);   /* 有内容但恒定的 PCM */
    }
    while (atomic_load_explicit(&g_record_run, memory_order_acquire)) {
        usleep((useconds_t)atomic_load_explicit(&g_interval_ms, memory_order_relaxed) * 1000);
        if (!atomic_load_explicit(&g_record_run, memory_order_acquire)) {
            break;
        }
        mars_media_record_cb_t cb = g_cb;
        if (cb != NULL) {
            cb(g_frame, STUB_FRAME_SAMPLES, g_cb_ctx);
            atomic_fetch_add_explicit(&g_record_calls, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

esp_err_t mars_media_init(const void *cfg) { (void)cfg; return ESP_OK; }
esp_err_t mars_media_deinit(void) { return ESP_OK; }
bool mars_media_is_ready(void) { return g_ready != 0; }
esp_err_t mars_media_set_volume(int percent) { (void)percent; return ESP_OK; }
esp_err_t mars_media_play_tone(uint16_t f, uint32_t d, int v)
{
    (void)f; (void)d; (void)v;
    esp_err_t rc = mars_media_output_claim();
    if (rc == ESP_OK) mars_media_output_release();
    return rc;
}

esp_err_t mars_media_enable_output(bool enable)
{
    pthread_mutex_lock(&g_mu);
    if (output_owner_is_claimed(&g_owner)) {
        pthread_mutex_unlock(&g_mu);
        return ESP_ERR_INVALID_STATE;
    }
    g_out_enabled = enable ? 1 : 0;
    pthread_mutex_unlock(&g_mu);
    return ESP_OK;
}

esp_err_t mars_media_enable_input(bool enable)
{
    (void)enable;
    return ESP_OK;
}

esp_err_t mars_media_output_claim(void)
{
    pthread_mutex_lock(&g_mu);
    if (g_foreign_held || output_owner_is_claimed(&g_owner)) {
        pthread_mutex_unlock(&g_mu);
        return ESP_ERR_INVALID_STATE;   /* 已被占用（单所有者） */
    }
    bool ok = output_owner_claim(&g_owner, g_out_enabled != 0);
    pthread_mutex_unlock(&g_mu);
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t mars_media_output_release(void)
{
    pthread_mutex_lock(&g_mu);
    bool need_disable = output_owner_release(&g_owner);
    /* 与 production 一致：只关 claim 前是关的那条 */
    if (need_disable) {
        g_out_enabled = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return ESP_OK;
}

bool mars_media_output_is_enabled(void)
{
    int v;
    pthread_mutex_lock(&g_mu);
    v = g_out_enabled;
    pthread_mutex_unlock(&g_mu);
    return v != 0;
}

esp_err_t mars_media_play(const int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    (void)timeout_ms;
    pthread_mutex_lock(&g_mu);
    if (!output_owner_is_claimed(&g_owner)) {
        pthread_mutex_unlock(&g_mu);
        return ESP_ERR_INVALID_STATE;
    }
    g_out_enabled = 1;   /* 与真实 mars_media 一致：先按需打开输出通路 */
    if (g_play_fail_next > 0) {
        g_play_fail_next--;
        pthread_mutex_unlock(&g_mu);
        return ESP_FAIL;   /* 通路已开但写失败（用于验证 release 仍能恢复原状态） */
    }
    if(g_play_calls<4)g_first_samples[g_play_calls]=samples?pcm[0]:0;
    g_play_calls++;
    g_play_samples += samples;
    pthread_mutex_unlock(&g_mu);
    return ESP_OK;
}

esp_err_t mars_media_record_start(mars_media_record_cb_t cb, void *ctx)
{
    pthread_mutex_lock(&g_mu);
    if (atomic_load_explicit(&g_record_run, memory_order_acquire)) {
        pthread_mutex_unlock(&g_mu);
        return ESP_FAIL;
    }
    g_cb = cb;
    g_cb_ctx = ctx;
    atomic_store_explicit(&g_record_run, 1, memory_order_release);
    pthread_mutex_unlock(&g_mu);
    if (pthread_create(&g_record_tid, NULL, record_thread, NULL) != 0) {
        pthread_mutex_lock(&g_mu);
        atomic_store_explicit(&g_record_run, 0, memory_order_release);
        pthread_mutex_unlock(&g_mu);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mars_media_record_stop(void)
{
    pthread_mutex_lock(&g_mu);
    atomic_store_explicit(&g_record_run, 0, memory_order_release);
    pthread_mutex_unlock(&g_mu);
    pthread_join(g_record_tid, NULL);
    pthread_mutex_lock(&g_mu);
    g_cb = NULL;
    pthread_mutex_unlock(&g_mu);
    return ESP_OK;
}

bool mars_media_record_is_active(void)
{
    int run;
    pthread_mutex_lock(&g_mu);
    run = atomic_load_explicit(&g_record_run, memory_order_acquire);
    pthread_mutex_unlock(&g_mu);
    return run != 0;
}

/* ── 测试辅助 ── */
void stub_media_set_record_interval_ms(int ms)
{
    atomic_store_explicit(&g_interval_ms, ms, memory_order_relaxed);
}
void stub_media_set_play_fail_next(int fail)
{
    pthread_mutex_lock(&g_mu);
    g_play_fail_next = fail;
    pthread_mutex_unlock(&g_mu);
}
unsigned long stub_media_play_calls(void)
{
    unsigned long v;
    pthread_mutex_lock(&g_mu);
    v = g_play_calls;
    pthread_mutex_unlock(&g_mu);
    return v;
}
unsigned long stub_media_play_total_samples(void)
{
    unsigned long v;
    pthread_mutex_lock(&g_mu);
    v = g_play_samples;
    pthread_mutex_unlock(&g_mu);
    return v;
}
int stub_media_output_enabled(void)
{
    int v;
    pthread_mutex_lock(&g_mu);
    v = g_out_enabled;
    pthread_mutex_unlock(&g_mu);
    return v;
}
unsigned long stub_media_record_calls(void)
{
    return atomic_load_explicit(&g_record_calls, memory_order_relaxed);
}
void stub_media_foreign_claim(int held)
{
    pthread_mutex_lock(&g_mu);
    g_foreign_held = held ? 1 : 0;
    pthread_mutex_unlock(&g_mu);
}
void stub_media_set_output_enabled(int on)
{
    pthread_mutex_lock(&g_mu);
    g_out_enabled = on ? 1 : 0;
    pthread_mutex_unlock(&g_mu);
}
void stub_media_reset(void)
{
    pthread_mutex_lock(&g_mu);
    atomic_store_explicit(&g_record_calls, 0, memory_order_relaxed);
    g_play_fail_next = 0;
    g_play_calls = 0;
    g_play_samples = 0;
    g_out_enabled = 0;
    output_owner_init(&g_owner);
    g_foreign_held = 0;
    pthread_mutex_unlock(&g_mu);
}

int stub_media_first_sample(unsigned call)
{
    pthread_mutex_lock(&g_mu);
    int result=call<4 && call<g_play_calls?g_first_samples[call]:-40000;
    pthread_mutex_unlock(&g_mu);return result;
}
