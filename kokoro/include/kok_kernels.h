/* kok_kernels.h -- portable C99 primitives for the Kokoro-family runtime.
 *
 * Channel-major layout throughout: activations are [C, T] (row per channel,
 * contiguous in time), matching the numpy reference and the sanoTTS kernels.
 * Convolutions follow PyTorch: x [Cin, T], w [Cout, Cin/groups, K] for Conv1d
 * and w [Cin, Cout/groups, K] for ConvTranspose1d, with explicit zero padding.
 *
 * No dynamic allocation and no VLAs: every function that needs scratch takes a
 * caller arena (kok_arena), the same discipline as sanotts/mcu.
 */
#ifndef KOK_KERNELS_H
#define KOK_KERNELS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    float *base;
    size_t cap;     /* floats */
    size_t top;     /* floats used */
} kok_arena;

void kok_arena_init(kok_arena *a, void *mem, size_t floats);
void *kok_arena_alloc(kok_arena *a, size_t floats);   /* NULL when exhausted */
void kok_arena_reset(kok_arena *a);
size_t kok_arena_peak(const kok_arena *a);

/* A weight tensor in either precision: fp32, or per-output-channel symmetric
 * int8 with its scales. The int8 form carries one scale per output channel
 * (per_row) or a single one for the whole tensor (ConvTranspose1d, whose dim 0
 * is the input channel). The kernels branch once per call, not per tap, and the
 * int8 path applies the row's scale to the accumulated output row at the end,
 * which is what weight-only quantisation means: no activation quantisation. */
typedef struct {
    const float *f;      /* fp32 weights, NULL when quantised */
    const int8_t *q;     /* int8 weights, NULL when fp32 */
    const float *scale;  /* per-row when per_row, else a single value */
    int per_row;
} kok_w;

/* ---- linear algebra ---- */

/* Convolution over dynamically quantised activations. The input is scanned
 * once for a per-input-channel absmax, quantised to int8 on that scale, run
 * through the integer kernels with per-output-channel int8 weights, and the
 * channels are combined in fp32 so that each keeps its own scale. The scan
 * costs Cin*T, which is 1/(Cout*K) of the convolution itself, and it adapts to
 * the input instead of needing calibration baked in; per-channel scales are
 * what the accuracy requires, a single scale per tensor does not survive the
 * graph (see the quantised-activation section of kokoro/README.md).
 *
 * xq and tacc (Cin*T int8, Tp int32) are caller-owned scratch. The weights must
 * be the per-output-channel int8 blob. */
void kok_conv1d_aq8(float *restrict out, const float *restrict x,
                    const kok_w *restrict w, const float *restrict b,
                    int Cin, int Cout, int T, int K, int stride, int pad, int dil,
                    int8_t *restrict xq, float *restrict xs, int32_t *restrict tacc);

/* The same scheme for the transposed convolution. The weights are laid out
 * [in, out, K] and carry a scale per output channel, as kok_convtr1d reads
 * them. Both call sites are covered: the decoder's upsampling stages
 * (groups=1, stride 10 and 6 with kernels 20 and 12) and the AdainResBlk
 * depthwise pool upsample (groups=Cin, kernel 3, stride 2). */
/* The same scheme for a projection: x is [Cin, T], the weight [Cout, Cin] with a
 * scale per output channel. */
void kok_linear_aq8(float *restrict out, const float *restrict x,
                    const kok_w *restrict w, const float *restrict b,
                    int Cin, int Cout, int T,
                    int8_t *restrict xq, float *restrict xs, int32_t *restrict tacc);

void kok_convtr1d_aq8(float *restrict out, const float *restrict x,
                      const kok_w *restrict w, const float *restrict b,
                      int Cin, int Cout, int T, int K, int stride, int pad,
                      int out_pad, int groups,
                      int8_t *restrict xq, float *restrict xs, int32_t *restrict tacc);
void kok_conv1d(float *restrict out, const float *restrict x, const kok_w *restrict w,
                const float *restrict b, int Cin, int Cout, int T, int K,
                int stride, int pad, int dil);
void kok_convtr1d(float *restrict out, const float *restrict x,
                  const kok_w *restrict w, const float *restrict b,
                  int Cin, int Cout, int T, int K, int stride, int pad,
                  int out_pad, int groups);
void kok_linear(float *restrict out, const float *restrict x, const kok_w *restrict w,
                const float *restrict b, int Cin, int Cout, int T);

/* ---- normalisation ---- */
void kok_layer_norm(float *x, const float *gamma, const float *beta,
                    int C, int T, float eps);            /* in place */
void kok_inst_norm(float *out, const float *x, const float *gamma,
                   const float *beta, int C, int T, float eps);
void kok_adain1d(float *out, const float *x, int C, int T,
                 const float *s, int style_dim,
                 const float *fc_w, const float *fc_b,
                 const float *gamma, const float *beta, float eps);
