/**
 * @file mars_app_loader.h
 * @brief 火星动态应用加载器对外 API
 *
 * 功能：
 *  - 从应用分区（app_0/1/2）加载 MARSAPP v2 应用包
 *  - 解析 ELF、重定位、映射到内存
 *  - 每个应用独立任务 + 独立堆（与系统同一地址空间）
 *  - 退出后卸载，释放该应用运行资源
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MARS_APP_SLOTS 10

/* 系统符号表条目（动态链接用） */
typedef struct {
    const char* name;
    uint32_t    addr;
} mars_symtab_entry_t;

/* 应用状态 */
typedef enum {
    MARS_APP_STATE_EMPTY = 0,   /* 分区为空 */
    MARS_APP_STATE_LOADED,      /* 已加载（未运行） */
    MARS_APP_STATE_RUNNING,     /* 运行中 */
    MARS_APP_STATE_CRASHED,     /* 崩溃（等待重启/卸载） */
    MARS_APP_STATE_EXITED,      /* 应用退出 */
} mars_app_state_t;

/* 应用信息 */
typedef struct {
    int             slot;
    mars_app_state_t state;
    char            name[32];
    char            version[16];
    uint32_t        load_addr;      /* 代码映射基址 */
    uint32_t        data_addr;      /* 数据段地址 */
    uint32_t        data_size;
    uint32_t        bss_size;
    uint32_t        stack_size;
    uint32_t        heap_size;
    uint32_t        heap_used;
    uint32_t        heap_peak;
    uint32_t        crash_count;
    uint32_t        uptime_ms;      /* 本次运行时长 */
} mars_app_info_t;

/**
 * 初始化应用加载器（开机调用一次）
 */
void mars_app_loader_init(void);

/**
 * 加载应用分区（解析 + 重定位 + 分配内存，不运行）
 * @param slot 0/1/2
 * @return 0 成功，负值错误码
 */
int mars_app_load(int slot);

/**
 * 启动应用（创建独立任务，调用入口）
 * @param slot 0/1/2
 * @return 0 成功
 */
int mars_app_start(int slot);

/**
 * 停止应用（删除任务）
 */
int mars_app_stop(int slot);

/**
 * 卸载应用（释放内存）
 */
int mars_app_unload(int slot);

/**
 * 加载并启动（一步）
 */
int mars_app_launch(int slot);

/** Dispatch optional lifecycle callbacks exported by an application package. */
int mars_app_handle_key(int slot, int key);
int mars_app_handle_touch(int slot, uint16_t x, uint16_t y);

/**
 * 获取应用信息
 */
int mars_app_get_info(int slot, mars_app_info_t* info);

/**
 * 扫描所有分区，返回已安装应用列表
 * @return 已加载应用数量
 */
int mars_app_scan_all(void);

/**
 * 系统符号查找（供重定位器使用）
 */
uint32_t mars_symtab_lookup(const char* name);

#ifdef __cplusplus
}
#endif
