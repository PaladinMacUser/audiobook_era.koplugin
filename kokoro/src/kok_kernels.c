/* kok_kernels.c -- portable C99 primitives. See kok_kernels.h for the layout
 * contract. Correctness first: plain loops, no SIMD, no fast-math assumptions. */
#include <math.h>
#include <string.h>

#include "kok_kernels.h"

/* ---- arena ---- */
void kok_arena_init(kok_arena *a, void *mem, size_t floats) {
    a->base = (float *)mem;
    a->cap = floats;
    a->top = 0;
}

void *kok_arena_alloc(kok_arena *a, size_t floats) {
    if (a->top + floats > a->cap) return NULL;
    float *p = a->base + a->top;
    a->top += floats;
    return p;
}

void kok_arena_reset(kok_arena *a) { a->top = 0; }

size_t kok_arena_peak(const kok_arena *a) { return a->top; }

/* ---- linear algebra ---- */
void kok_conv1d(float *out, const float *x, const float *w, const float *b,
                int Cin, int Cout, int T, int K, int pad, int dil) {
    int Tp = T + 2 * pad - dil * (K - 1);
    for (int o = 0; o < Cout; o++) {
        float *orow = out + (size_t)o * Tp;
        const float *wrow = w + (size_t)o * Cin * K;
        for (int t = 0; t < Tp; t++) {
            float acc = b ? b[o] : 0.0f;
            for (int k = 0; k < K; k++) {
                int ti = t + k * dil - pad;
                if (ti < 0 || ti >= T) continue;
                for (int c = 0; c < Cin; c++)
                    acc += x[(size_t)c * T + ti] * wrow[(size_t)c * K + k];
            }
            orow[t] = acc;
        }
    }
}

void kok_convtr1d(float *out, const float *x, const float *w, const float *b,
                  int Cin, int Cout, int T, int K, int stride, int pad,
                  int out_pad, int groups) {
    int Tp = (T - 1) * stride - 2 * pad + K + out_pad;
    int cig = Cin / groups, cog = Cout / groups;
    for (int o = 0; o < Cout; o++) {
        float v = b ? b[o] : 0.0f;
        for (int t = 0; t < Tp; t++) out[(size_t)o * Tp + t] = v;
    }
    for (int g = 0; g < groups; g++) {
        for (int ci = 0; ci < cig; ci++) {
            const float *xrow = x + (size_t)(g * cig + ci) * T;
            const float *wrow = w + (size_t)(g * cig + ci) * cog * K;
            for (int t = 0; t < T; t++) {
                float v = xrow[t];
                if (v == 0.0f) continue;
                for (int k = 0; k < K; k++) {
                    int to = t * stride - pad + k;
                    if (to < 0 || to >= Tp) continue;
                    for (int co = 0; co < cog; co++)
                        out[(size_t)(g * cog + co) * Tp + to] += v * wrow[(size_t)co * K + k];
                }
            }
        }
    }
}

void kok_linear(float *out, const float *x, const float *w, const float *b,
                int Cin, int Cout, int T) {
    for (int o = 0; o < Cout; o++) {
        const float *wrow = w + (size_t)o * Cin;
        float bias = b ? b[o] : 0.0f;
        float *orow = out + (size_t)o * T;
        for (int t = 0; t < T; t++) orow[t] = bias;
        for (int c = 0; c < Cin; c++) {
            float wv = wrow[c];
            if (wv == 0.0f) continue;
            const float *xrow = x + (size_t)c * T;
            for (int t = 0; t < T; t++) orow[t] += wv * xrow[t];
        }
    }
}

/* ---- normalisation ---- */
void kok_layer_norm(float *x, const float *gamma, const float *beta, int C, int T, float eps) {
    for (int t = 0; t < T; t++) {
        double mean = 0.0, var = 0.0;
        for (int c = 0; c < C; c++) mean += x[(size_t)c * T + t];
        mean /= C;
        for (int c = 0; c < C; c++) {
            double d = x[(size_t)c * T + t] - mean;
            var += d * d;
        }
        var /= C;
        double inv = 1.0 / sqrt(var + eps);
        for (int c = 0; c < C; c++)
            x[(size_t)c * T + t] = (float)((x[(size_t)c * T + t] - mean) * inv) * gamma[c] + beta[c];
    }
}

void kok_inst_norm(float *out, const float *x, const float *gamma, const float *beta,
                   int C, int T, float eps) {
    for (int c = 0; c < C; c++) {
        const float *xrow = x + (size_t)c * T;
        float *orow = out + (size_t)c * T;
        double mean = 0.0, var = 0.0;
        for (int t = 0; t < T; t++) mean += xrow[t];
        mean /= T;
        for (int t = 0; t < T; t++) {
            double d = xrow[t] - mean;
            var += d * d;
        }
        var /= T;
        double inv = 1.0 / sqrt(var + eps);
        for (int t = 0; t < T; t++)
            orow[t] = (float)((xrow[t] - mean) * inv) * gamma[c] + beta[c];
    }
}

