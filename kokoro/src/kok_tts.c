/* kok_tts.c -- Kokoro-family runtime driver: phoneme ids in, 24 kHz WAV out.
 *
 * This is the core the plugin's server will wrap (piper's JSON-line protocol),
 * and it is what the device benchmark runs. It takes ids rather than text: the
 * on-device G2P (misaki) is a separate piece, and every gate so far injects the
 * source, so the two integration points stay explicit.
 *
 * Usage:
 *   kok_tts --meta kok_meta.bin --text kok_text.bin --dec kok_dec.bin \
 *           --ids ids.bin --out out.wav [--seed N] [--arena-mb N] [--quiet]
 *
 * ids.bin is raw little-endian int32 phoneme ids (padded with 0 at both ends
 * by the caller, as the model expects). Timing is reported per stage.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kok_runtime.h"

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

static void *xload(const char *path, size_t *bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "kok_tts: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "kok_tts: short read %s\n", path);
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    if (bytes) *bytes = (size_t)sz;
    return buf;
}

/* 16-bit mono WAV at the model's rate */
static int write_wav(const char *path, const float *pcm, long n, int rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t data = (uint32_t)(n * 2), riff = data + 36, fmtsz = 16;
    uint16_t fmt = 1, chan = 1, align = 2, bits = 16;
    uint32_t byte_rate = (uint32_t)rate * 2;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtsz, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&chan, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (long i = 0; i < n; i++) {
        float s = pcm[i];
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        int16_t v = (int16_t)(s * 32767.0f);
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *meta_p = NULL, *text_p = NULL, *dec_p = NULL;
    const char *ids_p = NULL, *out_p = "out.wav";
    const char *src_p = NULL;   /* optional fixed harmonic source, for A/B */
    unsigned long long seed = 1234;
    double arena_mb = 128.0;
    int quiet = 0;
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--meta")) meta_p = argv[++i];
        else if (!strcmp(argv[i], "--text")) text_p = argv[++i];
        else if (!strcmp(argv[i], "--dec")) dec_p = argv[++i];
        else if (!strcmp(argv[i], "--ids")) ids_p = argv[++i];
        else if (!strcmp(argv[i], "--out")) out_p = argv[++i];
        else if (!strcmp(argv[i], "--src")) src_p = argv[++i];
        else if (!strcmp(argv[i], "--seed")) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--arena-mb")) arena_mb = atof(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
    }
    if (!meta_p || !text_p || !dec_p || !ids_p) {
        fprintf(stderr, "usage: %s --meta M --text T --dec D --ids I [--out O] "
                        "[--seed N] [--arena-mb N]\n", argv[0]);
        return 2;
    }

    double t0 = now_s();
    size_t meta_n, text_n, dec_n, ids_n;
    void *meta = xload(meta_p, &meta_n), *text = xload(text_p, &text_n);
    void *dec = xload(dec_p, &dec_n), *idsv = xload(ids_p, &ids_n);
    if (!meta || !text || !dec || !idsv) return 2;

    kok_model mo;
    int rc = kok_model_init(&mo, meta, meta_n, text, dec);
    if (rc) { fprintf(stderr, "kok_tts: model init rc %d\n", rc); return 2; }
    double t_init = now_s() - t0;

    const kok_config *c = &mo.meta.cfg;
    long nt = (long)(ids_n / sizeof(int));
    int *ids = (int *)idsv;

    kok_arena arena;
    float *arena_mem = malloc((size_t)(arena_mb * 1048576.0 / 4.0) * sizeof(float) + 64);
    if (!arena_mem) { fprintf(stderr, "kok_tts: no memory for the arena\n"); return 2; }
    kok_arena_init(&arena, arena_mem, (size_t)(arena_mb * 1048576.0 / 4.0));

    kok_text_out out;
    memset(&out, 0, sizeof out);
    const int dhid = c->v[KOK_D_PROSODY_D_HID], sd = c->v[KOK_D_TEXT_STYLE_DIM];
    const int asr = c->v[KOK_D_ASR_DIM];
    out.dur_raw = malloc((size_t)nt * sizeof(float));
    out.durs = malloc((size_t)nt * sizeof(long));
    out.d = malloc((size_t)nt * (size_t)(dhid + sd) * sizeof(float));
    out.asr_tok = malloc((size_t)asr * (size_t)nt * sizeof(float));

    double t_a = now_s();
    rc = kok_text_forward(&mo, ids, nt, 0, &out, &arena);
    if (rc) { fprintf(stderr, "kok_tts: text rc %d\n", rc); return 2; }
    long F = 0;
    for (long i = 0; i < nt; i++) F += out.durs[i];

    /* the aligned stages need buffers sized by F, so reset and run again */
    kok_arena_reset(&arena);
    out.en = malloc((size_t)(dhid + sd) * (size_t)F * sizeof(float));
    out.asr = malloc((size_t)asr * (size_t)F * sizeof(float));
    out.f0 = malloc((size_t)(2 * F) * sizeof(float));
    out.nz = malloc((size_t)(2 * F) * sizeof(float));
    rc = kok_text_forward(&mo, ids, nt, 1, &out, &arena);
    if (rc) { fprintf(stderr, "kok_tts: text(mode 1) rc %d\n", rc); return 2; }
    double t_text = now_s() - t_a;

    long src_len = kok_dec_source_floats(&mo, (int)F);
    float *src_fixed = NULL;
    if (src_p) {
        size_t sn = 0;
        src_fixed = xload(src_p, &sn);
        if (!src_fixed || sn != (size_t)src_len * sizeof(float)) {
            fprintf(stderr, "kok_tts: --src must be %ld float32\n", src_len);
            return 2;
        }
    }
    float *wav = malloc((size_t)src_len * 2 * sizeof(float));
    if (!wav) { fprintf(stderr, "kok_tts: no memory for the waveform\n"); return 2; }
    double t_b = now_s();
    long wlen = 0;
    rc = kok_dec_forward(&mo, out.asr, (int)F, out.f0, out.nz, src_fixed, (uint64_t)seed,
                         wav, &wlen, NULL, &arena);
    if (rc) { fprintf(stderr, "kok_tts: decoder rc %d\n", rc); return 2; }
    double t_dec = now_s() - t_b;

    if (write_wav(out_p, wav, wlen, c->v[KOK_D_SR])) {
        fprintf(stderr, "kok_tts: cannot write %s\n", out_p);
        return 2;
    }
    if (!quiet) {
        double secs = (double)wlen / (double)c->v[KOK_D_SR];
        printf("tokens %ld, frames %ld, samples %ld (%.2fs audio)\n", nt, F, wlen, secs);
        printf("  init %.3fs  text %.3fs  decoder %.3fs  total %.3fs\n",
               t_init, t_text, t_dec, t_init + t_text + t_dec);
        printf("  RTF %.3f (decoder %.3f)  arena peak %.1f MB\n",
               (t_init + t_text + t_dec) / secs, t_dec / secs,
               (double)kok_arena_peak(&arena) * sizeof(float) / 1048576.0);
        printf("  wrote %s\n", out_p);
    }
    return 0;
}