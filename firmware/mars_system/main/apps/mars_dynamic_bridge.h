/**
 * @file mars_dynamic_bridge.h
 * @brief 动态应用桥：把应用分区中的 MARSAPP 应用注册到桌面
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 扫描应用分区并注册动态应用（开机调用一次）
 */
void mars_dynamic_bridge_init(void);

/**
 * 已注册的动态应用数量
 */
int mars_dynamic_bridge_count(void);

/**
 * 指定槽位是否为动态应用
 */
bool mars_dynamic_bridge_is_slot(int slot);

/** Registry name (APP0..APP2) for an installed package id, or NULL. */
const char* mars_dynamic_bridge_registry_name(const char* package_name);
const char* mars_dynamic_bridge_package_name(int slot);
const char* mars_dynamic_bridge_display_name(int slot);
/** Draw a 48x48 package icon. Returns false for legacy packages. */
bool mars_dynamic_bridge_draw_icon(int slot, int x, int y);

#ifdef __cplusplus
}
#endif
