#define _POSIX_C_SOURCE 200809L
/* kok_dec.c -- the decoder half: StudentDecoder (AdaIN encode/decode stack,
 * stride-2 F0/N convs, asr skip), the istftnet generator, and the 2-8 kHz
 * phase-lock post-filter. Semantics are the ones pinned in kokoro/README.md.
 *
 * The sine source is stochastic (torch.rand for the harmonics' initial phase,
 * torch.randn for the additive noise). Until the RNG path lands, the caller
 * passes the harmonic source in, which is how the golden fixtures gate the
 * deterministic kernels. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <time.h>

#include "kok_fft.h"
#include "kok_runtime.h"

/* DBINDW: as DBIND, but the destination is a kok_w and may be int8, with its
 * companion <name>.scale slot resolved alongside (blob 1: the decoder). */
#define DBINDW(dst, name, want)                                                 \
    do {                                                                        \
        int rc_ = 0;                                                            \
        const kok_slot *sl_ = kok_find_blob(m, name, 1, (want), &rc_);          \
        if (!sl_) {                                                             \
            fprintf(stderr, "dec bind: %s (want %ld) rc %d\n", name,              \
                    (long)(want), rc_);                                         \
            return rc_;                                                         \
        }                                                                       \
        if (sl_->dtype == KOK_DT_I8) {                                          \
            char sn_[300];                                                      \
            (dst).q = (const int8_t *)(const void *)((const unsigned char *)blob + sl_->offset); \
            (dst).f = NULL;                                                     \
            snprintf(sn_, sizeof sn_, "%s.scale", name);                        \
            int rc2_ = 0;                                                       \
            const kok_slot *ss_ = kok_find_blob(m, sn_, 1, -1, &rc2_);          \
            if (!ss_) { fprintf(stderr, "dec bind: %s missing\n", sn_); return -11; } \
            (dst).scale = kok_f32(ss_, blob);                                    \
            (dst).per_row = ss_->count > 1;                                     \
            if (!(dst).scale) return -10;                                       \
        } else {                                                                \
            (dst).f = kok_f32(sl_, blob);                                       \
            (dst).q = NULL;                                                     \
            (dst).scale = NULL;                                                 \
            (dst).per_row = 0;                                                  \
            if (!(dst).f) return -10;                                           \
        }                                                                       \
    } while (0)

#define DBIND(dst, name, count)                                                 \
    do {                                                                        \
        int rc_ = 0;                                                            \
        const kok_slot *sl_ = kok_find_blob(m, name, 1, (count), &rc_);               \
        if (!sl_) {                                                             \
            fprintf(stderr, "dec bind: %s (want %ld) rc %d\n", name,              \
                    (long)(count), rc_);                                        \
            return rc_;                                                         \
        }                                                                       \
        (dst) = kok_f32(sl_, blob);                                             \
        if (!(dst)) { fprintf(stderr, "dec bind: %s not f32\n", name); return -10; } \
    } while (0)

static int bind_adainw(kok_adain_w *dst, const kok_meta *m, const void *blob,
                       const char *prefix, int channels, int style_dim) {
    char name[256];
    snprintf(name, sizeof name, "%s.fc.weight", prefix);
    DBIND(dst->fc_w, name, 2L * channels * style_dim);
    snprintf(name, sizeof name, "%s.fc.bias", prefix);
    DBIND(dst->fc_b, name, 2L * channels);
    snprintf(name, sizeof name, "%s.norm.weight", prefix);
    DBIND(dst->norm_w, name, channels);
    snprintf(name, sizeof name, "%s.norm.bias", prefix);
    DBIND(dst->norm_b, name, channels);
    return 0;
}

static int bind_res1(kok_res1 *b, const kok_meta *m, const void *blob, const char *prefix,
                     int channels, int kernel) {
    char name[256];
    b->channels = channels;
    b->kernel = kernel;
    for (int i = 0; i < 3; i++) {
        snprintf(name, sizeof name, "%s.adain1.%d", prefix, i);
        int rc = bind_adainw(&b->a1[i], m, blob, name, channels, 16);
        if (rc) return rc;
        snprintf(name, sizeof name, "%s.adain2.%d", prefix, i);
        rc = bind_adainw(&b->a2[i], m, blob, name, channels, 16);
        if (rc) return rc;
        snprintf(name, sizeof name, "%s.convs1.%d.weight", prefix, i);
        DBINDW(b->c1w[i], name, (long)channels * channels * kernel);
        snprintf(name, sizeof name, "%s.convs1.%d.bias", prefix, i);
        DBIND(b->c1b[i], name, channels);
        snprintf(name, sizeof name, "%s.convs2.%d.weight", prefix, i);
        DBINDW(b->c2w[i], name, (long)channels * channels * kernel);
        snprintf(name, sizeof name, "%s.convs2.%d.bias", prefix, i);
        DBIND(b->c2b[i], name, channels);
        snprintf(name, sizeof name, "%s.alpha1.%d", prefix, i);
        DBIND(b->alpha1[i], name, channels);
        snprintf(name, sizeof name, "%s.alpha2.%d", prefix, i);
        DBIND(b->alpha2[i], name, channels);
    }
    return 0;
}

