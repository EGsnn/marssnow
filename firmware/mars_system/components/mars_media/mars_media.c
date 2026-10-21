/* Shared media ownership, recording lifecycle and PCM API. Board audio is selected at build time. */
#include "mars_media.h"

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mars_audio_hw.h"
#include "mars_event.h"
#include "output_owner.h"
#include "freertos/semphr.h"

#define RECORD_FRAME_SAMPLES 1280   /* 80ms @16kHz */
#define RECORD_TASK_STACK    4096
#define RECORD_TASK_PRIO     6

static const char *TAG = "mars_media";

/* ---- 静态状态 ---- */
static int s_input_rate = 16000;
static int s_output_rate = 16000;
static int s_volume = 60;
static bool s_ready;
static atomic_bool s_out_en;
static atomic_bool s_in_en;
static TaskHandle_t s_record_task;
static mars_media_record_cb_t s_record_cb;
static void *s_record_ctx;
static atomic_bool s_record_stop_req;
static SemaphoreHandle_t s_record_mutex;
static SemaphoreHandle_t s_record_exited;
/* 输出通路独占所有权（P1 单音频所有者）。
 * 状态判定委托给纯 C 的 output_owner（与 host 测试共用同一份实现），
 * 并发互斥由 s_owner_mutex 保护——claim/check/snapshot/release/restore
 * 全部在同一把锁内完成，杜绝两核同时 check-then-set。 */
static output_owner_t   s_owner;
static SemaphoreHandle_t s_owner_mutex;

static void post_state(mars_media_state_t state)
{
    mars_event_post(MARS_EVENT_MEDIA_STATE_CHANGED, &state, sizeof(state));
}

static esp_err_t update_device_state(void)
{
    return mars_audio_hw_state(atomic_load_explicit(&s_out_en, memory_order_acquire),
                               atomic_load_explicit(&s_in_en, memory_order_acquire), s_volume);
}

