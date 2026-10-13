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

typedef struct {
    float *base;
    size_t cap;     /* floats */
    size_t top;     /* floats used */
} kok_arena;

void kok_arena_init(kok_arena *a, void *mem, size_t floats);
void *kok_arena_alloc(kok_arena *a, size_t floats);   /* NULL when exhausted */
void kok_arena_reset(kok_arena *a);
size_t kok_arena_peak(const kok_arena *a);

/* ---- linear algebra ---- */
void kok_conv1d(float *out, const float *x, const float *w, const float *b,
                int Cin, int Cout, int T, int K, int pad, int dil);
void kok_convtr1d(float *out, const float *x, const float *w, const float *b,
                  int Cin, int Cout, int T, int K, int stride, int pad,
                  int out_pad, int groups);
void kok_linear(float *out, const float *x, const float *w, const float *b,
                int Cin, int Cout, int T);

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
    const float *c1_w, *c1_b, *c2_w, *c2_b;
    const float *sc_w;              /* NULL when dim_in == dim_out */
    const float *pool_w, *pool_b;   /* NULL unless upsampling */
    int dim_in, dim_out, upsample;
} kok_arb;

/* Kokoro's AdainResBlk1d: AdaIN -> leaky(0.2) -> [depthwise ConvT upsample] ->
 * conv1 -> AdaIN -> leaky(0.2) -> conv2, plus a nearest-upsampled (and 1x1
 * projected) shortcut, the sum scaled by 1/sqrt(2). x [dim_in, T]. */
int kok_arb_forward(float *out, const float *x, const kok_arb *b,
                    const float *s, int style_dim, int T, kok_arena *a);

#endif