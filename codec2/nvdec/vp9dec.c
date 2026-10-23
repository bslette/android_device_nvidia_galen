/*
 * VP9 decoding on the Tegra NVDEC engine through the upstream tegra-drm
 * channel UAPI (see vp9dec.h). The picture setup is NVIDIA's nvdec_drv.h
 * nvdec_vp9_pic_s. The engine decodes the tile data; the driver parses both
 * frame headers, keeps the four probability contexts, hands the engine the
 * frame's probabilities and adapts them afterwards from the symbol counts the
 * engine writes back. The probability/count tables and the auxiliary buffer
 * formats are those of the Hantro G2 core the engine's VP9 block descends
 * from (see Linux drivers/media/platform/verisilicon/hantro_g2_vp9_dec.c).
 * Header parsing, default probabilities and adaptation follow libvpx.
 */
#include "vp9dec.h"
#include "host1x.h"
#include "nvdec_bits.h"
#include "nvdec_drv.h"
#include "vp9_tables.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__ANDROID__) && !defined(H264DEC_STDERR)
#include <log/log.h>
#define DEC_ERR(...) ALOGE(__VA_ARGS__)
#define DEC_WARN(...) ALOGW(__VA_ARGS__)
#else
#define DEC_ERR(...) (fprintf(stderr, "vp9dec: " __VA_ARGS__), fputc('\n', stderr))
#define DEC_WARN(...) DEC_ERR(__VA_ARGS__)
#endif

#define NVC5B0_SET_APPLICATION_ID           0x200
#define NVC5B0_EXECUTE                      0x300
#define NVC5B0_SET_CONTROL_PARAMS           0x400
#define NVC5B0_SET_DRV_PIC_SETUP_OFFSET     0x404
#define NVC5B0_SET_IN_BUF_BASE_OFFSET       0x408
#define NVC5B0_SET_PICTURE_INDEX            0x40c
#define NVC5B0_SET_NVDEC_STATUS_OFFSET      0x424
#define NVC5B0_SET_PICTURE_LUMA_OFFSET0     0x430
#define NVC5B0_SET_PICTURE_CHROMA_OFFSET0   0x474
#define NVC5B0_VP9_SET_PROB_TAB_BUF_OFFSET  0x5c0
#define NVC5B0_VP9_SET_CTX_COUNTER_BUF_OFFSET 0x5c4
#define NVC5B0_VP9_SET_SEGMENT_READ_BUF_OFFSET 0x5c8
#define NVC5B0_VP9_SET_SEGMENT_WRITE_BUF_OFFSET 0x5cc
#define NVC5B0_VP9_SET_TILE_SIZE_BUF_OFFSET 0x5d0
#define NVC5B0_VP9_SET_COL_MVWRITE_BUF_OFFSET 0x5d4
#define NVC5B0_VP9_SET_COL_MVREAD_BUF_OFFSET 0x5d8
#define NVC5B0_VP9_SET_FILTER_BUFFER_OFFSET 0x5dc
#define APP_ID_VP9 9
#define CODEC_VP9 9
#define EXECUTE_AWAKEN (1u << 8)

#define NSURF NVDEC_NSURF
#define BITSTREAM_SIZE (8u << 20)
#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

enum { KEY_FRAME = 0, NON_KEY_FRAME = 1 };
enum { ONLY_4X4, ALLOW_8X8, ALLOW_16X16, ALLOW_32X32, TX_MODE_SELECT };
enum { SINGLE_REFERENCE, COMPOUND_REFERENCE, REFERENCE_MODE_SELECT };
enum { INTRA_FRAME, LAST_FRAME, GOLDEN_FRAME, ALTREF_FRAME };
/* libvpx INTERP_FILTER (as NVIDIA's CUVID interface numbers it too). The engine's
 * mcomp_filter_type uses the VP9 spec order instead, with smooth and regular swapped;
 * its switchable filter counts are in libvpx order. */
enum { EIGHTTAP, EIGHTTAP_SMOOTH, EIGHTTAP_SHARP, BILINEAR, SWITCHABLE };
static int spec_filter(int f) { return f <= EIGHTTAP_SMOOTH ? !f : f; }
enum { SEG_LVL_ALT_Q, SEG_LVL_ALT_LF, SEG_LVL_REF_FRAME, SEG_LVL_SKIP };

/* ---- boolean decoder (VP9 spec 9.2) ---- */
struct bd { const uint8_t *p; size_t n, pos; unsigned value, range; size_t max_bits; };

static unsigned bd_bit(struct bd *b) {
    if (b->pos >= b->n * 8) return 0;
    unsigned v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}

static int bd_read(struct bd *b, int prob) {
    unsigned split = 1 + (((b->range - 1) * (unsigned)prob) >> 8), bit;
    if (b->value < split) {
        b->range = split;
        bit = 0;
    } else {
        b->range -= split;
        b->value -= split;
        bit = 1;
    }
    while (b->range < 128) {
        unsigned nb = 0;
        if (b->max_bits) { nb = bd_bit(b); b->max_bits--; }
        b->range <<= 1;
        b->value = (b->value << 1) | nb;
    }
    return bit;
}

static int bd_literal(struct bd *b, int n) { int v = 0; while (n--) v = (v << 1) | bd_read(b, 128); return v; }

static int bd_init(struct bd *b, const uint8_t *p, size_t n) {
    if (!n) return -EINVAL;
    *b = (struct bd){ .p = p, .n = n, .range = 255, .max_bits = 8 * n - 8 };
    for (int i = 0; i < 8; i++) b->value = (b->value << 1) | bd_bit(b);
    return bd_read(b, 128) ? -EINVAL : 0; /* marker bit */
}

/* 6.3.4/6.3.5 delta-coded probability updates (libvpx vp9_dsubexp.c) */
static int inv_recenter_nonneg(int v, int m) {
    if (v > 2 * m) return v;
    return (v & 1) ? m - ((v + 1) >> 1) : m + (v >> 1);
}

static int inv_remap_prob(int v, int m) {
    static const uint8_t inv_map_table[255] = {
        7,   20,  33,  46,  59,  72,  85,  98,  111, 124, 137, 150, 163, 176, 189, 202, 215, 228, 241, 254,
        1,   2,   3,   4,   5,   6,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  21,  22,
        23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,
        44,  45,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  60,  61,  62,  63,  64,  65,
        66,  67,  68,  69,  70,  71,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  86,  87,
        88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108,
        109, 110, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 125, 126, 127, 128, 129, 130,
        131, 132, 133, 134, 135, 136, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 151, 152,
        153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173,
        174, 175, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 190, 191, 192, 193, 194, 195,
        196, 197, 198, 199, 200, 201, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 216, 217,
        218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238,
        239, 240, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 253,
    };
    v = inv_map_table[v < 254 ? v : 254];
    m--;
    if ((m << 1) <= 255) return 1 + inv_recenter_nonneg(v, m);
    return 255 - inv_recenter_nonneg(v, 255 - 1 - m);
}

