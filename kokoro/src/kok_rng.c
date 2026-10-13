/* kok_rng.c -- torch-CPU-parity RNG for the generator's stochastic source.
 *
 * The Paradee pipeline draws twice per utterance: torch.rand for the harmonics'
 * initial phase and torch.randn for the additive noise. The kernel the author
 * trained against is the CPU version, so matching torch on x86/ARM CPU is the
 * contract.
 *
 * The integer half is bit-exact: ATen's mt19937_engine::init_with_uint32, the
 * standard tempering, and uniform_real_distribution<float> implemented as
 * (raw & 0xffffff) * 2^-24. The normal draws go through float32 Box-Muller in
 * 16-sample chunks exactly as ATen's normal_fill_16 does, but logf/cosf/sinf are
 * not bit-identical across libms, so a minority of draws differ by a few ulp
 * (measured at 26 percent of draws differing by at most 1.9e-06 on the sanoTTS
 * engine, which uses this same generator). Claiming bit-exactness here would be
 * false; the tests compare distributions and the rendered audio instead.
 *
 * The SHA-256 (one or two blocks, no streaming) seeds per sentence text, so a
 * sentence renders identically every time. Ported from sanotts/mcu/src/snt_nano.c
 * (MIT), which proved the approach against ATen. */
#include <math.h>
#include <string.h>

#include "kok_rng.h"

/* ---- SHA-256 ----------------------------------------------------------- */
static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t *h, const unsigned char *block) {
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
               ((uint32_t)block[4 * i + 2] << 8) | (uint32_t)block[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

int kok_rng_seed_from_text(const char *text, uint64_t *seed_out) {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    unsigned char tail[128];
    size_t len, full, rem, i, tail_len;
    uint64_t bits, seed = 0;
    if (!text || !seed_out) return -1;
    len = strlen(text);
    full = len / 64;
    rem = len % 64;
    bits = (uint64_t)len * 8u;
    for (i = 0; i < full; i++) sha256_block(h, (const unsigned char *)text + i * 64);
    memset(tail, 0, sizeof tail);
    memcpy(tail, text + full * 64, rem);
    tail[rem] = 0x80;
    tail_len = (rem < 56) ? 64u : 128u;
    for (i = 0; i < 8; i++) tail[tail_len - 1 - i] = (unsigned char)((bits >> (8 * i)) & 0xffu);
    sha256_block(h, tail);
    if (tail_len == 128) sha256_block(h, tail + 64);
    for (i = 0; i < 4; i++) seed = (seed << 16) | (uint64_t)((h[i] >> 16) ^ (h[i] & 0xffffu));
    *seed_out = seed;
    return 0;
}

/* ---- MT19937 (ATen's engine) ------------------------------------------- */
#define MT_N 624
#define MT_M 397
#define MT_MATRIX_A 0x9908b0dfu
#define MT_UPPER_MASK 0x80000000u
#define MT_LOWER_MASK 0x7fffffffu

static uint32_t mt_twist(uint32_t u, uint32_t v) {
    return (((u & MT_UPPER_MASK) | (v & MT_LOWER_MASK)) >> 1) ^
           ((v & 1u) ? MT_MATRIX_A : 0u);
}

void kok_rng_seed(kok_rng *g, uint64_t seed) {
    g->state[0] = (uint32_t)(seed & 0xffffffffu);
    for (int i = 1; i < MT_N; i++)
        g->state[i] = (uint32_t)(1812433253u * (g->state[i - 1] ^ (g->state[i - 1] >> 30)) + (uint32_t)i);
    g->left = 1;
    g->next = 0;
}

static void mt_next_state(kok_rng *g) {
    uint32_t *s = g->state;
    int i;
    g->left = MT_N;
    g->next = 0;
    for (i = 0; i < MT_N - MT_M; i++) s[i] = s[i + MT_M] ^ mt_twist(s[i], s[i + 1]);
    for (; i < MT_N - 1; i++) s[i] = s[i + MT_M - MT_N] ^ mt_twist(s[i], s[i + 1]);
    s[MT_N - 1] = s[MT_M - 1] ^ mt_twist(s[MT_N - 1], s[0]);
}

static uint32_t mt_random(kok_rng *g) {
    uint32_t y;
    if (--g->left <= 0) mt_next_state(g);
    y = g->state[g->next++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= (y >> 18);
    return y;
}

/* at::uniform_real_distribution<float>: (raw & (2^24 - 1)) * 2^-24 */
float kok_rng_uniform(kok_rng *g) {
    return (float)(mt_random(g) & 0xffffffu) * (1.0f / 16777216.0f);
}

static void normal_fill_16(float *d) {
    for (int j = 0; j < 8; j++) {
        float u1 = 1.0f - d[j];
        float u2 = d[j + 8];
        float radius = sqrtf(-2.0f * logf(u1));
        float theta = (float)(2.0 * 3.14159265358979323846) * u2;
        d[j] = radius * cosf(theta);
        d[j + 8] = radius * sinf(theta);
    }
}

int kok_rng_normal_fill(kok_rng *g, float *out, long n) {
    long i;
    if (!g || !out || n < 16) return -1;   /* torch uses a scalar path below 16 */
    for (i = 0; i < n; i++) out[i] = kok_rng_uniform(g);
    for (i = 0; i + 16 <= n; i += 16) normal_fill_16(out + i);
    if (n % 16 != 0) {
        float tail[16];
        for (int j = 0; j < 16; j++) tail[j] = kok_rng_uniform(g);
        normal_fill_16(tail);
        for (int j = 0; j < 16; j++) out[n - 16 + j] = tail[j];
    }
    return 0;
}

/* ---- F.interpolate(mode='linear', align_corners=False) ------------------ */
int kok_interp_linear(float *out, long out_len, const float *x, long in_len, double scale) {
    for (long j = 0; j < out_len; j++) {
        double pos = ((double)j + 0.5) / scale - 0.5;
        if (pos < 0.0) pos = 0.0;
        if (pos > (double)(in_len - 1)) pos = (double)(in_len - 1);
        double f = floor(pos);
        long i0 = (long)f;
        long i1 = (i0 + 1 < in_len) ? i0 + 1 : in_len - 1;
        float w = (float)(pos - f);
        out[j] = x[i0] * (1.0f - w) + x[i1] * w;
    }
    return 0;
}