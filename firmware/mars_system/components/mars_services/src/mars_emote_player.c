#include "mars_emote_player.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "box3_gfx.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gfx.h"
#include "mars_services.h"

#define EMOTE_WIDTH 320
#define EMOTE_HEIGHT 240
#define EMOTE_BG 0x0000
#define LEGACY_EMOTE_BG 0x1203

static const char *TAG = "mars_emote";

static gfx_handle_t s_gfx;
static gfx_disp_t *s_display;
static gfx_obj_t *s_animation;
static uint16_t *s_canvas;
static SemaphoreHandle_t s_canvas_lock;
static atomic_bool s_active;
static atomic_bool s_frame_ready;
static uint32_t s_revision;
static uint32_t s_presented_revision;
static void release_renderer(void)
{
    if (s_gfx != NULL) gfx_emote_deinit(s_gfx);
    s_gfx = NULL; s_display = NULL; s_animation = NULL;
    free(s_canvas); s_canvas = NULL;
    if (s_canvas_lock != NULL) vSemaphoreDelete(s_canvas_lock);
    s_canvas_lock = NULL;
    s_active = false; s_frame_ready = false;
}

static const char *resource_for_emote(const char *name)
{
    /* Apps can choose files using their own manifests; legacy names remain
     * compatible with previously installed packages. */
    if (name && strlen(name) < 32 && strstr(name, ".eaf") && !strchr(name, '/')) return name;
    if (name == NULL || name[0] == '\0' || strcmp(name, "idle") == 0 ||
        strcmp(name, "neutral") == 0) return "neutral.eaf";
    if (strcmp(name, "listening") == 0 || strcmp(name, "listen") == 0) return "listen.eaf";
    if (strcmp(name, "happy") == 0 || strcmp(name, "laughing") == 0 ||
        strcmp(name, "funny") == 0 || strcmp(name, "loving") == 0 ||
        strcmp(name, "confident") == 0 || strcmp(name, "delicious") == 0 ||
        strcmp(name, "embarrassed") == 0 || strcmp(name, "kissy") == 0) return "Happy.eaf";
    if (strcmp(name, "sad") == 0) return "Sad.eaf";
    if (strcmp(name, "crying") == 0 || strcmp(name, "cry") == 0) return "cry.eaf";
    if (strcmp(name, "angry") == 0) return "angry.eaf";
    if (strcmp(name, "surprised") == 0 || strcmp(name, "shocked") == 0) return "shocked.eaf";
    if (strcmp(name, "thinking") == 0 || strcmp(name, "confused") == 0) return "confused.eaf";
    if (strcmp(name, "sleepy") == 0 || strcmp(name, "relaxed") == 0) return "sleep.eaf";
    if (strcmp(name, "winking") == 0 || strcmp(name, "silly") == 0) return "winking.eaf";
    return "neutral.eaf";
}

static void emote_flush(gfx_disp_t *display, int x1, int y1, int x2, int y2,
                        const void *data)
{
    if (data != NULL && s_canvas != NULL && s_canvas_lock != NULL) {
        const int source_width = x2 - x1;
        const int left = x1 < 0 ? 0 : x1;
        const int top = y1 < 0 ? 0 : y1;
        const int right = x2 > EMOTE_WIDTH ? EMOTE_WIDTH : x2;
        const int bottom = y2 > EMOTE_HEIGHT ? EMOTE_HEIGHT : y2;
        if (source_width > 0 && left < right && top < bottom &&
            xSemaphoreTake(s_canvas_lock, portMAX_DELAY) == pdTRUE) {
            const uint16_t *source = (const uint16_t *)data;
            for (int y = top; y < bottom; ++y) {
                const size_t source_offset = (size_t)(y - y1) * (size_t)source_width +
                                             (size_t)(left - x1);
                uint16_t *target = &s_canvas[y * EMOTE_WIDTH + left];
                for (int x = 0; x < right - left; ++x) {
                    const uint16_t pixel = source[source_offset + (size_t)x];
                    target[x] = pixel == LEGACY_EMOTE_BG ? EMOTE_BG : pixel;
                }
            }
            if (gfx_disp_is_flushing_last(display)) {
                s_frame_ready = s_active;
                ++s_revision;
            }
            xSemaphoreGive(s_canvas_lock);
        }
    }
    gfx_disp_flush_ready(display, true);
}

