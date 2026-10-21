#include "box3_gfx.h"
#include "font_cn.h"
#include "mars_glyph.h"
#include "gfx_blend.h"

static int cn_scale_of(int scale) { return scale > 1 ? (scale + 1) / 2 : 1; }

#include <stddef.h>
#include <string.h>
#include <stdatomic.h>

#include "box3_board.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

/* The official firmware renders through a 20-row DMA strip and enables
 * swap_bytes. Keep our canvas in native RGB565 and perform that same byte swap
 * while copying each strip to DMA-capable memory. */
/* 显存放 PSRAM（153.6KB）：把内部 SRAM 让给 Wi-Fi/BLE/LCD DMA，三方共存的关键 */
EXT_RAM_BSS_ATTR static uint16_t s_framebuffer[BOX3_LCD_WIDTH * BOX3_LCD_HEIGHT];
EXT_RAM_BSS_ATTR static uint16_t s_background[BOX3_LCD_WIDTH * BOX3_LCD_HEIGHT];
DMA_ATTR static uint16_t s_dma_strip[BOX3_LCD_WIDTH * 20];
static const char *TAG = "box_gfx";

enum { FIRST_FRAME_READY = 1, FIRST_FRAME_RELEASED = 2, FIRST_FRAME_CANCELED = 4 };
static EventGroupHandle_t s_first_frame_events;
static TaskHandle_t s_transition_owner;
static atomic_bool s_hold_first_frame;
static _Atomic(TaskHandle_t) s_deferred_owner;
void gfx_defer_owner_present(bool defer)
{ atomic_store(&s_deferred_owner, defer ? xTaskGetCurrentTaskHandle() : NULL); }
void gfx_copy_frame(uint16_t *destination,int stride)
{
    if (!destination || stride < BOX3_LCD_WIDTH) return;
    for (int y=0;y<BOX3_LCD_HEIGHT;++y)
        memcpy(destination+y*stride,s_framebuffer+y*BOX3_LCD_WIDTH,BOX3_LCD_WIDTH*sizeof(uint16_t));
}

bool gfx_hold_first_frame(void)
{
    if (s_first_frame_events == NULL) s_first_frame_events = xEventGroupCreate();
    if (s_first_frame_events == NULL) return false;
    xEventGroupClearBits(s_first_frame_events, FIRST_FRAME_READY | FIRST_FRAME_RELEASED | FIRST_FRAME_CANCELED);
    s_transition_owner = xTaskGetCurrentTaskHandle();
    atomic_store_explicit(&s_hold_first_frame, true, memory_order_release);
    return true;
}

bool gfx_wait_first_frame(uint32_t timeout_ms)
{
    return (xEventGroupWaitBits(s_first_frame_events, FIRST_FRAME_READY,
            pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & FIRST_FRAME_READY) != 0;
}

void gfx_release_first_frame(bool frame_presented)
{
    atomic_store_explicit(&s_hold_first_frame, false, memory_order_release);
    if (s_first_frame_events != NULL) xEventGroupSetBits(s_first_frame_events,
            FIRST_FRAME_RELEASED | (frame_presented ? 0 : FIRST_FRAME_CANCELED));
    ESP_LOGI(TAG, "dynamic display released (transition complete=%d)", frame_presented);
}

static bool first_frame_held(void)
{
    if (!atomic_load_explicit(&s_hold_first_frame, memory_order_acquire) ||
            xTaskGetCurrentTaskHandle() == s_transition_owner) return false;
    /* 此刻 App 的首帧已画完整。阻塞它的绘制任务，禁止下一帧改写动画源。 */
    xEventGroupSetBits(s_first_frame_events, FIRST_FRAME_READY);
    ESP_LOGI(TAG, "dynamic first frame ready; display held for transition");
    EventBits_t bits = xEventGroupWaitBits(s_first_frame_events, FIRST_FRAME_RELEASED,
            pdFALSE, pdTRUE, portMAX_DELAY);
    return (bits & FIRST_FRAME_CANCELED) == 0;
}

const uint16_t *gfx_frame_pixels(void){return s_framebuffer;}
bool gfx_first_frame_barrier(void){return first_frame_held();}

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} gfx_clip_t;

static gfx_clip_t s_clip = {0, 0, BOX3_LCD_WIDTH, BOX3_LCD_HEIGHT};
static gfx_clip_t s_clip_stack[4];
static int s_clip_depth;


void gfx_clear(uint16_t color)
{
    for (size_t i = 0; i < BOX3_LCD_WIDTH * BOX3_LCD_HEIGHT; ++i) s_framebuffer[i] = color;
}

void gfx_clip_reset(void)
{
    s_clip = (gfx_clip_t){0, 0, BOX3_LCD_WIDTH, BOX3_LCD_HEIGHT};
    s_clip_depth = 0;
}

