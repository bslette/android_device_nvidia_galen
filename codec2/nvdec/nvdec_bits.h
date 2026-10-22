/* Exp-Golomb bit reader over RBSP and emulation-prevention removal, for the NVDEC parsers. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* ---- bit reader over RBSP; overruns set err and read zeros ---- */
struct br { const uint8_t *p; size_t n, pos; int err; };

static inline unsigned u1(struct br *b) {
    if (b->pos >= b->n * 8) { b->err = 1; return 0; }
    unsigned v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}
static inline unsigned un(struct br *b, int n) { unsigned v = 0; while (n--) v = (v << 1) | u1(b); return v; }
static inline unsigned ue(struct br *b) {
    int z = 0;
    while (!u1(b)) {
        if (b->err || ++z > 31) { b->err = 1; return 0; }
    }
    return ((1u << z) - 1) + un(b, z);
}
static inline int se(struct br *b) { unsigned k = ue(b); return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2); }
static inline int more_rbsp_data(struct br *b) {
    size_t last = b->n * 8;
    while (last > 0 && !((b->p[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1)) last--;
    return last > 0 && b->pos < last - 1;
}

static inline size_t unescape(const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0, z = 0;
    for (size_t i = 0; i < n; i++) {
        if (z >= 2 && in[i] == 3) { z = 0; continue; }
        z = in[i] == 0 ? z + 1 : 0;
        out[o++] = in[i];
    }
    return o;
}
