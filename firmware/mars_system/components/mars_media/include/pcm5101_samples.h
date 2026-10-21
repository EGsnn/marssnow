#pragma once
#include <stdint.h>
#include <limits.h>
/* Multiplication (not left-shifting a negative signed value) avoids C UB. */
static inline int32_t pcm5101_output_sample(int16_t sample,int volume)
{
    if (volume<0) volume=0;
    if (volume>100) volume=100;
    return (int32_t)((int64_t)sample*65536*volume/100);
}
static inline int16_t pcm5101_input_sample(int32_t sample)
{
    /* Same microphone scaling as the upstream board; saturate before narrowing. */
    int32_t value=sample/4096;
    if (value>INT16_MAX) value=INT16_MAX;
    if (value<INT16_MIN) value=INT16_MIN;
    return (int16_t)value;
}
