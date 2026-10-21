#pragma once

#include <stdint.h>
#include <stdbool.h>

void gfx_clear(uint16_t color);
void gfx_clip_reset(void);
void gfx_clip_push(int x, int y, int width, int height);
void gfx_clip_pop(void);
void gfx_rect(int x, int y, int width, int height, uint16_t color);
void gfx_round_rect(int x, int y, int width, int height, int radius, uint16_t color);
void gfx_frame(int x, int y, int width, int height, int thickness, uint16_t color);
void gfx_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color);
void gfx_bitmap(int x, int y, int width, int height, const uint16_t *pixels);
/* Nearest-neighbour RGB565 compositing. No LCD access or extra framebuffer.
 * width/height <= 320/240 and integer scale 1..4; source stays app-owned. */
void gfx_bitmap_scaled(int x, int y, int width, int height, int scale, const uint16_t *pixels);
void gfx_text(int x, int y, const char *text, int scale, uint16_t foreground, uint16_t background);
void gfx_text_overlay(int x, int y, const char *text, int scale, uint16_t foreground);
int gfx_text_width(const char *text, int scale);
int gfx_text_height(const char *text, int scale);
void gfx_present(void);
/* 只刷新 framebuffer 的 [x, x+width) 列区间。 */
void gfx_present_window(int x, int width);
/* 保存静止的桌面；当前 framebuffer 是滑动的 App，offset 为它的屏幕左边缘。 */
void gfx_capture_background(void);
void gfx_present_slide(int offset);
/* Modal compositing reuses the existing desktop snapshot. */
void gfx_restore_background(void);
void gfx_shade_background(uint8_t opacity);
void gfx_present_fade(uint8_t opacity);
/* 异步 App：第一帧 present 时通知启动器并等待动画结束，期间冻结画布。 */
bool gfx_hold_first_frame(void);
bool gfx_wait_first_frame(uint32_t timeout_ms);
void gfx_release_first_frame(bool frame_presented);

/* Launcher only: copy a frozen/stopped app frame to the native shell canvas.
 * Caller must own display and freeze an async producer before reading. */
void gfx_copy_frame(uint16_t *destination, int stride);
void gfx_defer_owner_present(bool defer);

/* Kernel-only: caller owns/froze the producing app task. */
const uint16_t *gfx_frame_pixels(void);
bool gfx_first_frame_barrier(void);