static int ensure_initialized(void)
{
    if (s_gfx != NULL) return 0;

    s_canvas_lock = xSemaphoreCreateMutex();
    s_canvas = heap_caps_malloc(EMOTE_WIDTH * EMOTE_HEIGHT * sizeof(uint16_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_canvas_lock == NULL || s_canvas == NULL) { release_renderer(); return -12; }
    memset(s_canvas, 0, EMOTE_WIDTH * EMOTE_HEIGHT * sizeof(uint16_t));

    gfx_core_config_t core = {
        .fps = 20,
        .task = {
            .task_priority = 4,
            .task_stack = 8 * 1024,
            .task_affinity = -1,
            .task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        },
    };
    s_gfx = gfx_emote_init(&core);
    if (s_gfx == NULL) { release_renderer(); return -12; }

    gfx_disp_config_t display = {
        .h_res = EMOTE_WIDTH,
        .v_res = EMOTE_HEIGHT,
        .flush_cb = emote_flush,
        .update_cb = NULL,
        .user_data = NULL,
        .flags = {
            .swap = false,
            .buff_dma = false,
            .buff_spiram = true,
            .double_buffer = true,
            .full_frame = false,
        },
        .buffers = {
            .buf1 = NULL,
            .buf2 = NULL,
            .buf_pixels = EMOTE_WIDTH * 16,
        },
    };
    s_display = gfx_disp_add(s_gfx, &display);
    if (s_display == NULL) { release_renderer(); return -12; }
    gfx_disp_set_bg_color(s_display, GFX_COLOR_HEX(0x000000));

    s_animation = gfx_anim_create(s_display);
    if (s_animation == NULL) { release_renderer(); return -12; }
    ESP_LOGI(TAG, "EAF renderer ready (320x240 @20fps, PSRAM canvas)");
    return 0;
}

int mars_emote_player_start(const char *emote_name)
{
    const void *data = NULL;
    size_t size = 0;
    const char *resource = resource_for_emote(emote_name);
    extern mars_app_resource_provider_t mars_services_get_app_resource_provider(void);
    mars_app_resource_provider_t provider = mars_services_get_app_resource_provider();
    if (provider == NULL || provider(resource, &data, &size) != 0 || data == NULL || size == 0) {
        ESP_LOGE(TAG, "app resource unavailable: %s", resource);
        return -2;
    }
    int rc = ensure_initialized();
    if (rc != 0) return rc;

    if (xSemaphoreTake(s_canvas_lock, portMAX_DELAY) == pdTRUE) {
        memset(s_canvas, 0, EMOTE_WIDTH * EMOTE_HEIGHT * sizeof(uint16_t));
        s_frame_ready = false;
        ++s_revision;
        xSemaphoreGive(s_canvas_lock);
    }

    gfx_anim_src_t source = {
        .type = GFX_ANIM_SRC_TYPE_MEMORY,
        .data = data,
        .data_len = size,
    };
    gfx_emote_lock(s_gfx);
    gfx_anim_stop(s_animation);
    esp_err_t err = gfx_anim_set_src_desc(s_animation, &source);
    if (err == ESP_OK) err = gfx_obj_align(s_animation, GFX_ALIGN_LEFT_MID, 10, 30);
    if (err == ESP_OK) err = gfx_anim_set_auto_mirror(s_animation, true);
    if (err == ESP_OK) err = gfx_anim_set_segment(s_animation, 0, UINT32_MAX, 20, true);
    s_active = err == ESP_OK;
    if (err == ESP_OK) err = gfx_anim_start(s_animation);
    if (err != ESP_OK) s_active = false;
    gfx_emote_unlock(s_gfx);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start %s failed: %s", resource, esp_err_to_name(err));
        return -(int)err;
    }
    ESP_LOGI(TAG, "playing %s from dynamic app package (%u bytes)", resource,
             (unsigned)size);
    ESP_LOGI(TAG, "MEM[face-ready] internal=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

int mars_emote_player_render(void)
{
    if (!s_active || !s_frame_ready || s_canvas == NULL || s_canvas_lock == NULL) return 0;
    if (xSemaphoreTake(s_canvas_lock, pdMS_TO_TICKS(20)) != pdTRUE) return -16;
    gfx_bitmap(0, 0, EMOTE_WIDTH, EMOTE_HEIGHT, s_canvas);
    s_presented_revision = s_revision;
    xSemaphoreGive(s_canvas_lock);
    return 1;
}

int mars_emote_player_stop(void)
{
    s_active = false;
    if (s_gfx != NULL && s_animation != NULL) {
        gfx_emote_lock(s_gfx);
        gfx_anim_stop(s_animation);
        gfx_emote_unlock(s_gfx);
    }
    if (s_canvas_lock != NULL && xSemaphoreTake(s_canvas_lock, portMAX_DELAY) == pdTRUE) {
        s_frame_ready = false;
        s_revision = 0;
        s_presented_revision = 0;
        xSemaphoreGive(s_canvas_lock);
    }
    /* Releases animation decoder and its app mmap pointer before unload. */
    release_renderer();
    return 0;
}
