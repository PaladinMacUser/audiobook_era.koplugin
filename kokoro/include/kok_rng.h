/* kok_rng.h -- torch-CPU-parity RNG and the linear interpolation the sine
 * source needs. See kok_rng.c for how exact each half is. */
#ifndef KOK_RNG_H
#define KOK_RNG_H

#include <stdint.h>

#define KOK_MT_N 624

typedef struct {
    uint32_t state[KOK_MT_N];
    int left;
    int next;
} kok_rng;

/* SHA-256 of the sentence text -> a 64-bit seed (one or two blocks). */
int kok_rng_seed_from_text(const char *text, uint64_t *seed_out);

void kok_rng_seed(kok_rng *g, uint64_t seed);
float kok_rng_uniform(kok_rng *g);                  /* at::uniform_real_distribution<float> */
int kok_rng_normal_fill(kok_rng *g, float *out, long n);   /* 16-sample Box-Muller chunks */

/* F.interpolate(mode='linear', align_corners=False): out_len samples of x
 * (in_len long) at scale `scale`. */
int kok_interp_linear(float *out, long out_len, const float *x, long in_len, double scale);

#endif