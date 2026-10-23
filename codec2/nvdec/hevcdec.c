/*
 * H.265 decoding on the Tegra NVDEC engine through the upstream tegra-drm
 * channel UAPI (see hevcdec.h). The picture setup is NVIDIA's nvdec_drv.h
 * nvdec_hevc_pic_s. The engine parses the slice headers itself, except for
 * the reference picture set syntax: the driver derives the RPS, describes the
 * reference lists, and tells the engine how many header bits to skip. The
 * auxiliary buffer formats (tile sizes, scaling lists) are those of the Hantro
 * G2 core the engine's HEVC block descends from ("GIP" in nvdec_drv.h).
 */
#include "hevcdec.h"
#include "host1x.h"
#include "nvdec_bits.h"
#include "nvdec_drv.h"

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
#define DEC_ERR(...) (fprintf(stderr, "hevcdec: " __VA_ARGS__), fputc('\n', stderr))
#define DEC_WARN(...) DEC_ERR(__VA_ARGS__)
#endif

#define NVC5B0_SET_APPLICATION_ID           0x200
#define NVC5B0_EXECUTE                      0x300
#define NVC5B0_SET_CONTROL_PARAMS           0x400
#define NVC5B0_SET_DRV_PIC_SETUP_OFFSET     0x404
#define NVC5B0_SET_IN_BUF_BASE_OFFSET       0x408
#define NVC5B0_SET_PICTURE_INDEX            0x40c
#define NVC5B0_SET_COLOC_DATA_OFFSET        0x414
#define NVC5B0_SET_NVDEC_STATUS_OFFSET      0x424
#define NVC5B0_SET_PICTURE_LUMA_OFFSET0     0x430
#define NVC5B0_SET_PICTURE_CHROMA_OFFSET0   0x474
#define NVC5B0_HEVC_SET_SCALING_LIST_OFFSET 0x580
#define NVC5B0_HEVC_SET_TILE_SIZES_OFFSET   0x584
#define NVC5B0_HEVC_SET_FILTER_BUFFER_OFFSET 0x588
#define NVC5B0_HEVC_SET_SAO_BUFFER_OFFSET   0x58c
#define APP_ID_HEVC 7
#define CODEC_HEVC 7
#define EXECUTE_AWAKEN (1u << 8)

#define NSURF NVDEC_NSURF
#define MAX_REFS 16
#define BITSTREAM_SIZE (8u << 20)
#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

enum {
    NAL_RADL_N = 6, NAL_RADL_R = 7, NAL_RASL_N = 8, NAL_RASL_R = 9,
    NAL_BLA_W_LP = 16, NAL_BLA_N_LP = 18, NAL_IDR_W_RADL = 19, NAL_IDR_N_LP = 20, NAL_CRA = 21,
    NAL_VPS = 32, NAL_SPS = 33, NAL_PPS = 34, NAL_EOS = 36, NAL_EOB = 37,
};

/* ---- scaling lists (7.3.4), kept in coefficient (up-right diagonal) order ---- */
struct scaling {
    uint8_t list[4][6][64]; /* [sizeId][matrixId][i]; 4x4 uses 16 entries */
    uint8_t dc[4][6];       /* sizeId 2 and 3 */
};

static const uint8_t default_intra[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18, 17, 18, 18, 17, 18, 21,
    19, 20, 21, 20, 19, 21, 24, 22, 22, 24, 24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29,
    31, 35, 35, 31, 29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115,
};
static const uint8_t default_inter[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 20,
    20, 20, 20, 20, 20, 20, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28,
    28, 28, 28, 28, 28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91,
};

static void default_list(struct scaling *s, int size, int m) {
    if (size == 0) memset(s->list[0][m], 16, 16);
    else memcpy(s->list[size][m], m < 3 ? default_intra : default_inter, 64);
    s->dc[size][m] = 16;
}

static void default_scaling(struct scaling *s) {
    for (int size = 0; size < 4; size++)
        for (int m = 0; m < 6; m++) default_list(s, size, m);
}

static int parse_scaling(struct br *b, struct scaling *s) {
    for (int size = 0; size < 4; size++) {
        int coefs = size ? 64 : 16;
        for (int m = 0; m < 6; m += size == 3 ? 3 : 1) {
            if (!u1(b)) { /* scaling_list_pred_mode_flag */
                unsigned delta = ue(b) * (size == 3 ? 3 : 1);
                if (delta > (unsigned)m) return -EINVAL;
                if (!delta) {
                    default_list(s, size, m);
                } else {
                    memcpy(s->list[size][m], s->list[size][m - delta], 64);
                    s->dc[size][m] = s->dc[size][m - delta];
                }
            } else {
                int next = 8;
                if (size > 1) next = s->dc[size][m] = se(b) + 8;
                for (int i = 0; i < coefs; i++) {
                    next = (next + se(b) + 256) % 256;
                    s->list[size][m][i] = next;
                }
            }
        }
    }
    /* 32x32 chroma lists (4:4:4 only) follow their 16x16 counterparts */
    for (int m = 1; m < 6; m++)
        if (m != 3) {
            memcpy(s->list[3][m], s->list[2][m], 64);
            s->dc[3][m] = s->dc[2][m];
        }
    return b->err ? -EINVAL : 0;
}

/* up-right diagonal scan (6.5.3): coefficient i -> raster position */
static void diag_scan(int n, uint8_t *pos) {
    int i = 0, x = 0, y = 0;
    while (i < n * n) {
        while (y >= 0) {
            if (x < n && y < n) pos[i++] = y * n + x;
            y--;
            x++;
        }
        y = x;
        x = 0;
    }
}

/* GIP layout: 8 DC values, pad to 16 bytes, then each list in raster order, column by column */
static void write_scaling(const struct scaling *s, uint8_t *out) {
    uint8_t scan4[16], scan8[64], raster[64];
    diag_scan(4, scan4);
    diag_scan(8, scan8);
    uint8_t *p = out;
    for (int m = 0; m < 6; m++) *p++ = s->dc[2][m];
    *p++ = s->dc[3][0];
    *p++ = s->dc[3][3];
    p += 8;
    for (int m = 0; m < 6; m++) {
        for (int i = 0; i < 16; i++) raster[scan4[i]] = s->list[0][m][i];
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++) *p++ = raster[row * 4 + col];
    }
    for (int size = 1; size < 4; size++)
        for (int m = 0; m < 6; m += size == 3 ? 3 : 1) {
            for (int i = 0; i < 64; i++) raster[scan8[i]] = s->list[size][m][i];
            for (int col = 0; col < 8; col++)
                for (int row = 0; row < 8; row++) *p++ = raster[row * 8 + col];
        }
}

