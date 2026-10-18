/*
 * snt_jp_server -- Japanese TTS server for the audiobook.koplugin plugin.
 *
 * Speaks the same JSON-lines protocol as sanotts/snt_server.c so the plugin's
 * PiperQueue can drive it unchanged: one line per utterance on stdin,
 *
 *     {"text": "...", "output_file": "/tmp/....wav"}
 *
 * and the WAV path echoed on stdout per line (always, even when the line
 * synthesizes to silence -- the caller polls a <path>.done marker and then
 * the file itself).  Weights and dictionary are the user's own downloads
 * (bring-your-own-weights; the plugin ships none):
 *
 *     snt_jp_server --weights saanotts-jp-v4-int8.bin --dict k1-dict-438750.bin
 *
 * Flags the queue's wrapper appends are accepted as recognized no-ops:
 * --json-input (this server is always JSON-lines), --sentence_silence N,
 * --engine X, --length-scale F, --pause-scale F.  The v1 core has no user
 * speed knob, so the rate slider genuinely does not apply to this voice.
 *
 * Inference core: the vendored sanoTTS-jp C99 sources under csrc/ (MIT),
 * see PROVENANCE.md.  Route selection mirrors upstream esp32/main/saan_kanji.c:
 * kana text goes straight through saan_g2p(); kanji-mixed text goes through
 * the dictionary Viterbi + NJD chain + accent rules + JPCommon labels.
 * Text is split into sentence-sized chunks so every saan_synthesize call
 * stays in the length range the model was validated on; chunk PCM is
 * concatenated into one WAV.
 *
 * Exit codes: 0 = stdin consumed, 1 = usage, 2 = weights bad/missing,
 * 3 = dictionary bad/missing.
 *
 * SPDX-License-Identifier: MIT (driver; vendored core carries its own headers)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "saanotts.h"
#include "g2p.h"
#include "jdict.h"
#include "accent.h"
#include "njd_rules.h"
#include "label_ids.h"
#include "dan_table.h"
#include "openjtalk/njd.h"
#include "openjtalk/jpcommon.h"
#include "openjtalk/mecab2njd.h"
#include "openjtalk/njd2jpcommon.h"
#include "openjtalk/njd_set_pronunciation.h"
#include "openjtalk/njd_set_digit.h"
#include "openjtalk/njd_set_accent_phrase.h"
#include "openjtalk/njd_set_accent_type.h"
#include "openjtalk/njd_set_unvoiced_vowel.h"
#include "openjtalk/njd_set_long_vowel.h"

/* Sizing mirrors upstream esp32/main/saan_kanji.h: MAX_TOK is the array
 * dimension for the NJD stages (njd_set_digit grows nodes), while the INPUT
 * cap is the separate MAX_INPUT_TOK (44) that bounds OpenJTalk's temporary
 * heap.  Chunking keeps us inside all of it. */
#define JP_LINE_MAX     65536
#define JP_CHUNK_MAX    512      /* bytes per synthesis chunk (UTF-8 safe) */
#define JP_MAX_UNITS    512      /* highlight units per request */
#define SAAN_HOP_SAMPLES 256     /* SAAN_HOP: frames to PCM samples */
#define JP_KEY_MAX      1024
#define JP_MAX_TOK      96
#define JP_MAX_LABEL    512
#define JP_FEAT_MAX     320
#define JP_VITERBI_N    (48u * 1024u)
#define JP_SILENCE_MS   100

static uint8_t       s_key[JP_KEY_MAX];
static jdict_token_t s_tok[JP_MAX_TOK];
static char          s_feat_store[JP_MAX_TOK][JP_FEAT_MAX];
static char         *s_feat[JP_MAX_TOK];
static accent_node_t s_k4[JP_MAX_TOK];
static char         *s_lab[JP_MAX_LABEL];
static uint8_t       s_vit[JP_VITERBI_N];