void gfx_clip_push(int x, int y, int width, int height)
{
    if (s_clip_depth < (int)(sizeof(s_clip_stack) / sizeof(s_clip_stack[0]))) {
        s_clip_stack[s_clip_depth++] = s_clip;
    }
    const int right = x + width;
    const int bottom = y + height;
    if (x > s_clip.left) s_clip.left = x;
    if (y > s_clip.top) s_clip.top = y;
    if (right < s_clip.right) s_clip.right = right;
    if (bottom < s_clip.bottom) s_clip.bottom = bottom;
    if (s_clip.right < s_clip.left) s_clip.right = s_clip.left;
    if (s_clip.bottom < s_clip.top) s_clip.bottom = s_clip.top;
}

void gfx_clip_pop(void)
{
    if (s_clip_depth > 0) s_clip = s_clip_stack[--s_clip_depth];
}

void gfx_rect(int x, int y, int width, int height, uint16_t color)
{
    if (width <= 0 || height <= 0) return;
    int left = x < 0 ? 0 : x;
    int top = y < 0 ? 0 : y;
    int right = x + width > BOX3_LCD_WIDTH ? BOX3_LCD_WIDTH : x + width;
    int bottom = y + height > BOX3_LCD_HEIGHT ? BOX3_LCD_HEIGHT : y + height;
    if (left < s_clip.left) left = s_clip.left;
    if (top < s_clip.top) top = s_clip.top;
    if (right > s_clip.right) right = s_clip.right;
    if (bottom > s_clip.bottom) bottom = s_clip.bottom;
    for (int py = top; py < bottom; ++py) {
        uint16_t *row = &s_framebuffer[py * BOX3_LCD_WIDTH + left];
        for (int px = left; px < right; ++px) row[px - left] = color;
    }
}

void gfx_round_rect(int x, int y, int width, int height, int radius, uint16_t color)
{
    if (width <= 0 || height <= 0) return;
    if (radius <= 0) { gfx_rect(x, y, width, height, color); return; }
    const int max_radius = (width < height ? width : height) / 2;
    if (radius > max_radius) radius = max_radius;
    const int rr = radius * radius;
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            int dx = 0;
            int dy = 0;
            if (px < radius) dx = radius - 1 - px;
            else if (px >= width - radius) dx = px - (width - radius);
            if (py < radius) dy = radius - 1 - py;
            else if (py >= height - radius) dy = py - (height - radius);
            if (dx * dx + dy * dy <= rr) gfx_rect(x + px, y + py, 1, 1, color);
        }
    }
}

void gfx_bitmap(int x, int y, int width, int height, const uint16_t *pixels)
{
    if (!pixels || width <= 0 || height <= 0) return;
    int left = x < 0 ? 0 : x;
    int top = y < 0 ? 0 : y;
    int right = x + width > BOX3_LCD_WIDTH ? BOX3_LCD_WIDTH : x + width;
    int bottom = y + height > BOX3_LCD_HEIGHT ? BOX3_LCD_HEIGHT : y + height;
    if (left < s_clip.left) left = s_clip.left;
    if (top < s_clip.top) top = s_clip.top;
    if (right > s_clip.right) right = s_clip.right;
    if (bottom > s_clip.bottom) bottom = s_clip.bottom;
    for (int py = top; py < bottom; ++py) {
        const uint16_t *src = pixels + (py - y) * width + (left - x);
        memcpy(&s_framebuffer[py * BOX3_LCD_WIDTH + left], src, (size_t)(right - left) * sizeof(uint16_t));
    }
}

void gfx_bitmap_scaled(int x, int y, int width, int height, int scale, const uint16_t *pixels)
{
    if (!pixels || width <= 0 || height <= 0 || width > BOX3_LCD_WIDTH ||
        height > BOX3_LCD_HEIGHT || scale < 1 || scale > 4) return;
    /* Reject remote coordinates before arithmetic; avoid pointer formation
     * for empty/off-screen rectangles and honour the current UI clip. */
    int w = width * scale, h = height * scale;
    if (x >= BOX3_LCD_WIDTH || y >= BOX3_LCD_HEIGHT || x <= -w || y <= -h) return;
    int left = x > s_clip.left ? x : s_clip.left;
    int top = y > s_clip.top ? y : s_clip.top;
    int right = x + w < s_clip.right ? x + w : s_clip.right;
    int bottom = y + h < s_clip.bottom ? y + h : s_clip.bottom;
    if (left >= right || top >= bottom) return;
    for (int py = top; py < bottom; ++py) {
        const uint16_t *src = pixels + ((py - y) / scale) * width;
        uint16_t *dst = s_framebuffer + py * BOX3_LCD_WIDTH;
        for (int px = left; px < right; ++px) dst[px] = src[(px - x) / scale];
    }
}

