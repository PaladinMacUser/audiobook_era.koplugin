/* kok_meta.h -- Kokoro-family model config and weight slot table.
 *
 * The meta blob (kok_meta.bin) carries:
 *   u32 magic 'KOKM', u32 version, u32 n_dims, i32 dims[n_dims],
 *   u32 n_names, { u32 name_off, u32 name_len } * n_names,
 *   u32 name_blob_bytes, u8 name_blob[],
 *   u32 n_slots, { u32 name_index, u32 blob_id, i32 dtype, i32 qkind,
 *                  i32 count, u32 offset, i32 rows, i32 row_len } * n_slots,
 *   f32 scale (per slot, for quantized kinds)
 *
 * Weight tensors are addressed by NAME, not position: the family spans Paradee
 * presets, Kokoro-7M-Distill and Kokoro-82M with different widths and optional
 * blocks, so positional slots would not scale. Every lookup validates the
 * element count against the shape the config implies and fails loudly, the same
 * contract as snt_piperlite_init in sanotts/mcu.
 *
 * Dims are a fixed-order list, mirrored from tools/export_kokoro_blobs.py
 * (DIMS). Extend only at the end.
 */
#ifndef KOK_META_H
#define KOK_META_H

#include <stddef.h>
#include <stdint.h>

#define KOK_MAGIC 0x4B4F4B4D    /* 'KOKM' */
#define KOK_VERSION 1

#define KOK_DT_F32 0
#define KOK_DT_I8 1
#define KOK_DT_F16 2
#define KOK_QK_NONE 0
#define KOK_QK_PER_ROW_I8 1

/* dims order (mirrors DIMS in the exporter) */
enum {
    KOK_D_N_TOKEN, KOK_D_SR,
    KOK_D_ALBERT_HIDDEN, KOK_D_ALBERT_LAYERS, KOK_D_ALBERT_HEADS,
    KOK_D_ALBERT_INTERMEDIATE, KOK_D_ALBERT_EMBED, KOK_D_ALBERT_MAX_POS,
    KOK_D_TEXT_STYLE_DIM, KOK_D_PROSODY_D_HID, KOK_D_PROSODY_NLAYERS,
    KOK_D_PROSODY_MAX_DUR, KOK_D_TEXTENC_CHANNELS, KOK_D_TEXTENC_DEPTH,
    KOK_D_TEXTENC_KERNEL, KOK_D_ASR_DIM, KOK_D_ASR_HIDDEN,
    KOK_D_DEC_WIDTH, KOK_D_DEC_UIC,
    KOK_D_DEC_KERNEL0, KOK_D_DEC_KERNEL1, KOK_D_DEC_KERNEL2,
    KOK_D_DEC_UPRATE0, KOK_D_DEC_UPRATE1,
    KOK_D_DEC_UPKERN0, KOK_D_DEC_UPKERN1,
    KOK_D_DEC_ISTFT_NFFT, KOK_D_DEC_ISTFT_HOP, KOK_D_DEC_STYLE_DIM,
    KOK_D_DEC_DIL0, KOK_D_DEC_DIL1, KOK_D_DEC_DIL2,
    KOK_D_LOCK_L, KOK_D_LOCK_NFFT, KOK_D_LOCK_HOP,
    KOK_D_LOCK_BAND_LO_HZ, KOK_D_LOCK_BAND_HI_HZ, KOK_D_LOCK_VOICED_HZ,
    KOK_D_GEN_HARMONICS,
    KOK_D_COUNT
};

typedef struct {
    int v[KOK_D_COUNT];
} kok_config;

typedef struct {
    const char *name;       /* points into the meta buffer */
    int blob_id;            /* 0 = text blob, 1 = decoder blob */
    int dtype;              /* KOK_DT_* */
    int qkind;              /* KOK_QK_* */
    long count;             /* elements */
    long offset;            /* bytes into the blob */
    int rows;               /* -1 when 1-D */
    int row_len;
    float scale;            /* per-slot, for quantized kinds */
} kok_slot;

typedef struct {
    const unsigned char *base;
    size_t bytes;
    kok_config cfg;
    const char *name_blob;
    uint32_t name_blob_bytes;
    const uint32_t *name_entries;   /* 2 per name: offset, length */
    uint32_t n_names;
    const unsigned char *slots_raw;
    uint32_t n_slots;
} kok_meta;

/* Parse the meta blob. Returns 0, or a negative code:
 * -1 bad args, -2 truncated, -3 bad magic, -4 bad version, -5 bad dims,
 * -6 bad name table, -7 bad slot table. */
int kok_meta_init(kok_meta *m, const void *blob, size_t bytes);

/* Slot lookup by name; NULL when absent. */
const kok_slot *kok_find(const kok_meta *m, const char *name);

/* Lookup that also validates the element count; returns NULL and leaves
 * *rc = 0 on success, or sets *rc negative (-8 not found, -9 count mismatch). */
const kok_slot *kok_find_n(const kok_meta *m, const char *name, long count, int *rc);

/* Blob-scoped lookup with a count check: the text and decoder halves each have a
 * "style" slot, so a name alone is ambiguous. rc -8 not found, -9 count. */
const kok_slot *kok_find_blob(const kok_meta *m, const char *name, int blob_id,
                              long count, int *rc);

/* f32 view of a slot inside its blob (NULL when the dtype is not f32). Pass the
 * blob that matches slot->blob_id. */
const float *kok_f32(const kok_slot *s, const void *blob);

#endif