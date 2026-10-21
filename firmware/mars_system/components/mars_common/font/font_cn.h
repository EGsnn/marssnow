#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 从内存镜像初始化字库。data 由调用方持有且必须保持有效；
 * 返回 true 表示镜像有效并已挂接。格式见 gen_font_cn.py。 */
bool font_cn_init(const uint8_t *data, size_t size);

/* v2 layout: validate and attach the single copy embedded in the system image. */
bool font_cn_init_embedded(void);

/* 按 unicode 码点查 GB2312 16x16 点阵 (32字节), 未收录返回 NULL */
const uint8_t *font_cn_glyph(uint32_t cp);

/* 按 GB2312 区位(1-94)查点阵, 未收录返回 NULL */
const uint8_t *font_cn_glyph_qw(int q, int w);