void kok_ada_layer_norm(float *out, const float *x, int C, int T,
                        const float *s, int style_dim,
                        const float *fc_w, const float *fc_b, float eps);

/* ---- activations ---- */
void kok_leaky_relu(float *x, long n, float slope);
void kok_gelu_erf(float *x, long n);        /* HF 'gelu', nn.GELU default */
void kok_gelu_new(float *x, long n);        /* HF 'gelu_new' (tanh form) */
void kok_sigmoid(float *x, long n);
void kok_snake(float *x, const float *alpha, int C, int T);
void kok_softmax(float *x, int rows, int len);   /* in place, over the last axis */

/* ---- sequence ---- */
void kok_embed(float *out, const float *table, long id, int C);

typedef struct {
    const float *w_ih, *w_hh, *b_ih, *b_hh;
} kok_lstm_w;

/* One direction: x [Cin, T] -> out [H, T]. reverse follows PyTorch's
 * bidirectional convention (the backward direction's output is aligned to the
 * input index, i.e. the reverse of the order the states are produced). */
int kok_lstm_dir(float *out, const float *x, int Cin, int T, int H,
                 const kok_lstm_w *w, int reverse, kok_arena *a);
/* x [Cin, T] -> out [2H, T]: forward then backward concatenated. */
int kok_bilstm(float *out, const float *x, int Cin, int T, int H,
               const kok_lstm_w *fwd, const kok_lstm_w *bwd, kok_arena *a);

/* ---- composed blocks ---- */
typedef struct {
    const float *n1_fc_w, *n1_fc_b, *n1_norm_w, *n1_norm_b;
    const float *n2_fc_w, *n2_fc_b, *n2_norm_w, *n2_norm_b;
    kok_w c1_w, c2_w;
    const float *c1_b, *c2_b;
    kok_w sc_w;                     /* f == NULL when dim_in == dim_out */
    kok_w pool_w;                   /* f == NULL unless upsampling */
    const float *pool_b;
    int dim_in, dim_out, upsample;
} kok_arb;

/* ------------------------------------------------------------------------- */
/* spectral: the author's ConvSTFT as convolutions over DFT bases             */
/* ------------------------------------------------------------------------- */
typedef struct {
    int n, hop, K;      /* K = number of bins in [bin_lo, bin_hi] */
    int bin_lo;
    float *fwd;         /* [2K, n]: rows 0..K-1 cosine, K..2K-1 -sine, both windowed */
    float *inv;         /* [2K, n]: inverse bases, scaled 1/n with the DC/Nyquist halving */
    float *w2;          /* [n]: squared window, for the overlap envelope */
} kok_stft;

/* Floats needed for the bases: 4K*n + n. */
long kok_stft_mem_floats(int n, int bin_lo, int bin_hi);
int kok_stft_init(kok_stft *s, int n, int hop, int bin_lo, int bin_hi, float *mem);

/* x [T] -> mag, phase [K, frames]; reflect padding like torch.stft(center=True). */
int kok_stft_transform(const kok_stft *s, const float *x, int T, float *mag,
                       float *phase, kok_arena *a);
/* Raw basis convolutions (no magnitude/angle): re, im [K, frames]. The phase
 * lock needs these, not the polar form. */
int kok_stft_transform_raw(const kok_stft *s, const float *x, int T, float *re,
                           float *im, kok_arena *a);
/* mag, phase [K, frames] -> out, with the window-overlap envelope divided out. */
int kok_stft_inverse(const kok_stft *s, const float *mag, const float *phase,
                     int frames, float *out, long *out_len, kok_arena *a);

/* ------------------------------------------------------------------------- */
/* generator blocks                                                           */
/* ------------------------------------------------------------------------- */
typedef struct {
    const float *fc_w, *fc_b, *norm_w, *norm_b;
} kok_adain_w;

/* Kokoro's AdaINResBlock1: 3 branches, dilations 1/3/5, Snake activations,
 * residual outside. */
typedef struct {
    kok_adain_w a1[3], a2[3];
    kok_w c1w[3], c2w[3];
    const float *c1b[3], *c2b[3];
    const float *alpha1[3], *alpha2[3];
    int channels, kernel;
} kok_res1;

/* (out = sum of branches / 3 is done by the caller) */
int kok_res1_forward(float *out, const float *x, const kok_res1 *b, const float *s,
                     int style_dim, int T, kok_arena *a);

/* x [C, T] -> out [C, 2T] by nearest-neighbour duplication (F.interpolate). */
void kok_upsample_nearest2(float *out, const float *x, int C, int T);

/* Kokoro's AdainResBlk1d: AdaIN -> leaky(0.2) -> [depthwise ConvT upsample] ->
 * conv1 -> AdaIN -> leaky(0.2) -> conv2, plus a nearest-upsampled (and 1x1
 * projected) shortcut, the sum scaled by 1/sqrt(2). x [dim_in, T]. */
int kok_arb_forward(float *out, const float *x, const kok_arb *b,
                    const float *s, int style_dim, int T, kok_arena *a);

#endif