/* ---- parameter sets ---- */
struct st_rps {
    int num_neg, num_pos;
    int delta[32]; /* negatives (S0, closest first), then positives (S1) */
    uint8_t used[32];
};

struct sps {
    int valid, chroma_format_idc, separate_colour_plane, width, height;
    int conf_left, conf_right, conf_top, conf_bottom; /* luma samples */
    int bit_depth_luma, bit_depth_chroma, log2_max_poc_lsb, max_num_reorder;
    int log2_min_cb, log2_diff_max_min_cb, log2_min_tb, log2_diff_max_min_tb;
    int max_th_depth_inter, max_th_depth_intra;
    int scaling_list_enabled, amp, sao, pcm, pcm_bd_luma, pcm_bd_chroma, log2_min_pcm, log2_diff_max_min_pcm,
        pcm_loop_filter_disabled;
    int num_st_rps;
    struct st_rps st[65];
    int long_term_present, num_lt_sps, lt_poc_lsb_sps[33], lt_used_sps[33];
    int temporal_mvp, strong_intra_smoothing;
    struct scaling sl;
};

struct pps {
    int valid, sps_id, dependent_slice_segments, output_flag_present, num_extra_slice_header_bits,
        sign_data_hiding, cabac_init_present, num_ref_idx_l0_default, num_ref_idx_l1_default,
        init_qp_minus26, constrained_intra_pred, transform_skip, cu_qp_delta_enabled,
        diff_cu_qp_delta_depth, cb_qp_offset, cr_qp_offset, slice_chroma_qp_offsets_present,
        weighted_pred, weighted_bipred, transquant_bypass, tiles_enabled, entropy_coding_sync,
        num_tile_cols, num_tile_rows, uniform_spacing, col_width[20], row_height[22],
        loop_filter_across_tiles, loop_filter_across_slices, deblocking_control_present,
        deblocking_override_enabled, deblocking_disabled, beta_offset_div2, tc_offset_div2,
        scaling_list_present, lists_modification_present, log2_parallel_merge_level,
        slice_header_extension_present;
    struct scaling sl;
};

static void skip_ptl(struct br *b, int max_sub_layers_minus1) {
    un(b, 32); un(b, 32); un(b, 32); /* general profile/tier/flags, general_level_idc */
    int prof[8] = { 0 }, lvl[8] = { 0 };
    for (int i = 0; i < max_sub_layers_minus1; i++) { prof[i] = u1(b); lvl[i] = u1(b); }
    if (max_sub_layers_minus1 > 0)
        for (int i = max_sub_layers_minus1; i < 8; i++) un(b, 2);
    for (int i = 0; i < max_sub_layers_minus1; i++) {
        if (prof[i]) { un(b, 32); un(b, 32); un(b, 24); }
        if (lvl[i]) un(b, 8);
    }
}

/* st_ref_pic_set(idx) (7.3.7, 7.4.8); in a slice header idx == sps->num_st_rps */
static int parse_st_rps(struct br *b, const struct sps *s, int idx, struct st_rps *out, int *ref_num_delta) {
    memset(out, 0, sizeof(*out));
    if (ref_num_delta) *ref_num_delta = 0;
    if (idx && u1(b)) { /* inter_ref_pic_set_prediction_flag */
        int delta_idx = idx == s->num_st_rps ? (int)ue(b) + 1 : 1;
        if (delta_idx > idx) return -EINVAL;
        const struct st_rps *r = &s->st[idx - delta_idx];
        int sign = u1(b);
        int delta_rps = (int)(ue(b) + 1) * (sign ? -1 : 1);
        int n = r->num_neg + r->num_pos;
        uint8_t used[33], use_delta[33];
        for (int j = 0; j <= n; j++) {
            used[j] = u1(b);
            use_delta[j] = used[j] ? 1 : u1(b);
        }
        if (ref_num_delta) *ref_num_delta = n;
        /* (7-61): S0 from r's S1 (far to near), the reference itself, r's S0 */
        int i = 0;
        for (int j = r->num_pos - 1; j >= 0; j--) {
            int d = r->delta[r->num_neg + j] + delta_rps;
            if (d < 0 && use_delta[r->num_neg + j]) { out->delta[i] = d; out->used[i++] = used[r->num_neg + j]; }
        }
        if (delta_rps < 0 && use_delta[n]) { out->delta[i] = delta_rps; out->used[i++] = used[n]; }
        for (int j = 0; j < r->num_neg; j++) {
            int d = r->delta[j] + delta_rps;
            if (d < 0 && use_delta[j]) { out->delta[i] = d; out->used[i++] = used[j]; }
        }
        out->num_neg = i;
        /* (7-62) */
        for (int j = r->num_neg - 1; j >= 0; j--) {
            int d = r->delta[j] + delta_rps;
            if (d > 0 && use_delta[j]) { out->delta[i] = d; out->used[i++] = used[j]; }
        }
        if (delta_rps > 0 && use_delta[n]) { out->delta[i] = delta_rps; out->used[i++] = used[n]; }
        for (int j = 0; j < r->num_pos; j++) {
            int d = r->delta[r->num_neg + j] + delta_rps;
            if (d > 0 && use_delta[r->num_neg + j]) { out->delta[i] = d; out->used[i++] = used[r->num_neg + j]; }
        }
        out->num_pos = i - out->num_neg;
    } else {
        out->num_neg = ue(b);
        out->num_pos = ue(b);
        if (out->num_neg > 16 || out->num_pos > 16 || out->num_neg + out->num_pos > 16) return -EINVAL;
        int poc = 0;
        for (int i = 0; i < out->num_neg; i++) {
            poc -= ue(b) + 1;
            out->delta[i] = poc;
            out->used[i] = u1(b);
        }
        poc = 0;
        for (int i = 0; i < out->num_pos; i++) {
            poc += ue(b) + 1;
            out->delta[out->num_neg + i] = poc;
            out->used[out->num_neg + i] = u1(b);
        }
    }
    return b->err ? -EINVAL : 0;
}

