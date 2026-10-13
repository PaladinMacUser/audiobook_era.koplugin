/* kok_source.c -- the generator's harmonic source (kokoro's SineGen plus the
 * tanh merge), so the runtime can synthesize from ids alone.
 *
 * Reproduces, in order:
 *   f0 -> nearest-upsampled by 300 -> harmonics 1..9 -> radians mod 1 ->
 *   random initial phase on the first timestep (torch.rand, first harmonic
 *   zeroed) -> linear downsample by 300 -> cumulative phase * 2*pi -> linear
 *   upsample by 300 -> sin * sine_amp -> voiced mask -> additive noise
 *   (torch.randn, amplitude sine_amp/3 unvoiced, 0.003 voiced) -> tanh(Linear).
 *
 * The two interpolations use align_corners=False, matching torch. The RNG draw
 * order is rand(9) then randn(L*9), the same order as the reference. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "kok_rng.h"
#include "kok_runtime.h"

int kok_source_forward(const kok_config *c, const kok_dec_w *dw, const float *f0,
                       int F, kok_rng *g, float *src, kok_arena *a) {
    const int sr = c->v[KOK_D_SR];
    const int gen_hop = c->v[KOK_D_DEC_UPRATE0] * c->v[KOK_D_DEC_UPRATE1] * c->v[KOK_D_DEC_ISTFT_HOP];
    const int dim = c->v[KOK_D_GEN_HARMONICS];
    if (dim < 1 || dim > 32) return -1;
    const long L = (long)(2 * F) * gen_hop;      /* per-sample rate */
    const long Lf = 2 * F;                       /* the F0 curve's rate */
    const float sine_amp = 0.1f, noise_std = 0.003f, voiced_thresh = 10.0f;

    float *fn = kok_arena_alloc(a, (size_t)L * dim);        /* f0 * (h+1) */
    float *rad = kok_arena_alloc(a, (size_t)L * dim);       /* per-sample radians */
    float *phase = kok_arena_alloc(a, (size_t)Lf * dim);
    float *sines = kok_arena_alloc(a, (size_t)L * dim);
    float *noise = kok_arena_alloc(a, (size_t)L * dim);
    float *tmp = kok_arena_alloc(a, (size_t)L);
    if (!fn || !rad || !phase || !sines || !noise || !tmp) return -1;

    /* harmonics at the sample rate */
    for (long l = 0; l < L; l++) {
        float f = f0[l / gen_hop < Lf ? l / gen_hop : Lf - 1];
        for (int h = 0; h < dim; h++) fn[l * dim + h] = f * (float)(h + 1);
    }
    /* radians mod 1, per sample and harmonic (the full rate: torch
     * interpolates this down to the frame rate, it does not decimate) */
    for (long l = 0; l < L * dim; l++) {
        float v = fn[l] / (float)sr;
        rad[l] = v - floorf(v);
    }
    /* random initial phase, first harmonic zeroed (torch.rand(1, dim)) */
    {
        float ini[32];
        for (int h = 0; h < dim; h++) ini[h] = kok_rng_uniform(g);
        ini[0] = 0.0f;
        for (int h = 0; h < dim; h++) rad[h] += ini[h];      /* first timestep */
    }
    /* downsample the radians by 1/300, accumulate the phase * 2*pi, then
     * upsample by 300 and take sin. Both interpolations are per harmonic and
     * write into a row, so the helper fills a contiguous scratch that is then
     * scattered into the [sample][harmonic] layout. */
    {
        float *rowL = tmp;                            /* L floats */
        float *rowF = kok_arena_alloc(a, (size_t)Lf); /* Lf floats */
        float *acc = kok_arena_alloc(a, (size_t)L);   /* L floats */
        if (!rowF || !acc) return -1;
        for (int h = 0; h < dim; h++) {
            for (long l = 0; l < L; l++) rowL[l] = rad[l * dim + h];
            kok_interp_linear(rowF, Lf, rowL, L, 1.0 / (double)gen_hop);
            {
                /* torch.cumsum on a float32 tensor accumulates in float32 */
                float a = 0.0f;
                for (long l = 0; l < Lf; l++) {
                    a += rowF[l];
                    phase[l * dim + h] = a * (float)(2.0 * 3.14159265358979323846);
                }
            }
        }
#ifdef KOK_DUMP_STAGES
        {
            FILE *fh = fopen("/tmp/ck_phase.bin", "wb");
            if (fh) { fwrite(phase, sizeof(float), (size_t)Lf * dim, fh); fclose(fh); }
        }
#endif
        for (int h = 0; h < dim; h++) {
            for (long l = 0; l < Lf; l++) rowF[l] = phase[l * dim + h] * (float)gen_hop;
            kok_interp_linear(acc, L, rowF, Lf, (double)gen_hop);
            for (long l = 0; l < L; l++) sines[l * dim + h] = sinf(acc[l]) * sine_amp;
        }
    }

#ifdef KOK_DUMP_STAGES
    {
        FILE *fh = fopen("/tmp/ck_sines_pre.bin", "wb");
        if (fh) { fwrite(sines, sizeof(float), (size_t)L * dim, fh); fclose(fh); }
    }
#endif
    /* noise: randn over the whole [L, dim] block, then the voiced mask */
    if (kok_rng_normal_fill(g, noise, (long)L * dim)) return -1;
#ifdef KOK_DUMP_STAGES
    {
        FILE *fh = fopen("/tmp/ck_noise.bin", "wb");
        if (fh) { fwrite(noise, sizeof(float), (size_t)L * dim, fh); fclose(fh); }
    }
#endif
    for (long l = 0; l < L; l++) {
        float f0s = f0[l / gen_hop < Lf ? l / gen_hop : Lf - 1];
        float uv = (f0s > voiced_thresh) ? 1.0f : 0.0f;
        float noise_amp = uv * noise_std + (1.0f - uv) * sine_amp / 3.0f;
        for (int h = 0; h < dim; h++)
            sines[l * dim + h] = sines[l * dim + h] * uv + noise_amp * noise[l * dim + h];
    }
#ifdef KOK_DUMP_STAGES
    {
        FILE *fh = fopen("/tmp/ck_sines.bin", "wb");
        if (fh) { fwrite(sines, sizeof(float), (size_t)L * dim, fh); fclose(fh); }
        fh = fopen("/tmp/ck_fn.bin", "wb");
        if (fh) { fwrite(fn, sizeof(float), (size_t)L, fh); fclose(fh); }
    }
#endif
    /* tanh(l_linear(.)), the l_linear weights being [1, dim] */
    /* l_linear is on the exporter's fp32 keep-list, so this reads .f directly */
    const float *lw = dw->src_w.f;
    if (!lw || dw->src_w.q) return -12;
    for (long l = 0; l < L; l++) {
        double acc = dw->src_b ? dw->src_b[0] : 0.0;
        for (int h = 0; h < dim; h++) acc += (double)lw[h] * sines[l * dim + h];
        src[l] = tanhf((float)acc);
    }
    return 0;
}