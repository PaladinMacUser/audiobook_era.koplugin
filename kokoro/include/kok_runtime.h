/* kok_runtime.h -- Kokoro-family runtime entry points.
 *
 * The text half turns phoneme ids into the five things the decoder needs:
 * per-token durations, the prosody features, the aligned F0 and N curves, and
 * the 512-channel asr tokens. Every weight is resolved and validated once at
 * bind time (kok_text_w_bind), so a mismatch fails at startup, not mid-sentence.
 */
#ifndef KOK_RUNTIME_H
#define KOK_RUNTIME_H

#include "kok_kernels.h"
#include "kok_meta.h"

#define KOK_MAX_BLOCKS 8

typedef struct {
    const float *w_ih, *w_hh, *b_ih, *b_hh;
    const float *w_ih_r, *w_hh_r, *b_ih_r, *b_hh_r;
} kok_bilstm_w;

typedef struct {
    const float *fc_w, *fc_b;         /* AdaLayerNorm(style -> 2C) */
} kok_adaln_w;

typedef struct {
    /* ALBERT: one shared layer applied albert_layers times */
    const float *word_emb, *pos_emb, *tok_emb, *emb_ln_w, *emb_ln_b;
    const float *map_w, *map_b;
    const float *q_w, *q_b, *k_w, *k_b, *v_w, *v_b;
    const float *dense_w, *dense_b, *att_ln_w, *att_ln_b;
    const float *ffn_w, *ffn_b, *ffn_out_w, *ffn_out_b, *full_ln_w, *full_ln_b;
    /* BERT -> prosody projection */
    const float *be_w, *be_b;
    /* duration encoder: alternating biLSTM and AdaLayerNorm */
    int de_layers;
    struct { kok_bilstm_w lstm; kok_adaln_w adaln; } de[KOK_MAX_BLOCKS];
    /* prosody predictor */
    kok_bilstm_w pros_lstm, pros_shared;
    const float *dur_w, *dur_b;
    kok_arb f0_blk[3], n_blk[3];
    const float *f0_proj_w, *f0_proj_b, *n_proj_w, *n_proj_b;
    const float *style_text;
    /* text encoder */
    const float *te_emb;
    int te_depth;
    struct { const float *w, *b, *ln_w, *ln_b; } te_cnn[KOK_MAX_BLOCKS];
    kok_bilstm_w te_lstm;
    /* asr projection: Linear -> GELU -> Linear */
    const float *ap0_w, *ap0_b, *ap2_w, *ap2_b;
} kok_text_w;

typedef struct {
    long n_tokens;
    long n_frames;        /* sum of durations */
    float *dur_raw;       /* [n_tokens] */
    long *durs;           /* [n_tokens] */
    float *d;             /* [n_tokens, d_hid + style] time-major */
    float *asr_tok;       /* [asr_dim, n_tokens] */
    /* the aligned stages (filled when the caller asks for them) */
    float *en;            /* [d_hid + style, n_frames] */
    float *asr;           /* [asr_dim, n_frames] */
    float *f0;            /* [2 * n_frames] */
    float *nz;            /* [2 * n_frames] */
} kok_text_out;

/* ---- decoder half ---- */
typedef struct {
    kok_arb encode;                 /* (asr_dim + 2) -> width */
    kok_arb decode[4];              /* the last one upsampling */
    const float *f0_conv_w, *f0_conv_b, *n_conv_w, *n_conv_b;
    const float *asr_res_w, *asr_res_b;
    const float *style;
    /* generator */
    kok_res1 noise_res[2];
    kok_res1 resblocks[6];
    const float *ups_w[2], *ups_b[2];
    const float *noise_w[2], *noise_b[2];
    const float *conv_post_w, *conv_post_b;
    const float *src_w, *src_b;      /* the harmonic source's l_linear */
    /* dims */
    int width, uic, kernels[3], uprate[2], upkern[2], istft_n, istft_hop, style_dim;
    int asr_dim;
} kok_dec_w;

typedef struct {
    kok_meta meta;
    const void *text_blob;
    const void *dec_blob;
    kok_text_w tw;
    kok_dec_w dw;
} kok_model;

int kok_model_init(kok_model *mo, const void *meta_blob, size_t meta_bytes,
                   const void *text_blob, const void *dec_blob);
int kok_text_w_bind(kok_text_w *tw, const kok_meta *m, const void *text_blob);

/* shared internal helper: bind one Kokoro AdainResBlk1d by name prefix */
int kok_bind_arb(kok_arb *b, const kok_meta *m, const void *blob, int blob_id,
                 const char *prefix, int dim_in, int dim_out, int upsample,
                 int style_dim);

int kok_dec_w_bind(kok_dec_w *dw, const kok_meta *m, const void *blob);

/* the 2-8 kHz phase-lock post-filter, exposed for testing */
int kok_lock_apply(const kok_config *c, const float *s, long n, const float *src,
                   const float *f0, long n_f0, float *out, kok_arena *a);

/* Run the decoder half: asr [asr_dim, F], f0 and nz [2F], the harmonic source
 * src [6 * F * F0S] (see kok_dec_forward for the exact length) -> wave. */
int kok_dec_forward(const kok_model *mo, const float *asr, int F, const float *f0,
                    const float *nz, const float *src, float *wave, long *wave_len,
                    float *pre_lock_out, kok_arena *a);

/* The harmonic-source length the decoder expects for F frames. */
long kok_dec_source_floats(const kok_model *mo, int F);

/* Run the text half. ids are phoneme ids (padded at both ends by the caller,
 * as the model expects). mode 0 = durations only, 1 = also the aligned stages. */
int kok_text_forward(const kok_model *mo, const int *ids, long n_tokens, int mode,
                     kok_text_out *out, kok_arena *a);

#endif