static int parse_sps(struct br *b, struct sps *tab) {
    struct sps *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    int err = -EINVAL;
    un(b, 4); /* sps_video_parameter_set_id */
    int max_sub_layers_minus1 = un(b, 3);
    u1(b); /* temporal_id_nesting */
    skip_ptl(b, max_sub_layers_minus1);
    unsigned id = ue(b);
    if (id >= 16) goto out;
    s->chroma_format_idc = ue(b);
    if (s->chroma_format_idc == 3) s->separate_colour_plane = u1(b);
    s->width = ue(b);
    s->height = ue(b);
    if (u1(b)) { /* conformance window, in chroma samples */
        int sw = s->chroma_format_idc == 1 || s->chroma_format_idc == 2 ? 2 : 1;
        int sh = s->chroma_format_idc == 1 ? 2 : 1;
        s->conf_left = ue(b) * sw;
        s->conf_right = ue(b) * sw;
        s->conf_top = ue(b) * sh;
        s->conf_bottom = ue(b) * sh;
    }
    s->bit_depth_luma = ue(b) + 8;
    s->bit_depth_chroma = ue(b) + 8;
    s->log2_max_poc_lsb = ue(b) + 4;
    int ordering_all = u1(b);
    for (int i = ordering_all ? 0 : max_sub_layers_minus1; i <= max_sub_layers_minus1; i++) {
        ue(b); /* max_dec_pic_buffering_minus1 */
        s->max_num_reorder = ue(b);
        ue(b); /* max_latency_increase_plus1 */
    }
    s->log2_min_cb = ue(b) + 3;
    s->log2_diff_max_min_cb = ue(b);
    s->log2_min_tb = ue(b) + 2;
    s->log2_diff_max_min_tb = ue(b);
    s->max_th_depth_inter = ue(b);
    s->max_th_depth_intra = ue(b);
    default_scaling(&s->sl);
    if ((s->scaling_list_enabled = u1(b)) && u1(b) && (err = parse_scaling(b, &s->sl))) goto out;
    err = -EINVAL;
    s->amp = u1(b);
    s->sao = u1(b);
    if ((s->pcm = u1(b))) {
        s->pcm_bd_luma = un(b, 4) + 1;
        s->pcm_bd_chroma = un(b, 4) + 1;
        s->log2_min_pcm = ue(b) + 3;
        s->log2_diff_max_min_pcm = ue(b);
        s->pcm_loop_filter_disabled = u1(b);
    }
    s->num_st_rps = ue(b);
    if (s->num_st_rps > 64) goto out;
    for (int i = 0; i < s->num_st_rps; i++)
        if (parse_st_rps(b, s, i, &s->st[i], NULL)) goto out;
    if ((s->long_term_present = u1(b))) {
        s->num_lt_sps = ue(b);
        if (s->num_lt_sps > 32) goto out;
        for (int i = 0; i < s->num_lt_sps; i++) {
            s->lt_poc_lsb_sps[i] = un(b, s->log2_max_poc_lsb);
            s->lt_used_sps[i] = u1(b);
        }
    }
    s->temporal_mvp = u1(b);
    s->strong_intra_smoothing = u1(b);
    /* VUI and extensions: nothing the engine needs */
    if (b->err) goto out;
    if (s->chroma_format_idc != 1 || s->bit_depth_luma != 8 || s->bit_depth_chroma != 8) {
        DEC_ERR("only 8-bit 4:2:0 is supported (chroma format %d, depth %d/%d)", s->chroma_format_idc,
                s->bit_depth_luma, s->bit_depth_chroma);
        err = -ENOTSUP;
        goto out;
    }
    if (s->width < 16 || s->height < 16 || s->width > 4096 || s->height > 4096) { err = -ENOTSUP; goto out; }
    s->valid = 1;
    tab[id] = *s;
    err = 0;
out:
    free(s);
    return err;
}

static int parse_pps(struct br *b, struct pps *tab) {
    struct pps s = { .valid = 1, .loop_filter_across_tiles = 1, .num_tile_cols = 1, .num_tile_rows = 1 };
    unsigned id = ue(b);
    if (id >= 64) return -EINVAL;
    s.sps_id = ue(b);
    s.dependent_slice_segments = u1(b);
    s.output_flag_present = u1(b);
    s.num_extra_slice_header_bits = un(b, 3);
    s.sign_data_hiding = u1(b);
    s.cabac_init_present = u1(b);
    s.num_ref_idx_l0_default = ue(b) + 1;
    s.num_ref_idx_l1_default = ue(b) + 1;
    s.init_qp_minus26 = se(b);
    s.constrained_intra_pred = u1(b);
    s.transform_skip = u1(b);
    if ((s.cu_qp_delta_enabled = u1(b))) s.diff_cu_qp_delta_depth = ue(b);
    s.cb_qp_offset = se(b);
    s.cr_qp_offset = se(b);
    s.slice_chroma_qp_offsets_present = u1(b);
    s.weighted_pred = u1(b);
    s.weighted_bipred = u1(b);
    s.transquant_bypass = u1(b);
    s.tiles_enabled = u1(b);
    s.entropy_coding_sync = u1(b);
    if (s.tiles_enabled) {
        s.num_tile_cols = ue(b) + 1;
        s.num_tile_rows = ue(b) + 1;
        if (s.num_tile_cols > 20 || s.num_tile_rows > 22) return -EINVAL;
        if (!(s.uniform_spacing = u1(b))) {
            for (int i = 0; i < s.num_tile_cols - 1; i++) s.col_width[i] = ue(b) + 1;
            for (int i = 0; i < s.num_tile_rows - 1; i++) s.row_height[i] = ue(b) + 1;
        }
        s.loop_filter_across_tiles = u1(b);
    }
    s.loop_filter_across_slices = u1(b);
    if ((s.deblocking_control_present = u1(b))) {
        s.deblocking_override_enabled = u1(b);
        if (!(s.deblocking_disabled = u1(b))) {
            s.beta_offset_div2 = se(b);
            s.tc_offset_div2 = se(b);
        }
    }
    if ((s.scaling_list_present = u1(b))) {
        default_scaling(&s.sl);
        if (parse_scaling(b, &s.sl)) return -EINVAL;
    }
    s.lists_modification_present = u1(b);
    s.log2_parallel_merge_level = ue(b) + 2;
    s.slice_header_extension_present = u1(b);
    if (b->err || s.sps_id >= 16) return -EINVAL;
    tab[id] = s;
    return 0;
}

