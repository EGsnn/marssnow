/**
 * @file mars_app_heap.c
 * @brief 应用独立堆实现（first-fit + 边界标记）
 *
 * 块格式：
 *   [8 字节块头][数据区...]
 *   块头低 32 位 = 块大小（含头，8 字节对齐，bit0 = 已分配标志）
 *   空闲块在数据区前 4 字节存 next 指针（空闲链表）
 */

#include "mars_app_heap.h"

#include <string.h>
#include <stdio.h>

#define HEAP_MAGIC_FREE 0xFA11C0DEu
#define HEAP_MAGIC_USED 0x1553D000u

typedef struct mars_heap_block {
    uint32_t size_flags;          /* size(31:1) | used(0) */
    union {
        struct mars_heap_block* next;  /* 空闲链表 */
        uint32_t magic;               /* 已分配校验 */
    } u;
    /* 数据区从这里开始 */
} mars_heap_block_t;

#define BLOCK_SIZE_MASK 0xFFFFFFFEu
#define BLOCK_USED      0x1u

static inline uint32_t block_size(const mars_heap_block_t* b)
{
    return b->size_flags & BLOCK_SIZE_MASK;
}

static inline bool block_used(const mars_heap_block_t* b)
{
    return (b->size_flags & BLOCK_USED) != 0;
}

static inline uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

void mars_heap_init(mars_app_heap_t* heap, void* pool, uint32_t pool_size)
{
    memset(heap, 0, sizeof(*heap));
    heap->pool = (uint8_t*)align_up((uint32_t)(uintptr_t)pool, MARS_HEAP_ALIGN);
    uint32_t adj = (uint32_t)((uintptr_t)heap->pool - (uintptr_t)pool);
    heap->pool_size = pool_size - adj;

    /* 初始一个空闲大块 */
    mars_heap_block_t* blk = (mars_heap_block_t*)heap->pool;
    uint32_t avail = heap->pool_size;
    if (avail < sizeof(mars_heap_block_t) + MARS_HEAP_ALIGN) {
        heap->pool_size = 0;
        return;
    }
    blk->size_flags = avail;
    blk->u.next = NULL;
    heap->free_list = blk;
    heap->used = 0;
    heap->blocks = 0;
}

void* mars_heap_malloc(mars_app_heap_t* heap, uint32_t size)
{
    if (!heap || !heap->pool || size == 0) return NULL;

    uint32_t need = align_up(size + sizeof(mars_heap_block_t), MARS_HEAP_ALIGN);
    if (need < sizeof(mars_heap_block_t) + MARS_HEAP_ALIGN) {
        need = sizeof(mars_heap_block_t) + MARS_HEAP_ALIGN;
    }

    /* first-fit 查找 */
    mars_heap_block_t** pprev = (mars_heap_block_t**)&heap->free_list;
    mars_heap_block_t*  blk = heap->free_list;

    while (blk) {
        uint32_t sz = block_size(blk);
        if (sz >= need) {
            /* 从空闲链表摘除 */
            *pprev = blk->u.next;

            /* 分裂：如果剩余 >= 最小块 */
            if (sz - need >= sizeof(mars_heap_block_t) + MARS_HEAP_ALIGN) {
                mars_heap_block_t* rest = (mars_heap_block_t*)((uint8_t*)blk + need);
                rest->size_flags = sz - need;
                rest->u.next = *pprev;
                *pprev = rest;
                blk->size_flags = need;
            }

            blk->size_flags |= BLOCK_USED;
            blk->u.magic = HEAP_MAGIC_USED;
            heap->used += block_size(blk);
            heap->blocks++;
            if (heap->used > heap->peak) heap->peak = heap->used;

            return (void*)((uint8_t*)blk + sizeof(mars_heap_block_t));
        }
        pprev = &blk->u.next;
        blk = blk->u.next;
    }

    return NULL;  /* 堆满 */
}

void mars_heap_free(mars_app_heap_t* heap, void* ptr)
{
    if (!heap || !ptr) return;

    mars_heap_block_t* blk = (mars_heap_block_t*)((uint8_t*)ptr - sizeof(mars_heap_block_t));
    if (!block_used(blk)) return;  /* 双重释放保护 */
    if (blk->u.magic != HEAP_MAGIC_USED) return;

    uint32_t sz = block_size(blk);
    blk->size_flags = sz;  /* 清除 used 位 */
    blk->u.next = heap->free_list;
    heap->free_list = blk;
    heap->used -= sz;
    heap->blocks--;

    /* 简化：不合并相邻空闲块（对嵌入式足够） */
}

void* mars_heap_realloc(mars_app_heap_t* heap, void* ptr, uint32_t new_size)
{
    if (!ptr) return mars_heap_malloc(heap, new_size);
    if (new_size == 0) {
        mars_heap_free(heap, ptr);
        return NULL;
    }

    mars_heap_block_t* blk = (mars_heap_block_t*)((uint8_t*)ptr - sizeof(mars_heap_block_t));
    uint32_t old_size = block_size(blk) - sizeof(mars_heap_block_t);

    if (old_size >= new_size) return ptr;

    void* np = mars_heap_malloc(heap, new_size);
    if (!np) return NULL;
    memcpy(np, ptr, old_size);
    mars_heap_free(heap, ptr);
    return np;
}

void mars_heap_stats(const mars_app_heap_t* heap, uint32_t* used, uint32_t* peak, uint32_t* blocks)
{
    if (used) *used = heap->used;
    if (peak) *peak = heap->peak;
    if (blocks) *blocks = heap->blocks;
}