static int decode_term_subexp(struct bd *b) {
    if (!bd_read(b, 128)) return bd_literal(b, 4);
    if (!bd_read(b, 128)) return bd_literal(b, 4) + 16;
    if (!bd_read(b, 128)) return bd_literal(b, 5) + 32;
    int v = bd_literal(b, 7); /* decode_uniform */
    return (v < 65 ? v : (v << 1) - 65 + bd_read(b, 128)) + 64;
}

static void diff_update(struct bd *b, uint8_t *p) {
    if (bd_read(b, 252)) *p = inv_remap_prob(decode_term_subexp(b), *p);
}

static void mv_update(struct bd *b, uint8_t *p, int n) {
    for (int i = 0; i < n; i++)
        if (bd_read(b, 252)) p[i] = (bd_literal(b, 7) << 1) | 1;
}

/* ---- probability adaptation (libvpx vpx_dsp/prob.h, vp9_entropy*.c) ---- */
static uint8_t get_prob(unsigned num, unsigned den) {
    int p = (int)(((uint64_t)num * 256 + (den >> 1)) / den);
    return p > 255 ? 255 : p < 1 ? 1 : p;
}

static uint8_t weighted_prob(int p1, int p2, int factor) { return (p1 * (256 - factor) + p2 * factor + 128) >> 8; }

static uint8_t merge_probs(uint8_t pre, const unsigned ct[2], unsigned count_sat, unsigned max_update_factor) {
    unsigned den = ct[0] + ct[1];
    uint8_t prob = den ? get_prob(ct[0], den) : 128;
    unsigned count = MIN(den, count_sat);
    return weighted_prob(pre, prob, max_update_factor * count / count_sat);
}

static uint8_t mode_mv_merge(uint8_t pre, const unsigned ct[2]) {
    static const int factor[21] = { 0, 6, 12, 19, 25, 32, 38, 44, 51, 57, 64, 70, 76, 83, 89, 96, 102, 108, 115, 121, 128 };
    unsigned den = ct[0] + ct[1];
    if (!den) return pre;
    return weighted_prob(pre, get_prob(ct[0], den), factor[MIN(den, 20)]);
}

static unsigned tree_merge(const int8_t *tree, int i, const uint8_t *pre, const unsigned *counts, uint8_t *probs) {
    int l = tree[i], r = tree[i + 1];
    unsigned ct[2] = { l <= 0 ? counts[-l] : tree_merge(tree, l, pre, counts, probs),
                       r <= 0 ? counts[-r] : tree_merge(tree, r, pre, counts, probs) };
    probs[i >> 1] = mode_mv_merge(pre[i >> 1], ct);
    return ct[0] + ct[1];
}

static const int8_t intra_mode_tree[18] = { -0, 2, -9, 4, -1, 6, 8, 12, -2, 10, -4, -5, -3, 14, -8, 16, -6, -7 };
static const int8_t inter_mode_tree[6] = { -2, 2, -0, 4, -1, -3 }; /* NEAREST 0, NEAR 1, ZERO 2, NEW 3 */
static const int8_t partition_tree[6] = { -0, 2, -1, 4, -2, -3 };
static const int8_t interp_tree[4] = { -EIGHTTAP, 2, -EIGHTTAP_SMOOTH, -EIGHTTAP_SHARP };
static const int8_t mv_joint_tree[6] = { -0, 2, -1, 4, -2, -3 };
static const int8_t mv_class_tree[20] = { -0, 2, -1, 4, 6, 8, -2, -3, 10, 12, -4, -5, -6, 14, 16, 18, -7, -8, -9, -10 };
static const int8_t mv_class0_tree[2] = { -0, -1 };
static const int8_t mv_fp_tree[6] = { -0, 2, -1, 4, -2, -3 };

static void default_ctx(struct vp9_frame_ctx *fc) {
    memcpy(fc->y_mode, vp9_default_y_mode_prob, sizeof(fc->y_mode));
    memcpy(fc->uv_mode, vp9_default_uv_mode_prob, sizeof(fc->uv_mode));
    memcpy(fc->partition, vp9_default_partition_prob, sizeof(fc->partition));
    memcpy(fc->coef[0], vp9_default_coef_4x4, sizeof(fc->coef[0]));
    memcpy(fc->coef[1], vp9_default_coef_8x8, sizeof(fc->coef[1]));
    memcpy(fc->coef[2], vp9_default_coef_16x16, sizeof(fc->coef[2]));
    memcpy(fc->coef[3], vp9_default_coef_32x32, sizeof(fc->coef[3]));
    memcpy(fc->interp, vp9_default_interp_prob, sizeof(fc->interp));
    memcpy(fc->inter_mode, vp9_default_inter_mode_prob, sizeof(fc->inter_mode));
    memcpy(fc->intra_inter, vp9_default_intra_inter_prob, sizeof(fc->intra_inter));
    memcpy(fc->comp_inter, vp9_default_comp_inter_prob, sizeof(fc->comp_inter));
    memcpy(fc->single_ref, vp9_default_single_ref_prob, sizeof(fc->single_ref));
    memcpy(fc->comp_ref, vp9_default_comp_ref_prob, sizeof(fc->comp_ref));
    fc->tx = vp9_default_tx_probs;
    memcpy(fc->skip, vp9_default_skip_prob, sizeof(fc->skip));
    fc->mv = vp9_default_mv_probs;
}

/* ---- frame header state ---- */
struct seg_params {
    int enabled, update_map, temporal_update, update_data, abs_delta;
    uint8_t tree_probs[7], pred_probs[3];
    uint8_t feature_enabled[8][4];
    int16_t feature_data[8][4];
};

struct lf_params { int level, sharpness, delta_enabled; int8_t ref_deltas[4], mode_deltas[2]; };

struct header {
    int profile, show_existing_frame, existing_idx;
    int frame_type, show_frame, error_res, intra_only, reset_frame_context;
    int width, height, refresh_frame_flags, ref_idx[3], sign_bias[4];
    int allow_hp, interp_filter, refresh_frame_context, frame_parallel, frame_context_idx;
    int base_q, delta_y_dc, delta_uv_dc, delta_uv_ac, lossless;
    int log2_tile_cols, log2_tile_rows;
    size_t uncompressed_size, compressed_size;
    /* compressed header */
    int tx_mode, reference_mode, comp_fixed_ref, comp_var_ref[2];
};

struct surf {
    struct t_bo bo;
    unsigned pitch, alloc_h, chroma; /* allocation */
    int width, height;               /* the frame it holds */
};

struct vp9dec {
    struct t_dev dev;
    nvdec_picture_cb cb;
    void *opaque;
    struct surf surf[NSURF];
    struct t_bo bits, setup, status, probs, counts, tiles, segmap[2], mv[2], filter;
    int aux_w, aux_h;
    unsigned seg_size, mv_size, bsd_offset, above_offset;
    int ref_map[8];           /* surface per reference slot, -1 if empty */
    int seg_active, mv_cur;
    struct vp9_frame_ctx fctx[4], fc;
    nvdec_vp9EntropyCounts_t counts_copy;
    struct seg_params seg;
    struct lf_params lf;
    /* the previous decoded frame, as libvpx tracks it */
    int last_w, last_h, last_show_frame, last_frame_type, last_intra_only, intra_only_state, have_last;
    int need_key;
    unsigned pic_counter, frame_counter;
    uint8_t *staging;
    size_t staging_size;
    struct nvdec_vic vic;
};