int kok_dec_w_bind(kok_dec_w *dw, const kok_meta *m, const void *blob) {
    const kok_config *c = &m->cfg;
    const int width = c->v[KOK_D_DEC_WIDTH];
    const int uic = c->v[KOK_D_DEC_UIC];
    const int asr = c->v[KOK_D_ASR_DIM];
    const int sd = c->v[KOK_D_DEC_STYLE_DIM];
    char name[256];
    int rc;

    memset(dw, 0, sizeof *dw);
    dw->width = width;
    dw->uic = uic;
    dw->asr_dim = asr;
    dw->style_dim = sd;
    dw->kernels[0] = c->v[KOK_D_DEC_KERNEL0];
    dw->kernels[1] = c->v[KOK_D_DEC_KERNEL1];
    dw->kernels[2] = c->v[KOK_D_DEC_KERNEL2];
    dw->uprate[0] = c->v[KOK_D_DEC_UPRATE0];
    dw->uprate[1] = c->v[KOK_D_DEC_UPRATE1];
    dw->upkern[0] = c->v[KOK_D_DEC_UPKERN0];
    dw->upkern[1] = c->v[KOK_D_DEC_UPKERN1];
    dw->istft_n = c->v[KOK_D_DEC_ISTFT_NFFT];
    dw->istft_hop = c->v[KOK_D_DEC_ISTFT_HOP];

    rc = kok_bind_arb(&dw->encode, m, blob, 1, "encode", asr + 2, width, 0, sd);
    if (rc) return rc;
    for (int i = 0; i < 4; i++) {
        int din = width + 2 + 64;
        int dout = (i == 3) ? uic : width;
        snprintf(name, sizeof name, "decode.%d", i);
        rc = kok_bind_arb(&dw->decode[i], m, blob, 1, name, din, dout, i == 3, sd);
        if (rc) return rc;
    }
    DBINDW(dw->f0_conv_w, "F0_conv.weight", 3);
    DBIND(dw->f0_conv_b, "F0_conv.bias", 1);
    DBINDW(dw->n_conv_w, "N_conv.weight", 3);
    DBIND(dw->n_conv_b, "N_conv.bias", 1);
    DBINDW(dw->asr_res_w, "asr_res.0.weight", 64L * asr);
    DBIND(dw->asr_res_b, "asr_res.0.bias", 64);
    DBIND(dw->style, "style", sd);
    DBINDW(dw->src_w, "generator.m_source.l_linear.weight", c->v[KOK_D_GEN_HARMONICS]);
    DBIND(dw->src_b, "generator.m_source.l_linear.bias", 1);

    for (int i = 0; i < 2; i++) {
        snprintf(name, sizeof name, "generator.noise_res.%d", i);
        rc = bind_res1(&dw->noise_res[i], m, blob, name, uic >> (i + 1), i == 0 ? 7 : 11);
        if (rc) return rc;
        snprintf(name, sizeof name, "generator.ups.%d.weight", i);
        DBINDW(dw->ups_w[i], name, (long)(uic >> i) * (uic >> (i + 1)) * dw->upkern[i]);
        snprintf(name, sizeof name, "generator.ups.%d.bias", i);
        DBIND(dw->ups_b[i], name, uic >> (i + 1));
        if (i == 0) {
            int stride = dw->uprate[1];
            DBINDW(dw->noise_w[i], "generator.noise_convs.0.weight",
                  (long)(uic >> 1) * (c->v[KOK_D_DEC_ISTFT_NFFT] + 2) * (stride * 2));
            DBIND(dw->noise_b[i], "generator.noise_convs.0.bias", uic >> 1);
        } else {
            DBINDW(dw->noise_w[i], "generator.noise_convs.1.weight",
                  (long)(uic >> 2) * (c->v[KOK_D_DEC_ISTFT_NFFT] + 2));
            DBIND(dw->noise_b[i], "generator.noise_convs.1.bias", uic >> 2);
        }
    }
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 3; j++) {
            snprintf(name, sizeof name, "generator.resblocks.%d", i * 3 + j);
            rc = bind_res1(&dw->resblocks[i * 3 + j], m, blob, name, uic >> (i + 1),
                           dw->kernels[j]);
            if (rc) return rc;
        }
    }
    DBINDW(dw->conv_post_w, "generator.conv_post.weight",
          (long)(c->v[KOK_D_DEC_ISTFT_NFFT] + 2) * (uic >> 2) * 7);
    DBIND(dw->conv_post_b, "generator.conv_post.bias", c->v[KOK_D_DEC_ISTFT_NFFT] + 2);
    return 0;
}

