/**
 * @file mars_app_heap.h
 * @brief 应用独立堆分配器
 *
 * 每个应用有自己独立的堆池，malloc/free 在池内分配，
 * 应用的内存泄漏/越界不会影响系统其他部分。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MARS_HEAP_ALIGN 8

/**
 * 堆池控制块
 */
typedef struct mars_app_heap {
    uint8_t* pool;        /* 堆池起始地址（8 字节对齐） */
    uint32_t pool_size;   /* 池总大小 */
    uint32_t used;        /* 已用字节数（统计） */
    uint32_t peak;        /* 峰值使用 */
    uint32_t blocks;      /* 当前块数 */
    /* 简单 first-fit 空闲链表（块头存 size 和 next 指针） */
    void*    free_list;
} mars_app_heap_t;

/**
 * 初始化堆池
 * @param heap    堆控制块
 * @param pool    内存池（8 字节对齐）
 * @param pool_size 池大小
 */
void mars_heap_init(mars_app_heap_t* heap, void* pool, uint32_t pool_size);

/**
 * 从堆池分配
 */
void* mars_heap_malloc(mars_app_heap_t* heap, uint32_t size);

/**
 * 释放
 */
void mars_heap_free(mars_app_heap_t* heap, void* ptr);

/**
 * 重新分配
 */
void* mars_heap_realloc(mars_app_heap_t* heap, void* ptr, uint32_t new_size);

/**
 * 堆统计
 */
void mars_heap_stats(const mars_app_heap_t* heap, uint32_t* used, uint32_t* peak, uint32_t* blocks);

#ifdef __cplusplus
}
#endif