static int sub(struct br *b, int n) { int v = un(b, n); return u1(b) ? -v : v; }

static void setup_past_independence(struct vp9dec *d, struct header *h) {
    memset(d->seg.feature_enabled, 0, sizeof(d->seg.feature_enabled));
    memset(d->seg.feature_data, 0, sizeof(d->seg.feature_data));
    d->seg.abs_delta = 0;
    for (int i = 0; i < 2; i++)
        if (d->segmap[i].ptr) memset(d->segmap[i].ptr, 0, d->segmap[i].size);
    d->lf.delta_enabled = 1;
    static const int8_t ref_deltas[4] = { 1, 0, -1, -1 };
    memcpy(d->lf.ref_deltas, ref_deltas, 4);
    d->lf.mode_deltas[0] = d->lf.mode_deltas[1] = 0;
    default_ctx(&d->fc);
    if (h->frame_type == KEY_FRAME || h->error_res || h->reset_frame_context == 3)
        for (int i = 0; i < 4; i++) d->fctx[i] = d->fc;
    else if (h->reset_frame_context == 2)
        d->fctx[h->frame_context_idx] = d->fc;
    memset(h->sign_bias, 0, sizeof(h->sign_bias));
    h->frame_context_idx = 0;
}

static int read_frame_size(struct br *b, struct header *h) {
    h->width = un(b, 16) + 1;
    h->height = un(b, 16) + 1;
    return 0;
}

static void skip_render_size(struct br *b) { if (u1(b)) un(b, 32); }

static int read_sync_code(struct br *b) { return un(b, 24) == 0x498342 ? 0 : -EINVAL; }

static int read_color_config(struct br *b, struct header *h) {
    if (h->profile >= 2) return -ENOTSUP; /* 10/12-bit */
    int cs = un(b, 3);
    if (cs == 7) return -ENOTSUP; /* sRGB is 4:4:4 */
    u1(b); /* color_range */
    if (h->profile == 1) return -ENOTSUP;
    return 0;
}

/* 6.2 uncompressed_header() */
static int parse_uncompressed(struct vp9dec *d, const uint8_t *p, size_t n, struct header *h) {
    struct br b = { p, n, 0, 0 };
    memset(h, 0, sizeof(*h));
    if (un(&b, 2) != 2) return -EINVAL;
    h->profile = u1(&b);
    h->profile |= u1(&b) << 1;
    if (h->profile == 3 && u1(&b)) return -EINVAL;
    if (h->profile) return -ENOTSUP;
    h->show_existing_frame = u1(&b);
    if (h->show_existing_frame) {
        h->existing_idx = un(&b, 3);
        return b.err ? -EINVAL : 0;
    }
    h->frame_type = u1(&b);
    h->show_frame = u1(&b);
    h->error_res = u1(&b);
    int err = 0;
    if (h->frame_type == KEY_FRAME) {
        if ((err = read_sync_code(&b)) || (err = read_color_config(&b, h))) return err;
        h->refresh_frame_flags = 0xff;
        read_frame_size(&b, h);
        skip_render_size(&b);
    } else {
        h->intra_only = h->show_frame ? 0 : u1(&b);
        h->reset_frame_context = h->error_res ? 0 : un(&b, 2);
        if (h->intra_only) {
            if ((err = read_sync_code(&b))) return err;
            h->refresh_frame_flags = un(&b, 8);
            read_frame_size(&b, h);
            skip_render_size(&b);
        } else {
            h->refresh_frame_flags = un(&b, 8);
            for (int i = 0; i < 3; i++) {
                h->ref_idx[i] = un(&b, 3);
                h->sign_bias[LAST_FRAME + i] = u1(&b);
            }
            int found = 0;
            for (int i = 0; i < 3 && !found; i++) {
                if (!u1(&b)) continue;
                int s = d->ref_map[h->ref_idx[i]];
                if (s < 0) return -EINVAL;
                h->width = d->surf[s].width;
                h->height = d->surf[s].height;
                found = 1;
            }
            if (!found) read_frame_size(&b, h);
            skip_render_size(&b);
            h->allow_hp = u1(&b);
            static const int literal_to_filter[4] = { EIGHTTAP_SMOOTH, EIGHTTAP, EIGHTTAP_SHARP, BILINEAR };
            h->interp_filter = u1(&b) ? SWITCHABLE : literal_to_filter[un(&b, 2)];
        }
    }
    if (!h->error_res) {
        h->refresh_frame_context = u1(&b);
        h->frame_parallel = u1(&b);
    } else {
        h->frame_parallel = 1;
    }
    h->frame_context_idx = un(&b, 2);
    if (h->frame_type == KEY_FRAME || h->intra_only || h->error_res) setup_past_independence(d, h);

    /* loop_filter_params() */
    struct lf_params *lf = &d->lf;
    lf->level = un(&b, 6);
    lf->sharpness = un(&b, 3);
    lf->delta_enabled = u1(&b);
    if (lf->delta_enabled && u1(&b)) {
        for (int i = 0; i < 4; i++) if (u1(&b)) lf->ref_deltas[i] = sub(&b, 6);
        for (int i = 0; i < 2; i++) if (u1(&b)) lf->mode_deltas[i] = sub(&b, 6);
    }
    /* quantization_params() */
    h->base_q = un(&b, 8);
    h->delta_y_dc = u1(&b) ? sub(&b, 4) : 0;
    h->delta_uv_dc = u1(&b) ? sub(&b, 4) : 0;
    h->delta_uv_ac = u1(&b) ? sub(&b, 4) : 0;
    h->lossless = !h->base_q && !h->delta_y_dc && !h->delta_uv_dc && !h->delta_uv_ac;
    /* segmentation_params() */
    struct seg_params *s = &d->seg;
    s->update_map = s->update_data = 0;
    s->enabled = u1(&b);
    if (s->enabled) {
        s->update_map = u1(&b);
        if (s->update_map) {
            for (int i = 0; i < 7; i++) s->tree_probs[i] = u1(&b) ? un(&b, 8) : 255;
            s->temporal_update = u1(&b);
            for (int i = 0; i < 3; i++) s->pred_probs[i] = s->temporal_update && u1(&b) ? un(&b, 8) : 255;
        }
        s->update_data = u1(&b);
        if (s->update_data) {
            static const int bits[4] = { 8, 6, 2, 0 }, max[4] = { 255, 63, 3, 0 };
            s->abs_delta = u1(&b);
            memset(s->feature_enabled, 0, sizeof(s->feature_enabled));
            memset(s->feature_data, 0, sizeof(s->feature_data));
            for (int i = 0; i < 8; i++)
                for (int j = 0; j < 4; j++) {
                    if (!(s->feature_enabled[i][j] = u1(&b))) continue;
                    int v = un(&b, bits[j]);
                    if (v > max[j]) v = max[j];
                    if (j < 2 && u1(&b)) v = -v;
                    s->feature_data[i][j] = v;
                }
        }
    }
    /* tile_info() */
    int sb64_cols = ((h->width + 7) / 8 + 7) / 8, min_log2 = 0, max_log2 = 1;
    while ((64 << min_log2) < sb64_cols) min_log2++;
    while ((sb64_cols >> max_log2) >= 4) max_log2++;
    max_log2--;
    h->log2_tile_cols = min_log2;
    for (int ones = max_log2 - min_log2; ones-- > 0 && u1(&b);) h->log2_tile_cols++;
    h->log2_tile_rows = u1(&b);
    if (h->log2_tile_rows) h->log2_tile_rows += u1(&b);
    h->compressed_size = un(&b, 16);
    h->uncompressed_size = (b.pos + 7) / 8;
    if (b.err || !h->compressed_size || h->log2_tile_cols > 6) return -EINVAL;
    return 0;
}

