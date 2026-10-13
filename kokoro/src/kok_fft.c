/* kok_fft.c -- iterative radix-2 complex FFT (Cooley-Tukey, decimation in time).
 * See kok_fft.h. The bit-reversal and stage loop follow sanotts/mcu/src/
 * snt_nano.c's fft_inv (MIT), with the twiddle table generalised to any power
 * of two and a forward transform added. */
#include <math.h>
#include <string.h>

#include "kok_fft.h"

long kok_fft_mem_floats(int n) { return 2L * n; }

int kok_fft_init(kok_fft *f, int n, float *mem) {
    if (!f || !mem || n < 2 || (n & (n - 1)) != 0) return -1;
    f->n = n;
    f->tw_re = mem;
    f->tw_im = mem + n;
    const double pi = 3.14159265358979323846;
    for (int k = 0; k < n; k++) {
        /* the negative-frequency half is what both directions index into */
        f->tw_re[k] = (float)cos(-2.0 * pi * (double)k / (double)n);
        f->tw_im[k] = (float)sin(-2.0 * pi * (double)k / (double)n);
    }
    return 0;
}

static void fft_core(const kok_fft *f, float *re, float *im, int inverse) {
    const int n = f->n;
    /* bit reversal */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int stride = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                int t = k * stride;
                float wr = f->tw_re[t], wi = inverse ? -f->tw_im[t] : f->tw_im[t];
                int a = i + k, b = a + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
    if (inverse) {
        const float inv = 1.0f / (float)n;
        for (int i = 0; i < n; i++) {
            re[i] *= inv;
            im[i] *= inv;
        }
    }
}

void kok_fft_forward(const kok_fft *f, float *re, float *im) {
    fft_core(f, re, im, 0);
}

void kok_fft_inverse(const kok_fft *f, float *re, float *im) {
    fft_core(f, re, im, 1);
}