static void record_task(void *arg)
{
    (void)arg;
    int16_t *frame = NULL;
    while (!atomic_load_explicit(&s_record_stop_req, memory_order_acquire) &&
           atomic_load_explicit(&s_in_en, memory_order_acquire) && mars_audio_hw_active()) {
        if (frame == NULL) {
            frame = malloc(RECORD_FRAME_SAMPLES * sizeof(int16_t));
            if (frame == NULL) {
                ESP_LOGE(TAG, "record frame alloc failed; retrying");
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
        }
        int result = mars_audio_hw_read(frame, RECORD_FRAME_SAMPLES);
        if (result != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (s_record_cb != NULL) {
            s_record_cb(frame, RECORD_FRAME_SAMPLES, s_record_ctx);
        }
    }
    free(frame);
    xSemaphoreGive(s_record_exited);
    vTaskDelete(NULL);
}

/* ---- 公开接口 ---- */

esp_err_t mars_media_init(const mars_media_cfg_t *cfg)
{
    if (cfg != NULL) {
        if (cfg->input_sample_rate > 0) s_input_rate = cfg->input_sample_rate;
        if (cfg->output_sample_rate > 0) s_output_rate = cfg->output_sample_rate;
    }
    if (s_ready) {
        return ESP_OK;
    }
    /* 输出所有权互斥（与状态机解耦：锁在本层，判定在 output_owner.c） */
    if (s_owner_mutex == NULL) s_owner_mutex = xSemaphoreCreateMutex();
    if (s_record_mutex == NULL) s_record_mutex = xSemaphoreCreateMutex();
    if (s_record_exited == NULL) s_record_exited = xSemaphoreCreateBinary();
    if (s_owner_mutex == NULL || s_record_mutex == NULL || s_record_exited == NULL) {
        if (s_owner_mutex != NULL) { vSemaphoreDelete(s_owner_mutex); s_owner_mutex = NULL; }
        if (s_record_mutex != NULL) { vSemaphoreDelete(s_record_mutex); s_record_mutex = NULL; }
        if (s_record_exited != NULL) { vSemaphoreDelete(s_record_exited); s_record_exited = NULL; }
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_owner_mutex, portMAX_DELAY);
    output_owner_init(&s_owner);
    xSemaphoreGive(s_owner_mutex);
    if (mars_audio_hw_init(s_input_rate, s_output_rate) != ESP_OK) {
        ESP_LOGW(TAG, "Audio hardware init failed; audio unavailable");
        post_state(MARS_MEDIA_STATE_NO_CODEC);
        return ESP_OK; /* 无音频不阻塞系统启动 */
    }
    s_ready = true;
    ESP_LOGI(TAG, "Audio ready (%dHz in / %dHz out)", s_input_rate, s_output_rate);
    post_state(MARS_MEDIA_STATE_READY);
    return ESP_OK;
}

esp_err_t mars_media_deinit(void)
{
    esp_err_t stop_err = mars_media_record_stop();
    if (stop_err != ESP_OK) return stop_err;
    atomic_store_explicit(&s_in_en, false, memory_order_release);
    /* 显式复位所有权状态机：即使上次有 owner 未 release，重 init 后也不会永久 EBUSY。
     * 不删 mutex——它跨 deinit/init 复用，避免悬空句柄。 */
    if (s_owner_mutex != NULL) xSemaphoreTake(s_owner_mutex, portMAX_DELAY);
    atomic_store_explicit(&s_out_en, false, memory_order_release);
    output_owner_init(&s_owner);
    update_device_state();
    if (s_owner_mutex != NULL) xSemaphoreGive(s_owner_mutex);
    mars_audio_hw_deinit();
    s_ready = false;
    return ESP_OK;
}

bool mars_media_is_ready(void) { return s_ready; }

esp_err_t mars_media_enable_output(bool enable)
{
    if (s_owner_mutex == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_owner_mutex, portMAX_DELAY);
    if (output_owner_is_claimed(&s_owner)) {
        xSemaphoreGive(s_owner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_out_en, enable, memory_order_release);
    esp_err_t err = update_device_state();
    if (err != ESP_OK) {
        /* Caller receives the hardware failure; restore disabled state. */
        if (enable) { atomic_store(&s_out_en, false); update_device_state(); }
    }
    xSemaphoreGive(s_owner_mutex);
    if (err != ESP_OK) return err;
    post_state(enable ? MARS_MEDIA_STATE_OUTPUT_ON : MARS_MEDIA_STATE_OUTPUT_OFF);
    return ESP_OK;
}

static esp_err_t set_input_enabled(bool enable)
{
    if (s_owner_mutex == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_owner_mutex, portMAX_DELAY);
    atomic_store_explicit(&s_in_en, enable, memory_order_release);
    esp_err_t err = update_device_state();
    if (err != ESP_OK && enable) { atomic_store(&s_in_en, false); update_device_state(); }
    xSemaphoreGive(s_owner_mutex);
    if (err != ESP_OK) return err;
    post_state(enable ? MARS_MEDIA_STATE_INPUT_ON : MARS_MEDIA_STATE_INPUT_OFF);
    return ESP_OK;
}

esp_err_t mars_media_enable_input(bool enable)
{
    if (s_record_mutex == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_record_mutex, portMAX_DELAY);
    if (s_record_task != NULL) {
        xSemaphoreGive(s_record_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = set_input_enabled(enable);
    xSemaphoreGive(s_record_mutex);
    return err;
}

esp_err_t mars_media_output_claim(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_owner_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_owner_mutex, portMAX_DELAY);
    bool was_enabled = atomic_load_explicit(&s_out_en, memory_order_acquire);
    bool ok = output_owner_claim(&s_owner, was_enabled);
    xSemaphoreGive(s_owner_mutex);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;   /* P1 单所有者：已被占用 */
    }
    ESP_LOGI(TAG, "output claimed (was %s)", was_enabled ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t mars_media_output_release(void)
{
    if (!s_ready) {
        return ESP_OK;
    }
    if (s_owner_mutex == NULL) {
        return ESP_OK;
    }
    xSemaphoreTake(s_owner_mutex, portMAX_DELAY);
    bool need_disable = output_owner_release(&s_owner);
    /* release 与恢复必须在同一临界区，禁止新 owner 插入后被旧 owner 误关。 */
    if (need_disable) {
        atomic_store_explicit(&s_out_en, false, memory_order_release);
        update_device_state();
    }
    xSemaphoreGive(s_owner_mutex);
    if (need_disable) post_state(MARS_MEDIA_STATE_OUTPUT_OFF);
    ESP_LOGI(TAG, "output released (need_disable=%d)", (int)need_disable);
    return ESP_OK;
}

bool mars_media_output_is_enabled(void)
{
    return atomic_load_explicit(&s_out_en, memory_order_acquire);
}

esp_err_t mars_media_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    if (mars_audio_hw_active()) {
        return mars_audio_hw_volume(s_volume);
    }
    return ESP_OK;
}

int mars_media_get_volume(void) { return s_volume; }

esp_err_t mars_media_play(const int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (!s_ready || pcm == NULL || samples == 0 || samples > INT_MAX / sizeof(int16_t) || s_owner_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_owner_mutex, portMAX_DELAY);
    if (!output_owner_is_claimed(&s_owner)) {
        xSemaphoreGive(s_owner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    bool turned_on = false;
    if (!atomic_load_explicit(&s_out_en, memory_order_acquire)) {
        atomic_store_explicit(&s_out_en, true, memory_order_release);
        esp_err_t err = update_device_state();
        if (err != ESP_OK) {
            atomic_store(&s_out_en, false);
            update_device_state();
            xSemaphoreGive(s_owner_mutex);
            return err;
        }
        turned_on = true;
    }
    if (!mars_audio_hw_active()) {
        xSemaphoreGive(s_owner_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    int result = mars_audio_hw_write(pcm, samples, timeout_ms);
    xSemaphoreGive(s_owner_mutex);
    if (turned_on) post_state(MARS_MEDIA_STATE_OUTPUT_ON);
    return result == ESP_OK ? ESP_OK : ESP_FAIL;
}

int mars_media_read(int16_t *pcm, size_t samples)
{
    if (!s_ready || pcm == NULL || samples == 0 || samples > INT_MAX / sizeof(int16_t) ||
        s_record_mutex == NULL) {
        return -1;
    }
    xSemaphoreTake(s_record_mutex, portMAX_DELAY);
    if (s_record_task != NULL) {
        xSemaphoreGive(s_record_mutex);
        return -1;
    }
    if (!atomic_load_explicit(&s_in_en, memory_order_acquire) &&
        set_input_enabled(true) != ESP_OK) {
        xSemaphoreGive(s_record_mutex);
        return -1;
    }
    if (!mars_audio_hw_active()) {
        xSemaphoreGive(s_record_mutex);
        return -1;
    }
    int result = mars_audio_hw_read(pcm, samples);
    xSemaphoreGive(s_record_mutex);
    return result == ESP_OK ? (int)samples : -1;
}

esp_err_t mars_media_record_start(mars_media_record_cb_t cb, void *ctx)
{
    if (!s_ready || cb == NULL || s_record_mutex == NULL || s_record_exited == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_record_mutex, portMAX_DELAY);
    if (s_record_task != NULL) {
        bool stopping = atomic_load_explicit(&s_record_stop_req, memory_order_acquire);
        xSemaphoreGive(s_record_mutex);
        if (stopping) return ESP_ERR_TIMEOUT;
        return ESP_OK; /* 已启动，幂等 */
    }
    s_record_cb = cb;
    s_record_ctx = ctx;
    if (!atomic_load_explicit(&s_in_en, memory_order_acquire)) {
        esp_err_t enable_err = set_input_enabled(true);
        if (enable_err != ESP_OK) {
            s_record_cb = NULL;
            s_record_ctx = NULL;
            xSemaphoreGive(s_record_mutex);
            return enable_err;
        }
    }
    if (!mars_audio_hw_active()) {
        s_record_cb = NULL;
        s_record_ctx = NULL;
        set_input_enabled(false);
        xSemaphoreGive(s_record_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    while (xSemaphoreTake(s_record_exited, 0) == pdTRUE) {
    }
    atomic_store_explicit(&s_record_stop_req, false, memory_order_release);
    esp_err_t result = xTaskCreate(record_task, "mars_record", RECORD_TASK_STACK, NULL,
                                  RECORD_TASK_PRIO, &s_record_task) == pdPASS
                           ? ESP_OK : ESP_FAIL;
    if (result != ESP_OK) {
        s_record_task = NULL;
        s_record_cb = NULL;
        s_record_ctx = NULL;
        set_input_enabled(false);
    }
    xSemaphoreGive(s_record_mutex);
    return result;
}

esp_err_t mars_media_record_stop(void)
{
    if (s_record_mutex == NULL) return ESP_OK;
    xSemaphoreTake(s_record_mutex, portMAX_DELAY);
    if (s_record_task != NULL) {
        atomic_store_explicit(&s_record_stop_req, true, memory_order_release);
        if (xSemaphoreTake(s_record_exited, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "record task did not stop within 1 s");
            xSemaphoreGive(s_record_mutex);
            return ESP_ERR_TIMEOUT;
        }
        s_record_task = NULL;
    }
    s_record_cb = NULL;
    s_record_ctx = NULL;
    atomic_store_explicit(&s_record_stop_req, false, memory_order_release);
    set_input_enabled(false);
    xSemaphoreGive(s_record_mutex);
    return ESP_OK;
}

bool mars_media_record_is_active(void)
{
    if (s_record_mutex == NULL) return false;
    xSemaphoreTake(s_record_mutex, portMAX_DELAY);
    bool active = s_record_task != NULL;
    xSemaphoreGive(s_record_mutex);
    return active;
}

esp_err_t mars_media_play_tone(uint16_t freq_hz, uint32_t duration_ms, int volume_percent)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz == 0 || duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t total = (size_t)((uint64_t)s_output_rate * duration_ms / 1000);
    int16_t *tone = malloc(total * sizeof(int16_t));
    if (tone == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const float amplitude = (float)volume_percent / 100.0f * 12000.0f;
    for (size_t i = 0; i < total; ++i) {
        tone[i] = (int16_t)(amplitude * sinf(2.0f * (float)M_PI * freq_hz * (float)i /
                                             (float)s_output_rate));
    }
    esp_err_t err = mars_media_output_claim();
    if (err == ESP_OK) {
        err = mars_media_play(tone, total, 0);
        mars_media_output_release();
    }
    free(tone);
    return err;
}