/* ---- slice segment header, up to the end of the RPS syntax ---- */
struct slice {
    int nal_type, tid, pps_id, type, pic_output, poc_lsb;
    struct st_rps st;
    int st_bits, st_ref_num_delta;
    int num_lt;
    int lt_poc_lsb[33], lt_used[33], lt_msb_present[33], lt_msb_cycle[33];
    int skip_bits; /* from pic_output_flag to the end of the long-term syntax */
};

static int ceil_log2(unsigned v) { int n = 0; while ((1u << n) < v) n++; return n; }

static int parse_slice(struct br *b, int nal_type, int tid, const struct sps *spss, const struct pps *ppss,
                       struct slice *s) {
    memset(s, 0, sizeof(*s));
    s->nal_type = nal_type;
    s->tid = tid;
    s->pic_output = 1;
    if (!u1(b)) return -EINVAL; /* first_slice_segment_in_pic_flag: callers only parse first segments */
    if (nal_type >= NAL_BLA_W_LP && nal_type <= 23) u1(b); /* no_output_of_prior_pics_flag */
    s->pps_id = ue(b);
    if (b->err || s->pps_id >= 64) return -EINVAL;
    const struct pps *pp = &ppss[s->pps_id];
    if (!pp->valid || !spss[pp->sps_id].valid) return -EINVAL;
    const struct sps *sp = &spss[pp->sps_id];
    un(b, pp->num_extra_slice_header_bits);
    s->type = ue(b);
    size_t skip_start = b->pos;
    if (pp->output_flag_present) s->pic_output = u1(b);
    if (sp->separate_colour_plane) un(b, 2);
    if (nal_type != NAL_IDR_W_RADL && nal_type != NAL_IDR_N_LP) {
        s->poc_lsb = un(b, sp->log2_max_poc_lsb);
        if (!u1(b)) { /* short_term_ref_pic_set_sps_flag */
            size_t st_start = b->pos;
            if (parse_st_rps(b, sp, sp->num_st_rps, &s->st, &s->st_ref_num_delta)) return -EINVAL;
            s->st_bits = b->pos - st_start;
        } else {
            if (!sp->num_st_rps) return -EINVAL;
            int idx = sp->num_st_rps > 1 ? (int)un(b, ceil_log2(sp->num_st_rps)) : 0;
            if (idx >= sp->num_st_rps) return -EINVAL;
            s->st = sp->st[idx];
        }
        if (sp->long_term_present) {
            int num_lt_sps = sp->num_lt_sps > 0 ? (int)ue(b) : 0;
            int num_lt_pics = ue(b);
            if (num_lt_sps > sp->num_lt_sps || num_lt_sps + num_lt_pics > 32) return -EINVAL;
            s->num_lt = num_lt_sps + num_lt_pics;
            for (int i = 0; i < s->num_lt; i++) {
                if (i < num_lt_sps) {
                    int idx = sp->num_lt_sps > 1 ? (int)un(b, ceil_log2(sp->num_lt_sps)) : 0;
                    s->lt_poc_lsb[i] = sp->lt_poc_lsb_sps[idx];
                    s->lt_used[i] = sp->lt_used_sps[idx];
                } else {
                    s->lt_poc_lsb[i] = un(b, sp->log2_max_poc_lsb);
                    s->lt_used[i] = u1(b);
                }
                if ((s->lt_msb_present[i] = u1(b))) {
                    int cycle = ue(b);
                    /* (7-52): DeltaPocMsbCycleLt accumulates within each group */
                    if (i != 0 && i != num_lt_sps) cycle += s->lt_msb_cycle[i - 1];
                    s->lt_msb_cycle[i] = cycle;
                } else if (i != 0 && i != num_lt_sps) {
                    s->lt_msb_cycle[i] = s->lt_msb_cycle[i - 1];
                }
            }
        }
    }
    s->skip_bits = b->pos - skip_start;
    return b->err ? -EINVAL : 0;
}

/* ---- decoder ---- */
struct ref { int used, surf, poc, long_term; };

struct hevcdec {
    struct t_dev dev;
    nvdec_picture_cb cb;
    void *opaque;
    struct sps spss[16];
    struct pps ppss[64];
    /* engine buffers, sized for alloc_w x alloc_h */
    struct t_bo surf[NSURF], bits, setup, status, coloc, filter, tiles, scaling;
    int alloc_w, alloc_h;
    unsigned pitch, luma_size, colmv_size, sao_offset, bsd_offset, flt_above_offset, sao_above_offset;
    size_t surf_size; /* surfaces are allocated on first use */
    struct ref refs[MAX_REFS];
    int period, reorder, prev_tid0_poc;
    int need_irap;     /* after create/reset/EOS: skip to the next IRAP, which then has NoRaslOutputFlag */
    int skip_rasl;     /* the associated IRAP had NoRaslOutputFlag: RASL pictures cannot be decoded */
    unsigned pic_counter;
    /* the picture being assembled */
    uint8_t *pic, *rbsp;
    size_t pic_len;
    int nslices;
    struct slice first;
    uint64_t pic_ts;
    uint8_t *staging;
    size_t staging_size;
    struct nvdec_vic vic;
};

static void free_buffers(struct hevcdec *d) {
    struct t_bo *all[] = { &d->bits, &d->setup, &d->status, &d->coloc, &d->filter, &d->tiles, &d->scaling };
    nvdec_vic_unmap(&d->vic);
    for (int i = 0; i < NSURF; i++) t_bo_free(&d->dev, &d->surf[i]);
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) t_bo_free(&d->dev, all[i]);
    d->alloc_w = d->alloc_h = 0;
}