long kok_dec_source_floats(const kok_model *mo, int F) {
    const kok_config *c = &mo->meta.cfg;
    long per_frame = (long)c->v[KOK_D_DEC_UPRATE0] * c->v[KOK_D_DEC_UPRATE1] *
                     c->v[KOK_D_DEC_ISTFT_HOP];
    return (long)(2 * F) * per_frame;
}

/* The phase-lock filter: 1024-point analysis at hop 256 over bins 2-8 kHz,
 * voiced frames only. Implemented with a radix-2 FFT rather than the DFT-as-
 * convolution bases the reference uses: it is the same DFT, but the convolution
 * costs 512 basis rows times 1024 taps per frame where the FFT is O(n log n),
 * and this was the lock's dominant cost in the first device benchmark.
 *
 * The arithmetic between the two transforms is the reference's, unchanged:
 *   Em = max(|E|, 1e-6);  rel = S * conj(E) / Em
 *   rel_smoothed = hann(L) * rel        (zero-padded)
 *   Rm = max(|rel_smoothed|, 1e-12);  Sm = |S|;  u = unit(E) * unit(rel_smoothed)
 *   D = (Sm * u - S) * voiced
 * then the change is brought back with an inverse FFT and overlap-added. */
int kok_lock_apply(const kok_config *c, const float *s, long n, const float *src,
                      const float *f0, long n_f0, float *out, kok_arena *a) {
    const int nfft = c->v[KOK_D_LOCK_NFFT], hop = c->v[KOK_D_LOCK_HOP];
    const int L = c->v[KOK_D_LOCK_L];
    const float lo = (float)c->v[KOK_D_LOCK_BAND_LO_HZ], hi = (float)c->v[KOK_D_LOCK_BAND_HI_HZ];
    const float voiced = (float)c->v[KOK_D_LOCK_VOICED_HZ];
    const int sr = c->v[KOK_D_SR];
    const int gen_hop = c->v[KOK_D_DEC_UPRATE0] * c->v[KOK_D_DEC_UPRATE1] * c->v[KOK_D_DEC_ISTFT_HOP];
    const int pad = nfft / 2;
    int bin_lo = (int)ceilf(lo * (float)nfft / (float)sr);
    int bin_hi = (int)ceilf(hi * (float)nfft / (float)sr) - 1;
    const int K = bin_hi - bin_lo + 1;
    int frames = (int)(n / hop) + 1;
    if (K <= 0 || frames <= 0) return -1;

    kok_fft fft;
    float *tw = kok_arena_alloc(a, (size_t)kok_fft_mem_floats(nfft));
    float *win = kok_arena_alloc(a, (size_t)nfft);
    float *sk = kok_arena_alloc(a, (size_t)L);
    float *re = kok_arena_alloc(a, (size_t)nfft);
    float *im = kok_arena_alloc(a, (size_t)nfft);
    if (!tw || !win || !sk || !re || !im || kok_fft_init(&fft, nfft, tw)) return -1;
    {
        const double pi2 = 2.0 * 3.14159265358979323846;
        double sum = 0.0;
        for (int i = 0; i < nfft; i++)
            win[i] = (float)(0.5 - 0.5 * cos(pi2 * (double)i / (double)nfft));
        for (int j = 0; j < L; j++) {
            sk[j] = (float)(0.5 - 0.5 * cos(pi2 * (double)(j + 1) / (double)(L + 2)));
            sum += sk[j];
        }
        for (int j = 0; j < L; j++) sk[j] = (float)(sk[j] / sum);
    }
    const size_t plane = (size_t)K * (size_t)frames;
    float *Sre = kok_arena_alloc(a, plane), *Sim = kok_arena_alloc(a, plane);
    float *Ere = kok_arena_alloc(a, plane), *Eim = kok_arena_alloc(a, plane);
    float *Rre = kok_arena_alloc(a, plane), *Rim = kok_arena_alloc(a, plane);
    float *Dre = kok_arena_alloc(a, plane), *Dim = kok_arena_alloc(a, plane);
    long total = n + nfft;
    float *ola = kok_arena_alloc(a, (size_t)total);
    float *env = kok_arena_alloc(a, (size_t)total);
    if (!Sre || !Sim || !Ere || !Eim || !Rre || !Rim || !Dre || !Dim || !ola || !env)
        return -1;
    for (long i = 0; i < total; i++) { ola[i] = 0.0f; env[i] = 0.0f; }

    /* analysis: one FFT per frame per signal, keeping the band bins */
    for (int t = 0; t < frames; t++) {
        long base = (long)t * hop;
        for (int which = 0; which < 2; which++) {
            const float *sig = which ? src : s;
            for (int i = 0; i < nfft; i++) {
                long idx = base + i - pad;
                if (idx < 0) idx = -idx;                      /* reflect */
                if (idx >= n) idx = 2 * n - 2 - idx;
                re[i] = sig[idx] * win[i];
                im[i] = 0.0f;
            }
            kok_fft_forward(&fft, re, im);
            float *pr = which ? Ere : Sre;
            float *pi = which ? Eim : Sim;
            for (int j = 0; j < K; j++) {
                pr[(size_t)j * frames + t] = re[bin_lo + j];
                pi[(size_t)j * frames + t] = im[bin_lo + j];
            }
        }
    }
    /* rel = S * conj(E) / |E|, then smoothed over L frames */
    for (size_t i = 0; i < plane; i++) {
        float er = Ere[i], ei = Eim[i], sr = Sre[i], si = Sim[i];
        float em = sqrtf(er * er + ei * ei);
        if (em < 1e-6f) em = 1e-6f;
        Rre[i] = (sr * er + si * ei) / em;
        Rim[i] = (si * er - sr * ei) / em;
    }
    {
        const int sp = L / 2;
        for (int j = 0; j < K; j++) {
            for (int t = 0; t < frames; t++) {
                double ar = 0.0, ai = 0.0;
                for (int u = 0; u < L; u++) {
                    int idx = t + u - sp;
                    if (idx < 0 || idx >= frames) continue;
                    ar += (double)sk[u] * Rre[(size_t)j * frames + idx];
                    ai += (double)sk[u] * Rim[(size_t)j * frames + idx];
                }
                Dre[(size_t)j * frames + t] = (float)ar;
                Dim[(size_t)j * frames + t] = (float)ai;
            }
        }
    }
    /* D = (|S| * unit(E) * unit(rel_smoothed) - S) * voiced, per bin and frame */
    for (size_t i = 0; i < plane; i++) {
        float er = Ere[i], ei = Eim[i], sr = Sre[i], si = Sim[i];
        float rr = Dre[i], ri = Dim[i];
        float em = sqrtf(er * er + ei * ei);
        if (em < 1e-6f) em = 1e-6f;
        float rm = sqrtf(rr * rr + ri * ri);
        if (rm < 1e-12f) rm = 1e-12f;
        float sm = sqrtf(sr * sr + si * si);
        float ur = (er * rr - ei * ri) / (em * rm);
        float ui = (er * ri + ei * rr) / (em * rm);
        Dre[i] = sm * ur - sr;
        Dim[i] = sm * ui - si;
    }
    /* synthesis: build the change spectrum per frame (Hermitian), inverse FFT,
     * window and overlap-add; the envelope is accumulated the same way */
    for (int t = 0; t < frames; t++) {
        long fidx = (long)t * hop / gen_hop;
        if (fidx >= n_f0) fidx = n_f0 - 1;
        float v = (f0[fidx] > voiced) ? 1.0f : 0.0f;
        for (int i = 0; i < nfft; i++) { re[i] = 0.0f; im[i] = 0.0f; }
        if (v > 0.0f) {
            for (int j = 0; j < K; j++) {
                size_t idx = (size_t)j * frames + t;
                int k = bin_lo + j;
                re[k] = Dre[idx];
                im[k] = Dim[idx];
                re[nfft - k] = Dre[idx];            /* conjugate mirror */
                im[nfft - k] = -Dim[idx];
            }
        }
        kok_fft_inverse(&fft, re, im);
        long base = (long)t * hop;
        for (int i = 0; i < nfft; i++) {
            float w = win[i];
            ola[base + i] += re[i] * w;
            env[base + i] += w * w;
        }
    }
    for (long i = 0; i < n; i++) {
        long si = i + pad;
        float e = (si < total) ? env[si] : 0.0f;
        float d = (si < total && e > 1e-8f) ? ola[si] / e : 0.0f;
        out[i] = s[i] + d;
    }
    return 0;
}

