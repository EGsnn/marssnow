#pragma once
#include <stddef.h>
#include <stdint.h>

/* Generic, duration-preserving 16 kHz mono PCM effects. No callbacks or heap. */
enum { MARS_VOICE_ORIGINAL, MARS_VOICE_ROBOT, MARS_VOICE_WARM, MARS_VOICE_COUNT };
typedef struct { int32_t low; uint32_t phase; uint8_t mode; } mars_voice_effect_t;
void mars_voice_effect_process(mars_voice_effect_t *state, int16_t *pcm,
                               size_t count, uint8_t mode);
