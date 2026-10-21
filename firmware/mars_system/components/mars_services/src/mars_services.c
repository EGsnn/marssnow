/**
 * @file mars_services.c
 * @brief 火星系统同进程公共服务。动态应用通过稳定 C ABI 调用。
 */

#include "mars_services.h"
#include "mars_runtime.h"
#include "mars_emote_player.h"

/* 真正保留机制：这些符号在 mars_app_loader/src/mars_exports_def.c 的
 * mars_symtab[] 中以 MARS_EXP_ENTRY(sym)（取 &sym）被真实引用，
 * 链接器不会因 --gc-sections 裁剪。__attribute__((used)) 为 GCC 合法；
 * 不再使用工具链不接受的 `retain`（P1-2 消除源码警告）。 */
#define MARS_KEEP __attribute__((used))

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 现有底层接口（从 mars_system 组件引入） */
#include "mars_media.h"
#include "mars_net.h"

static const char *TAG = "mars_services";
void mars_system_delay_ms(uint32_t milliseconds)
{
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    vTaskDelay(ticks ? ticks : 1);
}
static uint32_t s_recorded_frames;
static mars_app_resource_provider_t s_resource_provider;

void mars_services_set_app_resource_provider(mars_app_resource_provider_t provider)
{
    s_resource_provider = provider;
}

mars_app_resource_provider_t mars_services_get_app_resource_provider(void)
{
    return s_resource_provider;
}
int mars_app_resource_get(const char *name, const void **data, size_t *size)
{
    return s_resource_provider ? s_resource_provider(name, data, size) : -2;
}

/* ═══════════════════════════════════════════
 *  服务元信息（P1）
 * ═══════════════════════════════════════════ */

MARS_KEEP
uint32_t mars_services_api_version(void)
{
    return 1;
}

MARS_KEEP
uint64_t mars_services_capabilities(void)
{
    return MARS_CAP_OPUS_ENCODE | MARS_CAP_OPUS_DECODE |
           MARS_CAP_WSS | MARS_CAP_SECURE_CONFIG | MARS_CAP_AUDIO_LEVELS | MARS_CAP_VOICE_EFFECTS |
           MARS_CAP_CLOCK_READ | MARS_CAP_TOUCH_STATE | MARS_CAP_BACK_VISIBILITY | MARS_CAP_FONT_GLYPH;
}

static void record_sink(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)pcm;
    (void)samples;
    (void)ctx;
    ++s_recorded_frames;
}

/* ═══════════════════════════════════════════
 *  音频服务
 * ═══════════════════════════════════════════ */

MARS_KEEP
int mars_audio_start_recording(void)
{
    s_recorded_frames = 0;
    esp_err_t err = mars_media_record_start(record_sink, NULL);
    ESP_LOGI(TAG, "audio_start_recording: %s", esp_err_to_name(err));
    return err == ESP_OK ? 0 : -(int)err;
}

MARS_KEEP
int mars_audio_stop_recording(void)
{
    esp_err_t err = mars_media_record_stop();
    ESP_LOGI(TAG, "audio_stop_recording: %s, frames=%u", esp_err_to_name(err),
             (unsigned)s_recorded_frames);
    return err == ESP_OK ? 0 : -(int)err;
}

MARS_KEEP
int mars_audio_send_packet(const void* data, size_t len)
{
    ESP_LOGD(TAG, "audio_send_packet: %d bytes", (int)len);
    /* TODO: 通过 MQTT 发送到服务器 */
    return 0;
}

MARS_KEEP
int mars_audio_play_packet(const void* data, size_t len)
{
    ESP_LOGD(TAG, "audio_play_packet: %d bytes", (int)len);
    /* TODO: 解码并播放 */
    return 0;
}

MARS_KEEP
int mars_audio_set_volume(int volume)
{
    esp_err_t err = mars_media_set_volume(volume);
    return err == ESP_OK ? 0 : -(int)err;
}


/* ═══════════════════════════════════════════
 *  网络服务
 * ═══════════════════════════════════════════ */

MARS_KEEP
mars_net_state_t mars_network_get_state(void)
{
    return mars_net_state();
}

MARS_KEEP
bool mars_network_has_saved(void)
{
    return mars_net_has_saved();
}

MARS_KEEP
int mars_network_mqtt_publish(const char* topic, const void* payload, size_t len)
{
    ESP_LOGI(TAG, "mqtt_publish: topic=%s, %d bytes", topic, (int)len);
    /* TODO: 调用 MQTT 客户端发布 */
    return 0;
}


/* ═══════════════════════════════════════════
 *  显示服务
 * ═══════════════════════════════════════════ */

MARS_KEEP
int mars_display_draw_layout(const char* layout_json)
{
    ESP_LOGD(TAG, "display_draw_layout: %.80s...", layout_json ? layout_json : "(null)");
    /* TODO: 调用 LVGL 渲染 */
    return 0;
}

MARS_KEEP
int mars_display_play_emote(const char* emote_name)
{
    ESP_LOGI(TAG, "display_play_emote: %s", emote_name ? emote_name : "(null)");
    return mars_emote_player_start(emote_name);
}

MARS_KEEP
int mars_display_stop_emote(void)
{
    ESP_LOGI(TAG, "display_stop_emote");
    return mars_emote_player_stop();
}

MARS_KEEP
int mars_display_render_emote(void)
{
    return mars_emote_player_render();
}


/* ═══════════════════════════════════════════
 *  唤醒服务
 * ═══════════════════════════════════════════ */

static mars_wakeword_callback_t s_wakeword_cb = NULL;

MARS_KEEP
int mars_wakeword_set_callback(mars_wakeword_callback_t cb)
{
    s_wakeword_cb = cb;
    ESP_LOGI(TAG, "wakeword_callback registered");
    return 0;
}

MARS_KEEP
int mars_wakeword_enable(void)
{
    ESP_LOGI(TAG, "wakeword_enable");
    /* TODO: 启用 WakeNet 检测 */
    return 0;
}

MARS_KEEP
int mars_wakeword_disable(void)
{
    ESP_LOGI(TAG, "wakeword_disable");
    return 0;
}


/* ═══════════════════════════════════════════
 *  系统服务
 * ═══════════════════════════════════════════ */

MARS_KEEP
int64_t mars_system_get_uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

MARS_KEEP
int mars_system_restart_to_app(int slot)
{
    ESP_LOGI(TAG, "restart_to_app: slot %d (stub)", slot);
    /* TODO: 设置 boot 标志并重启 */
    return 0;
}

MARS_KEEP
int mars_system_exit_to_desktop(void)
{
    ESP_LOGI(TAG, "exit_to_desktop (stub)");
    /* TODO: 切换回桌面应用 */
    return 0;
}
