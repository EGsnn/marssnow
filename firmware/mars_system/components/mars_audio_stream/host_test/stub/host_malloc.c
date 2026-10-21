/* host 桩：malloc/free 计数实现 */
#include "host_malloc.h"

#include <stdatomic.h>
#include <stdlib.h>

static atomic_size_t g_alloc;
static atomic_size_t g_free;
static atomic_size_t g_fail_at;

void *host_malloc(size_t size)
{
    size_t n = atomic_fetch_add_explicit(&g_alloc, 1, memory_order_relaxed) + 1;
    size_t fail_at = atomic_load_explicit(&g_fail_at, memory_order_relaxed);
    if (fail_at != 0 && n == fail_at) {
        return NULL;
    }
    return malloc(size);
}

void host_free(void *ptr)
{
    if (ptr != NULL) {
        atomic_fetch_add_explicit(&g_free, 1, memory_order_relaxed);
        free(ptr);
    }
}

size_t host_malloc_count(void) { return atomic_load_explicit(&g_alloc, memory_order_relaxed); }
size_t host_free_count(void) { return atomic_load_explicit(&g_free, memory_order_relaxed); }
size_t host_malloc_free_count(void)
{
    return host_malloc_count() - host_free_count();
}
void host_malloc_set_fail_at(size_t n)
{
    atomic_store_explicit(&g_fail_at, n, memory_order_relaxed);
}
void host_malloc_reset(void)
{
    atomic_store_explicit(&g_alloc, 0, memory_order_relaxed);
    atomic_store_explicit(&g_free, 0, memory_order_relaxed);
    atomic_store_explicit(&g_fail_at, 0, memory_order_relaxed);
}