/* 6.3 compressed_header(): forward updates of d->fc */
static int parse_compressed(struct vp9dec *d, const uint8_t *p, size_t n, struct header *h) {
    struct vp9_frame_ctx *fc = &d->fc;
    struct bd b;
    if (bd_init(&b, p, n)) return -EINVAL;
    if (h->lossless) {
        h->tx_mode = ONLY_4X4;
    } else {
        h->tx_mode = bd_literal(&b, 2);
        if (h->tx_mode == ALLOW_32X32) h->tx_mode += bd_read(&b, 128);
    }
    if (h->tx_mode == TX_MODE_SELECT) {
        for (int i = 0; i < 2; i++) diff_update(&b, &fc->tx.p8x8[i][0]);
        for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) diff_update(&b, &fc->tx.p16x16[i][j]);
        for (int i = 0; i < 2; i++) for (int j = 0; j < 3; j++) diff_update(&b, &fc->tx.p32x32[i][j]);
    }
    int max_tx = h->tx_mode == TX_MODE_SELECT ? 3 : h->tx_mode;
    for (int t = 0; t <= max_tx; t++) {
        if (!bd_read(&b, 128)) continue;
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 6; k++)
                    for (int l = 0; l < (k ? 6 : 3); l++)
                        for (int m = 0; m < 3; m++) diff_update(&b, &fc->coef[t][i][j][k][l][m]);
    }
    for (int i = 0; i < 3; i++) diff_update(&b, &fc->skip[i]);
    if (h->frame_type != KEY_FRAME && !h->intra_only) {
        for (int i = 0; i < 7; i++) for (int j = 0; j < 3; j++) diff_update(&b, &fc->inter_mode[i][j]);
        if (h->interp_filter == SWITCHABLE)
            for (int i = 0; i < 4; i++) for (int j = 0; j < 2; j++) diff_update(&b, &fc->interp[i][j]);
        for (int i = 0; i < 4; i++) diff_update(&b, &fc->intra_inter[i]);
        int sb = h->sign_bias[LAST_FRAME];
        if (h->sign_bias[GOLDEN_FRAME] != sb || h->sign_bias[ALTREF_FRAME] != sb) {
            h->reference_mode = !bd_read(&b, 128) ? SINGLE_REFERENCE
                                : bd_read(&b, 128) ? REFERENCE_MODE_SELECT : COMPOUND_REFERENCE;
        }
        if (h->reference_mode != SINGLE_REFERENCE) { /* vp9_setup_compound_reference_mode() */
            if (sb == h->sign_bias[GOLDEN_FRAME]) {
                h->comp_fixed_ref = ALTREF_FRAME; h->comp_var_ref[0] = LAST_FRAME; h->comp_var_ref[1] = GOLDEN_FRAME;
            } else if (sb == h->sign_bias[ALTREF_FRAME]) {
                h->comp_fixed_ref = GOLDEN_FRAME; h->comp_var_ref[0] = LAST_FRAME; h->comp_var_ref[1] = ALTREF_FRAME;
            } else {
                h->comp_fixed_ref = LAST_FRAME; h->comp_var_ref[0] = GOLDEN_FRAME; h->comp_var_ref[1] = ALTREF_FRAME;
            }
        }
        if (h->reference_mode == REFERENCE_MODE_SELECT)
            for (int i = 0; i < 5; i++) diff_update(&b, &fc->comp_inter[i]);
        if (h->reference_mode != COMPOUND_REFERENCE)
            for (int i = 0; i < 5; i++) { diff_update(&b, &fc->single_ref[i][0]); diff_update(&b, &fc->single_ref[i][1]); }
        if (h->reference_mode != SINGLE_REFERENCE)
            for (int i = 0; i < 5; i++) diff_update(&b, &fc->comp_ref[i]);
        for (int i = 0; i < 4; i++) for (int j = 0; j < 9; j++) diff_update(&b, &fc->y_mode[i][j]);
        for (int i = 0; i < 16; i++) for (int j = 0; j < 3; j++) diff_update(&b, &fc->partition[i][j]);
        struct vp9_mv_probs *mv = &fc->mv;
        mv_update(&b, mv->joints, 3);
        for (int i = 0; i < 2; i++) {
            mv_update(&b, &mv->comps[i].sign, 1);
            mv_update(&b, mv->comps[i].classes, 10);
            mv_update(&b, mv->comps[i].class0, 1);
            mv_update(&b, mv->comps[i].bits, 10);
        }
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) mv_update(&b, mv->comps[i].class0_fp[j], 3);
            mv_update(&b, mv->comps[i].fp, 3);
        }
        if (h->allow_hp)
            for (int i = 0; i < 2; i++) {
                mv_update(&b, &mv->comps[i].class0_hp, 1);
                mv_update(&b, &mv->comps[i].hp, 1);
            }
    }
    return 0;
}

