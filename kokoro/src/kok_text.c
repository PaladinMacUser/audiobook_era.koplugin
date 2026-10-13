/* kok_text.c -- the text half: ALBERT, text encoder, prosody (durations, F0,
 * N) and the asr projection. Channel-major [C, T] throughout; see
 * kok_runtime.h and the semantics pinned in kokoro/README.md. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "kok_runtime.h"

#define BIND(dst, name, count)                                                  \
    do {                                                                        \
        int rc_ = 0;                                                            \
        const kok_slot *sl_ = kok_find_n(m, name, (count), &rc_);               \
        if (!sl_) {                                                             \
            fprintf(stderr, "bind: %s (want %ld) rc %d\n", name, (long)(count), rc_); \
            return rc_;                                                         \
        }                                                                       \
        (dst) = kok_f32(sl_, blob);                                             \
        if (!(dst)) { fprintf(stderr, "bind: %s not f32\n", name); return -10; } \
    } while (0)

#define BIND_OPT(dst, name, count)                                              \
    do {                                                                        \
        int rc_ = 0;                                                            \
        const kok_slot *sl_ = kok_find(m, name);                                \
        if (!sl_ || sl_->count != (count)) { (dst) = NULL; }                    \
        else { (dst) = kok_f32(sl_, blob); (void)rc_; }                         \
    } while (0)

static int bind_bilstm(kok_bilstm_w *b, const kok_meta *m, const void *blob,
                       const char *prefix, int cin, int h) {
    char name[256];
    long iw = 4L * h * cin, hw = 4L * h * h, bw = 4L * h;
    snprintf(name, sizeof name, "%s.weight_ih_l0", prefix);
    BIND(b->w_ih, name, iw);
    snprintf(name, sizeof name, "%s.weight_hh_l0", prefix);
    BIND(b->w_hh, name, hw);
    snprintf(name, sizeof name, "%s.bias_ih_l0", prefix);
    BIND(b->b_ih, name, bw);
    snprintf(name, sizeof name, "%s.bias_hh_l0", prefix);
    BIND(b->b_hh, name, bw);
    snprintf(name, sizeof name, "%s.weight_ih_l0_reverse", prefix);
    BIND(b->w_ih_r, name, iw);
    snprintf(name, sizeof name, "%s.weight_hh_l0_reverse", prefix);
    BIND(b->w_hh_r, name, hw);
    snprintf(name, sizeof name, "%s.bias_ih_l0_reverse", prefix);
    BIND(b->b_ih_r, name, bw);
    snprintf(name, sizeof name, "%s.bias_hh_l0_reverse", prefix);
    BIND(b->b_hh_r, name, bw);
    return 0;
}

static int bind_arb(kok_arb *b, const kok_meta *m, const void *blob, const char *prefix,
                    int dim_in, int dim_out, int upsample, int style_dim) {
    char name[256];
    memset(b, 0, sizeof *b);
    b->dim_in = dim_in;
    b->dim_out = dim_out;
    b->upsample = upsample;
    snprintf(name, sizeof name, "%s.norm1.fc.weight", prefix);
    BIND(b->n1_fc_w, name, 2L * dim_in * style_dim);
    snprintf(name, sizeof name, "%s.norm1.fc.bias", prefix);
    BIND(b->n1_fc_b, name, 2L * dim_in);
    snprintf(name, sizeof name, "%s.norm1.norm.weight", prefix);
    BIND(b->n1_norm_w, name, dim_in);
    snprintf(name, sizeof name, "%s.norm1.norm.bias", prefix);
    BIND(b->n1_norm_b, name, dim_in);
    snprintf(name, sizeof name, "%s.norm2.fc.weight", prefix);
    BIND(b->n2_fc_w, name, 2L * dim_out * style_dim);
    snprintf(name, sizeof name, "%s.norm2.fc.bias", prefix);
    BIND(b->n2_fc_b, name, 2L * dim_out);
    snprintf(name, sizeof name, "%s.norm2.norm.weight", prefix);
    BIND(b->n2_norm_w, name, dim_out);
    snprintf(name, sizeof name, "%s.norm2.norm.bias", prefix);
    BIND(b->n2_norm_b, name, dim_out);
    snprintf(name, sizeof name, "%s.conv1.weight", prefix);
    BIND(b->c1_w, name, 3L * dim_out * dim_in);
    snprintf(name, sizeof name, "%s.conv1.bias", prefix);
    BIND(b->c1_b, name, dim_out);
    snprintf(name, sizeof name, "%s.conv2.weight", prefix);
    BIND(b->c2_w, name, 3L * dim_out * dim_out);
    snprintf(name, sizeof name, "%s.conv2.bias", prefix);
    BIND(b->c2_b, name, dim_out);
    if (dim_in != dim_out) {
        snprintf(name, sizeof name, "%s.conv1x1.weight", prefix);
        BIND(b->sc_w, name, (long)dim_out * dim_in);
    }
    if (upsample) {
        snprintf(name, sizeof name, "%s.pool.weight", prefix);
        BIND(b->pool_w, name, 3L * dim_in);
        snprintf(name, sizeof name, "%s.pool.bias", prefix);
        BIND(b->pool_b, name, dim_in);
    }
    return 0;
}

int kok_text_w_bind(kok_text_w *tw, const kok_meta *m, const void *blob) {
    const kok_config *c = &m->cfg;
    const int C = c->v[KOK_D_ALBERT_HIDDEN];
    const int L = c->v[KOK_D_ALBERT_LAYERS];
    const int inter = c->v[KOK_D_ALBERT_INTERMEDIATE];
    const int emb = c->v[KOK_D_ALBERT_EMBED];
    const int pos = c->v[KOK_D_ALBERT_MAX_POS];
    const int tok = c->v[KOK_D_N_TOKEN];
    const int sd = c->v[KOK_D_TEXT_STYLE_DIM];
    const int dhid = c->v[KOK_D_PROSODY_D_HID];
    const int nlay = c->v[KOK_D_PROSODY_NLAYERS];
    const int maxdur = c->v[KOK_D_PROSODY_MAX_DUR];
    const int tech = c->v[KOK_D_TEXTENC_CHANNELS];
    const int ted = c->v[KOK_D_TEXTENC_DEPTH];
    const int tek = c->v[KOK_D_TEXTENC_KERNEL];
    const int asr = c->v[KOK_D_ASR_DIM];
    char name[256];
    int i;

    if (L > KOK_MAX_BLOCKS || nlay > KOK_MAX_BLOCKS || ted > KOK_MAX_BLOCKS) return -11;

    BIND(tw->word_emb, "bert.embeddings.word_embeddings.weight", (long)tok * emb);
    BIND(tw->pos_emb, "bert.embeddings.position_embeddings.weight", (long)pos * emb);
    BIND(tw->tok_emb, "bert.embeddings.token_type_embeddings.weight", 2L * emb);
    BIND(tw->emb_ln_w, "bert.embeddings.LayerNorm.weight", emb);
    BIND(tw->emb_ln_b, "bert.embeddings.LayerNorm.bias", emb);
    BIND(tw->map_w, "bert.encoder.embedding_hidden_mapping_in.weight", (long)C * emb);
    BIND(tw->map_b, "bert.encoder.embedding_hidden_mapping_in.bias", C);

    const char *lp = "bert.encoder.albert_layer_groups.0.albert_layers.0";
    snprintf(name, sizeof name, "%s.attention.query.weight", lp); BIND(tw->q_w, name, (long)C * C);
    snprintf(name, sizeof name, "%s.attention.query.bias", lp); BIND(tw->q_b, name, C);
    snprintf(name, sizeof name, "%s.attention.key.weight", lp); BIND(tw->k_w, name, (long)C * C);
    snprintf(name, sizeof name, "%s.attention.key.bias", lp); BIND(tw->k_b, name, C);
    snprintf(name, sizeof name, "%s.attention.value.weight", lp); BIND(tw->v_w, name, (long)C * C);
    snprintf(name, sizeof name, "%s.attention.value.bias", lp); BIND(tw->v_b, name, C);
    snprintf(name, sizeof name, "%s.attention.dense.weight", lp); BIND(tw->dense_w, name, (long)C * C);
    snprintf(name, sizeof name, "%s.attention.dense.bias", lp); BIND(tw->dense_b, name, C);
    snprintf(name, sizeof name, "%s.attention.LayerNorm.weight", lp); BIND(tw->att_ln_w, name, C);
    snprintf(name, sizeof name, "%s.attention.LayerNorm.bias", lp); BIND(tw->att_ln_b, name, C);
    snprintf(name, sizeof name, "%s.ffn.weight", lp); BIND(tw->ffn_w, name, (long)inter * C);
    snprintf(name, sizeof name, "%s.ffn.bias", lp); BIND(tw->ffn_b, name, inter);
    snprintf(name, sizeof name, "%s.ffn_output.weight", lp); BIND(tw->ffn_out_w, name, (long)C * inter);
    snprintf(name, sizeof name, "%s.ffn_output.bias", lp); BIND(tw->ffn_out_b, name, C);
    snprintf(name, sizeof name, "%s.full_layer_layer_norm.weight", lp); BIND(tw->full_ln_w, name, C);
    snprintf(name, sizeof name, "%s.full_layer_layer_norm.bias", lp); BIND(tw->full_ln_b, name, C);

    BIND(tw->be_w, "bert_encoder.weight", (long)dhid * C);
    BIND(tw->be_b, "bert_encoder.bias", dhid);

    tw->de_layers = nlay;
    for (i = 0; i < nlay; i++) {
        snprintf(name, sizeof name, "predictor.text_encoder.lstms.%d", 2 * i);
        int rc = bind_bilstm(&tw->de[i].lstm, m, blob, name, dhid + sd, dhid / 2);
        if (rc) return rc;
        snprintf(name, sizeof name, "predictor.text_encoder.lstms.%d.fc.weight", 2 * i + 1);
        BIND(tw->de[i].adaln.fc_w, name, 2L * dhid * sd);
        snprintf(name, sizeof name, "predictor.text_encoder.lstms.%d.fc.bias", 2 * i + 1);
        BIND(tw->de[i].adaln.fc_b, name, 2L * dhid);
    }

    i = bind_bilstm(&tw->pros_lstm, m, blob, "predictor.lstm", dhid + sd, dhid / 2);
    if (i) return i;
    i = bind_bilstm(&tw->pros_shared, m, blob, "predictor.shared", dhid + sd, dhid / 2);
    if (i) return i;
    BIND(tw->dur_w, "predictor.duration_proj.linear_layer.weight", (long)maxdur * dhid);
    BIND(tw->dur_b, "predictor.duration_proj.linear_layer.bias", maxdur);

    for (i = 0; i < 3; i++) {
        /* F0/N heads: AdainResBlk1d(d_hid, d_hid), (d_hid, d_hid/2, upsample),
         * (d_hid/2, d_hid/2) */
        int din = (i == 2) ? dhid / 2 : dhid;
        int dout = (i == 0) ? dhid : dhid / 2;
        snprintf(name, sizeof name, "predictor.F0.%d", i);
        int rc = bind_arb(&tw->f0_blk[i], m, blob, name, din, dout, i == 1, sd);
        if (rc) return rc;
        snprintf(name, sizeof name, "predictor.N.%d", i);
        rc = bind_arb(&tw->n_blk[i], m, blob, name, din, dout, i == 1, sd);
        if (rc) return rc;
    }
    BIND(tw->f0_proj_w, "predictor.F0_proj.weight", dhid / 2);
    BIND(tw->f0_proj_b, "predictor.F0_proj.bias", 1);
    BIND(tw->n_proj_w, "predictor.N_proj.weight", dhid / 2);
    BIND(tw->n_proj_b, "predictor.N_proj.bias", 1);
    BIND(tw->style_text, "style", sd);

    BIND(tw->te_emb, "text_encoder.embedding.weight", (long)tok * tech);
    tw->te_depth = ted;
    for (i = 0; i < ted; i++) {
        snprintf(name, sizeof name, "text_encoder.cnn.%d.0.weight", i);
        BIND(tw->te_cnn[i].w, name, (long)tech * tech * tek);
        snprintf(name, sizeof name, "text_encoder.cnn.%d.0.bias", i);
        BIND(tw->te_cnn[i].b, name, tech);
        snprintf(name, sizeof name, "text_encoder.cnn.%d.1.gamma", i);
        BIND(tw->te_cnn[i].ln_w, name, tech);
        snprintf(name, sizeof name, "text_encoder.cnn.%d.1.beta", i);
        BIND(tw->te_cnn[i].ln_b, name, tech);
    }
    i = bind_bilstm(&tw->te_lstm, m, blob, "text_encoder.lstm", tech, tech / 2);
    if (i) return i;

    BIND(tw->ap0_w, "asr_proj.0.weight", (long)asr * tech);
    BIND(tw->ap0_b, "asr_proj.0.bias", asr);
    BIND(tw->ap2_w, "asr_proj.2.weight", (long)asr * asr);
    BIND(tw->ap2_b, "asr_proj.2.bias", asr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* modules                                                            */
/* ------------------------------------------------------------------ */
static int albert_layer(const kok_text_w *tw, const kok_config *c, float *x, int T,
                        kok_arena *a) {
    const int C = c->v[KOK_D_ALBERT_HIDDEN];
    const int heads = c->v[KOK_D_ALBERT_HEADS];
    const int inter = c->v[KOK_D_ALBERT_INTERMEDIATE];
    const int hs = C / heads;
    float *q = kok_arena_alloc(a, (size_t)C * T);
    float *k = kok_arena_alloc(a, (size_t)C * T);
    float *v = kok_arena_alloc(a, (size_t)C * T);
    float *ctx = kok_arena_alloc(a, (size_t)C * T);
    float *sc = kok_arena_alloc(a, (size_t)T * T);
    float *ff = kok_arena_alloc(a, (size_t)inter * T);
    if (!q || !k || !v || !ctx || !sc || !ff) return -1;

    kok_linear(q, x, tw->q_w, tw->q_b, C, C, T);
    kok_linear(k, x, tw->k_w, tw->k_b, C, C, T);
    kok_linear(v, x, tw->v_w, tw->v_b, C, C, T);
    const float scale = 1.0f / sqrtf((float)hs);
    for (int h = 0; h < heads; h++) {
        const float *qh = q + (size_t)h * hs * T;
        const float *kh = k + (size_t)h * hs * T;
        const float *vh = v + (size_t)h * hs * T;
        for (int i = 0; i < T; i++) {
            for (int j = 0; j < T; j++) {
                double acc = 0.0;
                for (int d = 0; d < hs; d++)
                    acc += (double)qh[(size_t)d * T + i] * kh[(size_t)d * T + j];
                sc[(size_t)i * T + j] = (float)acc * scale;
            }
        }
        kok_softmax(sc, T, T);
        for (int d = 0; d < hs; d++) {
            float *crow = ctx + (size_t)(h * hs + d) * T;
            const float *vrow = vh + (size_t)d * T;
            for (int i = 0; i < T; i++) {
                double acc = 0.0;
                const float *srow = sc + (size_t)i * T;
                for (int j = 0; j < T; j++) acc += (double)srow[j] * vrow[j];
                crow[i] = (float)acc;
            }
        }
    }
    /* dense + residual LayerNorm */
    kok_linear(ff, ctx, tw->dense_w, tw->dense_b, C, C, T);
    for (long i = 0; i < (long)C * T; i++) x[i] += ff[i];
    kok_layer_norm(x, tw->att_ln_w, tw->att_ln_b, C, T, 1e-12f);

    /* feed-forward with gelu_new, then the full-layer LayerNorm */
    kok_linear(ff, x, tw->ffn_w, tw->ffn_b, C, inter, T);
    kok_gelu_new(ff, (long)inter * T);
    kok_linear(ctx, ff, tw->ffn_out_w, tw->ffn_out_b, inter, C, T);
    for (long i = 0; i < (long)C * T; i++) x[i] += ctx[i];
    kok_layer_norm(x, tw->full_ln_w, tw->full_ln_b, C, T, 1e-12f);
    return 0;
}

static int albert_forward(const kok_text_w *tw, const kok_config *c, const int *ids,
                          int T, float *out, kok_arena *a) {
    const int C = c->v[KOK_D_ALBERT_HIDDEN];
    const int emb = c->v[KOK_D_ALBERT_EMBED];
    float *h = kok_arena_alloc(a, (size_t)C * T);
    if (!h) return -1;
    for (int t = 0; t < T; t++) {
        const float *we = tw->word_emb + (size_t)ids[t] * emb;
        const float *pe = tw->pos_emb + (size_t)t * emb;
        for (int d = 0; d < emb; d++) h[d * T + t] = we[d] + pe[d] + tw->tok_emb[d];
    }
    kok_layer_norm(h, tw->emb_ln_w, tw->emb_ln_b, emb, T, 1e-12f);
    kok_linear(out, h, tw->map_w, tw->map_b, emb, C, T);
    /* each application of the shared block needs the same scratch, so release
     * it between layers while keeping the caller's outer allocations */
    size_t mark = a->top;
    for (int l = 0; l < c->v[KOK_D_ALBERT_LAYERS]; l++) {
        int rc = albert_layer(tw, c, out, T, a);
        if (rc) return rc;
        a->top = mark;
    }
    return 0;
}

static int textenc_forward(const kok_text_w *tw, const kok_config *c, const int *ids,
                           int T, float *out, kok_arena *a) {
    const int ch = c->v[KOK_D_TEXTENC_CHANNELS];
    const int depth = c->v[KOK_D_TEXTENC_DEPTH];
    const int K = c->v[KOK_D_TEXTENC_KERNEL];
    const int pad = (K - 1) / 2;
    float *buf = kok_arena_alloc(a, (size_t)ch * T);
    if (!buf) return -1;
    for (int t = 0; t < T; t++) {
        const float *e = tw->te_emb + (size_t)ids[t] * ch;
        for (int d = 0; d < ch; d++) out[d * T + t] = e[d];
    }
    for (int i = 0; i < depth; i++) {
        kok_conv1d(buf, out, tw->te_cnn[i].w, tw->te_cnn[i].b, ch, ch, T, K, pad, 1);
        kok_layer_norm(buf, tw->te_cnn[i].ln_w, tw->te_cnn[i].ln_b, ch, T, 1e-5f);
        kok_leaky_relu(buf, (long)ch * T, 0.2f);
        memcpy(out, buf, (size_t)ch * T * sizeof(float));
    }
    /* the input to the LSTM is [T, ch]; the bilstm helper wants [ch, T] */
    float *x2 = kok_arena_alloc(a, (size_t)ch * T);
    if (!x2) return -1;
    memcpy(x2, out, (size_t)ch * T * sizeof(float));
    kok_lstm_w f = {tw->te_lstm.w_ih, tw->te_lstm.w_hh, tw->te_lstm.b_ih, tw->te_lstm.b_hh};
    kok_lstm_w b = {tw->te_lstm.w_ih_r, tw->te_lstm.w_hh_r, tw->te_lstm.b_ih_r, tw->te_lstm.b_hh_r};
    return kok_bilstm(out, x2, ch, T, ch / 2, &f, &b, a);
}

static int duration_encoder_forward(const kok_text_w *tw, const kok_config *c,
                                    const float *bert_out, int T, float *out,
                                    kok_arena *a) {
    const int dhid = c->v[KOK_D_PROSODY_D_HID];
    const int sd = c->v[KOK_D_TEXT_STYLE_DIM];
    const int nlay = c->v[KOK_D_PROSODY_NLAYERS];
    const float *s = tw->style_text;
    int cur = dhid, i;
    float *buf = kok_arena_alloc(a, (size_t)(dhid + sd) * T);
    float *tmp = kok_arena_alloc(a, (size_t)(dhid + sd) * T);
    if (!buf || !tmp) return -1;

    /* the input to the duration encoder is the BERT projection (dhid wide) */
    float *x = kok_arena_alloc(a, (size_t)(dhid + sd) * T);
    if (!x) return -1;
    for (int t = 0; t < T; t++)
        for (int d = 0; d < dhid; d++) x[d * T + t] = bert_out[d * T + t];
    for (int d = 0; d < sd; d++)
        for (int t = 0; t < T; t++) x[(size_t)(dhid + d) * T + t] = s[d];

    for (i = 0; i < nlay; i++) {
        kok_lstm_w f = {tw->de[i].lstm.w_ih, tw->de[i].lstm.w_hh, tw->de[i].lstm.b_ih, tw->de[i].lstm.b_hh};
        kok_lstm_w b = {tw->de[i].lstm.w_ih_r, tw->de[i].lstm.w_hh_r, tw->de[i].lstm.b_ih_r, tw->de[i].lstm.b_hh_r};
        int rc = kok_bilstm(buf, x, dhid + sd, T, dhid / 2, &f, &b, a);
        if (rc) return rc;
        /* AdaLayerNorm then re-concatenate the style vector */
        kok_ada_layer_norm(tmp, buf, dhid, T, s, sd, tw->de[i].adaln.fc_w,
                           tw->de[i].adaln.fc_b, 1e-5f);
        memcpy(x, tmp, (size_t)dhid * T * sizeof(float));
        for (int d = 0; d < sd; d++)
            for (int t = 0; t < T; t++) x[(size_t)(dhid + d) * T + t] = s[d];
        cur = dhid + sd;
    }
    memcpy(out, x, (size_t)cur * T * sizeof(float));
    return 0;
}

static int durations_forward(const kok_text_w *tw, const kok_config *c, const float *d,
                             int T, float *dur_raw, long *durs, float speed, kok_arena *a) {
    const int dhid = c->v[KOK_D_PROSODY_D_HID];
    const int sd = c->v[KOK_D_TEXT_STYLE_DIM];
    const int maxdur = c->v[KOK_D_PROSODY_MAX_DUR];
    float *h = kok_arena_alloc(a, (size_t)dhid * T);
    float *logits = kok_arena_alloc(a, (size_t)maxdur * T);
    if (!h || !logits) return -1;
    kok_lstm_w f = {tw->pros_lstm.w_ih, tw->pros_lstm.w_hh, tw->pros_lstm.b_ih, tw->pros_lstm.b_hh};
    kok_lstm_w b = {tw->pros_lstm.w_ih_r, tw->pros_lstm.w_hh_r, tw->pros_lstm.b_ih_r, tw->pros_lstm.b_hh_r};
    int rc = kok_bilstm(h, d, dhid + sd, T, dhid / 2, &f, &b, a);
    if (rc) return rc;
    kok_linear(logits, h, tw->dur_w, tw->dur_b, dhid, maxdur, T);
    kok_sigmoid(logits, (long)maxdur * T);
    for (int t = 0; t < T; t++) {
        double acc = 0.0;
        for (int k = 0; k < maxdur; k++) acc += logits[(size_t)k * T + t];
        dur_raw[t] = (float)acc;
        /* round half to even, like torch.round, then clamp to >= 1 */
        double v = (double)dur_raw[t] / (double)speed;
        double r = floor(v);
        if (v - r > 0.5 || (v - r == 0.5 && fmod(r, 2.0) != 0.0)) r += 1.0;
        if (r < 1.0) r = 1.0;
        durs[t] = (long)r;
    }
    return 0;
}

/* the F0/N heads are also used by the decoder path, so keep this separate */
static int f0n_forward(const kok_text_w *tw, const kok_config *c, const float *en,
                       int F, float *f0, float *nz, kok_arena *a) {
    const int dhid = c->v[KOK_D_PROSODY_D_HID];
    const int sd = c->v[KOK_D_TEXT_STYLE_DIM];
    float *x = kok_arena_alloc(a, (size_t)dhid * F);
    float *ping = kok_arena_alloc(a, (size_t)dhid * 2 * F);
    float *pong = kok_arena_alloc(a, (size_t)dhid * 2 * F);
    if (!x || !ping || !pong) return -1;
    kok_lstm_w fw = {tw->pros_shared.w_ih, tw->pros_shared.w_hh, tw->pros_shared.b_ih, tw->pros_shared.b_hh};
    kok_lstm_w bw = {tw->pros_shared.w_ih_r, tw->pros_shared.w_hh_r, tw->pros_shared.b_ih_r, tw->pros_shared.b_hh_r};
    int rc = kok_bilstm(x, en, dhid + sd, F, dhid / 2, &fw, &bw, a);
    if (rc) return rc;
    for (int head = 0; head < 2; head++) {
        const kok_arb *blks = head ? tw->n_blk : tw->f0_blk;
        float *cur = x;
        int ch = dhid, len = F, alt = 0;
        for (int i = 0; i < 3; i++) {
            float *dst = alt ? pong : ping;
            rc = kok_arb_forward(dst, cur, &blks[i], tw->style_text, sd, len, a);
            if (rc) return rc;
            cur = dst;
            ch = blks[i].dim_out;
            len = blks[i].upsample ? 2 * len : len;
            alt = !alt;
        }
        const float *w = head ? tw->n_proj_w : tw->f0_proj_w;
        const float *bias = head ? tw->n_proj_b : tw->f0_proj_b;
        float *dst = head ? nz : f0;
        for (int t = 0; t < len; t++) {
            double acc = bias[0];
            for (int i = 0; i < ch; i++) acc += (double)w[i] * cur[(size_t)i * len + t];
            dst[t] = (float)acc;
        }
    }
    return 0;
}

static int asr_proj_forward(const kok_text_w *tw, const kok_config *c, const float *t_en,
                            int T, float *out, kok_arena *a) {
    const int tech = c->v[KOK_D_TEXTENC_CHANNELS];
    const int asr = c->v[KOK_D_ASR_DIM];
    float *mid = kok_arena_alloc(a, (size_t)asr * T);
    if (!mid) return -1;
    kok_linear(mid, t_en, tw->ap0_w, tw->ap0_b, tech, asr, T);
    kok_gelu_erf(mid, (long)asr * T);
    kok_linear(out, mid, tw->ap2_w, tw->ap2_b, asr, asr, T);
    return 0;
}

int kok_model_init(kok_model *mo, const void *meta_blob, size_t meta_bytes,
                   const void *text_blob, const void *dec_blob) {
    int rc;
    if (!mo) return -1;
    memset(mo, 0, sizeof *mo);
    rc = kok_meta_init(&mo->meta, meta_blob, meta_bytes);
    if (rc) return rc;
    mo->text_blob = text_blob;
    mo->dec_blob = dec_blob;
    return kok_text_w_bind(&mo->tw, &mo->meta, text_blob);
}

int kok_text_forward(const kok_model *mo, const int *ids, long n_tokens, int mode,
                     kok_text_out *out, kok_arena *a) {
    const kok_config *c = &mo->meta.cfg;
    const kok_text_w *tw = &mo->tw;
    const int C = c->v[KOK_D_ALBERT_HIDDEN];
    const int dhid = c->v[KOK_D_PROSODY_D_HID];
    const int sd = c->v[KOK_D_TEXT_STYLE_DIM];
    const int asr = c->v[KOK_D_ASR_DIM];
    const int T = (int)n_tokens;
    float *bert_out = kok_arena_alloc(a, (size_t)C * T);
    float *d_en = kok_arena_alloc(a, (size_t)dhid * T);
    float *t_en = kok_arena_alloc(a, (size_t)c->v[KOK_D_TEXTENC_CHANNELS] * T);
    float *d = kok_arena_alloc(a, (size_t)(dhid + sd) * T);
    int rc;

    if (!bert_out || !d_en || !t_en || !d) return -1;
    rc = albert_forward(tw, c, ids, T, bert_out, a);
    if (rc) return rc;
    kok_linear(d_en, bert_out, tw->be_w, tw->be_b, C, dhid, T);
    rc = duration_encoder_forward(tw, c, d_en, T, d, a);
    if (rc) return rc;
    rc = durations_forward(tw, c, d, T, out->dur_raw, out->durs, 1.0f, a);
    if (rc) return rc;
    rc = textenc_forward(tw, c, ids, T, t_en, a);
    if (rc) return rc;
    rc = asr_proj_forward(tw, c, t_en, T, out->asr_tok, a);
    if (rc) return rc;
    for (int t = 0; t < T; t++)
        for (int i = 0; i < dhid + sd; i++)
            out->d[(size_t)t * (dhid + sd) + i] = d[(size_t)i * T + t];
    out->n_tokens = n_tokens;

    if (mode == 0) return 0;

    /* alignment expansion, then the F0/N heads and the expanded asr */
    long F = 0;
    for (int t = 0; t < T; t++) F += out->durs[t];
    out->n_frames = F;
    float *en = out->en;
    for (long i = 0; i < (long)(dhid + sd) * F; i++) en[i] = 0.0f;
    {
        long pos = 0;
        for (int t = 0; t < T; t++) {
            for (long k = 0; k < out->durs[t]; k++, pos++)
                for (int i = 0; i < dhid + sd; i++)
                    en[(size_t)i * F + pos] = d[(size_t)i * T + t];
        }
    }
    rc = f0n_forward(tw, c, en, (int)F, out->f0, out->nz, a);
    if (rc) return rc;
    for (long i = 0; i < (long)asr * F; i++) out->asr[i] = 0.0f;
    {
        long pos = 0;
        for (int t = 0; t < T; t++) {
            for (long k = 0; k < out->durs[t]; k++, pos++)
                for (int i = 0; i < asr; i++)
                    out->asr[(size_t)i * F + pos] = out->asr_tok[(size_t)i * T + t];
        }
    }
    return 0;
}