/* Per-channel AdaIN coefficients: g_c and b_c are the two halves of
 * Linear(style -> 2C) applied to s, computed on the fly so no buffer is
 * needed (style_dim is 16-32). */
static void adain_ab(const float *s, int style_dim, const float *fc_w,
                     const float *fc_b, int C, int c, float *g, float *b) {
    const float *wr = fc_w + (size_t)c * style_dim;
    const float *wr2 = fc_w + (size_t)(C + c) * style_dim;
    double ag = fc_b ? fc_b[c] : 0.0;
    double ab = fc_b ? fc_b[C + c] : 0.0;
    for (int i = 0; i < style_dim; i++) {
        ag += (double)wr[i] * s[i];
        ab += (double)wr2[i] * s[i];
    }
    *g = (float)ag;
    *b = (float)ab;
}

void kok_adain1d(float *out, const float *x, int C, int T, const float *s, int style_dim,
                 const float *fc_w, const float *fc_b, const float *gamma,
                 const float *beta, float eps) {
    for (int c = 0; c < C; c++) {
        const float *xrow = x + (size_t)c * T;
        float *orow = out + (size_t)c * T;
        double mean = 0.0, var = 0.0;
        for (int t = 0; t < T; t++) mean += xrow[t];
        mean /= T;
        for (int t = 0; t < T; t++) {
            double d = xrow[t] - mean;
            var += d * d;
        }
        var /= T;
        double inv = 1.0 / sqrt(var + eps);
        float g, b;
        adain_ab(s, style_dim, fc_w, fc_b, C, c, &g, &b);
        float scale = 1.0f + g;
        for (int t = 0; t < T; t++)
            orow[t] = (float)((xrow[t] - mean) * inv) * gamma[c] * scale + beta[c] * scale + b;
    }
}

void kok_ada_layer_norm(float *out, const float *x, int C, int T, const float *s,
                        int style_dim, const float *fc_w, const float *fc_b, float eps) {
    for (int t = 0; t < T; t++) {
        double mean = 0.0, var = 0.0;
        for (int c = 0; c < C; c++) mean += x[(size_t)c * T + t];
        mean /= C;
        for (int c = 0; c < C; c++) {
            double d = x[(size_t)c * T + t] - mean;
            var += d * d;
        }
        var /= C;
        double inv = 1.0 / sqrt(var + eps);
        for (int c = 0; c < C; c++)
            out[(size_t)c * T + t] = (float)((x[(size_t)c * T + t] - mean) * inv);
    }
    for (int c = 0; c < C; c++) {
        float g, b;
        adain_ab(s, style_dim, fc_w, fc_b, C, c, &g, &b);
        float scale = 1.0f + g;
        float *orow = out + (size_t)c * T;
        for (int t = 0; t < T; t++) orow[t] = scale * orow[t] + b;
    }
}

/* ---- activations ---- */
void kok_leaky_relu(float *x, long n, float slope) {
    for (long i = 0; i < n; i++) if (x[i] < 0.0f) x[i] *= slope;
}

void kok_gelu_erf(float *x, long n) {
    const float inv = 0.70710678f;
    for (long i = 0; i < n; i++) x[i] = 0.5f * x[i] * (1.0f + erff(x[i] * inv));
}

void kok_gelu_new(float *x, long n) {
    const float c = 0.79788456f, k = 0.044715f;
    for (long i = 0; i < n; i++) {
        float v = x[i];
        x[i] = 0.5f * v * (1.0f + tanhf(c * (v + k * v * v * v)));
    }
}

void kok_sigmoid(float *x, long n) {
    for (long i = 0; i < n; i++) x[i] = 1.0f / (1.0f + expf(-x[i]));
}

void kok_snake(float *x, const float *alpha, int C, int T) {
    for (int c = 0; c < C; c++) {
        float a = alpha[c];
        float inv = (a != 0.0f) ? 1.0f / a : 0.0f;
        float *xrow = x + (size_t)c * T;
        for (int t = 0; t < T; t++) {
            float v = xrow[t];
            float s = sinf(a * v);
            xrow[t] = v + inv * s * s;
        }
    }
}

void kok_softmax(float *x, int rows, int len) {
    for (int r = 0; r < rows; r++) {
        float *row = x + (size_t)r * len;
        float mx = row[0];
        for (int i = 1; i < len; i++) if (row[i] > mx) mx = row[i];
        double sum = 0.0;
        for (int i = 0; i < len; i++) {
            row[i] = expf(row[i] - mx);
            sum += row[i];
        }
        float inv = (float)(1.0 / sum);
        for (int i = 0; i < len; i++) row[i] *= inv;
    }
}

/* ---- sequence ---- */
void kok_embed(float *out, const float *table, long id, int C) {
    memcpy(out, table + (size_t)id * C, (size_t)C * sizeof(float));
}