static int alloc_buffers(struct hevcdec *d, const struct sps *sp) {
    if (d->alloc_w == sp->width && d->alloc_h == sp->height) return 0;
    free_buffers(d);
    memset(d->refs, 0, sizeof(d->refs));
    unsigned w64 = ALIGN(sp->width, 64), h64 = ALIGN(sp->height, 64);
    d->pitch = w64;
    d->luma_size = ALIGN(d->pitch * h64, 4096);
    d->surf_size = d->luma_size + ALIGN(d->pitch * h64 / 2, 4096);
    /* collocated motion vectors: 16 bytes per 16x16 block, one set per surface */
    d->colmv_size = ALIGN(w64 * h64 / 16, 256);
    /* Filter buffer: tile-column (left) deblocking, SAO and BSD context sized by
     * height, then the above-row deblocking and SAO lines sized by width, with
     * room to spare at the end. */
    unsigned dblk_left = HEVC_DBLK_LEFT_BUF_SIZE(h64) * 4, sao_left = HEVC_SAO_LEFT_BUF_SIZE(h64) * 4;
    unsigned bsd = NVDEC_ALIGN(h64 * GIP_ASIC_BSD_CTRL_RAM_SIZE * 20) * 4;
    unsigned dblk_top = HEVC_DBLK_TOP_BUF_SIZE(w64) * 4, sao_top = HEVC_DBLK_TOP_BUF_SIZE(w64) * 4;
    d->sao_offset = dblk_left;
    d->bsd_offset = d->sao_offset + sao_left;
    d->flt_above_offset = d->bsd_offset + bsd;
    d->sao_above_offset = d->flt_above_offset + dblk_top;
    size_t filter_size = d->sao_above_offset + sao_top + (1u << 20);
    if (t_bo_new(&d->dev, &d->coloc, (size_t)d->colmv_size * NSURF) ||
        t_bo_new(&d->dev, &d->filter, filter_size) ||
        t_bo_new(&d->dev, &d->bits, BITSTREAM_SIZE) ||
        t_bo_new(&d->dev, &d->setup, 4096) || t_bo_new(&d->dev, &d->status, 4096) ||
        t_bo_new(&d->dev, &d->tiles, 4096) || t_bo_new(&d->dev, &d->scaling, 4096))
        return -ENOMEM;
    d->alloc_w = sp->width;
    d->alloc_h = sp->height;
    return 0;
}

/* Surfaces are allocated as the DPB first needs them: allocating all of them
 * up front delayed the first picture by a few hundred ms. */
static int ensure_surface(struct hevcdec *d, int s) {
    if (d->surf[s].handle) return 0;
    if (t_bo_new(&d->dev, &d->surf[s], d->surf_size)) return -ENOMEM;
    return nvdec_vic_map_surface(&d->vic, s, &d->surf[s], d->luma_size);
}

static int surf_busy(const struct hevcdec *d, int s) {
    for (int i = 0; i < MAX_REFS; i++) if (d->refs[i].used && d->refs[i].surf == s) return 1;
    return 0;
}

static void write_tile_sizes(const struct sps *sp, const struct pps *pp, uint16_t *p) {
    int ctb = 1 << (sp->log2_min_cb + sp->log2_diff_max_min_cb);
    int wc = (sp->width + ctb - 1) / ctb, hc = (sp->height + ctb - 1) / ctb;
    int cols = pp->tiles_enabled ? pp->num_tile_cols : 1, rows = pp->tiles_enabled ? pp->num_tile_rows : 1;
    int col_w[20], row_h[22];
    for (int i = 0, left = wc; i < cols; i++) {
        col_w[i] = !pp->tiles_enabled || pp->uniform_spacing ? (i + 1) * wc / cols - i * wc / cols
                   : i < cols - 1 ? pp->col_width[i] : left;
        left -= col_w[i];
    }
    for (int i = 0, left = hc; i < rows; i++) {
        row_h[i] = !pp->tiles_enabled || pp->uniform_spacing ? (i + 1) * hc / rows - i * hc / rows
                   : i < rows - 1 ? pp->row_height[i] : left;
        left -= row_h[i];
    }
    memset(p, 0, GIP_ASIC_TILE_SIZE);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            *p++ = col_w[c];
            *p++ = row_h[r];
        }
}

static double dec_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int find_ref(const struct hevcdec *d, int poc, int mask, int long_term_ok, int short_term_ok) {
    for (int i = 0; i < MAX_REFS; i++) {
        const struct ref *r = &d->refs[i];
        if (!r->used || (r->long_term ? !long_term_ok : !short_term_ok)) continue;
        if ((r->poc & mask) == poc) return i;
    }
    return -1;
}

