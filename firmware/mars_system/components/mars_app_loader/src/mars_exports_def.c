#include "mars_app_preferences.h"
/**
 * @file mars_exports_def.c
 * @brief 动态应用可见系统符号表——单一事实来源
 *
 * 新增/删除导出只需在 mars_symtab[] 中增删一行 MARS_EXP_ENTRY(...)，
 * 重新构建即可；地址由链接器在同一次链接中填充，无需事后重导。
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
/* ctype 常用名在头文件中是宏，但库中存在同名函数符号；取消宏以取函数地址 */
#undef isalpha
#undef isdigit

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "mars_services.h"
#include "mars_runtime.h"
#include "mars_native_ui.h"
#include "box3_gfx.h"
#include "mars_glyph.h"
#include "mars_export.h"
#include "cJSON.h"
/* libgcc helpers used by separately compiled Xtensa applications. */
extern long long __divdi3(long long, long long);
extern long long __moddi3(long long, long long);
extern uint32_t __bswapsi2(uint32_t);
extern float __divsf3(float, float);

const mars_symtab_entry_t mars_symtab[] = {
    MARS_EXP_ENTRY(__divdi3),
    MARS_EXP_ENTRY(__moddi3),
    MARS_EXP_ENTRY(__bswapsi2),
    MARS_EXP_ENTRY(__divsf3),
    /* ── newlib / libc ─────────────────────────────────────── */
    MARS_EXP_ENTRY(malloc),
    MARS_EXP_ENTRY(free),
    MARS_EXP_ENTRY(calloc),
    MARS_EXP_ENTRY(realloc),
    MARS_EXP_ENTRY(memcpy),
    MARS_EXP_ENTRY(memset),
    MARS_EXP_ENTRY(memmove),
    MARS_EXP_ENTRY(memcmp),
    MARS_EXP_ENTRY(memchr),
    MARS_EXP_ENTRY(strlen),
    MARS_EXP_ENTRY(strcmp),
    MARS_EXP_ENTRY(strncmp),
    MARS_EXP_ENTRY(strcpy),
    MARS_EXP_ENTRY(strncpy),
    MARS_EXP_ENTRY(strcat),
    MARS_EXP_ENTRY(strncat),
    MARS_EXP_ENTRY(strchr),
    MARS_EXP_ENTRY(strrchr),
    MARS_EXP_ENTRY(strstr),
    MARS_EXP_ENTRY(strtol),
    MARS_EXP_ENTRY(strtoul),
    MARS_EXP_ENTRY(strtoll),
    MARS_EXP_ENTRY(strtoull),
    MARS_EXP_ENTRY(sprintf),
    MARS_EXP_ENTRY(snprintf),
    MARS_EXP_ENTRY(vsnprintf),
    MARS_EXP_ENTRY(printf),
    MARS_EXP_ENTRY(puts),
    MARS_EXP_ENTRY(abs),
    MARS_EXP_ENTRY(atoi),
    MARS_EXP_ENTRY(atol),
    MARS_EXP_ENTRY(atof),
    MARS_EXP_ENTRY(isalpha),
    MARS_EXP_ENTRY(isdigit),
    MARS_EXP_ENTRY(rand),
    MARS_EXP_ENTRY(srand),
    MARS_EXP_ENTRY(qsort),
    MARS_EXP_ENTRY(bsearch),

    /* ── ESP-IDF ───────────────────────────────────────────── */
    MARS_EXP_ENTRY(esp_log_write),
    MARS_EXP_ENTRY(esp_log_level_set),
    MARS_EXP_ENTRY(vTaskDelay),
    MARS_EXP_ENTRY(xTaskDelayUntil),
    MARS_EXP_ENTRY(xTaskCreate),
    MARS_EXP_ENTRY(xTaskCreatePinnedToCore),
    MARS_EXP_ENTRY(esp_timer_get_time),

    /* ── 火星系统服务（17） ────────────────────────────────── */
    MARS_EXP_ENTRY(mars_services_api_version),
    MARS_EXP_ENTRY(mars_services_capabilities),
    MARS_EXP_ENTRY(mars_time_read),
    MARS_EXP_ENTRY(mars_input_read_touch),
    MARS_EXP_ENTRY(mars_input_set_back_visible),
    MARS_EXP_ENTRY(mars_audio_start_recording),
    MARS_EXP_ENTRY(mars_audio_stop_recording),
    MARS_EXP_ENTRY(mars_audio_send_packet),
    MARS_EXP_ENTRY(mars_audio_play_packet),
    MARS_EXP_ENTRY(mars_audio_set_volume),

    /* ── 火星系统 Opus 流式音频服务（P1） ──────────────────── */
    MARS_EXP_ENTRY(mars_audio_stream_start),
    MARS_EXP_ENTRY(mars_audio_stream_read),
    MARS_EXP_ENTRY(mars_audio_stream_stop),
    MARS_EXP_ENTRY(mars_audio_stream_finish),
    MARS_EXP_ENTRY(mars_audio_play_opus),
    MARS_EXP_ENTRY(mars_audio_play_opus_effect),
    MARS_EXP_ENTRY(mars_app_pref_get_u32),
    MARS_EXP_ENTRY(mars_app_pref_set_u32),
    MARS_EXP_ENTRY(mars_audio_playback_stop),
    MARS_EXP_ENTRY(mars_audio_playback_pending),
    MARS_EXP_ENTRY(mars_audio_stream_stats),
    MARS_EXP_ENTRY(mars_audio_stream_is_running),
    MARS_EXP_ENTRY(mars_audio_levels),

    MARS_EXP_ENTRY(mars_network_get_state),
    MARS_EXP_ENTRY(mars_ws_saved_config),
    MARS_EXP_ENTRY(mars_ws_open_saved),
    MARS_EXP_ENTRY(mars_ws_send_text),
    MARS_EXP_ENTRY(mars_ws_send_binary),
    MARS_EXP_ENTRY(mars_ws_receive),
    MARS_EXP_ENTRY(mars_ws_close),
    MARS_EXP_ENTRY(cJSON_ParseWithLength),
    MARS_EXP_ENTRY(cJSON_GetObjectItemCaseSensitive),
    MARS_EXP_ENTRY(cJSON_IsString),
    MARS_EXP_ENTRY(cJSON_IsNumber),
    MARS_EXP_ENTRY(cJSON_Delete),
    MARS_EXP_ENTRY(cJSON_GetArraySize),
    MARS_EXP_ENTRY(cJSON_GetArrayItem),
    MARS_EXP_ENTRY(mars_app_resource_get),
    MARS_EXP_ENTRY(mars_network_has_saved),
    MARS_EXP_ENTRY(mars_network_mqtt_publish),
    MARS_EXP_ENTRY(mars_display_draw_layout),
    MARS_EXP_ENTRY(mars_display_play_emote),
    MARS_EXP_ENTRY(mars_display_stop_emote),
    MARS_EXP_ENTRY(mars_display_render_emote),
    MARS_EXP_ENTRY(mars_wakeword_set_callback),
    MARS_EXP_ENTRY(mars_wakeword_enable),
    MARS_EXP_ENTRY(mars_wakeword_disable),
    MARS_EXP_ENTRY(mars_system_get_uptime_ms),
    MARS_EXP_ENTRY(mars_system_delay_ms),
    MARS_EXP_ENTRY(mars_system_restart_to_app),
    MARS_EXP_ENTRY(mars_system_exit_to_desktop),

    /* ── gfx 屏幕绘制（5） ─────────────────────────────────── */
    MARS_EXP_ENTRY(gfx_clear),
    MARS_EXP_ENTRY(gfx_present),
    MARS_EXP_ENTRY(mars_native_ui_get),
    MARS_EXP_ENTRY(gfx_rect),
    MARS_EXP_ENTRY(gfx_text),
    MARS_EXP_ENTRY(gfx_text_overlay),
    MARS_EXP_ENTRY(gfx_text_width),
    MARS_EXP_ENTRY(gfx_bitmap_scaled),
    MARS_EXP_ENTRY(mars_glyph_copy),
};

const uint32_t mars_symtab_count = sizeof(mars_symtab) / sizeof(mars_symtab[0]);
