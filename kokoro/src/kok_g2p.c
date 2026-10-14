/* kok_g2p.c -- text to Paradee phoneme ids, on device and on the host.
 *
 * The hard part of running Kokoro-family models off-desktop is the G2P, and the
 * plugin already has it: sanotts/nano_lex_g2p.c is a C99 port of misaki's
 * English dictionary path (us_gold/us_silver, Apache-2.0, espeak-free), which
 * is the same front end the teacher and the distill were trained with. This
 * wrapper takes its phoneme string, maps each character through Paradee's
 * 178-symbol vocabulary, and frames the result the way the model expects:
 * a single 0 pad token at each end.
 *
 * It is deliberately thin. Nothing here re-implements pronunciation.
 */
#include <string.h>

#include "kok_g2p.h"
#include "kok_vocab.h"
#include "nano_lex_g2p.h"

/* Characters that are in the vocabulary but never survive the phoneme filter
 * are dropped silently, as the Piper path does; the caller gets the count back
 * so a firmware build can log it. */
int kok_text_to_ids(const char *text, int32_t *ids, int cap, int *dropped_out,
                    int *oov_out) {
    static char phon[4096];
    int dropped = 0;
    if (!text || !ids || cap < 3) return -1;
    if (nano_lex_g2p_text_to_phonemes(text, phon, sizeof phon) < 0) return -2;

    int n = 0;
    ids[n++] = 0;                       /* leading pad, as the model expects */
    for (const unsigned char *q = (const unsigned char *)phon; *q; ) {
        int len = 1;
        if ((*q & 0xE0) == 0xC0) len = 2;
        else if ((*q & 0xF0) == 0xE0) len = 3;
        else if ((*q & 0xF8) == 0xF0) len = 4;
        char sym[8];
        if (len > 7) break;
        memcpy(sym, q, (size_t)len);
        sym[len] = 0;
        int id = -1;
        for (int i = 0; i < KOK_VOCAB_N; i++)
            if (!strcmp(KOK_VOCAB[i].sym, sym)) { id = (int)KOK_VOCAB[i].id; break; }
        if (id >= 0) {
            if (n >= cap - 1) break;    /* leave room for the trailing pad */
            ids[n++] = id;
        } else {
            dropped++;
        }
        q += len;
    }
    ids[n++] = 0;                       /* trailing pad */
    if (dropped_out) *dropped_out = dropped;
    if (oov_out) {
        nano_lex_g2p_stats_t st;
        nano_lex_g2p_get_stats(&st);
        *oov_out = (int)st.oov_words;
    }
    return n;
}