/* 8.4.2-8.4.4 backward adaptation from the engine's symbol counts (libvpx vp9_adapt_*_probs) */
static void adapt_probs(struct vp9dec *d, const struct header *h, const nvdec_vp9EntropyCounts_t *c) {
    struct vp9_frame_ctx *fc = &d->fc;
    const struct vp9_frame_ctx *pre = &d->fctx[h->frame_context_idx];
    int intra = h->frame_type == KEY_FRAME || h->intra_only;
    unsigned uf = !intra && d->last_frame_type == KEY_FRAME ? 128 : 112;
    const unsigned (*coef_counts[4])[2][6][6][4] = { c->countCoeffs, c->countCoeffs8x8, c->countCoeffs16x16,
                                                      c->countCoeffs32x32 };
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 6; k++)
                    for (int l = 0; l < (k ? 6 : 3); l++) {
                        const unsigned *n = coef_counts[t][i][j][k][l];
                        unsigned eob = c->countEobs[t][i][j][k][l];
                        const unsigned ct[3][2] = { { n[3], eob - n[3] }, { n[0], n[1] + n[2] }, { n[1], n[2] } };
                        for (int m = 0; m < 3; m++)
                            fc->coef[t][i][j][k][l][m] = merge_probs(pre->coef[t][i][j][k][l][m], ct[m], 24, uf);
                    }
    if (intra) return;

    for (int i = 0; i < 4; i++) fc->intra_inter[i] = mode_mv_merge(pre->intra_inter[i], c->intra_inter_count[i]);
    for (int i = 0; i < 5; i++) fc->comp_inter[i] = mode_mv_merge(pre->comp_inter[i], c->comp_inter_count[i]);
    for (int i = 0; i < 5; i++) fc->comp_ref[i] = mode_mv_merge(pre->comp_ref[i], c->comp_ref_count[i]);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 2; j++) fc->single_ref[i][j] = mode_mv_merge(pre->single_ref[i][j], c->single_ref_count[i][j]);
    for (int i = 0; i < 7; i++) { /* the engine counts per tree node */
        const unsigned m[4] = { c->inter_mode_counts[i][1][0], c->inter_mode_counts[i][2][0],
                                c->inter_mode_counts[i][0][0], c->inter_mode_counts[i][2][1] };
        tree_merge(inter_mode_tree, 0, pre->inter_mode[i], m, fc->inter_mode[i]);
    }
    for (int i = 0; i < 4; i++) tree_merge(intra_mode_tree, 0, pre->y_mode[i], c->sb_ymode_counts[i], fc->y_mode[i]);
    for (int i = 0; i < 10; i++) tree_merge(intra_mode_tree, 0, pre->uv_mode[i], c->uv_mode_counts[i], fc->uv_mode[i]);
    for (int i = 0; i < 16; i++) tree_merge(partition_tree, 0, pre->partition[i], c->partition_counts[i], fc->partition[i]);
    if (h->interp_filter == SWITCHABLE)
        for (int i = 0; i < 4; i++) tree_merge(interp_tree, 0, pre->interp[i], c->switchable_interp_counts[i], fc->interp[i]);
    if (h->tx_mode == TX_MODE_SELECT)
        for (int i = 0; i < 2; i++) {
            const unsigned *t8 = c->tx8x8_count[i], *t16 = c->tx16x16_count[i], *t32 = c->tx32x32_count[i];
            const unsigned b8[1][2] = { { t8[0], t8[1] } };
            const unsigned b16[2][2] = { { t16[0], t16[1] + t16[2] }, { t16[1], t16[2] } };
            const unsigned b32[3][2] = { { t32[0], t32[1] + t32[2] + t32[3] }, { t32[1], t32[2] + t32[3] }, { t32[2], t32[3] } };
            fc->tx.p8x8[i][0] = mode_mv_merge(pre->tx.p8x8[i][0], b8[0]);
            for (int j = 0; j < 2; j++) fc->tx.p16x16[i][j] = mode_mv_merge(pre->tx.p16x16[i][j], b16[j]);
            for (int j = 0; j < 3; j++) fc->tx.p32x32[i][j] = mode_mv_merge(pre->tx.p32x32[i][j], b32[j]);
        }
    for (int i = 0; i < 3; i++) fc->skip[i] = mode_mv_merge(pre->skip[i], c->mbskip_count[i]);

    const nvdec_nmv_context_counts *mc = &c->nmvcount;
    tree_merge(mv_joint_tree, 0, pre->mv.joints, mc->joints, fc->mv.joints);
    for (int i = 0; i < 2; i++) {
        struct vp9_mv_comp_probs *cp = &fc->mv.comps[i];
        const struct vp9_mv_comp_probs *pp = &pre->mv.comps[i];
        cp->sign = mode_mv_merge(pp->sign, mc->sign[i]);
        tree_merge(mv_class_tree, 0, pp->classes, mc->classes[i], cp->classes);
        tree_merge(mv_class0_tree, 0, pp->class0, mc->class0[i], cp->class0);
        for (int j = 0; j < 10; j++) cp->bits[j] = mode_mv_merge(pp->bits[j], mc->bits[i][j]);
        for (int j = 0; j < 2; j++) tree_merge(mv_fp_tree, 0, pp->class0_fp[j], mc->class0_fp[i][j], cp->class0_fp[j]);
        tree_merge(mv_fp_tree, 0, pp->fp, mc->fp[i], cp->fp);
        if (h->allow_hp) {
            cp->class0_hp = mode_mv_merge(pp->class0_hp, mc->class0_hp[i]);
            cp->hp = mode_mv_merge(pp->hp, mc->hp[i]);
        }
    }
}

/* the engine's probability table for this frame (hantro_g2_vp9_dec.c config_probs()) */
static void write_probs(const struct vp9dec *d, nvdec_vp9EntropyProbs_t *t) {
    const struct vp9_frame_ctx *fc = &d->fc;
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 10; j++) {
            memcpy(t->kf_bmode_prob[i][j], vp9_kf_y_mode_prob[i][j], 8);
            t->kf_bmode_probB[i][j][0] = vp9_kf_y_mode_prob[i][j][8];
        }
        memcpy(t->kf_uv_mode_prob[i], vp9_kf_uv_mode_prob[i], 8);
        t->kf_uv_mode_probB[i][0] = vp9_kf_uv_mode_prob[i][8];
    }
    memcpy(t->mb_segment_tree_probs, d->seg.tree_probs, 7);
    memcpy(t->segment_pred_probs, d->seg.pred_probs, 3);

    nvdec_vp9AdaptiveEntropyProbs_t *a = &t->a;
    for (int i = 0; i < 7; i++) memcpy(a->inter_mode_prob[i], fc->inter_mode[i], 3);
    memcpy(a->intra_inter_prob, fc->intra_inter, 4);
    for (int i = 0; i < 10; i++) {
        memcpy(a->uv_mode_prob[i], fc->uv_mode[i], 8);
        a->uv_mode_probB[i][0] = fc->uv_mode[i][8];
    }
    for (int i = 0; i < 2; i++) {
        a->tx8x8_prob[i][0] = fc->tx.p8x8[i][0];
        memcpy(a->tx16x16_prob[i], fc->tx.p16x16[i], 2);
        memcpy(a->tx32x32_prob[i], fc->tx.p32x32[i], 3);
    }
    for (int i = 0; i < 4; i++) {
        memcpy(a->sb_ymode_prob[i], fc->y_mode[i], 8);
        a->sb_ymode_probB[i][0] = fc->y_mode[i][8];
    }
    for (int i = 0; i < 16; i++) {
        memcpy(a->partition_prob[0][i], vp9_kf_partition_prob[i], 3);
        memcpy(a->partition_prob[1][i], fc->partition[i], 3);
    }
    memcpy(a->switchable_interp_prob, fc->interp, sizeof(a->switchable_interp_prob));
    memcpy(a->comp_inter_prob, fc->comp_inter, 5);
    memcpy(a->mbskip_probs, fc->skip, 3);
    nvdec_nmv_context *mv = &a->nmvc;
    memcpy(mv->joints, fc->mv.joints, 3);
    for (int i = 0; i < 2; i++) {
        const struct vp9_mv_comp_probs *c = &fc->mv.comps[i];
        mv->sign[i] = c->sign;
        mv->class0[i][0] = c->class0[0];
        memcpy(mv->fp[i], c->fp, 3);
        mv->class0_hp[i] = c->class0_hp;
        mv->hp[i] = c->hp;
        memcpy(mv->classes[i], c->classes, 10);
        memcpy(mv->class0_fp[i], c->class0_fp, 6);
        memcpy(mv->bits[i], c->bits, 10);
    }
    memcpy(a->single_ref_prob, fc->single_ref, sizeof(a->single_ref_prob));
    memcpy(a->comp_ref_prob, fc->comp_ref, 5);
    unsigned char (*coef[4])[2][6][6][4] = { a->probCoeffs, a->probCoeffs8x8, a->probCoeffs16x16, a->probCoeffs32x32 };
    for (int t2 = 0; t2 < 4; t2++)
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 6; k++)
                    for (int l = 0; l < 6; l++) memcpy(coef[t2][i][j][k][l], fc->coef[t2][i][j][k][l], 3);
}

