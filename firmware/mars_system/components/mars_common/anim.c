#include "mars_anim.h"

#include <stddef.h>

float mars_ease_apply(mars_ease_t ease, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    switch (ease) {
    case MARS_EASE_IN:
        return t * t;
    case MARS_EASE_OUT:
        return t * (2.0f - t);
    case MARS_EASE_IN_OUT:
        return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    case MARS_EASE_BACK_OUT: {
        const float c1 = 1.70158f;
        const float c3 = c1 + 1.0f;
        const float u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }
    case MARS_EASE_BOUNCE: {
        const float n1 = 7.5625f;
        const float d1 = 2.75f;
        if (t < 1.0f / d1) return n1 * t * t;
        if (t < 2.0f / d1) {
            const float x = t - 1.5f / d1;
            return n1 * x * x + 0.75f;
        }
        if (t < 2.5f / d1) {
            const float x = t - 2.25f / d1;
            return n1 * x * x + 0.9375f;
        }
        const float x = t - 2.625f / d1;
        return n1 * x * x + 0.984375f;
    }
    case MARS_EASE_LINEAR:
    default:
        return t;
    }
}

void mars_anim_start(mars_anim_t *anim, float from, float to, uint32_t duration_ms,
                     mars_ease_t ease, void (*step)(float, void *), void (*done)(void *), void *ctx)
{
    if (anim == NULL) {
        return;
    }
    anim->from = from;
    anim->to = to;
    anim->duration_ms = duration_ms;
    anim->ease = ease;
    anim->step = step;
    anim->done = done;
    anim->ctx = ctx;
    anim->running = true;
    anim->start_ms = 0; /* 由首次 tick 记录 */
}

bool mars_anim_running(const mars_anim_t *anim)
{
    return anim != NULL && anim->running;
}

bool mars_anim_tick(mars_anim_t *anim, int64_t now_ms)
{
    if (anim == NULL || !anim->running) {
        return false;
    }
    if (anim->start_ms == 0) {
        anim->start_ms = (uint32_t)now_ms;
    }
    const uint32_t elapsed = (uint32_t)now_ms - anim->start_ms;
    if (elapsed >= anim->duration_ms) {
        anim->running = false;
        if (anim->step != NULL) {
            anim->step(anim->to, anim->ctx);
        }
        if (anim->done != NULL) {
            anim->done(anim->ctx);
        }
        return false;
    }
    const float t = anim->duration_ms > 0 ? (float)elapsed / (float)anim->duration_ms : 1.0f;
    const float value = anim->from + (anim->to - anim->from) * mars_ease_apply(anim->ease, t);
    if (anim->step != NULL) {
        anim->step(value, anim->ctx);
    }
    return true;
}
