#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 补间动画引擎（L0，不依赖具体 UI）
 * 用法：
 *   mars_anim_t a;
 *   mars_anim_start(&a, 320.0f, 0.0f, 240, MARS_EASE_OUT, on_step, on_done, ctx);
 *   // 每帧（如 20ms tick）调用 mars_anim_tick(&a, now_ms)，返回 false 即结束 */

typedef enum {
    MARS_EASE_LINEAR = 0,
    MARS_EASE_IN,        /* 加速 */
    MARS_EASE_OUT,       /* 减速（常用） */
    MARS_EASE_IN_OUT,
    MARS_EASE_BACK_OUT,  /* 带一点回弹 */
    MARS_EASE_BOUNCE,
} mars_ease_t;

typedef struct {
    uint32_t duration_ms;
    mars_ease_t ease;
    float from;
    float to;
    void (*step)(float value, void *ctx); /* 每帧回调，value ∈ [from,to] */
    void (*done)(void *ctx);              /* 结束回调 */
    void *ctx;
    /* internal */
    bool running;
    uint32_t start_ms;
} mars_anim_t;

/* 0..1 进度应用缓动曲线 */
float mars_ease_apply(mars_ease_t ease, float t);

void mars_anim_start(mars_anim_t *anim, float from, float to, uint32_t duration_ms,
                     mars_ease_t ease, void (*step)(float, void *), void (*done)(void *), void *ctx);

bool mars_anim_running(const mars_anim_t *anim);

/* 驱动一帧；动画结束时调用 done 并返回 false */
bool mars_anim_tick(mars_anim_t *anim, int64_t now_ms);
