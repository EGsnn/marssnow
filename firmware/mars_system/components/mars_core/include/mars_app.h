#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* App 生命周期框架
 * 内置 App 与外部 App 统一注册；open/close 时发布 MARS_EVENT_APP_OPEN/CLOSE。
 * 简单 App 可省略不需要的回调（置 NULL）。 */

typedef struct mars_app mars_app_t;

struct mars_app {
    const char *name;              /* 显示名 */
    uint16_t accent;               /* 强调色 RGB565 */
    void (*open)(void);            /* 进入：渲染初始画面 */
    void (*close)(void);           /* 退出（可为 NULL） */
    void (*key)(int key);          /* 按键（可为 NULL） */
    void (*touch)(uint16_t x, uint16_t y); /* 触摸（可为 NULL） */
    void (*tick)(int64_t now_ms);  /* 周期刷新（可为 NULL） */
};

/* 注册一个 App（保存指针，表满返回 false） */
bool mars_app_register(const mars_app_t *app);

size_t mars_app_count(void);
const mars_app_t *mars_app_get(size_t index);

/* 打开/关闭：调用对应回调并发布事件；index 越界返回 false */
bool mars_app_open(size_t index);
bool mars_app_close(size_t index);