int kok_lstm_dir(float *out, const float *x, int Cin, int T, int H,
                 const kok_lstm_w *w, int reverse, kok_arena *a) {
    float *h = kok_arena_alloc(a, H);
    float *c = kok_arena_alloc(a, H);
    float *gates = kok_arena_alloc(a, 4 * H);
    if (!h || !c || !gates) return -1;
    memset(h, 0, (size_t)H * sizeof(float));
    memset(c, 0, (size_t)H * sizeof(float));
    for (int n = 0; n < T; n++) {
        int t = reverse ? (T - 1 - n) : n;
        const float *xt = x + (size_t)t;
        for (int o = 0; o < 4 * H; o++) {
            double acc = w->b_ih ? w->b_ih[o] : 0.0;
            if (w->b_hh) acc += w->b_hh[o];
            const float *wr = w->w_ih + (size_t)o * Cin;
            for (int i = 0; i < Cin; i++) acc += (double)wr[i] * xt[(size_t)i * T];
            const float *wh = w->w_hh + (size_t)o * H;
            for (int i = 0; i < H; i++) acc += (double)wh[i] * h[i];
            gates[o] = (float)acc;
        }
        for (int i = 0; i < H; i++) {
            float gi = 1.0f / (1.0f + expf(-gates[i]));
            float gf = 1.0f / (1.0f + expf(-gates[H + i]));
            float gg = tanhf(gates[2 * H + i]);
            float go = 1.0f / (1.0f + expf(-gates[3 * H + i]));
            c[i] = gf * c[i] + gi * gg;
            h[i] = go * tanhf(c[i]);
            /* PyTorch aligns the backward output to the input index */
            out[(size_t)i * T + t] = h[i];
        }
    }
    return 0;
}

int kok_bilstm(float *out, const float *x, int Cin, int T, int H,
               const kok_lstm_w *fwd, const kok_lstm_w *bwd, kok_arena *a) {
    int rc = kok_lstm_dir(out, x, Cin, T, H, fwd, 0, a);
    if (rc) return rc;
    return kok_lstm_dir(out + (size_t)H * T, x, Cin, T, H, bwd, 1, a);
}

/* ---- composed blocks ---- */
int kok_arb_forward(float *out, const float *x, const kok_arb *b, const float *s,
                    int style_dim, int T, kok_arena *a) {
    const int Cin = b->dim_in, Cout = b->dim_out;
    const int T_out = b->upsample ? 2 * T : T;
    const int rows = Cin > Cout ? Cin : Cout;
    float *buf = kok_arena_alloc(a, (size_t)rows * T_out);
    float *res = kok_arena_alloc(a, (size_t)Cout * T_out);
    float *shc = kok_arena_alloc(a, (size_t)Cout * T_out);
    if (!buf || !res || !shc) return -1;

    /* residual: AdaIN(x) -> leaky 0.2 -> [depthwise ConvT upsample] -> conv1 */
    kok_adain1d(buf, x, Cin, T, s, style_dim, b->n1_fc_w, b->n1_fc_b,
                b->n1_norm_w, b->n1_norm_b, 1e-5f);
    kok_leaky_relu(buf, (long)Cin * T, 0.2f);
    if (b->upsample) {
        float *up = kok_arena_alloc(a, (size_t)Cin * T_out);
        if (!up) return -1;
        kok_convtr1d(up, buf, b->pool_w, b->pool_b, Cin, Cin, T, 3, 2, 1, 1, Cin);
        kok_conv1d(res, up, b->c1_w, b->c1_b, Cin, Cout, T_out, 3, 1, 1);
    } else {
        kok_conv1d(res, buf, b->c1_w, b->c1_b, Cin, Cout, T, 3, 1, 1);
    }
    /* AdaIN -> leaky -> conv2 */
    kok_adain1d(buf, res, Cout, T_out, s, style_dim, b->n2_fc_w, b->n2_fc_b,
                b->n2_norm_w, b->n2_norm_b, 1e-5f);
    kok_leaky_relu(buf, (long)Cout * T_out, 0.2f);
    kok_conv1d(res, buf, b->c2_w, b->c2_b, Cout, Cout, T_out, 3, 1, 1);

    /* shortcut: nearest upsample (PyTorch interpolate), then 1x1 if the widths differ */
    if (b->upsample) {
        for (int c = 0; c < Cin; c++) {
            const float *xrow = x + (size_t)c * T;
            float *srow = shc + (size_t)c * T_out;
            for (int t = 0; t < T; t++) { srow[2 * t] = xrow[t]; srow[2 * t + 1] = xrow[t]; }
        }
        if (b->sc_w)
            kok_conv1d(out, shc, b->sc_w, NULL, Cin, Cout, T_out, 1, 0, 1);
        else
            memcpy(out, shc, (size_t)Cin * T_out * sizeof(float));
    } else {
        if (b->sc_w)
            kok_conv1d(out, x, b->sc_w, NULL, Cin, Cout, T, 1, 0, 1);
        else
            memcpy(out, x, (size_t)Cin * T * sizeof(float));
    }

    const float inv_sqrt2 = 0.70710678f;
    for (long i = 0; i < (long)Cout * T_out; i++)
        out[i] = (res[i] + out[i]) * inv_sqrt2;
    return 0;
}