static void wr16(FILE *f, unsigned v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void wr32(FILE *f, unsigned v) { wr16(f, v & 0xffff); wr16(f, v >> 16); }

/* 22.05 kHz / mono / 16-bit PCM; rounding matches upstream dump_pcm.c. */
static int write_wav(const char *path, const float *pcm, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const unsigned data = (unsigned)n * 2u;
    fwrite("RIFF", 1, 4, f); wr32(f, 36u + data); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); wr32(f, 16); wr16(f, 1); wr16(f, 1);
    wr32(f, 22050); wr32(f, 22050u * 2u); wr16(f, 2); wr16(f, 16);
    fwrite("data", 1, 4, f); wr32(f, data);
    for (int i = 0; i < n; ++i) {
        float v = pcm[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        long q = (long)(v * 32767.0f + (v >= 0.0f ? 0.5f : -0.5f));
        if (q > 32767) q = 32767;
        if (q < -32768) q = -32768;
        wr16(f, (unsigned)(q & 0xffff));
    }
    fclose(f);
    return 0;
}

/* Append count samples to a growable float buffer.  Returns 0 or -1 (OOM). */
static int pcm_append(float **acc, size_t *count, size_t *cap, const float *src, int count_add) {
    if (count_add <= 0) return 0;
    if (*count + (size_t)count_add > *cap) {
        size_t need = *cap ? *cap : 8192;
        while (need < *count + (size_t)count_add) need *= 2;
        float *grown = realloc(*acc, need * sizeof(float));
        if (!grown) return -1;
        *acc = grown;
        *cap = need;
    }
    memcpy(*acc + *count, src, (size_t)count_add * sizeof(float));
    *count += (size_t)count_add;
    return 0;
}

static int pcm_append_silence(float **acc, size_t *count, size_t *cap, int samples) {
    static const float zero;
    for (int i = 0; i < samples; ++i)
        if (pcm_append(acc, count, cap, &zero, 1) != 0) return -1;
    return 0;
}

/* Slurp a whole file.  Returns malloc'd buffer or NULL. */
static void *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)sz;
    return buf;
}

/* mmap the dictionary read-only; falls back to a heap read when mmap is
 * unavailable.  blob/out_size follow the slurp contract. */
static uint8_t *dict_slurp(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return NULL; }
    size_t len = (size_t)st.st_size;
    void *p = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p != MAP_FAILED) {
        *size = len;
        return (uint8_t *)p;
    }
    return slurp(path, size);
}

/* Kanji-mixed text -> student ids.  Host adaptation of upstream
 * esp32/main/saan_kanji.c saan_kanji_to_ids(): same chain, plain libc for
 * the OpenJTalk temporary heap, static arrays for the fixed-capacity stages.
 * Returns 0 on success; negative on too-long/key/analyze/feature/ids errors. */
/* Timing out-params (optional): per-NJD-node UTF-8 surface byte length and
 * mora count, for the read-along highlight's per-character spans. */
/* ── per-character timing units ────────────────────────────────────────
 * Japanese has no whitespace words; the read-along highlight works per
 * character, merging small kana and the long-vowel mark into the preceding
 * unit, and skipping whitespace and CJK punctuation.  The plugin's
 * TextParser implements the same rule client-side. */

