#pragma once
#include <stdint.h>

/* Blend RGB565 channels independently; opacity 0/255 preserves endpoints. */
static inline uint16_t gfx_blend_rgb565(uint16_t back, uint16_t front, uint8_t opacity)
{
    unsigned a = opacity, b = 255 - a;
    unsigned r = ((back >> 11) * b + (front >> 11) * a + 127) / 255;
    unsigned g = (((back >> 5) & 63) * b + ((front >> 5) & 63) * a + 127) / 255;
    unsigned blue = ((back & 31) * b + (front & 31) * a + 127) / 255;
    return (uint16_t)((r << 11) | (g << 5) | blue);
}
