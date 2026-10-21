/* GB2312 16x16 点阵字库 —— 运行时从内存镜像加载。
 * 镜像由 gen_font_cn.py 生成；v2 固件内嵌，旧布局从 uidata 读取。
 * 格式: magic "FCN1" | u32 count | u32 index_off | u32 bitmap_off
 *       | index[count] 每项 {u16 cp, u16 idx} 小端 | bitmap[count*32] */
#include "font_cn.h"

#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

static const uint8_t *s_index_bytes; /* index 区起始（小端 u16 对） */
static const uint8_t *s_bitmap;
static uint32_t s_count;

bool font_cn_init(const uint8_t *data, size_t size)
{
    if (!data || size < 16) return false;
    if (memcmp(data, "FCN1", 4) != 0) return false;
    const uint32_t count = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                           ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    const uint32_t index_off = (uint32_t)data[8] | ((uint32_t)data[9] << 8) |
                               ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
    const uint32_t bitmap_off = (uint32_t)data[12] | ((uint32_t)data[13] << 8) |
                                ((uint32_t)data[14] << 16) | ((uint32_t)data[15] << 24);
    if (bitmap_off < index_off || count == 0) return false;
    if (size < (size_t)bitmap_off + (size_t)count * 32) return false;
    s_index_bytes = data + index_off;
    s_bitmap = data + bitmap_off;
    s_count = count;
    return true;
}

bool font_cn_init_embedded(void)
{
#if defined(CONFIG_MARS_LAYOUT_V2) && !defined(CONFIG_IDF_TARGET_ESP32C3)
    extern const uint8_t start[] asm("_binary_font_cn_bin_start");
    extern const uint8_t end[] asm("_binary_font_cn_bin_end");
    return font_cn_init(start, (size_t)(end - start));
#else
    return false;
#endif
}

const uint8_t *font_cn_glyph(uint32_t cp)
{
    if (!s_index_bytes || s_count == 0) return NULL;
    int lo = 0;
    int hi = (int)s_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) >> 1;
        const uint8_t *entry = s_index_bytes + (size_t)mid * 4;
        const uint32_t entry_cp = (uint32_t)entry[0] | ((uint32_t)entry[1] << 8);
        if (entry_cp < cp) lo = mid + 1;
        else if (entry_cp > cp) hi = mid - 1;
        else {
            const uint32_t idx = (uint32_t)entry[2] | ((uint32_t)entry[3] << 8);
            return s_bitmap + (size_t)idx * 32;
        }
    }
    return NULL;
}

const uint8_t *font_cn_glyph_qw(int q, int w)
{
    if (!s_bitmap || s_count == 0) return NULL;
    const int idx = (q - 1) * 94 + (w - 1);
    if (idx < 0 || (uint32_t)idx >= s_count) return NULL;
    return s_bitmap + (size_t)idx * 32;
}
