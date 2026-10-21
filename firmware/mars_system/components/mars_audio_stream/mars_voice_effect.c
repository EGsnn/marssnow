#include "mars_voice_effect.h"

void mars_voice_effect_process(mars_voice_effect_t *s, int16_t *pcm, size_t n, uint8_t mode)
{
    if (!s || !pcm) return;
    if (mode >= MARS_VOICE_COUNT) mode = MARS_VOICE_ORIGINAL;
    if (s->mode != mode) { s->low = 0; s->phase = 0; s->mode = mode; }
    if (mode == MARS_VOICE_ORIGINAL) return; /* bit-identical legacy playback */
    for (size_t i = 0; i < n; ++i) {
        int32_t x = pcm[i], y;
        if (mode == MARS_VOICE_ROBOT) {
            /* 125 Hz triangle ring modulation; retain 60% clean speech for
             * intelligibility. Each packet continues the previous phase. */
            int32_t carrier = s->phase < 64 ? (int32_t)s->phase * 1024 - 32768
                                           : 98304 - (int32_t)s->phase * 1024;
            int32_t ring = x * carrier / 32768;
            y = (x * 3 + ring * 2) / 5;
            s->phase = (s->phase + 1) & 127;
        } else {
            /* Warm/darker EQ, not a gender conversion or pitch shifter.
             * Fixed sample count and speed; no queued delay/tail to lose. */
            s->low += (x - s->low) / 4;
            y = (x + 3 * s->low) / 4;
        }
        pcm[i] = (int16_t)y; /* both effects are bounded convex mixtures */
    }
}
