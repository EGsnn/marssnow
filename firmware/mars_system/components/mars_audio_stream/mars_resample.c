/* mars_resample：定点线性插值重采样（单声道 int16 PCM）
 *
 * 相位累加器：每输出 1 个样本，输入相位推进 step = in_rate/out_rate（Q16）。
 * 跨缓冲连续：保留上次缓冲最后一个样本，供下一个缓冲首样本插值。
 */

#include "mars_resample.h"

void mars_resample_init(mars_resample_t *r, uint32_t in_rate, uint32_t out_rate)
{
    if (r == NULL || in_rate == 0 || out_rate == 0) {
        return;
    }
    r->in_rate   = in_rate;
    r->out_rate  = out_rate;
    r->step      = ((uint64_t)in_rate << MARS_RESAMPLE_FRAC_BITS) / out_rate;
    r->phase     = 0;
    r->last_sample = 0;
    r->initialized = 0;   /* 尚无"上一缓冲最后样本" */
}

size_t mars_resample_process(mars_resample_t *r, const int16_t *in,
                             size_t in_n, int16_t *out, size_t out_cap)
{
    if (r == NULL || in == NULL || out == NULL || in_n == 0 || out_cap == 0) {
        return 0;
    }

    const uint64_t step = r->step;
    uint64_t phase = 0;               /* 本次调用从输入 0 开始 */
    const int has_last = r->initialized;
    const int16_t last = r->last_sample;
    size_t o = 0;

    while (o < out_cap) {
        uint32_t posi = (uint32_t)(phase >> MARS_RESAMPLE_FRAC_BITS);
        uint32_t frac = (uint32_t)(phase & (MARS_RESAMPLE_ONE - 1));
        if (posi >= in_n) {
            break;
        }
        int32_t a, b;
        if (posi == 0 && frac > 0 && has_last) {
            /* 第一个输出落在缓冲起点之后：用上一缓冲末样本插值 */
            a = last;
            b = in[0];
        } else if (posi + 1 < in_n) {
            a = in[posi];
            b = in[posi + 1];
        } else {
            a = in[posi];
            b = in[posi];
        }
        int32_t v = a + (int32_t)(((int64_t)(b - a) * (int64_t)frac) >>
                                  MARS_RESAMPLE_FRAC_BITS);
        out[o++] = (int16_t)v;
        phase += step;
    }

    r->last_sample = in[in_n - 1];
    r->initialized = 1;
    return o;
}