int kok_dec_forward(const kok_model *mo, const float *asr, int F, const float *f0,
                    const float *nz, const float *src, uint64_t seed, float *wave,
                    long *wave_len, float *pre_lock_out, kok_arena *a) {
    const kok_config *c = &mo->meta.cfg;
    const kok_dec_w *dw = &mo->dw;
    const int width = dw->width, asr_dim = dw->asr_dim, sd = dw->style_dim;
    const int uic0 = dw->uic;
    const int nfft = dw->istft_n, hop = dw->istft_hop;
    const int gen_hop = dw->uprate[0] * dw->uprate[1] * hop;
    const int nb = nfft / 2 + 1;
    int hb = 0, upd = 0;
    float *cur;
    int cur_ch = 0, cur_len = 0;

    /* ---- decoder stack ---- */
    float *F0 = kok_arena_alloc(a, F);
    float *Nn = kok_arena_alloc(a, F);
    float *x = kok_arena_alloc(a, (size_t)(asr_dim + 2) * F);
    float *asr_res = kok_arena_alloc(a, 64L * F);
    if (!F0 || !Nn || !x || !asr_res) return -1;
    /* stride 2, kernel 3, pad 1: the F0/N conditioning rate halves */
    kok_conv1d(F0, f0, &dw->f0_conv_w, dw->f0_conv_b, 1, 1, 2 * F, 3, 2, 1, 1);
    kok_conv1d(Nn, nz, &dw->n_conv_w, dw->n_conv_b, 1, 1, 2 * F, 3, 2, 1, 1);
    memcpy(x, asr, (size_t)asr_dim * F * sizeof(float));
    memcpy(x + (size_t)asr_dim * F, F0, (size_t)F * sizeof(float));
    memcpy(x + (size_t)(asr_dim + 1) * F, Nn, (size_t)F * sizeof(float));
    kok_conv1d(asr_res, asr, &dw->asr_res_w, dw->asr_res_b, asr_dim, 64, F, 1, 1, 0, 1);

    /* the width of the cat buffer down here grows to width+2+64, then halves */
    float *big = kok_arena_alloc(a, (size_t)(asr_dim + 2 + width + 64) * 2 * F);
    float *small = kok_arena_alloc(a, (size_t)(width + 2 + 64) * 2 * F);
    float *dec = kok_arena_alloc(a, (size_t)uic0 * 2 * F);
    if (!big || !small || !dec) return -1;
    int rc = kok_arb_forward(big, x, &dw->encode, dw->style, sd, F, a);
    if (rc) return rc;
#ifdef KOK_DUMP_STAGES
    {
        FILE *fh = fopen("/tmp/ck_dec_encode.bin", "wb");
        if (fh) { fwrite(big, sizeof(float), (size_t)width * F, fh); fclose(fh); }
    }
#endif
    {
        size_t mark = a->top;
        float *cur = big;
        int cur_w = width, cur_len = F;
        for (int i = 0; i < 4; i++) {
            float *dst = (i == 3) ? dec : small;
            {
                /* every decode block takes [x, asr_res, F0, N]; the reference
                 * clears its flag only after the upsampling block runs */
                float *cat = kok_arena_alloc(a, (size_t)(cur_w + 66) * cur_len);
                if (!cat) return -1;
                memcpy(cat, cur, (size_t)cur_w * cur_len * sizeof(float));
                memcpy(cat + (size_t)cur_w * cur_len, asr_res, 64L * cur_len * sizeof(float));
                memcpy(cat + (size_t)(cur_w + 64) * cur_len, F0, (size_t)cur_len * sizeof(float));
                memcpy(cat + (size_t)(cur_w + 65) * cur_len, Nn, (size_t)cur_len * sizeof(float));
                cur = cat;
                cur_w += 66;
            }
#ifdef KOK_DUMP_STAGES
            {
                char fn[128];
                snprintf(fn, sizeof fn, "/tmp/ck_dec_pre%d.bin", i);
                FILE *fh = fopen(fn, "wb");
                if (fh) { fwrite(cur, sizeof(float), (size_t)cur_w * cur_len, fh); fclose(fh); }
            }
#endif
            rc = kok_arb_forward(dst, cur, &dw->decode[i], dw->style, sd, cur_len, a);
            if (rc) return rc;
#ifdef KOK_DUMP_STAGES
            {
                char fn[128];
                snprintf(fn, sizeof fn, "/tmp/ck_dec_post%d.bin", i);
                FILE *fh = fopen(fn, "wb");
                if (fh) { fwrite(dst, sizeof(float), (size_t)dw->decode[i].dim_out * (dw->decode[i].upsample ? 2*cur_len : cur_len), fh); fclose(fh); }
            }
#endif
            cur = dst;
            cur_w = dw->decode[i].dim_out;
            if (dw->decode[i].upsample) cur_len *= 2;
            a->top = mark;              /* the cat buffers are dead after each block */
            /* cur must survive the reset: it is dec (allocated outside) for i==3,
             * and the previous block's output otherwise, so keep the mark before
             * the block's own scratch only */
            if (i < 3) mark = a->top;
        }
    }
#ifdef KOK_DUMP_STAGES
    {
        FILE *fh = fopen("/tmp/ck_gen_input.bin", "wb");
        if (fh) { fwrite(dec, sizeof(float), (size_t)uic0 * 2 * F, fh); fclose(fh); }
    }
#endif
    const float *gx = dec;              /* [uic0, 2F] */
    int glen = 2 * F;
#ifdef KOK_DEBUG_STAGES
    {
        double m = 0.0;
        for (long i = 0; i < (long)uic0 * glen; i++) { double v = fabs(gx[i]); if (v > m) m = v; }
        fprintf(stderr, "[dbg] dec out |max| %.3f (len %d, ch %d)\n", m, glen, uic0);
    }
#endif

#ifdef KOK_DUMP_STAGES
    for (int q = 0; q < 2; q++) {
        char fn[128];
        snprintf(fn, sizeof fn, "/tmp/ck_upsw%d.bin", q);
        FILE *fh = fopen(fn, "wb");
        if (fh) {
            /* weights are a kok_w now; only the fp32 blob can be dumped raw */
            if (dw->ups_w[q].f)
                fwrite(dw->ups_w[q].f, sizeof(float),
                       (size_t)(uic0 >> q) * (uic0 >> (q + 1)) * dw->upkern[q], fh);
            fclose(fh);
        }
    }
#endif
    /* ---- generator: harmonic source through the 20-point STFT ---- */
    long src_len = (long)(2 * F) * gen_hop;
    kok_stft st;
    float *stmem = kok_arena_alloc(a, (size_t)kok_stft_mem_floats(nfft, 0, nfft / 2));
    if (!stmem || kok_stft_init(&st, nfft, hop, 0, nfft / 2, stmem)) return -1;
    /* the harmonic source: injected for the goldens, generated otherwise */
    float *src_buf = NULL;
    if (!src) {
        src_buf = kok_arena_alloc(a, (size_t)src_len);
        kok_rng rng;
        if (!src_buf) return -1;
        kok_rng_seed(&rng, seed);
        if (kok_source_forward(c, dw, f0, F, &rng, src_buf, a)) return -1;
        src = src_buf;
    }
    int hframes = (int)((src_len + 2 * (nfft / 2) - nfft) / hop + 1);
    float *mag = kok_arena_alloc(a, (size_t)st.K * hframes);
    float *pha = kok_arena_alloc(a, (size_t)st.K * hframes);
    float *har = kok_arena_alloc(a, (size_t)(nfft + 2) * hframes);
    if (!mag || !pha || !har) return -1;
    if (kok_stft_transform(&st, src, (int)src_len, mag, pha, a) < 0) return -1;
#ifdef KOK_DEBUG_STAGES
    {
        double m = 0.0;
        for (long i = 0; i < (long)st.K * hframes; i++) { double v = fabs(mag[i]); if (v > m) m = v; }
        fprintf(stderr, "[dbg] har mag |max| %.3f (K %d frames %d); src|max| ", m, st.K, hframes);
        m = 0.0;
        for (long i = 0; i < src_len; i++) { double v = fabs(src[i]); if (v > m) m = v; }
        fprintf(stderr, "%.3f\n", m);
    }
#endif
    memcpy(har, mag, (size_t)st.K * hframes * sizeof(float));
    memcpy(har + (size_t)st.K * hframes, pha, (size_t)st.K * hframes * sizeof(float));

    /* Stage outputs live in two persistent buffers: the per-stage scratch is
     * released at the end of each stage, so the stage output must not be. */
    {
        const int max_ch = uic0 >> 1;
        const long max_len = (long)glen * (long)dw->uprate[0] * (long)dw->uprate[1] + 2;
        float *bufA = kok_arena_alloc(a, (size_t)max_ch * max_len);
        float *bufB = kok_arena_alloc(a, (size_t)max_ch * max_len);
        if (!bufA || !bufB) return -1;
        cur = (float *)gx;
        cur_ch = uic0;
        cur_len = glen;
        for (int i = 0; i < 2; i++) {
            size_t mark = a->top;
            int out_ch = uic0 >> (i + 1);
            float *nsrc = kok_arena_alloc(a, (size_t)out_ch * hframes);
            float *nres = kok_arena_alloc(a, (size_t)out_ch * hframes);
            float *ups = kok_arena_alloc(a, (size_t)out_ch * (max_len + 2));
            float *blk = kok_arena_alloc(a, (size_t)out_ch * (max_len + 2));
            float *dst = (i == 0) ? bufA : bufB;
            if (!nsrc || !nres || !ups || !blk) return -1;

            kok_leaky_relu(cur, (long)cur_ch * cur_len, 0.1f);
            if (i == 0) {
                int stride = dw->uprate[1];
                kok_conv1d_a(a, nsrc, har, &dw->noise_w[i], dw->noise_b[i], nfft + 2,
                             out_ch, hframes, stride * 2, stride, (stride + 1) / 2, 1);
                hb = (hframes + 2 * ((stride + 1) / 2) - stride * 2) / stride + 1;
            } else {
                kok_conv1d_a(a, nsrc, har, &dw->noise_w[i], dw->noise_b[i], nfft + 2,
                             out_ch, hframes, 1, 1, 0, 1);
                hb = hframes;
            }
#ifdef KOK_DUMP_STAGES
            {
                char fn[128];
                snprintf(fn, sizeof fn, "/tmp/ck_nconv%d.bin", i);
                FILE *fh = fopen(fn, "wb");
                if (fh) { fwrite(nsrc, sizeof(float), (size_t)out_ch * hb, fh); fclose(fh); }
                snprintf(fn, sizeof fn, "/tmp/ck_har.bin");
                fh = fopen(fn, "wb");
                if (fh) { fwrite(har, sizeof(float), (size_t)(nfft + 2) * hframes, fh); fclose(fh); }
            }
#endif
            if (kok_res1_forward(nres, nsrc, &dw->noise_res[i], dw->style, sd, hb, a)) return -1;
#ifdef KOK_DUMP_STAGES
            {
                double m = 0.0;
                for (long q = 0; q < (long)cur_ch * cur_len; q++) { double v = fabs(cur[q]); if (v > m) m = v; }
                fprintf(stderr, "[dbg] stage %d input: ch %d len %d |max| %.4f first [%.4f %.4f %.4f]\n",
                        i, cur_ch, cur_len, m, cur[0], cur[1], cur[2]);
            }
#endif
            upd = (cur_len - 1) * dw->uprate[i] - 2 * ((dw->upkern[i] - dw->uprate[i]) / 2)
                  + dw->upkern[i];
            kok_convtr1d_a(a, ups, cur, &dw->ups_w[i], dw->ups_b[i], cur_ch, out_ch,
                           cur_len, dw->upkern[i], dw->uprate[i],
                           (dw->upkern[i] - dw->uprate[i]) / 2, 0, 1);
#ifdef KOK_DUMP_STAGES
            {
                char fn[128];
                snprintf(fn, sizeof fn, "/tmp/ck_rawups%d.bin", i);
                FILE *fh = fopen(fn, "wb");
                if (fh) { fwrite(ups, sizeof(float), (size_t)out_ch * upd, fh); fclose(fh); }
            }
#endif
            if (i == 1) {
                /* ReflectionPad1d((1,0)): the odd STFT frame count. The channel
                 * stride grows by one, so shift from the last channel backwards
                 * or each row's shifted tail would clobber the next row's head. */
                {
                    const int old = upd, neu = upd + 1;
                    for (int ch = out_ch - 1; ch >= 0; ch--) {
                        float *src = ups + (size_t)ch * old;
                        float *dst = ups + (size_t)ch * neu + 1;  /* row = [reflect, x0, x1...] */
                        memmove(dst, src, (size_t)old * sizeof(float));
                        dst[0] = dst[2];      /* reflect, no edge repeat */
                    }
                    upd = neu;
                }
            }
#ifdef KOK_DUMP_STAGES
            {
                char fn[128];
                snprintf(fn, sizeof fn, "/tmp/ck_preups%d.bin", i);
                FILE *fh = fopen(fn, "wb");
                if (fh) { fwrite(ups, sizeof(float), (size_t)out_ch * upd, fh); fclose(fh); }
                snprintf(fn, sizeof fn, "/tmp/ck_nres%d.bin", i);
                fh = fopen(fn, "wb");
                if (fh) { fwrite(nres, sizeof(float), (size_t)out_ch * hb, fh); fclose(fh); }
            }
#endif
            for (int ch = 0; ch < out_ch; ch++) {
                float *urow = ups + (size_t)ch * upd;
                const float *nrow = nres + (size_t)ch * hb;
                for (int t = 0; t < upd && t < hb; t++) urow[t] += nrow[t];
            }
            for (int j = 0; j < 3; j++) {
                size_t m2 = a->top;
                if (kok_res1_forward(blk, ups, &dw->resblocks[i * 3 + j], dw->style, sd, upd, a))
                    return -1;
                if (j == 0) memcpy(dst, blk, (size_t)out_ch * upd * sizeof(float));
                else for (long q = 0; q < (long)out_ch * upd; q++) dst[q] += blk[q];
                a->top = m2;
            }
            for (long q = 0; q < (long)out_ch * upd; q++) dst[q] /= 3.0f;
#ifdef KOK_DUMP_STAGES
            {
                char fn[128];
                snprintf(fn, sizeof fn, "/tmp/ck_stage%d.bin", i);
                FILE *fh = fopen(fn, "wb");
                if (fh) { fwrite(dst, sizeof(float), (size_t)out_ch * upd, fh); fclose(fh); }
            }
#endif
            cur = dst;
            cur_ch = out_ch;
            cur_len = upd;
            a->top = mark;
        }
    }

    /* conv_post: exp() magnitude and sin() phase over post_n_fft */
    float *post = kok_arena_alloc(a, (size_t)(nfft + 2) * cur_len);
    if (!post) return -1;
    kok_leaky_relu(cur, (long)cur_ch * cur_len, 0.01f);
    kok_conv1d(post, cur, &dw->conv_post_w, dw->conv_post_b, cur_ch, nfft + 2, cur_len, 7, 1, 3, 1);
#ifdef KOK_DEBUG_STAGES
    {
        double mm = 0.0, mp = 0.0;
        for (int j = 0; j < nb; j++)
            for (int t = 0; t < cur_len; t++) {
                double v = fabs(post[(size_t)j * cur_len + t]);
                if (v > mm) mm = v;
            }
        for (int j = nb; j < nfft + 2; j++)
            for (int t = 0; t < cur_len; t++) {
                double v = fabs(post[(size_t)j * cur_len + t]);
                if (v > mp) mp = v;
            }
        double mc = 0.0;
        for (long q = 0; q < (long)cur_ch * cur_len; q++) { double v = fabs(cur[q]); if (v > mc) mc = v; }
        fprintf(stderr, "[dbg] pre-post cur(ch %d)|max| %.3f | conv_post signed: mag-ch [%.2f..%.2f] phase-ch [%.2f..%.2f]\n",
                cur_ch, mc, mm, mp, 0.0, 0.0);
        for (int j = 0; j < nfft + 2; j++) {
            double lo = 1e30, hi = -1e30;
            for (int t = 0; t < cur_len; t++) {
                double v = post[(size_t)j * cur_len + t];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            fprintf(stderr, "[dbg]   post ch %2d: [%.3f .. %.3f]\n", j, lo, hi);
        }
    }
#endif
#ifdef KOK_DUMP_STAGES
    {
        FILE *fh = fopen("/tmp/ck_post.bin", "wb");
        if (fh) { fwrite(post, sizeof(float), (size_t)(nfft + 2) * cur_len, fh); fclose(fh); }
    }
#endif
    float *spec = kok_arena_alloc(a, (size_t)nb * cur_len);
    float *ph = kok_arena_alloc(a, (size_t)nb * cur_len);
    if (!spec || !ph) return -1;
    for (int j = 0; j < nb; j++)
        for (int t = 0; t < cur_len; t++) {
            spec[(size_t)j * cur_len + t] = expf(post[(size_t)j * cur_len + t]);
            ph[(size_t)j * cur_len + t] = sinf(post[(size_t)(nb + j) * cur_len + t]);
        }
    long wlen = 0;
#ifdef KOK_DEBUG_STAGES
    struct timespec tg0, tg1, tl0, tl1;
    clock_gettime(CLOCK_MONOTONIC, &tl0);
#endif
    if (kok_stft_inverse(&st, spec, ph, cur_len, wave, &wlen, a) < 0) return -1;
#ifdef KOK_DEBUG_STAGES
    clock_gettime(CLOCK_MONOTONIC, &tg0);
    fprintf(stderr, "[dbg] generator (to wave_pre_lock) %.3fs\n",
            (double)(tg0.tv_sec - tl0.tv_sec) + 1e-9 * (tg0.tv_nsec - tl0.tv_nsec));
#endif
    *wave_len = wlen;
    if (pre_lock_out) memcpy(pre_lock_out, wave, (size_t)wlen * sizeof(float));

    /* ---- phase lock over the 2-8 kHz band, voiced frames only ---- */
    long k = src_len < wlen ? src_len : wlen;
    float *locked = kok_arena_alloc(a, (size_t)wlen);
    if (!locked) return -1;
    if (kok_lock_apply(c, wave, k, src, f0, 2 * F, locked, a)) return -1;
#ifdef KOK_DEBUG_STAGES
    clock_gettime(CLOCK_MONOTONIC, &tl1);
    fprintf(stderr, "[dbg] lock %.3fs\n",
            (double)(tl1.tv_sec - tl0.tv_sec) + 1e-9 * (tl1.tv_nsec - tl0.tv_nsec));
#endif
    memcpy(wave, locked, (size_t)wlen * sizeof(float));
    return 0;
}
