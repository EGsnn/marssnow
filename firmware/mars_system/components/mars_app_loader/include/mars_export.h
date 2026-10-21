#pragma once

/**
 * @file mars_export.h
 * @brief 系统符号导出宏（动态应用可见）
 *
 * 符号表条目用 &(sym) 表达式，地址由链接器在"同一次链接"中填充：
 *  - 天然自洽（表与固件不可能错位）；
 *  - 随最终链接自动更新，改服务代码后直接重新构建即可；
 *  - 条目构成真正的符号引用，服务函数不会被 --gc-sections 回收；
 *  - 无需构建后用 nm 重导、无迭代振荡。
 */

#include <stdint.h>
#include "mars_app_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在 mars_symtab[] 聚合数组中生成一个条目 */
#define MARS_EXP_ENTRY(_sym)                                                   \
    { #_sym, (uint32_t)(uintptr_t)(&(_sym)) }

#ifdef __cplusplus
}
#endif
