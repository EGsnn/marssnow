/* host 桩：malloc/free 计数 + 分配失败注入
 * （MARS_HOST_TEST 下由 mars_audio_stream.c 的宏替换接入）
 */
#ifndef STUB_HOST_MALLOC_H
#define STUB_HOST_MALLOC_H

#include <stddef.h>

void *host_malloc(size_t size);
void host_free(void *ptr);

size_t host_malloc_count(void);
size_t host_free_count(void);
size_t host_malloc_free_count(void);
/* 第 n 次 malloc 返回 NULL（0 = 关闭注入） */
void host_malloc_set_fail_at(size_t n);
void host_malloc_reset(void);

#endif
