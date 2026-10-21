#include "mars_runtime.h"
#include "mars_clock.h"
#include <string.h>

static uint32_t s_touch;
static uint32_t s_back_visible=1;
void mars_input_set_back_visible(uint32_t visible)
{
    __atomic_store_n(&s_back_visible,!!visible,__ATOMIC_RELEASE);
}
uint32_t mars_input_back_visible(void)
{
    return __atomic_load_n(&s_back_visible,__ATOMIC_ACQUIRE);
}

int mars_time_read(mars_time_snapshot_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->sync_state = (uint32_t)mars_clock_state();
    out->utc_offset_sec = mars_clock_tz_offset_sec();
    struct tm local;
    if (!mars_clock_get_local_time(&local)) return -2;
    out->valid = 1;
    out->year = (uint32_t)(local.tm_year + 1900);
    out->month = (uint32_t)(local.tm_mon + 1);
    out->day = (uint32_t)local.tm_mday;
    out->hour = (uint32_t)local.tm_hour;
    out->minute = (uint32_t)local.tm_min;
    out->second = (uint32_t)local.tm_sec;
    return 0;
}

void mars_input_publish_native_touch(uint32_t down, uint16_t x, uint16_t y)
{
    uint32_t packed = 0;
    if (down && x < 512 && y < 512)
        packed = (1u << 18) | ((uint32_t)y << 9) | x;
    __atomic_store_n(&s_touch, packed, __ATOMIC_RELEASE);
}

void mars_input_publish_touch(uint32_t down,uint16_t x,uint16_t y)
{mars_input_publish_native_touch(down&&x<320&&y<240,x,y);}

int mars_input_read_touch(mars_touch_snapshot_t *out)
{
    if (!out) return -1;
    const uint32_t packed = __atomic_load_n(&s_touch, __ATOMIC_ACQUIRE);
    out->down = (packed >> 18) & 1u;
    out->x = packed & 511u;
    out->y = (packed >> 9) & 511u;
    return 0;
}
