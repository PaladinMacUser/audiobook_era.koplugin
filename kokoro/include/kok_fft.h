/* kok_fft.h -- iterative radix-2 complex FFT for the phase lock.
 *
 * The lock's 1024-point analysis is the one place in the runtime where the
 * DFT-as-convolution design (which keeps the port simple and exact) is also the
 * dominant cost: 512 basis rows times 1024 taps per frame, against an FFT's
 * O(n log n). The transform is the same DFT either way, so the results are
 * unchanged to float32 precision.
 *
 * Twiddles are precomputed once; the caller supplies the storage. The iterative
 * structure follows sanotts/mcu/src/snt_nano.c's fft_inv (MIT).
 */
#ifndef KOK_FFT_H
#define KOK_FFT_H

/* Floats needed for the twiddle table: n. */
long kok_fft_mem_floats(int n);

typedef struct {
    int n;
    float *tw_re;   /* cos(-2*pi*k/n), k = 0..n/2-1 */
    float *tw_im;   /* sin(-2*pi*k/n) */
} kok_fft;

int kok_fft_init(kok_fft *f, int n, float *mem);

/* Unnormalised forward transform, in place. */
void kok_fft_forward(const kok_fft *f, float *re, float *im);

/* Inverse transform with the 1/n normalisation, in place. */
void kok_fft_inverse(const kok_fft *f, float *re, float *im);

#endif