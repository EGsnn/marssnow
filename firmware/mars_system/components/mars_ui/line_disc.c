/* line_disc：串口行 discipline 实现（纯 C，见 line_disc.h） */

#include "line_disc.h"

#include <string.h>

void mars_line_disc_init(mars_line_disc_t *d, char *buf, size_t cap)
{
    if (d == NULL) {
        return;
    }
    d->buf = buf;
    d->cap = cap;
    d->len = 0;
    d->discarding = false;
    if (buf != NULL && cap > 0) {
        buf[0] = '\0';
    }
}

int mars_line_disc_feed(mars_line_disc_t *d, const char *data, size_t n,
                        void (*on_line)(const char *line, void *ctx), void *ctx,
                        void (*on_overflow)(void *ctx))
{
    if (d == NULL || d->buf == NULL || d->cap == 0 || data == NULL || on_line == NULL) {
        return 0;
    }
    int delivered = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = data[i];
        /* 正在丢弃一条超长行：忽略一切字符直到换行；\r 也忽略。
         * 换行只结束丢弃状态，绝不把该行（含尾部）作为命令交付。 */
        if (d->discarding) {
            if (c == '\n') {
                d->discarding = false;
                d->len = 0;
                d->buf[0] = '\0';
            }
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            d->buf[d->len] = '\0';
            on_line(d->buf, ctx);
            ++delivered;
            d->len = 0;
            d->buf[0] = '\0';
            continue;
        }
        if (d->len + 1 < d->cap) {
            d->buf[d->len++] = c;
            d->buf[d->len] = '\0';
        } else {
            /* 行超长：进入整行丢弃状态，只触发一次 overflow，
             * 忽略本条超长行剩余全部字符直到换行（P0-1）。 */
            d->discarding = true;
            d->len = 0;
            d->buf[0] = '\0';
            if (on_overflow != NULL) {
                on_overflow(ctx);
            }
        }
    }
    return delivered;
}
