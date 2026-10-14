/* kok_g2p.h -- text to Paradee phoneme ids. See kok_g2p.c. */
#ifndef KOK_G2P_H
#define KOK_G2P_H

#include <stdint.h>

/* English text -> framed phoneme ids (0 pad, then the vocabulary ids, then a
 * trailing 0), at most cap ids. Returns the id count, or negative:
 * -1 bad args, -2 the lexicon G2P failed (empty or malformed text).
 * dropped_out counts phonemes outside Paradee's vocabulary, oov_out the words
 * the lexicon did not know (both may be NULL). */
int kok_text_to_ids(const char *text, int32_t *ids, int cap, int *dropped_out,
                    int *oov_out);

#endif