static int decode_picture(struct hevcdec *d) {
    double t_start = dec_now();
    const struct slice *s = &d->first;
    const struct pps *pp = &d->ppss[s->pps_id];
    const struct sps *sp = &d->spss[pp->sps_id];
    int irap = s->nal_type >= NAL_BLA_W_LP && s->nal_type <= 23;
    int idr = s->nal_type == NAL_IDR_W_RADL || s->nal_type == NAL_IDR_N_LP;
    int rasl = s->nal_type == NAL_RASL_N || s->nal_type == NAL_RASL_R;

    if (d->need_irap && !irap) return 0;
    int no_rasl_output = irap && (idr || s->nal_type <= NAL_BLA_N_LP || d->need_irap);
    if (irap) d->skip_rasl = no_rasl_output;
    if (rasl && d->skip_rasl) return 0;
    d->need_irap = 0;

    int err = alloc_buffers(d, sp);
    if (err) return err;

    /* 8.3.1 picture order count */
    int max_lsb = 1 << sp->log2_max_poc_lsb, msb = 0;
    if (!(irap && no_rasl_output)) {
        int prev_lsb = d->prev_tid0_poc & (max_lsb - 1), prev_msb = d->prev_tid0_poc - prev_lsb;
        if (s->poc_lsb < prev_lsb && prev_lsb - s->poc_lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
        else if (s->poc_lsb > prev_lsb && s->poc_lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
        else msb = prev_msb;
    }
    int poc = msb + s->poc_lsb;
    int sub_layer_non_ref = s->nal_type <= 14 && !(s->nal_type & 1);
    if (s->tid == 0 && !rasl && s->nal_type != NAL_RADL_N && s->nal_type != NAL_RADL_R && !sub_layer_non_ref)
        d->prev_tid0_poc = poc;
    if (irap && no_rasl_output) d->period++;

    /* 8.3.2 reference picture set: the current lists, and every other reference is dropped */
    int st_before[16], st_after[16], lt_curr[32], n_before = 0, n_after = 0, n_lt = 0;
    int keep[MAX_REFS] = { 0 }, missing = 0;
    if (irap && no_rasl_output) memset(d->refs, 0, sizeof(d->refs));
    for (int i = 0; i < s->num_lt; i++) {
        int lt = s->lt_poc_lsb[i], mask = max_lsb - 1;
        if (s->lt_msb_present[i]) {
            lt += poc - s->lt_msb_cycle[i] * max_lsb - (poc & (max_lsb - 1));
            mask = -1;
        }
        int r = find_ref(d, lt, mask, 1, 1);
        if (r < 0) { if (s->lt_used[i]) missing++; continue; }
        d->refs[r].long_term = 1;
        keep[r] = 1;
        if (s->lt_used[i]) lt_curr[n_lt++] = r;
    }
    for (int i = 0; i < s->st.num_neg + s->st.num_pos; i++) {
        int r = find_ref(d, poc + s->st.delta[i], -1, 0, 1);
        if (r < 0) { if (s->st.used[i]) missing++; continue; }
        keep[r] = 1;
        if (!s->st.used[i]) continue;
        if (i < s->st.num_neg) st_before[n_before++] = r;
        else st_after[n_after++] = r;
    }
    for (int i = 0; i < MAX_REFS; i++) if (!keep[i]) d->refs[i].used = 0;
    if (missing) DEC_WARN("picture %d: %d missing references", poc, missing);

    int cur = -1;
    for (int i = 0; i < NSURF && cur < 0; i++) if (!surf_busy(d, i)) cur = i;
    if (cur < 0) return -ENOSPC;
    if ((err = ensure_surface(d, cur))) return err;

    memcpy(d->bits.ptr, d->pic, d->pic_len);
    memset((uint8_t *)d->bits.ptr + d->pic_len, 0, 64);

    nvdec_hevc_pic_s *h = d->setup.ptr;
    memset(h, 0, sizeof(*h));
    h->stream_len = d->pic_len;
    h->gptimer_timeout_value = 81000000;
    h->tileformat = 0; /* TBL, as for H.264 */
    h->gob_height = 0;
    h->sw_start_code_e = 1;
    h->framestride[0] = d->pitch;
    h->framestride[1] = d->pitch;
    h->colMvBuffersize = d->colmv_size >> 8;
    h->HevcSaoBufferOffset = d->sao_offset >> 8;
    h->HevcBsdCtrlOffset = d->bsd_offset >> 8;
    h->v1.hevc_main10_444_ext.HevcFltAboveOffset = d->flt_above_offset >> 8;
    h->v1.hevc_main10_444_ext.HevcSaoAboveOffset = d->sao_above_offset >> 8;
    h->pic_width_in_luma_samples = sp->width;
    h->pic_height_in_luma_samples = sp->height;
    h->chroma_format_idc = sp->chroma_format_idc;
    h->bit_depth_luma = sp->bit_depth_luma;
    h->bit_depth_chroma = sp->bit_depth_chroma;
    h->log2_min_luma_coding_block_size = sp->log2_min_cb;
    h->log2_max_luma_coding_block_size = sp->log2_min_cb + sp->log2_diff_max_min_cb;
    h->log2_min_transform_block_size = sp->log2_min_tb;
    h->log2_max_transform_block_size = sp->log2_min_tb + sp->log2_diff_max_min_tb;
    h->max_transform_hierarchy_depth_inter = sp->max_th_depth_inter;
    h->max_transform_hierarchy_depth_intra = sp->max_th_depth_intra;
    h->scalingListEnable = sp->scaling_list_enabled;
    h->amp_enable_flag = sp->amp;
    h->sample_adaptive_offset_enabled_flag = sp->sao;
    h->pcm_enabled_flag = sp->pcm;
    if (sp->pcm) {
        h->pcm_sample_bit_depth_luma = sp->pcm_bd_luma;
        h->pcm_sample_bit_depth_chroma = sp->pcm_bd_chroma;
        h->log2_min_pcm_luma_coding_block_size = sp->log2_min_pcm;
        h->log2_max_pcm_luma_coding_block_size = sp->log2_min_pcm + sp->log2_diff_max_min_pcm;
        h->pcm_loop_filter_disabled_flag = sp->pcm_loop_filter_disabled;
    }
    h->sps_temporal_mvp_enabled_flag = sp->temporal_mvp;
    h->strong_intra_smoothing_enabled_flag = sp->strong_intra_smoothing;
    h->dependent_slice_segments_enabled_flag = pp->dependent_slice_segments;
    h->output_flag_present_flag = pp->output_flag_present;
    h->num_extra_slice_header_bits = pp->num_extra_slice_header_bits;
    h->sign_data_hiding_enabled_flag = pp->sign_data_hiding;
    h->cabac_init_present_flag = pp->cabac_init_present;
    h->num_ref_idx_l0_default_active = pp->num_ref_idx_l0_default;
    h->num_ref_idx_l1_default_active = pp->num_ref_idx_l1_default;
    h->init_qp = 26 + pp->init_qp_minus26 + 6 * (sp->bit_depth_luma - 8);
    h->constrained_intra_pred_flag = pp->constrained_intra_pred;
    h->transform_skip_enabled_flag = pp->transform_skip;
    h->cu_qp_delta_enabled_flag = pp->cu_qp_delta_enabled;
    h->diff_cu_qp_delta_depth = pp->diff_cu_qp_delta_depth;
    h->pps_cb_qp_offset = pp->cb_qp_offset;
    h->pps_cr_qp_offset = pp->cr_qp_offset;
    h->pps_beta_offset = pp->beta_offset_div2 * 2;
    h->pps_tc_offset = pp->tc_offset_div2 * 2;
    h->pps_slice_chroma_qp_offsets_present_flag = pp->slice_chroma_qp_offsets_present;
    h->weighted_pred_flag = pp->weighted_pred;
    h->weighted_bipred_flag = pp->weighted_bipred;
    h->transquant_bypass_enabled_flag = pp->transquant_bypass;
    h->tiles_enabled_flag = pp->tiles_enabled;
    h->entropy_coding_sync_enabled_flag = pp->entropy_coding_sync;
    h->num_tile_columns = pp->num_tile_cols;
    h->num_tile_rows = pp->num_tile_rows;
    h->loop_filter_across_tiles_enabled_flag = pp->loop_filter_across_tiles;
    h->loop_filter_across_slices_enabled_flag = pp->loop_filter_across_slices;
    h->deblocking_filter_control_present_flag = pp->deblocking_control_present;
    h->deblocking_filter_override_enabled_flag = pp->deblocking_override_enabled;
    h->pps_deblocking_filter_disabled_flag = pp->deblocking_disabled;
    h->lists_modification_present_flag = pp->lists_modification_present;
    h->log2_parallel_merge_level = pp->log2_parallel_merge_level;
    h->slice_segment_header_extension_present_flag = pp->slice_header_extension_present;

    /* references: engine picture index == surface index */
    int total_curr = n_before + n_after + n_lt;
    h->num_ref_frames = total_curr;
    for (int i = 0; i < MAX_REFS; i++) {
        const struct ref *r = &d->refs[i];
        if (!r->used) continue;
        int diff = poc - r->poc;
        h->RefDiffPicOrderCnts[r->surf] = diff < -128 ? -128 : diff > 127 ? 127 : diff;
        if (r->long_term && r->surf < 16) h->longtermflag |= 1u << (15 - r->surf);
    }
    if (total_curr) {
        int l0[48], l1[48], n0 = 0, n1 = 0;
        for (int i = 0; i < n_before; i++) l0[n0++] = d->refs[st_before[i]].surf;
        for (int i = 0; i < n_after; i++) l0[n0++] = d->refs[st_after[i]].surf;
        for (int i = 0; i < n_lt; i++) l0[n0++] = d->refs[lt_curr[i]].surf;
        for (int i = 0; i < n_after; i++) l1[n1++] = d->refs[st_after[i]].surf;
        for (int i = 0; i < n_before; i++) l1[n1++] = d->refs[st_before[i]].surf;
        for (int i = 0; i < n_lt; i++) l1[n1++] = d->refs[lt_curr[i]].surf;
        for (int i = 0; i < 16; i++) { /* RefPicListTemp0/1 (8-8, 8-10): the lists repeat */
            h->initreflistidxl0[i] = l0[i % n0];
            h->initreflistidxl1[i] = l1[i % n1];
        }
    }
    h->IDR_picture_flag = idr;
    h->RAP_picture_flag = irap;
    h->curr_pic_idx = cur;
    h->pattern_id = 2;
    h->sw_hdr_skip_length = s->skip_bits;
    h->separate_colour_plane_flag = sp->separate_colour_plane;
    h->log2_max_pic_order_cnt_lsb_minus4 = sp->log2_max_poc_lsb - 4;
    h->num_short_term_ref_pic_sets = sp->num_st_rps;
    h->num_long_term_ref_pics_sps = sp->num_lt_sps;
    h->num_delta_pocs_of_rps_idx = s->st_ref_num_delta;
    h->long_term_ref_pics_present_flag = sp->long_term_present;
    h->num_bits_short_term_ref_pics_in_slice = s->st_bits;

    write_tile_sizes(sp, pp, d->tiles.ptr);
    memset(d->scaling.ptr, 0, GIP_ASIC_SCALING_LIST_SIZE);
    if (sp->scaling_list_enabled) write_scaling(pp->scaling_list_present ? &pp->sl : &sp->sl, d->scaling.ptr);
    memset(d->status.ptr, 0, sizeof(nvdec_status_s));

    struct t_job j;
    t_job_init(&j);
    t_mthd(&j, NVC5B0_SET_APPLICATION_ID, APP_ID_HEVC);
    t_mthd(&j, NVC5B0_SET_CONTROL_PARAMS,
           CODEC_HEVC | 1 << 4 /* gptimer */ | 1 << 6 /* err conceal */ | (cur & 0x3f) << 7 | 1 << 13 /* mbtimer */);
    t_mthd_addr(&j, NVC5B0_SET_DRV_PIC_SETUP_OFFSET, &d->setup, 0);
    t_mthd_addr(&j, NVC5B0_SET_IN_BUF_BASE_OFFSET, &d->bits, 0);
    t_mthd(&j, NVC5B0_SET_PICTURE_INDEX, d->pic_counter);
    t_mthd_addr(&j, NVC5B0_SET_COLOC_DATA_OFFSET, &d->coloc, 0);
    t_mthd_addr(&j, NVC5B0_SET_NVDEC_STATUS_OFFSET, &d->status, 0);
    t_mthd_addr(&j, NVC5B0_HEVC_SET_SCALING_LIST_OFFSET, &d->scaling, 0);
    t_mthd_addr(&j, NVC5B0_HEVC_SET_TILE_SIZES_OFFSET, &d->tiles, 0);
    t_mthd_addr(&j, NVC5B0_HEVC_SET_FILTER_BUFFER_OFFSET, &d->filter, 0);
    t_mthd_addr(&j, NVC5B0_HEVC_SET_SAO_BUFFER_OFFSET, &d->filter, d->sao_offset);
    /* slots not allocated yet hold no reference: point them at the current surface */
    for (int i = 0; i < NSURF; i++)
        t_mthd_addr(&j, NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i, &d->surf[d->surf[i].handle ? i : cur], 0);
    for (int i = 0; i < NSURF; i++)
        t_mthd_addr(&j, NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i, &d->surf[d->surf[i].handle ? i : cur], d->luma_size);
    t_mthd(&j, NVC5B0_EXECUTE, EXECUTE_AWAKEN);
    t_syncpt_incr(&j, &d->dev);
    double t_submit = dec_now();
    if (t_submit_wait(&d->dev, &j, 2000, NULL)) return -ETIMEDOUT;
    double t_done = dec_now();
    d->pic_counter++;

    /* every decoded picture is a reference until a later RPS drops it */
    int slot = -1;
    for (int i = 0; i < MAX_REFS && slot < 0; i++) if (!d->refs[i].used) slot = i;
    if (slot < 0) {
        DEC_WARN("DPB overflow, dropping a reference");
        slot = 0;
    }
    d->refs[slot] = (struct ref){ .used = 1, .surf = cur, .poc = poc };
    d->reorder = sp->max_num_reorder;
    if (!s->pic_output) return 0;

    size_t surf_size = d->surf[cur].size;
    const uint8_t *pix = d->surf[cur].ptr;
    if (!d->vic.ok) { /* see h264dec.c: surfaces are write-combined */
        if (d->staging_size < surf_size) {
            free(d->staging);
            d->staging = malloc(surf_size);
            d->staging_size = d->staging ? surf_size : 0;
            if (!d->staging) return -ENOMEM;
        }
        memcpy(d->staging, d->surf[cur].ptr, surf_size);
        pix = d->staging;
    }
    if (getenv("H264DEC_TIMING"))
        fprintf(stderr, "setup %.2f ms, engine %.2f ms, staging copy %.2f ms\n",
                t_submit - t_start, t_done - t_submit, dec_now() - t_done);

    const nvdec_status_s *st = d->status.ptr;
    struct nvdec_picture pic = {
        .luma = pix, .chroma = pix + d->luma_size,
        .pitch = d->pitch,
        .coded_width = sp->width, .coded_height = sp->height,
        .crop_left = sp->conf_left, .crop_top = sp->conf_top,
        .width = sp->width - sp->conf_left - sp->conf_right,
        .height = sp->height - sp->conf_top - sp->conf_bottom,
        .poc = poc, .period = d->period, .timestamp = d->pic_ts,
        /* IRAP pictures without leading pictures start their period in display order */
        .leader = s->nal_type == NAL_IDR_N_LP || s->nal_type == NAL_BLA_N_LP,
        .mbs_in_error = st->mbs_in_error, .surface = cur,
    };
    return d->cb(d->opaque, &pic) ? -ECANCELED : 0;
}

struct hevcdec *hevcdec_create(const char *node, uint32_t host1x_class, nvdec_picture_cb cb, void *opaque) {
    struct hevcdec *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->cb = cb;
    d->opaque = opaque;
    d->period = -1;
    d->reorder = -1;
    d->need_irap = 1;
    d->pic = malloc(BITSTREAM_SIZE);
    d->rbsp = malloc(4096);
    if (!d->pic || !d->rbsp || t_open(&d->dev, node, host1x_class)) {
        hevcdec_destroy(d);
        return NULL;
    }
    nvdec_vic_open(&d->dev, &d->vic);
    if (!d->vic.ok) DEC_WARN("VIC unavailable, using CPU copies");
    return d;
}

void hevcdec_destroy(struct hevcdec *d) {
    if (!d) return;
    if (d->dev.fd > 0) {
        free_buffers(d);
        nvdec_vic_close(&d->vic);
        t_close(&d->dev);
    }
    free(d->pic);
    free(d->rbsp);
    free(d->staging);
    free(d);
}

void hevcdec_reset(struct hevcdec *d) {
    memset(d->refs, 0, sizeof(d->refs));
    d->need_irap = 1;
    d->nslices = 0;
    d->pic_len = 0;
}

int hevcdec_reorder_depth(const struct hevcdec *d) { return d->reorder; }

static size_t next_start_code(const uint8_t *in, size_t n, size_t from) {
    for (size_t i = from; i + 3 <= n; i++)
        if (in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1) return i + 3;
    return n;
}

static int finish_picture(struct hevcdec *d) {
    int err = d->nslices ? decode_picture(d) : 0;
    d->nslices = 0;
    d->pic_len = 0;
    return err;
}

int hevcdec_decode(struct hevcdec *d, const uint8_t *in, size_t n, uint64_t timestamp) {
    size_t pos = next_start_code(in, n, 0);
    while (pos < n) {
        size_t next = next_start_code(in, n, pos);
        size_t beg = pos, end = next < n ? next - 3 : n;
        while (end > beg && in[end - 1] == 0) end--; /* trailing zeros, 4-byte start codes */
        if (end > beg + 2) {
            int type = (in[beg] >> 1) & 0x3f, tid = (in[beg + 1] & 7) - 1, err = 0;
            if (type == NAL_SPS || type == NAL_PPS) {
                size_t rn = unescape(in + beg + 2, end - beg - 2 < 4096 ? end - beg - 2 : 4096, d->rbsp);
                struct br b = { d->rbsp, rn, 0, 0 };
                err = type == NAL_SPS ? parse_sps(&b, d->spss) : parse_pps(&b, d->ppss);
                if (err) DEC_ERR("bad %s: %d", type == NAL_SPS ? "SPS" : "PPS", err);
            } else if (type == NAL_EOS || type == NAL_EOB) {
                err = finish_picture(d);
                d->need_irap = 1;
            } else if (type <= 21 && (type <= 9 || type >= NAL_BLA_W_LP)) {
                int first = in[beg + 2] & 0x80;
                if (first) {
                    err = finish_picture(d); /* the previous picture is complete */
                    struct slice cur;
                    size_t rn = unescape(in + beg + 2, end - beg - 2 < 1024 ? end - beg - 2 : 1024, d->rbsp);
                    struct br b = { d->rbsp, rn, 0, 0 };
                    if (!err) err = parse_slice(&b, type, tid, d->spss, d->ppss, &cur);
                    if (!err) { d->first = cur; d->pic_ts = timestamp; }
                }
                if (!err && (first || d->nslices)) {
                    if (d->pic_len + 3 + (end - beg) > BITSTREAM_SIZE) err = -E2BIG;
                    if (!err) {
                        d->nslices++;
                        d->pic[d->pic_len++] = 0;
                        d->pic[d->pic_len++] = 0;
                        d->pic[d->pic_len++] = 1;
                        memcpy(d->pic + d->pic_len, in + beg, end - beg);
                        d->pic_len += end - beg;
                    }
                }
            }
            if (err) { d->nslices = 0; d->pic_len = 0; return err; }
        }
        pos = next;
    }
    /* MediaCodec delivers whole access units: finish the picture now */
    return finish_picture(d);
}

int hevcdec_vic_copy(struct hevcdec *d, const struct nvdec_picture *pic, int dmabuf_fd,
                     size_t dmabuf_size, unsigned pitch, unsigned chroma_offset) {
    return nvdec_vic_copy(&d->vic, pic, dmabuf_fd, dmabuf_size, pitch, chroma_offset);
}
