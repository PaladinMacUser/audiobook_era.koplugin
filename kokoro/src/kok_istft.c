/* kok_istft.c -- the author's ConvSTFT as convolution over DFT bases, plus the
 * generator's AdaINResBlock1 (Snake) and the nearest-neighbour upsampler.
 *
 * The bases are generated at init rather than stored: they are deterministic
 * functions of (n, bin range), computed in double and stored as float, which
 * matches torch to ~1e-7 (verified against the author's ConvSTFT in Python).
 *
 * Conventions reproduced exactly (see kokoro/README.md): reflect padding for
 * the forward transform, the DC and Nyquist imaginary basis rows zeroed, and an
 * inverse scaled by 1/n with the DC/Nyquist coefficient halved and the squared
 * window's overlap envelope divided out. */
#include <math.h>
#include <string.h>

#include "kok_kernels.h"

static float *mem_inv(kok_stft *s) { return s->fwd + (size_t)2 * s->K * s->n; }
static float *mem_w2(kok_stft *s) { return mem_inv(s) + (size_t)2 * s->K * s->n; }

long kok_stft_mem_floats(int n, int bin_lo, int bin_hi) {
    int K = bin_hi - bin_lo + 1;
    return 4L * K * n + n;
}

int kok_stft_init(kok_stft *s, int n, int hop, int bin_lo, int bin_hi, float *mem) {
    if (!s || !mem || n <= 0 || bin_lo < 0 || bin_hi > n / 2 || bin_lo > bin_hi) return -1;
    s->n = n;
    s->hop = hop;
    s->bin_lo = bin_lo;
    s->K = bin_hi - bin_lo + 1;
    s->fwd = mem;
    s->inv = mem_inv(s);
    s->w2 = mem_w2(s);
    const double pi = 3.14159265358979323846;
    /* periodic Hann window (torch.hann_window), in double then stored as float */
    for (int t = 0; t < n; t++) {
        double w = 0.5 - 0.5 * cos(2.0 * pi * (double)t / (double)n);
        s->w2[t] = (float)(w * w);
        for (int j = 0; j < s->K; j++) {
            int k = bin_lo + j;
            double ang = 2.0 * pi * (double)k * (double)t / (double)n;
            double c = cos(ang), sn = -sin(ang);
            int dc = (k == 0 || k == n / 2);
            s->fwd[(size_t)j * n + t] = (float)(c * w);
            s->fwd[(size_t)(s->K + j) * n + t] = dc ? 0.0f : (float)(sn * w);
            /* inverse bases: 1/n, halved at DC and Nyquist if in range */
            double sc = dc ? 1.0 : 2.0;
            s->inv[(size_t)j * n + t] = (float)(cos(ang) * sc / (double)n * w);
            s->inv[(size_t)(s->K + j) * n + t] = (float)(-sin(ang) * sc / (double)n * w);
        }
    }
    return 0;
}

static int stft_conv(const kok_stft *s, const float *x, int T, float *re, float *im,
                     kok_arena *a) {
    const int n = s->n, pad = n / 2;
    if (T < 2) return -1;
    float *xp = kok_arena_alloc(a, (size_t)(T + 2 * pad));
    if (!xp) return -1;
    for (int i = 0; i < pad; i++) xp[i] = x[pad - i];
    for (int t = 0; t < T; t++) xp[pad + t] = x[t];
    for (int i = 0; i < pad; i++) xp[pad + T + i] = x[T - 2 - i];
    int frames = (T + 2 * pad - n) / s->hop + 1;
    for (int j = 0; j < s->K; j++) {
        const float *fr = s->fwd + (size_t)j * n;
        const float *fi = s->fwd + (size_t)(s->K + j) * n;
        float *rr = re + (size_t)j * frames;
        float *ri = im + (size_t)j * frames;
        for (int c = 0; c < frames; c++) {
            const float *w = xp + c * s->hop;
            double a1 = 0.0, a2 = 0.0;
            for (int t = 0; t < n; t++) {
                a1 += (double)fr[t] * w[t];
                a2 += (double)fi[t] * w[t];
            }
            rr[c] = (float)a1;
            ri[c] = (float)a2;
        }
    }
    return frames;
}

int kok_stft_transform_raw(const kok_stft *s, const float *x, int T, float *re,
                           float *im, kok_arena *a) {
    return stft_conv(s, x, T, re, im, a);
}