void gfx_frame(int x, int y, int width, int height, int thickness, uint16_t color)
{
    gfx_rect(x, y, width, thickness, color);
    gfx_rect(x, y + height - thickness, width, thickness, color);
    gfx_rect(x, y, thickness, height, color);
    gfx_rect(x + width - thickness, y, thickness, height, color);
}

void gfx_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    /* 标准 Bresenham：err = dx - dy 的对称形式，正确处理垂直/水平线 */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    if (thickness < 1) thickness = 1;
    while (true) {
        gfx_rect(x0 - thickness / 2, y0 - thickness / 2, thickness, thickness, color);
        if (x0 == x1 && y0 == y1) break;
        const int twice = 2 * err;
        if (twice > -dy) { err -= dy; x0 += sx; }
        if (twice < dx)  { err += dx; y0 += sy; }
    }
}

static void render_ascii_char(int x, int y, const uint8_t *rows, int scale, uint16_t foreground, uint16_t background, bool overlay)
{
    const int char_width = 6 * scale;
    const int char_height = 7 * scale;
    for (int py = 0; py < char_height; ++py) {
        if (y + py < s_clip.top || y + py >= s_clip.bottom) continue;
        const uint8_t row = rows[py / scale];
        for (int px = 0; px < char_width; ++px) {
            if (x + px < s_clip.left || x + px >= s_clip.right) continue;
            const bool on = px < 5 * scale && (row & (1u << (4 - px / scale)));
            if (overlay) { if (on) s_framebuffer[(y + py) * BOX3_LCD_WIDTH + x + px] = foreground; }
            else s_framebuffer[(y + py) * BOX3_LCD_WIDTH + x + px] = on ? foreground : background;
        }
    }
}

static void render_cn_char(int x, int y, const uint8_t *rows, int scale, uint16_t foreground, uint16_t background, bool overlay)
{
    const int cs = cn_scale_of(scale);
    const int char_width = 16 * cs;
    const int char_height = 16 * cs;
    for (int py = 0; py < char_height; ++py) {
        if (y + py < s_clip.top || y + py >= s_clip.bottom) continue;
        const uint16_t line = ((uint16_t)rows[(py / cs) * 2] << 8) | rows[(py / cs) * 2 + 1];
        for (int px = 0; px < char_width; ++px) {
            if (x + px < s_clip.left || x + px >= s_clip.right) continue;
            const bool on = (line & (0x8000u >> (px / cs))) != 0;
            if (overlay) { if (on) s_framebuffer[(y + py) * BOX3_LCD_WIDTH + x + px] = foreground; }
            else s_framebuffer[(y + py) * BOX3_LCD_WIDTH + x + px] = on ? foreground : background;
        }
    }
}

int gfx_text_width(const char *text, int scale)
{
    if (!text) return 0;
    int width = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p >= 0xE0 && p[1] && p[2]) { width += 16 * cn_scale_of(scale); p += 3; }
        else if (*p < 0x80) { width += 6 * scale; p++; }
        else p++;
    }
    return width;
}

int gfx_text_height(const char *text, int scale)
{
    if (!text) return 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p >= 0xE0 && p[1] && p[2]) return 16 * cn_scale_of(scale);
        if (*p < 0x80) p++; else p++;
    }
    return 7 * scale;
}

void gfx_text(int x, int y, const char *text, int scale, uint16_t foreground, uint16_t background)
{
    if (!text || scale < 1 || scale > 4) return;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p >= 0xE0 && p[1] && p[2]) {
            const uint32_t cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | ((uint32_t)(p[2] & 0x3F));
            const uint8_t *rows = font_cn_glyph(cp);
            if (rows) render_cn_char(x, y, rows, scale, foreground, background, false);
            x += 16 * cn_scale_of(scale);
            p += 3;
        } else if (*p < 0x80) {
            render_ascii_char(x, y, mars_ascii_glyph((char)*p), scale, foreground, background, false);
            x += 6 * scale;
            p++;
        } else {
            p++;
        }
    }
}

void gfx_text_overlay(int x, int y, const char *text, int scale, uint16_t foreground)
{
    if (!text || scale < 1 || scale > 4) return;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p >= 0xE0 && p[1] && p[2]) {
            const uint32_t cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | ((uint32_t)(p[2] & 0x3F));
            const uint8_t *rows = font_cn_glyph(cp);
            if (rows) render_cn_char(x, y, rows, scale, foreground, 0, true);
            x += 16 * cn_scale_of(scale);
            p += 3;
        } else if (*p < 0x80) {
            render_ascii_char(x, y, mars_ascii_glyph((char)*p), scale, foreground, 0, true);
            x += 6 * scale;
            p++;
        } else {
            p++;
        }
    }
}

