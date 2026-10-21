#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 系统设置应用（替代原 SYSTEM 静态状态页）。
 * 由 apps.c 接入：draw / key / touch / tick。 */

/* first_draw renders to the framebuffer only; the launcher presents its
 * transition. Subsequent changed frames are presented by settings_draw(). */
void settings_draw(bool first_draw);
void settings_key(int key);
void settings_touch(uint16_t x, uint16_t y);
void settings_tick(int64_t now_ms);

/* 应用退出时重置内部页面状态，保证下次打开回到菜单页 */
void settings_reset(void);