int kok_stft_transform(const kok_stft *s, const float *x, int T, float *mag,
                       float *phase, kok_arena *a) {
    int frames = stft_conv(s, x, T, mag, phase, a);
    if (frames < 0) return frames;
    for (int j = 0; j < s->K; j++) {
        for (int c = 0; c < frames; c++) {
            double re = mag[(size_t)j * frames + c];
            double im = phase[(size_t)j * frames + c];
            mag[(size_t)j * frames + c] = (float)sqrt(re * re + im * im);
            /* torch.angle gives +pi where im == 0 and re < 0 */
            float ph;
            if (im == 0.0 && re < 0.0) ph = (float)3.14159265358979323846;
            else ph = atan2f((float)im, (float)re);
            phase[(size_t)j * frames + c] = ph;
        }
    }
    return frames;
}

int kok_stft_inverse(const kok_stft *s, const float *mag, const float *phase,
                     int frames, float *out, long *out_len, kok_arena *a) {
    const int n = s->n;
    float *z = kok_arena_alloc(a, (size_t)2 * s->K * frames);
    float *ones = kok_arena_alloc(a, (size_t)frames);
    if (!z || !ones) return -1;
    for (int j = 0; j < s->K; j++) {
        for (int c = 0; c < frames; c++) {
            float m = mag[(size_t)j * frames + c];
            float ph = phase[(size_t)j * frames + c];
            z[(size_t)j * frames + c] = m * cosf(ph);
            z[(size_t)(s->K + j) * frames + c] = m * sinf(ph);
        }
    }
    for (int c = 0; c < frames; c++) ones[c] = 1.0f;
    int total = (frames - 1) * s->hop + n;
    float *y = kok_arena_alloc(a, (size_t)total);
    float *env = kok_arena_alloc(a, (size_t)total);
    if (!y || !env) return -1;
    /* ConvTranspose1d over the inverse bases, then the same for the envelope */
    for (int i = 0; i < total; i++) { y[i] = 0.0f; env[i] = 0.0f; }
    for (int j = 0; j < 2 * s->K; j++) {
        const float *w = s->inv + (size_t)j * n;
        const float *row = z + (size_t)j * frames;
        for (int c = 0; c < frames; c++) {
            float v = row[c];
            if (v == 0.0f) continue;
            int base = c * s->hop;
            for (int t = 0; t < n; t++) y[base + t] += v * w[t];
        }
    }
    for (int c = 0; c < frames; c++) {
        int base = c * s->hop;
        for (int t = 0; t < n; t++) env[base + t] += s->w2[t];
    }
    long lo = n / 2, hi = total - n / 2;
    for (long i = lo; i < hi; i++) {
        float e = env[i];
        out[i - lo] = y[i] / (e > 1e-8f ? e : 1e-8f);
    }
    *out_len = hi - lo;
    return 0;
}

void kok_upsample_nearest2(float *out, const float *x, int C, int T) {
    for (int c = 0; c < C; c++) {
        const float *xr = x + (size_t)c * T;
        float *orow = out + (size_t)c * 2 * T;
        for (int t = 0; t < T; t++) {
            orow[2 * t] = xr[t];
            orow[2 * t + 1] = xr[t];
        }
    }
}

int kok_res1_forward(float *out, const float *x, const kok_res1 *b, const float *s,
                     int style_dim, int T, kok_arena *a) {
    const int C = b->channels, K = b->kernel;
    float *xt = kok_arena_alloc(a, (size_t)C * T);
    float *acc = kok_arena_alloc(a, (size_t)C * T);
    if (!xt || !acc) return -1;
    memcpy(acc, x, (size_t)C * T * sizeof(float));
    for (int i = 0; i < 3; i++) {
        int dil = (i == 0) ? 1 : (i == 1) ? 3 : 5;
        int pad = (K - 1) * dil / 2;
        kok_adain1d(xt, acc, C, T, s, style_dim, b->a1[i].fc_w, b->a1[i].fc_b,
                    b->a1[i].norm_w, b->a1[i].norm_b, 1e-5f);
        kok_snake(xt, b->alpha1[i], C, T);
        kok_conv1d(out, xt, b->c1w[i], b->c1b[i], C, C, T, K, 1, pad, dil);
        kok_adain1d(xt, out, C, T, s, style_dim, b->a2[i].fc_w, b->a2[i].fc_b,
                    b->a2[i].norm_w, b->a2[i].norm_b, 1e-5f);
        kok_snake(xt, b->alpha2[i], C, T);
        kok_conv1d(out, xt, b->c2w[i], b->c2b[i], C, C, T, K, 1, K / 2, 1);
        for (long j = 0; j < (long)C * T; j++) acc[j] += out[j];
    }
    memcpy(out, acc, (size_t)C * T * sizeof(float));
    return 0;
}