void gfx_present(void)
{
    if (atomic_load(&s_deferred_owner) == xTaskGetCurrentTaskHandle()) return;
    if (first_frame_held()) return; /* 启动器已呈现首帧 */
    for (int y = 0; y < BOX3_LCD_HEIGHT; y += 20) {
        const int rows = (BOX3_LCD_HEIGHT - y < 20) ? BOX3_LCD_HEIGHT - y : 20;
        const int count = BOX3_LCD_WIDTH * rows;
        const uint16_t *source = &s_framebuffer[y * BOX3_LCD_WIDTH];
        for (int i = 0; i < count; ++i) {
            const uint16_t value = source[i];
            s_dma_strip[i] = (uint16_t)((value << 8) | (value >> 8));
        }
        esp_err_t result = box3_display_blit(0, y, BOX3_LCD_WIDTH, rows, s_dma_strip);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Frame strip at y=%d failed: %s", y, esp_err_to_name(result));
            ESP_ERROR_CHECK(result);
        }
    }
}

void gfx_present_window(int x, int width)
{
    if (atomic_load(&s_deferred_owner) == xTaskGetCurrentTaskHandle()) return;
    if (first_frame_held()) return;
    if (x < 0) x = 0;
    if (x >= BOX3_LCD_WIDTH) return;
    if (x + width > BOX3_LCD_WIDTH) width = BOX3_LCD_WIDTH - x;
    if (width <= 0) return;
    for (int y = 0; y < BOX3_LCD_HEIGHT; y += 20) {
        const int rows = (BOX3_LCD_HEIGHT - y < 20) ? BOX3_LCD_HEIGHT - y : 20;
        for (int r = 0; r < rows; ++r) {
            const uint16_t *source = &s_framebuffer[(y + r) * BOX3_LCD_WIDTH + x];
            uint16_t *strip = &s_dma_strip[r * width];
            for (int i = 0; i < width; ++i) {
                const uint16_t value = source[i];
                strip[i] = (uint16_t)((value << 8) | (value >> 8));
            }
        }
        esp_err_t result = box3_display_blit(x, y, width, rows, s_dma_strip);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Window strip at y=%d failed: %s", y, esp_err_to_name(result));
            ESP_ERROR_CHECK(result);
        }
    }
}

void gfx_capture_background(void)
{
    memcpy(s_background, s_framebuffer, sizeof(s_background));
}

void gfx_restore_background(void)
{
    memcpy(s_framebuffer, s_background, sizeof(s_framebuffer));
}

void gfx_shade_background(uint8_t opacity)
{
    for (size_t i = 0; i < BOX3_LCD_WIDTH * BOX3_LCD_HEIGHT; ++i)
        s_framebuffer[i] = gfx_blend_rgb565(s_background[i], 0, opacity);
}

void gfx_present_fade(uint8_t opacity)
{
    for (int y = 0; y < BOX3_LCD_HEIGHT; y += 20) {
        const int rows = (BOX3_LCD_HEIGHT - y < 20) ? BOX3_LCD_HEIGHT - y : 20;
        const int start = y * BOX3_LCD_WIDTH;
        for (int i = 0; i < BOX3_LCD_WIDTH * rows; ++i) {
            const uint16_t pixel = gfx_blend_rgb565(s_background[start + i], s_framebuffer[start + i], opacity);
            s_dma_strip[i] = (uint16_t)((pixel << 8) | (pixel >> 8));
        }
        ESP_ERROR_CHECK(box3_display_blit(0, y, BOX3_LCD_WIDTH, rows, s_dma_strip));
    }
}

void gfx_present_slide(int offset)
{
    if (offset < 0) offset = 0;
    if (offset > BOX3_LCD_WIDTH) offset = BOX3_LCD_WIDTH;

    for (int y = 0; y < BOX3_LCD_HEIGHT; y += 20) {
        const int rows = (BOX3_LCD_HEIGHT - y < 20) ? BOX3_LCD_HEIGHT - y : 20;
        for (int r = 0; r < rows; ++r) {
            const uint16_t *background = &s_background[(y + r) * BOX3_LCD_WIDTH];
            const uint16_t *foreground = &s_framebuffer[(y + r) * BOX3_LCD_WIDTH];
            uint16_t *strip = &s_dma_strip[r * BOX3_LCD_WIDTH];
            for (int x = 0; x < offset; ++x) {
                const uint16_t value = background[x];
                strip[x] = (uint16_t)((value << 8) | (value >> 8));
            }
            for (int x = offset; x < BOX3_LCD_WIDTH; ++x) {
                const uint16_t value = foreground[x - offset];
                strip[x] = (uint16_t)((value << 8) | (value >> 8));
            }
        }
        const esp_err_t result = box3_display_blit(0, y, BOX3_LCD_WIDTH, rows, s_dma_strip);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Slide strip at y=%d failed: %s", y, esp_err_to_name(result));
            ESP_ERROR_CHECK(result);
        }
    }
}