/* tile column/row sizes in superblocks, per tile (hantro_g2_vp9_dec.c fill_tile_info()) */
static void write_tile_sizes(const struct header *h, uint16_t *p) {
    int sbs_c = (h->width + 63) / 64, sbs_r = (h->height + 63) / 64;
    int cols = 1 << h->log2_tile_cols, rows = 1 << h->log2_tile_rows;
    uint16_t col_w[64], row_h[4];
    for (int i = 0; i < cols; i++) col_w[i] = (i + 1) * sbs_c / cols - i * sbs_c / cols;
    for (int i = 0; i < rows; i++) row_h[i] = (i + 1) * sbs_r / rows - i * sbs_r / rows;
    memset(p, 0, 4096);
    /* leading empty tile rows (more tile rows than superblock rows) fold into the first */
    int first = rows == sbs_r + 1 ? 1 : rows == sbs_r + 2 ? 2 : 0;
    for (int r = first; r < rows; r++) {
        int height = row_h[r];
        if (r == first) for (int k = 0; k < first; k++) height += row_h[k];
        for (int c = 0; c < cols; c++) {
            *p++ = col_w[c];
            *p++ = height;
        }
    }
}

static void free_aux(struct vp9dec *d) {
    struct t_bo *all[] = { &d->segmap[0], &d->segmap[1], &d->mv[0], &d->mv[1], &d->filter };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) t_bo_free(&d->dev, all[i]);
    d->aux_w = d->aux_h = 0;
}

/* segment maps, collocated MVs and the filter buffer, for frames up to w x h */
static int alloc_aux(struct vp9dec *d, int w, int h) {
    if (w <= d->aux_w && h <= d->aux_h) return 0;
    w = w > d->aux_w ? w : d->aux_w;
    h = h > d->aux_h ? h : d->aux_h;
    free_aux(d);
    unsigned w64 = ALIGN(w, 64), h64 = ALIGN(h, 64), sbs = (w64 / 64) * (h64 / 64);
    d->seg_size = ALIGN(sbs * GIP_ASIC_VP9_CTB_SEG_SIZE, 256);
    d->mv_size = ALIGN(sbs * 64 * 32, 256); /* the G2 stores 16 bytes per 8x8 block; twice that */
    /* tile-column deblocking context (left), BSD control and the above-row filter lines */
    unsigned left = ALIGN(24 * h64 * 63, 256), bsd = ALIGN(16 * (h64 / 4) * 63, 256);
    unsigned above = ALIGN(VP9_DBLK_TOP_BUF_SIZE(w64) * 4, 256);
    if (left < VP9_DBLK_LEFT_BUF_SIZE(h64) * 4) left = ALIGN(VP9_DBLK_LEFT_BUF_SIZE(h64) * 4, 256);
    d->bsd_offset = left;
    d->above_offset = left + bsd;
    if (t_bo_new(&d->dev, &d->segmap[0], d->seg_size) || t_bo_new(&d->dev, &d->segmap[1], d->seg_size) ||
        t_bo_new(&d->dev, &d->mv[0], d->mv_size) || t_bo_new(&d->dev, &d->mv[1], d->mv_size) ||
        t_bo_new(&d->dev, &d->filter, d->above_offset + above + (1u << 20)))
        return -ENOMEM;
    memset(d->segmap[0].ptr, 0, d->seg_size);
    memset(d->segmap[1].ptr, 0, d->seg_size);
    d->aux_w = w;
    d->aux_h = h;
    return 0;
}

static int surf_busy(const struct vp9dec *d, int s) {
    for (int i = 0; i < 8; i++) if (d->ref_map[i] == s) return 1;
    return 0;
}

/* a free surface that can hold a w x h frame */
static int get_surface(struct vp9dec *d, int w, int h) {
    int s = -1;
    for (int i = 0; i < NSURF && s < 0; i++) if (!surf_busy(d, i)) s = i;
    if (s < 0) return -ENOSPC;
    struct surf *sf = &d->surf[s];
    unsigned pitch = ALIGN(w, 64), h64 = ALIGN(h, 64);
    if (sf->bo.handle && sf->pitch >= pitch && sf->alloc_h >= h64) return s;
    nvdec_vic_unmap_surface(&d->vic, s);
    t_bo_free(&d->dev, &sf->bo);
    sf->pitch = pitch;
    sf->alloc_h = h64;
    sf->chroma = ALIGN(pitch * h64, 4096);
    if (t_bo_new(&d->dev, &sf->bo, sf->chroma + ALIGN(pitch * h64 / 2, 4096))) return -ENOMEM;
    if (nvdec_vic_map_surface(&d->vic, s, &sf->bo, sf->chroma)) return -ENOMEM;
    return s;
}

static double dec_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int output(struct vp9dec *d, int s, uint64_t ts) {
    struct surf *sf = &d->surf[s];
    const uint8_t *pix = sf->bo.ptr;
    if (!d->vic.ok) { /* see h264dec.c: surfaces are write-combined */
        if (d->staging_size < sf->bo.size) {
            free(d->staging);
            d->staging = malloc(sf->bo.size);
            d->staging_size = d->staging ? sf->bo.size : 0;
            if (!d->staging) return -ENOMEM;
        }
        memcpy(d->staging, sf->bo.ptr, sf->bo.size);
        pix = d->staging;
    }
    const nvdec_status_s *st = d->status.ptr;
    struct nvdec_picture pic = {
        .luma = pix, .chroma = pix + sf->chroma, .pitch = sf->pitch,
        .coded_width = sf->width, .coded_height = sf->height,
        .width = sf->width, .height = sf->height,
        .poc = d->frame_counter++, .timestamp = ts,
        .mbs_in_error = st->mbs_in_error, .surface = s,
    };
    return d->cb(d->opaque, &pic) ? -ECANCELED : 0;
}

static void set_surface(struct t_job *j, int slot, struct surf *sf) {
    t_mthd_addr(j, NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * slot, &sf->bo, 0);
    t_mthd_addr(j, NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * slot, &sf->bo, sf->chroma);
}