static size_t utf8_decode(const char *s, size_t remaining, uint32_t *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && remaining >= 2) {
        *cp = ((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && remaining >= 3) {
        *cp = ((c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6)
              | ((unsigned char)s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && remaining >= 4) {
        *cp = ((c & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12)
              | (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
        return 4;
    }
    *cp = c;
    return 1;
}

static int cp_is_jp_content(uint32_t cp) {
    return (cp >= 0x3041 && cp <= 0x30FF)
        || (cp >= 0x3400 && cp <= 0x4DBF)
        || (cp >= 0x4E00 && cp <= 0x9FFF)
        || (cp >= 0xF900 && cp <= 0xFAFF)
        || (cp >= 0xFF66 && cp <= 0xFF9D);
}

static int cp_is_small_kana(uint32_t cp) {
    return (cp >= 0x3041 && cp <= 0x3083 && (cp == 0x3041 || cp == 0x3043
                || cp == 0x3045 || cp == 0x3047 || cp == 0x3049 || cp == 0x3063))
        || cp == 0x3083 || cp == 0x3085 || cp == 0x3087
        || cp == 0x3095 || cp == 0x3096
        || cp == 0x30A1 || cp == 0x30A3 || cp == 0x30A5 || cp == 0x30A7
        || cp == 0x30A9 || cp == 0x30C3 || cp == 0x30E3 || cp == 0x30E5
        || cp == 0x30E7 || cp == 0x30F5 || cp == 0x30F6
        || (cp >= 0xFF67 && cp <= 0xFF6F);
}

static int cp_is_prolong(uint32_t cp) {
    return cp == 0x30FC || cp == 0xFF70;
}

static int cp_is_jp_skip(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r'
        || cp == 0x3001 || cp == 0x3002
        || cp == 0xFF0C || cp == 0xFF0E
        || cp == 0xFF01 || cp == 0xFF1F
        || cp == 0xFF1B || cp == 0xFF1A
        || cp == 0x2026 || cp == 0x2015 || cp == 0x2025
        || cp == 0x300C || cp == 0x300D || cp == 0x300E || cp == 0x300F
        || cp == 0xFF08 || cp == 0xFF09;
}

static int chunk_units(const char *p, size_t end, size_t base,
                       size_t *ustart, size_t *uend, int cap) {
    size_t i = 0;
    int n = 0;
    while (i < end) {
        uint32_t cp;
        size_t w = utf8_decode(p + i, end - i, &cp);
        if (w == 0) w = 1;
        if (cp_is_jp_skip(cp)) { i += w; continue; }
        size_t us = i, ue = i + w;
        i += w;
        while (i < end) {
            uint32_t c2;
            size_t w2 = utf8_decode(p + i, end - i, &c2);
            if (w2 == 0) w2 = 1;
            if (cp_is_small_kana(c2) || cp_is_prolong(c2)) {
                ue = i + w2;
                i += w2;
                continue;
            }
            break;
        }
        if (n < cap) {
            ustart[n] = base + us;
            uend[n] = base + ue;
        }
        n++;
    }
    return n;
}

static size_t utf8_floor(const char *s, size_t pos);  /* defined below */

static int kanji_to_ids(const jdict_t *d, const char *text, size_t nbytes,
                        int32_t *ids, int32_t ids_cap, int32_t *n_ids) {
    *n_ids = 0;
    if (nbytes >= JP_KEY_MAX) return -1;

    size_t key_n = JP_KEY_MAX;
    if (jdict_encode_key(d, (const uint8_t *)text, nbytes, s_key, &key_n) != 0)
        return -2;

    int nt = jdict_analyze(d, s_key, key_n, s_vit, JP_VITERBI_N, s_tok, JP_MAX_TOK);
    if (nt <= 0) return -3;
    if (nt > 44) return -1;

    int nf = 0;
    for (int i = 0; i < nt && nf < JP_MAX_TOK; i++) {
        char surf[128];
        if (jdict_key_to_utf8(d, s_key, s_tok[i].begin, s_tok[i].end,
                              surf, sizeof surf) < 0)
            return -4;
        int r;
        if (s_tok[i].entry & JDICT_UNKNOWN_FLAG) {
            r = jdict_unk_guess(d, s_tok[i].entry, surf,
                                s_feat_store[nf], JP_FEAT_MAX);
            if (r < 0)
                r = jdict_unk_feature(d, s_tok[i].entry, surf,
                                      s_feat_store[nf], JP_FEAT_MAX);
        } else {
            r = jdict_entry_feature(d, s_tok[i].entry, surf,
                                    s_feat_store[nf], JP_FEAT_MAX);
        }
        if (r < 0) return -4;
        s_feat[nf] = s_feat_store[nf];
        nf++;
    }
    if (nf <= 0) return -4;

    NJD njd;
    JPCommon jp;
    NJD_initialize(&njd);
    JPCommon_initialize(&jp);
    mecab2njd(&njd, s_feat, nf);
    njd_set_pronunciation(&njd);
    njd_rules_before_chaining(&njd);
    njd_set_digit(&njd);
    njd_set_accent_phrase(&njd);
    njd_set_accent_type(&njd);
    njd_set_unvoiced_vowel(&njd);
    njd_set_long_vowel(&njd);

    njd2jpcommon(&jp, &njd);
    JPCommon_make_label(&jp);
    int nl = JPCommon_get_label_size(&jp);
    char **ls = JPCommon_get_label_feature(&jp);
    if (nl > JP_MAX_LABEL) nl = JP_MAX_LABEL;
    for (int i = 0; i < nl; i++) s_lab[i] = ls[i];

    label_ids_status ks = label_ids_convert((const char *const *)s_lab, nl,
                                            text, ids, ids_cap, n_ids);
    JPCommon_clear(&jp);
    NJD_clear(&njd);
    return (ks == LABEL_IDS_OK) ? 0 : -5;
}

/* --- JSON line parsing: verbatim shape of sanotts/snt_server.c ---------- */
static int json_fields(char *line, char **input, char **output) {
    *input = NULL; *output = NULL;
    char *cursor = line;
    while ((cursor = strchr(cursor, '"')) != NULL) {
        char *key = cursor + 1;
        char *kend = strchr(key, '"');
        if (!kend) break;
        *kend = 0;
        char *rest = kend + 1;
        while (*rest == ' ' || *rest == ':') rest++;
        if (*rest != '"') { cursor = rest; continue; }
        rest++;
        char *out = rest, *r = rest;
        while (*r && *r != '"') {
            if (*r == '\\' && r[1]) {
                r++;
                switch (*r) {
                case 'n': *out++ = '\n'; break;
                case 't': *out++ = '\t'; break;
                case 'r': *out++ = '\r'; break;
                default: *out++ = *r;
                }
                r++;
            } else *out++ = *r++;
        }
        if (*r != '"') { cursor = kend + 1; continue; }
        *out = 0;
        if ((!strcmp(key, "input") || !strcmp(key, "text")) && !*input) *input = rest;
        else if ((!strcmp(key, "output") || !strcmp(key, "output_file")) && !*output) *output = rest;
        cursor = r + 1;
        if (*input && *output) return 0;
    }
    return (*input && *output) ? 0 : -1;
}

/* Move a UTF-8 boundary backwards from pos (pos stays if already one). */
/* ── per-character timing units ────────────────────────────────────────
 * Japanese has no whitespace words; the read-along highlight works per
 * character, merging small kana (ゃゅょ...) and the long-vowel mark into
 * the preceding unit, and skipping whitespace and CJK punctuation.  The
 * audiobook plugin's TextParser implements the same rule client-side, so
 * the unit order here matches the words there one for one. */

static size_t utf8_floor(const char *s, size_t pos) {
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

/* Find the next chunk break after `from`: sentence punctuation first
 * (。！？\n), else a hard JP_CHUNK_MAX cut adjusted to a UTF-8 boundary. */
static size_t next_chunk(const char *text, size_t n, size_t from) {
    size_t limit = from + JP_CHUNK_MAX;
    if (limit >= n) return n;
    for (size_t i = from; i < n && i < limit; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') return i + 1;
        if (c == 0xE3 && i + 2 < n) {
            unsigned char c2 = (unsigned char)text[i + 1];
            unsigned char c3 = (unsigned char)text[i + 2];
            if (c2 == 0x80 && (c3 == 0x82 || c3 == 0x81)) return i + 3;
        }
    }
    size_t cut = utf8_floor(text, limit);
    return (cut > from) ? cut : from + 1;
}

int main(int argc, char **argv) {
    const char *weights_path = NULL;
    const char *dict_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--weights") && i + 1 < argc) {
            weights_path = argv[++i];
        } else if (!strcmp(argv[i], "--dict") && i + 1 < argc) {
            dict_path = argv[++i];
        } else if (!strcmp(argv[i], "--json-input")) {
            /* always on here */
        } else if (!strcmp(argv[i], "--sentence_silence") && i + 1 < argc) {
            i++; /* the queue appends it for snt_server; per-utterance WAVs
                    carry their own trailing silence */
        } else if ((!strcmp(argv[i], "--engine")
                    || !strcmp(argv[i], "--length-scale")
                    || !strcmp(argv[i], "--pause-scale")) && i + 1 < argc) {
            i++; /* accepted no-ops so one wrapper serves every backend */
        } else if (!strcmp(argv[i], "--version")) {
            printf("snt_jp_server 0.1.0 (sanoTTS-jp core)\n");
            return 0;
        }
    }
    if (!weights_path || !dict_path) {
        fprintf(stderr, "usage: %s --weights FILE --dict FILE\n", argv[0]);
        return 1;
    }

    size_t wsize = 0;
    uint8_t *wblob = slurp(weights_path, &wsize);
    saan_weights W;
    if (!wblob || saan_weights_open(&W, wblob, wsize) != SAAN_OK) {
        fprintf(stderr, "{\"error\": \"bad or missing weights blob (%s)\"}\n", weights_path);
        return 2;
    }

    size_t dsize = 0;
    uint8_t *dblob = dict_slurp(dict_path, &dsize);
    jdict_t D;
    if (!dblob || jdict_open(&D, dblob, dsize) != 0) {
        fprintf(stderr, "{\"error\": \"bad or missing dictionary blob (%s)\"}\n", dict_path);
        return 3;
    }
    fprintf(stderr, "snt_jp_server: ready (weights %zu B, dictionary %zu B)\n",
            wsize, D.blob_len);

    char line[JP_LINE_MAX];
    while (fgets(line, sizeof line, stdin)) {
        char *input = NULL, *output = NULL;
        if (json_fields(line, &input, &output) != 0) {
            fprintf(stderr, "{\"error\": \"bad line\"}\n");
            continue;
        }
        if (!input[0]) {
            fprintf(stderr, "{\"error\": \"empty input\"}\n");
            continue;
        }

        float *acc = NULL;
        size_t acc_n = 0, acc_cap = 0;
        int had_error = 0;

        /* Timing units for the request: audio-sample spans per highlight
         * unit (character), accumulated across chunks. */
        long *u_start = calloc(JP_MAX_UNITS, sizeof(long));
        long *u_end = calloc(JP_MAX_UNITS, sizeof(long));
        int n_units = 0;
        int align_ok = 1;

        /* Sentence-sized chunks; every saan_synthesize call stays in the
         * length range the core was validated on. */
        size_t text_n = strlen(input);
        size_t pos = 0;
        while (pos < text_n) {
            size_t end = next_chunk(input, text_n, pos);
            size_t nbytes = end - pos;
            const char *chunk = input + pos;
            pos = end;

            int32_t ids[saan_g2p_capacity(JP_CHUNK_MAX > JP_KEY_MAX
                                              ? JP_CHUNK_MAX : JP_KEY_MAX)];
            int32_t n_ids = 0;
            saan_g2p_route route = saan_g2p_classify(chunk, nbytes, NULL, NULL);
            if (route == SAAN_G2P_ROUTE_KANA) {
                saan_g2p(chunk, nbytes, ids, (int32_t)(sizeof ids / sizeof ids[0]),
                         &n_ids, NULL);
            } else if (route == SAAN_G2P_ROUTE_DICT) {
                if (kanji_to_ids(&D, chunk, nbytes, ids,
                                 (int32_t)(sizeof ids / sizeof ids[0]), &n_ids) != 0)
                    n_ids = 0;
            } else {
                n_ids = 0; /* REJECT: undecodable input, this chunk is silent */
            }

            if (n_ids <= 0) {
                pcm_append_silence(&acc, &acc_n, &acc_cap,
                                   22050 * JP_SILENCE_MS / 1000);
                if (align_ok && u_start) {
                    size_t us[JP_MAX_UNITS], ue[JP_MAX_UNITS];
                    if (chunk_units(chunk, nbytes, 0, us, ue, JP_MAX_UNITS) > 0)
                        align_ok = 0;   /* content chars we cannot span */
                }
                continue;
            }

            saan_arena A;
            size_t need = saan_arena_needed(n_ids);
            void *arena = malloc(need);
            if (!arena) { had_error = 1; break; }
            saan_arena_init(&A, arena, need);
            saan_output out;
            saan_status rc = saan_synthesize(&W, &A, ids, n_ids, SAAN_S_V, &out);
            if (rc != SAAN_OK) {
                free(arena);
                fprintf(stderr, "{\"error\": \"synth rc %d\"}\n", rc);
                had_error = 1;
                break;
            }
            /* Read-along timing: derive per-character sample spans from the
             * duration student's per-token output (out.d_hat). */
            if (align_ok && u_start && n_units < JP_MAX_UNITS) {
                size_t ustart[JP_MAX_UNITS], uend[JP_MAX_UNITS];
                int cap = JP_MAX_UNITS - n_units;
                int un = chunk_units(chunk, nbytes, 0, ustart, uend, cap);
                long base = (long)acc_n;   /* samples before this chunk */
                if (un <= 0) {
                    /* nothing pronounced in this chunk: no units, fine */
                } else if (route == SAAN_G2P_ROUTE_KANA) {
                    /* Kana: g2p each unit in isolation for its token count,
                     * then sum d_hat over that run.  The N-allophone rewrite
                     * is 1:1 so counts match the whole-chunk stream. */
                    int32_t cnt[JP_MAX_UNITS];
                    long sum = 0;
                    int bad = 0;
                    for (int uu = 0; uu < un; uu++) {
                        int32_t uid[512];
                        int32_t un_ids = 0;
                        if (saan_g2p(input + ustart[uu] - pos,
                                     uend[uu] - ustart[uu],
                                     uid, 512, &un_ids, NULL) != SAAN_OK
                                || un_ids <= 0) { bad = 1; break; }
                        cnt[uu] = un_ids;
                        sum += un_ids;
                    }
                    if (bad || sum != n_ids) {
                        fprintf(stderr, "{\"timing\": \"kana count %ld vs %d\"}\n",
                                sum, n_ids);
                        align_ok = 0;
                    } else {
                        long idx = 0;
                        long sstart = 0;
                        for (int uu = 0; uu < un; uu++) {
                            long frames = 0;
                            for (int k = 0; k < cnt[uu]; k++)
                                frames += out.d_hat[idx + (long)k];
                            idx += cnt[uu];
                            u_start[n_units] = base + sstart;
                            u_end[n_units] = base + sstart + frames * SAAN_HOP_SAMPLES;
                            n_units++;
                            sstart += frames * SAAN_HOP_SAMPLES;
                        }
                    }
                } else if (route == SAAN_G2P_ROUTE_DICT) {
                    /* Kanji chunks: NJD chaining merges word boundaries (は+い
                     * becomes one node), so exact per-character attribution is
                     * not derivable; distribute the chunk's duration evenly
                     * across its units.  Japanese being mora-timed, this
                     * tracks speech closely except where kanji readings are
                     * much longer than kana ones. */
                    long frames_total = 0;
                    for (int32_t i = 0; i < n_ids; i++) frames_total += out.d_hat[i];
                    if (frames_total <= 0) align_ok = 0;
                    else {
                        long running = 0;
                        for (int uu = 0; uu < un; uu++) {
                            long share = frames_total / un;
                            if (uu == un - 1) share = frames_total - running;
                            u_start[n_units] = base + running * SAAN_HOP_SAMPLES;
                            u_end[n_units] = base + (running + share)
                                             * SAAN_HOP_SAMPLES;
                            n_units++;
                            running += share;
                        }
                    }
                } else {
                    align_ok = 0;
                }
            }
            if (pcm_append(&acc, &acc_n, &acc_cap, out.pcm, out.n_samples) != 0)
                had_error = 1;
            free(arena);
            if (had_error) break;
        }

        if (acc_n == 0)
            pcm_append_silence(&acc, &acc_n, &acc_cap, 22050 * JP_SILENCE_MS / 1000);
        /* Read-along timing sidecar, same contract as sanotts/snt_server.c:
         * written and closed before the WAV path reaches stdout.  One clause
         * (the request), one span per highlight unit (character). */
        if (align_ok && n_units > 0 && acc_n > 0) {
            char tpath[1100];
            snprintf(tpath, sizeof tpath, "%s.timing", output);
            FILE *tf = fopen(tpath, "w");
            if (tf) {
                fprintf(tf, "sntt 1 22050 %d %zu 1 %d\n",
                        SAAN_HOP_SAMPLES, acc_n, n_units);
                fprintf(tf, "c 0 0 0 0 0 %zu 0 %d\n", acc_n, n_units);
                for (int i = 0; i < n_units; i++)
                    fprintf(tf, "w %d %ld %ld\n", i, u_start[i], u_end[i]);
                fclose(tf);
            }
        }
        free(u_start);
        free(u_end);
        if (write_wav(output, acc, (int)acc_n) != 0) {
            fprintf(stderr, "{\"error\": \"cannot write %s\"}\n", output);
            fflush(stderr);
            free(acc);
            continue;
        }
        free(acc);
        /* Always echo the path, even for error lines: the caller's .done
         * polling and while-read loop hang otherwise (snt_server precedent). */
        printf("%s\n", output);
        fflush(stdout);
        if (had_error) fflush(stderr);
    }
    return 0;
}
