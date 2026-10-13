/* text_golden_test.c -- host gate for the C text half.
 *
 * Loads the model blobs and the golden fixtures, runs the text side on the
 * golden ids, and compares every stage against torch. Pass/fail is relative to
 * each stage's own magnitude (F0 runs to ~300, the waveform to ~0.5), matching
 * the numpy gate.
 *
 * Usage: build/text_golden_test --meta kokoro/model/blobs/kok_meta.bin \
 *          --text kokoro/model/blobs/kok_text.bin \
 *          --fixtures kokoro/test/fixtures/paradee-8m [--sentences 0,1] [--tol 1e-4]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kok_runtime.h"

static void *xload(const char *path, size_t *bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "short read %s\n", path);
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    if (bytes) *bytes = (size_t)sz;
    return buf;
}

static int report(const char *stage, const float *mine, const float *ref, long n, double tol) {
    double worst = 0.0, refmax = 0.0;
    for (long i = 0; i < n; i++) {
        double d = fabs((double)mine[i] - (double)ref[i]);
        if (d > worst) worst = d;
        double r = fabs((double)ref[i]);
        if (r > refmax) refmax = r;
    }
    double rel = worst / (refmax > 1e-9 ? refmax : 1e-9);
    printf("    %-10s n=%7ld maxerr %10.3e rel %9.2e %s\n", stage, n, worst, rel,
           rel < tol ? "OK" : "FAIL");
    return rel < tol;
}

int main(int argc, char **argv) {
    const char *meta_path = "kokoro/model/blobs/kok_meta.bin";
    const char *text_path = "kokoro/model/blobs/kok_text.bin";
    const char *fx = "kokoro/test/fixtures/paradee-8m";
    const char *which = NULL;
    double tol = 1e-4;
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--meta")) meta_path = argv[++i];
        else if (!strcmp(argv[i], "--text")) text_path = argv[++i];
        else if (!strcmp(argv[i], "--fixtures")) fx = argv[++i];
        else if (!strcmp(argv[i], "--sentences")) which = argv[++i];
        else if (!strcmp(argv[i], "--tol")) tol = atof(argv[++i]);
    }

    size_t meta_bytes, text_bytes;
    void *meta = xload(meta_path, &meta_bytes);
    void *text = xload(text_path, &text_bytes);
    if (!meta || !text) return 2;

    kok_model mo;
    int rc = kok_model_init(&mo, meta, meta_bytes, text, NULL);
    if (rc) { fprintf(stderr, "kok_model_init rc %d\n", rc); return 2; }
    const kok_config *c = &mo.meta.cfg;
    const int dhid = c->v[KOK_D_PROSODY_D_HID], sd = c->v[KOK_D_TEXT_STYLE_DIM];
    const int asr = c->v[KOK_D_ASR_DIM];
    printf("model: token %d, albert %d x %d heads, d_hid %d style %d, asr %d, %u slots\n",
           c->v[KOK_D_N_TOKEN], c->v[KOK_D_ALBERT_LAYERS], c->v[KOK_D_ALBERT_HEADS],
           dhid, sd, asr, mo.meta.n_slots);

    const size_t ARENA_FLOATS = 24u * 1024 * 1024;   /* 96 MB of scratch */
    float *arena_mem = malloc(ARENA_FLOATS * sizeof(float));
    if (!arena_mem) return 2;

    int all_ok = 1;
    for (int s = 0; s < 64; s++) {
        char path[512];
        if (which && !strstr(which, "")) { /* no-op, keeps the parser simple */ }
        snprintf(path, sizeof path, "%s/s%d_ids.bin", fx, s);
        FILE *probe = fopen(path, "rb");
        if (!probe) break;
        fclose(probe);
        if (which) {
            char tag[16];
            snprintf(tag, sizeof tag, "%d", s);
            int wanted = 0;
            for (const char *p = which; *p; ) {
                if (*p == ',') { p++; continue; }
                if (atoi(p) == s) wanted = 1;
                while (*p && *p != ',') p++;
            }
            if (!wanted) continue;
        }

        size_t bytes;
        int *ids = xload(path, &bytes);
        long T = (long)(bytes / sizeof(int));
        float *f_ids = NULL; (void)f_ids; (void)probe;

        /* pass 0: durations */
        kok_arena arena;
        kok_arena_init(&arena, arena_mem, ARENA_FLOATS);
        kok_text_out out;
        memset(&out, 0, sizeof out);
        out.dur_raw = malloc((size_t)T * sizeof(float));
        out.durs = malloc((size_t)T * sizeof(long));
        out.d = malloc((size_t)T * (size_t)(dhid + sd) * sizeof(float));
        out.asr_tok = malloc((size_t)asr * (size_t)T * sizeof(float));
        rc = kok_text_forward(&mo, ids, T, 0, &out, &arena);
        if (rc) { fprintf(stderr, "s%d: text rc %d\n", s, rc); return 2; }
        long F = 0;
        for (long t = 0; t < T; t++) F += out.durs[t];

        /* pass 1: aligned stages */
        kok_arena_reset(&arena);
        out.en = malloc((size_t)(dhid + sd) * (size_t)F * sizeof(float));
        out.asr = malloc((size_t)asr * (size_t)F * sizeof(float));
        out.f0 = malloc((size_t)(2 * F) * sizeof(float));
        out.nz = malloc((size_t)(2 * F) * sizeof(float));
        rc = kok_text_forward(&mo, ids, T, 1, &out, &arena);
        if (rc) { fprintf(stderr, "s%d: text(mode 1) rc %d\n", s, rc); return 2; }

        printf("  sentence %d: T=%ld F=%ld (arena peak %.1f MB)\n", s, T, F,
               (double)kok_arena_peak(&arena) * sizeof(float) / 1048576.0);

        struct { const char *stage; const float *mine; long n; } stages[] = {
            {"dur_raw", out.dur_raw, T}, {"d", out.d, T * (dhid + sd)},
            {"asr_tok", out.asr_tok, asr * T}, {"en", out.en, (long)(dhid + sd) * F},
            {"asr", out.asr, (long)asr * F}, {"F0", out.f0, 2 * F}, {"N", out.nz, 2 * F},
        };
        for (unsigned k = 0; k < sizeof stages / sizeof stages[0]; k++) {
            snprintf(path, sizeof path, "%s/s%d_%s.bin", fx, s, stages[k].stage);
            size_t nb;
            float *ref = xload(path, &nb);
            if (!ref) { all_ok = 0; continue; }
            long rn = (long)(nb / sizeof(float));
            if (rn != stages[k].n)
                printf("    %-10s size mismatch: C %ld, golden %ld\n", stages[k].stage,
                       stages[k].n, rn);
            else if (!report(stages[k].stage, stages[k].mine, ref, rn, tol))
                all_ok = 0;
            free(ref);
        }
        /* integer stages */
        {
            snprintf(path, sizeof path, "%s/s%d_durs.bin", fx, s);
            size_t nb;
            int *ref = xload(path, &nb);
            long rn = (long)(nb / sizeof(int));
            long diff = 0;
            for (long t = 0; t < T && t < rn; t++) if (ref[t] != (int)out.durs[t]) diff++;
            printf("    %-10s %s (%ld of %ld differ)\n", "durs", diff ? "FAIL" : "OK", diff, T);
            if (diff) all_ok = 0;
            free(ref);
        }
        free(ids); free(out.dur_raw); free(out.durs); free(out.d);
        free(out.asr_tok); free(out.en); free(out.asr); free(out.f0); free(out.nz);
    }
    printf("%s\n", all_ok ? "text gate: PASS" : "text gate: FAIL");
    return all_ok ? 0 : 1;
}