/* Decode one frame; *shown is set to the surface to display, if any. */
static int decode_frame(struct vp9dec *d, const uint8_t *data, size_t n, int *shown) {
    double t_start = dec_now();
    struct header h;
    int err = parse_uncompressed(d, data, n, &h);
    if (err) return err;
    if (h.show_existing_frame) {
        int s = d->ref_map[h.existing_idx];
        if (d->need_key) return 0;
        if (s < 0) return -EINVAL;
        *shown = s;
        return 0;
    }
    /* the engine's limits (NVIDIA documents 128x128 minimum for VP9; smaller frames misdecode) */
    if (h.width > 8192 || h.height > 8192 || h.width < 128 || h.height < 128) return -ENOTSUP;
    int intra = h.frame_type == KEY_FRAME || h.intra_only;
    if (d->need_key && !intra) return 0; /* libvpx resyncs on intra-only frames too */
    d->need_key = 0;
    if (h.uncompressed_size + h.compressed_size > n) return -EINVAL;
    /* like libvpx: every reference must exist and one must be scalable to this frame
     * (2x down to 16x up); the others may be unusable, if the frame doesn't use them */
    int scalable = intra;
    for (int i = 0; i < 3 && !intra; i++) {
        int s = d->ref_map[h.ref_idx[i]];
        if (s < 0) return -EINVAL;
        const struct surf *r = &d->surf[s];
        scalable |= 2 * h.width >= r->width && 2 * h.height >= r->height && h.width <= 16 * r->width &&
                    h.height <= 16 * r->height;
    }
    if (!scalable) return -EINVAL;

    d->fc = d->fctx[h.frame_context_idx];
    if ((err = parse_compressed(d, data + h.uncompressed_size, h.compressed_size, &h))) return err;
    if (getenv("VP9DEC_DEBUG"))
        fprintf(stderr, "frame %u: %dx%d %s%s%s show %d refresh %02x refs %d/%d/%d bias %d%d%d ctx %d refresh_ctx %d par %d "
                "interp %d tx %d refmode %d q %d lf %d/%d seg %d/%d/%d/%d tiles %d/%d\n",
                d->pic_counter, h.width, h.height, h.frame_type == KEY_FRAME ? "key" : "inter",
                h.intra_only ? " intra-only" : "", h.error_res ? " err-res" : "", h.show_frame, h.refresh_frame_flags,
                h.ref_idx[0], h.ref_idx[1], h.ref_idx[2], h.sign_bias[1], h.sign_bias[2], h.sign_bias[3],
                h.frame_context_idx, h.refresh_frame_context, h.frame_parallel, h.interp_filter, h.tx_mode,
                h.reference_mode, h.base_q, d->lf.level, d->lf.sharpness, d->seg.enabled, d->seg.update_map,
                d->seg.temporal_update, d->seg.update_data, h.log2_tile_cols, h.log2_tile_rows);

    int resized = d->have_last && (h.width != d->last_w || h.height != d->last_h);
    if ((err = alloc_aux(d, h.width, h.height))) return err;
    if (resized) {
        memset(d->segmap[0].ptr, 0, d->seg_size);
        memset(d->segmap[1].ptr, 0, d->seg_size);
    }
    int cur = get_surface(d, h.width, h.height);
    if (cur < 0) return cur;
    struct surf *cs = &d->surf[cur];

    size_t tile_off = h.uncompressed_size + h.compressed_size, tile_len = n - tile_off;
    if (tile_len + 64 > BITSTREAM_SIZE) return -E2BIG;
    memcpy(d->bits.ptr, data + tile_off, tile_len);
    memset((uint8_t *)d->bits.ptr + tile_len, 0, 64);

    nvdec_vp9_pic_s *p = d->setup.ptr;
    memset(p, 0, sizeof(*p));
    p->stream_len = tile_len;
    p->gptimer_timeout_value = 81000000;
    p->tileformat = 0; /* TBL, as for H.264 */
    p->gob_height = 0;
    p->Vp9BsdCtrlOffset = d->bsd_offset >> 8;
    p->v1.Vp9FltAboveOffset = d->above_offset >> 8;
    p->v1.bit_depth = 8;
    struct surf *refs[3] = { cs, cs, cs };
    for (int i = 0; i < 3 && !intra; i++) refs[i] = &d->surf[d->ref_map[h.ref_idx[i]]];
    p->ref0_width = refs[0]->width; p->ref0_height = refs[0]->height;
    p->ref0_stride[0] = p->ref0_stride[1] = refs[0]->pitch;
    p->ref1_width = refs[1]->width; p->ref1_height = refs[1]->height;
    p->ref1_stride[0] = p->ref1_stride[1] = refs[1]->pitch;
    p->ref2_width = refs[2]->width; p->ref2_height = refs[2]->height;
    p->ref2_stride[0] = p->ref2_stride[1] = refs[2]->pitch;
    p->width = h.width;
    p->height = h.height;
    p->framestride[0] = p->framestride[1] = cs->pitch;
    p->keyFrame = h.frame_type == KEY_FRAME;
    p->prevIsKeyFrame = !d->have_last || d->last_frame_type == KEY_FRAME || d->last_intra_only;
    p->resolutionChange = !d->have_last || resized;
    p->errorResilient = h.error_res;
    p->prevShowFrame = d->last_show_frame;
    p->intraOnly = h.intra_only;
    for (int i = 0; i < 4; i++) p->refFrameSignBias[i] = h.sign_bias[i];
    p->loopFilterLevel = d->lf.level;
    p->loopFilterSharpness = d->lf.sharpness;
    p->qpYAc = h.base_q;
    p->qpYDc = h.delta_y_dc;
    p->qpChAc = h.delta_uv_ac;
    p->qpChDc = h.delta_uv_dc;
    p->lossless = h.lossless;
    p->transform_mode = h.tx_mode;
    p->allow_high_precision_mv = intra ? 0 : h.allow_hp;
    p->mcomp_filter_type = intra ? 0 : spec_filter(h.interp_filter);
    p->comp_pred_mode = h.reference_mode;
    p->comp_fixed_ref = h.comp_fixed_ref;
    p->comp_var_ref[0] = h.comp_var_ref[0];
    p->comp_var_ref[1] = h.comp_var_ref[1];
    p->log2_tile_columns = h.log2_tile_cols;
    p->log2_tile_rows = h.log2_tile_rows;
    p->segmentEnabled = d->seg.enabled;
    p->segmentMapUpdate = d->seg.update_map;
    p->segmentMapTemporalUpdate = d->seg.update_map && d->seg.temporal_update;
    p->segmentFeatureMode = d->seg.abs_delta;
    memcpy(p->segmentFeatureEnable, d->seg.feature_enabled, sizeof(p->segmentFeatureEnable));
    memcpy(p->segmentFeatureData, d->seg.feature_data, sizeof(p->segmentFeatureData));
    p->modeRefLfEnabled = d->lf.delta_enabled;
    memcpy(p->mbRefLfDelta, d->lf.ref_deltas, 4);
    memcpy(p->mbModeLfDelta, d->lf.mode_deltas, 2);

    write_probs(d, d->probs.ptr);
    write_tile_sizes(&h, d->tiles.ptr);
    memset(d->counts.ptr, 0, sizeof(nvdec_vp9EntropyCounts_t));
    memset(d->status.ptr, 0, sizeof(nvdec_status_s));

    struct t_job j;
    t_job_init(&j);
    t_mthd(&j, NVC5B0_SET_APPLICATION_ID, APP_ID_VP9);
    t_mthd(&j, NVC5B0_SET_CONTROL_PARAMS,
           CODEC_VP9 | 1 << 4 /* gptimer */ | 1 << 6 /* err conceal */ | (3 & 0x3f) << 7 | 1 << 13 /* mbtimer */);
    t_mthd_addr(&j, NVC5B0_SET_DRV_PIC_SETUP_OFFSET, &d->setup, 0);
    t_mthd_addr(&j, NVC5B0_SET_IN_BUF_BASE_OFFSET, &d->bits, 0);
    t_mthd(&j, NVC5B0_SET_PICTURE_INDEX, d->pic_counter);
    t_mthd_addr(&j, NVC5B0_SET_NVDEC_STATUS_OFFSET, &d->status, 0);
    t_mthd_addr(&j, NVC5B0_VP9_SET_PROB_TAB_BUF_OFFSET, &d->probs, 0);
    t_mthd_addr(&j, NVC5B0_VP9_SET_CTX_COUNTER_BUF_OFFSET, &d->counts, 0);
    t_mthd_addr(&j, NVC5B0_VP9_SET_SEGMENT_READ_BUF_OFFSET, &d->segmap[d->seg_active], 0);
    t_mthd_addr(&j, NVC5B0_VP9_SET_SEGMENT_WRITE_BUF_OFFSET, &d->segmap[!d->seg_active], 0);
    t_mthd_addr(&j, NVC5B0_VP9_SET_TILE_SIZE_BUF_OFFSET, &d->tiles, 0);
    t_mthd_addr(&j, NVC5B0_VP9_SET_COL_MVWRITE_BUF_OFFSET, &d->mv[d->mv_cur], 0);
    t_mthd_addr(&j, NVC5B0_VP9_SET_COL_MVREAD_BUF_OFFSET, &d->mv[!d->mv_cur], 0);
    t_mthd_addr(&j, NVC5B0_VP9_SET_FILTER_BUFFER_OFFSET, &d->filter, 0);
    for (int i = 0; i < 3; i++) set_surface(&j, i, refs[i]);
    set_surface(&j, 3, cs);
    t_mthd(&j, NVC5B0_EXECUTE, EXECUTE_AWAKEN);
    t_syncpt_incr(&j, &d->dev);
    double t_submit = dec_now();
    if (t_submit_wait(&d->dev, &j, 2000, NULL)) return -ETIMEDOUT;
    double t_done = dec_now();
    d->pic_counter++;

    /* 8.4.2: backward adaptation, then refresh the frame context */
    if (!h.error_res && !h.frame_parallel) {
        memcpy(&d->counts_copy, d->counts.ptr, sizeof(d->counts_copy)); /* read write-combined memory once */
        adapt_probs(d, &h, &d->counts_copy);
    }
    if (h.refresh_frame_context) d->fctx[h.frame_context_idx] = d->fc;

    cs->width = h.width;
    cs->height = h.height;
    for (int i = 0; i < 8; i++) if (h.refresh_frame_flags & (1 << i)) d->ref_map[i] = cur;
    if (d->seg.update_map) d->seg_active = !d->seg_active;
    d->mv_cur = !d->mv_cur;
    if (h.frame_type != KEY_FRAME) d->intra_only_state = h.intra_only;
    d->last_intra_only = d->intra_only_state;
    d->last_frame_type = h.frame_type;
    d->last_show_frame = h.show_frame;
    d->last_w = h.width;
    d->last_h = h.height;
    d->have_last = 1;
    if (getenv("H264DEC_TIMING"))
        fprintf(stderr, "setup %.2f ms, engine %.2f ms, adapt %.2f ms\n", t_submit - t_start, t_done - t_submit,
                dec_now() - t_done);
    const nvdec_status_s *st = d->status.ptr;
    if (st->error_status || st->mbs_in_error)
        DEC_WARN("frame %u: error status %#x, %u MBs in error", d->pic_counter - 1, st->error_status, st->mbs_in_error);
    if (h.show_frame) *shown = cur;
    return 0;
}

