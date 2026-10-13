/* kok_meta.c -- parse the model config and the by-name weight slot table.
 * See kok_meta.h for the byte layout. */
#include <string.h>

#include "kok_meta.h"

static int32_t rd_i32(const unsigned char *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static uint32_t rd_u32(const unsigned char *p) { return (uint32_t)rd_i32(p); }

static float rd_f32(const unsigned char *p) {
    union { uint32_t u; float f; } v;
    v.u = rd_u32(p);
    return v.f;
}

int kok_meta_init(kok_meta *m, const void *blob, size_t bytes) {
    const unsigned char *p = (const unsigned char *)blob;
    size_t off;
    uint32_t i;

    if (!m || !blob) return -1;
    if (bytes < 16) return -2;
    memset(m, 0, sizeof *m);
    if (rd_u32(p) != (uint32_t)KOK_MAGIC) return -3;
    if (rd_u32(p + 4) != (uint32_t)KOK_VERSION) return -4;

    uint32_t n_dims = rd_u32(p + 8);
    if (n_dims != (uint32_t)KOK_D_COUNT) return -5;
    if (bytes < 12 + (size_t)n_dims * 4 + 4) return -2;
    for (i = 0; i < n_dims; i++)
        m->cfg.v[i] = rd_i32(p + 12 + (size_t)i * 4);
    if (m->cfg.v[KOK_D_N_TOKEN] <= 0 || m->cfg.v[KOK_D_SR] <= 0 ||
        m->cfg.v[KOK_D_ALBERT_HIDDEN] <= 0 || m->cfg.v[KOK_D_ALBERT_LAYERS] <= 0 ||
        m->cfg.v[KOK_D_ALBERT_HEADS] <= 0)
        return -5;

    off = 12 + (size_t)n_dims * 4;
    m->n_names = rd_u32(p + off);
    off += 4;                                   /* past the name count */
    if (bytes < off + (size_t)m->n_names * 8 + 4) return -6;
    m->name_entries = (const uint32_t *)(const void *)(p + off);
    off += (size_t)m->n_names * 8;
    if (bytes < off + 4) return -6;
    m->name_blob_bytes = rd_u32(p + off);
    off += 4;
    if (bytes < off + m->name_blob_bytes + 4) return -6;
    m->name_blob = (const char *)(p + off);
    off += m->name_blob_bytes;

    m->n_slots = rd_u32(p + off);
    off += 4;
    if (bytes < off + (size_t)m->n_slots * 36) return -7;
    m->slots_raw = p + off;
    m->base = p;
    m->bytes = bytes;
    return 0;
}

static int name_eq(const kok_meta *m, uint32_t idx, const char *name) {
    if (idx >= m->n_names) return 0;
    uint32_t noff = m->name_entries[2 * idx];
    uint32_t nlen = m->name_entries[2 * idx + 1];
    size_t want = strlen(name);
    if (nlen != want) return 0;
    if ((size_t)noff + nlen > m->name_blob_bytes) return 0;
    return memcmp(m->name_blob + noff, name, want) == 0;
}

static const kok_slot *slot_at(const kok_meta *m, uint32_t i, kok_slot *scratch) {
    const unsigned char *p = m->slots_raw + (size_t)i * 36;
    uint32_t name_index = rd_u32(p);
    scratch->name = NULL;
    if (name_index < m->n_names) {
        uint32_t noff = m->name_entries[2 * name_index];
        scratch->name = m->name_blob + noff;
    }
    scratch->blob_id = (int)rd_i32(p + 4);
    scratch->qkind = (int)rd_i32(p + 8);
    scratch->dtype = (int)rd_i32(p + 12);
    scratch->count = (long)rd_i32(p + 16);
    scratch->offset = (long)rd_u32(p + 20);
    scratch->rows = (int)rd_i32(p + 24);
    scratch->row_len = (int)rd_i32(p + 28);
    scratch->scale = rd_f32(p + 32);
    return scratch;
}

const kok_slot *kok_find(const kok_meta *m, const char *name) {
    static kok_slot scratch;    /* single-threaded init use, like the family */
    uint32_t i;
    if (!m || !name) return NULL;
    for (i = 0; i < m->n_slots; i++) {
        if (name_eq(m, rd_u32(m->slots_raw + (size_t)i * 36), name))
            return slot_at(m, i, &scratch);
    }
    return NULL;
}

const kok_slot *kok_find_n(const kok_meta *m, const char *name, long count, int *rc) {
    const kok_slot *s = kok_find(m, name);
    if (!s) { if (rc) *rc = -8; return NULL; }
    if (s->count != count) { if (rc) *rc = -9; return NULL; }
    if (rc) *rc = 0;
    return s;
}

const kok_slot *kok_find_blob(const kok_meta *m, const char *name, int blob_id,
                              long count, int *rc) {
    static kok_slot scratch;
    if (!m || !name) { if (rc) *rc = -1; return NULL; }
    for (uint32_t i = 0; i < m->n_slots; i++) {
        uint32_t ni = rd_u32(m->slots_raw + (size_t)i * 36);
        if ((int)rd_i32(m->slots_raw + (size_t)i * 36 + 4) != blob_id) continue;
        if (!name_eq(m, ni, name)) continue;
        slot_at(m, i, &scratch);
        if (count >= 0 && scratch.count != count) { if (rc) *rc = -9; return NULL; }
        if (rc) *rc = 0;
        return &scratch;
    }
    if (rc) *rc = -8;
    return NULL;
}

const float *kok_f32(const kok_slot *s, const void *blob) {
    if (!s || !blob || s->dtype != KOK_DT_F32) return NULL;
    return (const float *)(const void *)((const unsigned char *)blob + s->offset);
}