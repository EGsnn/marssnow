#pragma once

/* line_disc：串口行 discipline（纯 C，可主机侧测试）
 *
 * 唯一串口消费者把任意分片字节流喂进来，本模块只在收到完整 CR/LF 行时
 * 回调一次；行内容以 NUL 结尾。'\r' 一律跳过，'\n' 作为行结束。
 * 行超长时进入"整行丢弃"状态：只触发一次 overflow 回调，忽略该超长行
 * 剩余全部字符直到换行（换行本身也不交付该行），保证超长 @UI 数据的尾部
 * 不会被拼成有效的 @ACTION/@SWITCH 命令。下一条正常行完整解析。
 *
 * 不依赖 FreeRTOS / ESP-IDF，供固件 ui_runtime 与主机碎片化命令测试共用。
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   *buf;        /* 外部提供的行缓冲 */
    size_t  cap;        /* 行缓冲容量（含 NUL） */
    size_t  len;        /* 当前已累积字节数 */
    bool    discarding; /* 正在丢弃一条超长行（直到遇到换行） */
} mars_line_disc_t;

/* 初始化：buf/cap 为外部行缓冲。 */
void mars_line_disc_init(mars_line_disc_t *d, char *buf, size_t cap);

/* 喂入 n 字节。每解析出一条完整行调用 on_line(line, ctx)（line 以 NUL 结尾，
 * 生命周期仅在回调内有效）；发生行溢出时调用 on_overflow(ctx)（可为 NULL）。
 * 返回本次喂入解析出的完整行数。 */
int mars_line_disc_feed(mars_line_disc_t *d, const char *data, size_t n,
                        void (*on_line)(const char *line, void *ctx), void *ctx,
                        void (*on_overflow)(void *ctx));

#ifdef __cplusplus
}
#endif