struct vp9dec *vp9dec_create(const char *node, uint32_t host1x_class, nvdec_picture_cb cb, void *opaque) {
    struct vp9dec *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->cb = cb;
    d->opaque = opaque;
    d->need_key = 1;
    memset(d->ref_map, -1, sizeof(d->ref_map));
    for (int i = 0; i < 4; i++) default_ctx(&d->fctx[i]);
    if (t_open(&d->dev, node, host1x_class) ||
        t_bo_new(&d->dev, &d->bits, BITSTREAM_SIZE) || t_bo_new(&d->dev, &d->setup, 4096) ||
        t_bo_new(&d->dev, &d->status, 4096) || t_bo_new(&d->dev, &d->probs, ALIGN(sizeof(nvdec_vp9EntropyProbs_t), 4096)) ||
        t_bo_new(&d->dev, &d->counts, ALIGN(sizeof(nvdec_vp9EntropyCounts_t), 4096)) || t_bo_new(&d->dev, &d->tiles, 4096)) {
        vp9dec_destroy(d);
        return NULL;
    }
    nvdec_vic_open(&d->dev, &d->vic);
    if (!d->vic.ok) DEC_WARN("VIC unavailable, using CPU copies");
    return d;
}

void vp9dec_destroy(struct vp9dec *d) {
    if (!d) return;
    if (d->dev.fd > 0) {
        struct t_bo *all[] = { &d->bits, &d->setup, &d->status, &d->probs, &d->counts, &d->tiles };
        free_aux(d);
        nvdec_vic_unmap(&d->vic);
        for (int i = 0; i < NSURF; i++) t_bo_free(&d->dev, &d->surf[i].bo);
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) t_bo_free(&d->dev, all[i]);
        nvdec_vic_close(&d->vic);
        t_close(&d->dev);
    }
    free(d->staging);
    free(d);
}

void vp9dec_reset(struct vp9dec *d) {
    memset(d->ref_map, -1, sizeof(d->ref_map));
    d->need_key = 1;
    d->have_last = 0;
}

int vp9dec_decode(struct vp9dec *d, const uint8_t *data, size_t n, uint64_t ts) {
    /* Annex B superframe index: the frames are the sizes it lists, ahead of it.
     * Like libvpx, only the last shown frame of a superframe is output (with
     * spatial layers, the top one). */
    int shown = -1, err = 0;
    uint8_t m = n ? data[n - 1] : 0;
    int frames = (m & 7) + 1, mag = ((m >> 3) & 3) + 1;
    size_t index = 2 + (size_t)mag * frames;
    if ((m & 0xe0) == 0xc0 && n >= index && data[n - index] == m) {
        const uint8_t *x = data + n - index + 1;
        size_t off = 0, end = n - index;
        for (int f = 0; f < frames && !err; f++) {
            size_t size = 0;
            for (int k = 0; k < mag; k++) size |= (size_t)*x++ << (8 * k);
            if (off + size > end) err = -EINVAL;
            else if (size) err = decode_frame(d, data + off, size, &shown);
            off += size;
        }
    } else {
        err = decode_frame(d, data, n, &shown);
    }
    if (err) {
        d->need_key = 1;
        return err;
    }
    return shown >= 0 ? output(d, shown, ts) : 0;
}

int vp9dec_vic_copy(struct vp9dec *d, const struct nvdec_picture *pic, int dmabuf_fd,
                    size_t dmabuf_size, unsigned pitch, unsigned chroma_offset) {
    return nvdec_vic_copy(&d->vic, pic, dmabuf_fd, dmabuf_size, pitch, chroma_offset);
}
