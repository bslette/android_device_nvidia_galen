/*
 * H.264 encoding on NVENC (Tegra194, class C5B7, firmware NVENC 7.0); see h264enc.h.
 *
 * The engine does motion search, mode decision, transform, entropy coding, slice
 * headers and rate control; the driver writes SPS/PPS, fills the picture setup
 * (nvenc_drv.h) and manages the two reference surfaces. Encoder control values
 * (motion search shapes, mode decision, dead zones, rate control start state)
 * are the ones NVIDIA's L4T r35 driver uses.
 */
#include "h264enc.h"

#include "host1x.h"
#include "nvenc_drv.h"
#include "vic42.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define M_APP_ID           0x200
#define M_EXECUTE          0x300
#define M_IN_REF_LUMA(i)   (0x400 + 4 * (i))
#define M_IN_REF_CHROMA(i) (0x440 + 4 * (i))
#define M_CONTROL_PARAMS   0x700
#define M_PICTURE_INDEX    0x704
#define M_IN_DRV_PIC_SETUP 0x710
#define M_OUT_ENC_STATUS   0x718
#define M_OUT_BITSTREAM    0x71c
#define M_IOHISTORY        0x720
#define M_IO_RC_PROCESS    0x724
#define M_IN_COLOC         0x728
#define M_OUT_COLOC        0x72c
#define M_OUT_REF_LUMA     0x730
#define M_IN_CUR_PIC       0x734
#define M_IN_MEPRED        0x738
#define M_OUT_MEPRED       0x73c
#define M_IN_CUR_CHROMA_U  0x740
#define M_IN_CUR_CHROMA_V  0x744
#define M_OUT_REF_CHROMA   0x74c

#define CTRL_H264          3
#define CTRL_FORCE_OUT_PIC (1u << 8)
#define CTRL_SLICE_STAT_ON (1u << 11)
#define CTRL_RCMODE(m)     ((uint32_t)(m) << 24)
#define RCMODE_CONSTQP     0
#define RCMODE_VBR_MBROW   17
#define RCMODE_CBR_MBROW   18
#define EXECUTE_AWAKEN     (1u << 8)
#define ENC_STATUS_DONE    2

#define MAGIC 0xC5B70006u /* NVENC 7.0 */

/* Surfaces are block linear with blocks of 2 GOBs (16 rows); the engine's
 * block_height field says 2 for that. */
#define BLK_LOG2  1
#define BLK_FIELD 2

#define VIC_CLASS                        0x5d
#define VIC_APP_ID                       0x200
#define VIC_EXECUTE                      0x300
#define VIC_SET_CONTROL_PARAMS           0x704
#define VIC_SET_CONFIG_STRUCT_OFFSET     0x708
#define VIC_SET_FILTER_STRUCT_OFFSET     0x70c
#define VIC_SET_OUTPUT_LUMA_OFFSET       0x720
#define VIC_SET_OUTPUT_CHROMA_U_OFFSET   0x724
#define VIC_SET_SURFACE0_LUMA_OFFSET     0x1200
#define VIC_SET_SURFACE0_CHROMA_U_OFFSET 0x1204
#define VIC_FMT_A8B8G8R8                 31 /* bytes R, G, B, A */
#define VIC_FMT_A8R8G8B8                 32 /* bytes B, G, R, A */
#define VIC_FMT_X8B8G8R8                 35
#define VIC_FMT_X8R8G8B8                 36
#define VIC_FMT_NV12                     67
#define VIC_BLK_KIND_PITCH               0
#define VIC_BLK_KIND_GENERIC_16Bx2       1
#define VIC_CACHE_WIDTH_64Bx4            2

#define ALIGN(x, a) (((x) + (a) - 1) / (a) * (a))

struct pic { struct t_bo luma, chroma; };

struct h264enc {
    struct t_dev dev;
    struct h264enc_config cfg;
    unsigned mbw, mbh, w16, h16, pitch;
    struct pic cur, ref[2];
    struct t_bo setup, status, bits, hist, rc, coloc[2], mepred[2];
    unsigned frame, gop_pos, idr_id, last;
    int have_ref;
    /* VIC: converts the input into cur */
    struct t_dev vic;
    int vic_ok;
    struct t_bo vic_config, vic_filter, vic_luma, vic_chroma;
};

/* --- Annex B parameter sets --- */

struct bw { uint8_t buf[256]; int bits; };

static void put(struct bw *b, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--, b->bits++)
        if ((v >> i) & 1) b->buf[b->bits / 8] |= 0x80 >> (b->bits % 8);
}

static void ue(struct bw *b, uint32_t v) {
    int n = 0;
    while ((v + 1) >> (n + 1)) n++;
    put(b, 0, n);
    put(b, v + 1, n + 1);
}

static void se(struct bw *b, int v) { ue(b, v <= 0 ? -2 * v : 2 * v - 1); }

static int nal(uint8_t *out, size_t cap, int type, struct bw *b) {
    put(b, 1, 1);
    while (b->bits % 8) put(b, 0, 1);
    uint8_t tmp[300] = {0, 0, 0, 1, (uint8_t)(0x60 | type)};
    int n = 5, zeros = 0;
    for (int i = 0; i < b->bits / 8; i++) {
        if (zeros >= 2 && b->buf[i] <= 3) { tmp[n++] = 3; zeros = 0; }
        tmp[n++] = b->buf[i];
        zeros = b->buf[i] ? 0 : zeros + 1;
    }
    if ((size_t)n > cap) return -ENOSPC;
    memcpy(out, tmp, n);
    return n;
}

int h264enc_headers(struct h264enc *e, uint8_t *buf, size_t cap) {
    const struct h264enc_config *c = &e->cfg;
    int profile = c->profile_idc;
    struct bw b = {{0}, 0};
    put(&b, profile, 8);
    put(&b, profile == 66 ? 0xc0 : 0, 8); /* constrained baseline: set0 + set1 */
    put(&b, c->level_idc, 8);
    ue(&b, 0);                 /* seq_parameter_set_id */
    if (profile == 100) {
        ue(&b, 1);             /* chroma_format_idc 4:2:0 */
        ue(&b, 0); ue(&b, 0);  /* bit depths 8 */
        put(&b, 0, 1);         /* qpprime_y_zero_transform_bypass */
        put(&b, 0, 1);         /* seq_scaling_matrix_present */
    }
    ue(&b, 4);                 /* log2_max_frame_num_minus4 */
    ue(&b, 0);                 /* pic_order_cnt_type */
    ue(&b, 4);                 /* log2_max_pic_order_cnt_lsb_minus4 */
    ue(&b, 1);                 /* max_num_ref_frames */
    put(&b, 0, 1);             /* gaps_in_frame_num_allowed */
    ue(&b, e->mbw - 1);
    ue(&b, e->mbh - 1);
    put(&b, 1, 1);             /* frame_mbs_only */
    put(&b, 1, 1);             /* direct_8x8_inference */
    if (c->width != e->w16 || c->height != e->h16) {
        put(&b, 1, 1);         /* frame_cropping, in 2-pixel units for 4:2:0 */
        ue(&b, 0); ue(&b, (e->w16 - c->width) / 2);
        ue(&b, 0); ue(&b, (e->h16 - c->height) / 2);
    } else {
        put(&b, 0, 1);
    }
    put(&b, 1, 1);             /* vui_parameters_present */
    put(&b, 0, 1);             /* aspect_ratio_info_present */
    put(&b, 0, 1);             /* overscan_info_present */
    put(&b, 1, 1);             /* video_signal_type_present */
    put(&b, 5, 3);             /* video_format: unspecified */
    put(&b, c->full_range ? 1 : 0, 1);
    put(&b, 1, 1);             /* colour_description_present */
    put(&b, c->primaries, 8);
    put(&b, c->transfer, 8);
    put(&b, c->matrix, 8);
    put(&b, 0, 1);             /* chroma_loc_info_present */
    put(&b, 0, 1);             /* timing_info_present */
    put(&b, 0, 1);             /* nal_hrd_parameters_present */
    put(&b, 0, 1);             /* vcl_hrd_parameters_present */
    put(&b, 0, 1);             /* pic_struct_present */
    put(&b, 1, 1);             /* bitstream_restriction */
    put(&b, 1, 1);             /* motion_vectors_over_pic_boundaries */
    ue(&b, 0);                 /* max_bytes_per_pic_denom */
    ue(&b, 0);                 /* max_bits_per_mb_denom */
    ue(&b, 16); ue(&b, 16);    /* log2_max_mv_length horizontal/vertical */
    ue(&b, 0);                 /* max_num_reorder_frames: no B pictures */
    ue(&b, 1);                 /* max_dec_frame_buffering */
    int n = nal(buf, cap, 7, &b);
    if (n < 0) return n;

    memset(&b, 0, sizeof(b));
    ue(&b, 0);                 /* pic_parameter_set_id */
    ue(&b, 0);                 /* seq_parameter_set_id */
    put(&b, profile != 66, 1); /* entropy_coding_mode */
    put(&b, 0, 1);             /* bottom_field_pic_order_in_frame_present */
    ue(&b, 0);                 /* num_slice_groups_minus1 */
    ue(&b, 0); ue(&b, 0);      /* num_ref_idx_l0/l1_default_active_minus1 */
    put(&b, 0, 1);             /* weighted_pred */
    put(&b, 0, 2);             /* weighted_bipred_idc */
    se(&b, 0); se(&b, 0);      /* pic_init_qp/qs_minus26 */
    se(&b, 0);                 /* chroma_qp_index_offset */
    put(&b, 1, 1);             /* deblocking_filter_control_present */
    put(&b, 0, 1);             /* constrained_intra_pred */
    put(&b, 0, 1);             /* redundant_pic_cnt_present */
    if (profile == 100) {
        put(&b, 1, 1);         /* transform_8x8_mode */
        put(&b, 0, 1);         /* pic_scaling_matrix_present */
        se(&b, 0);             /* second_chroma_qp_index_offset */
    }
    int m = nal(buf + n, cap - n, 8, &b);
    return m < 0 ? m : n + m;
}

/* --- engine setup --- */

/* Block linear: GOBs of 64 B x 8 rows (16Bx2 sectors), blocks of 2^bh GOBs. */
static inline size_t bl_offset(unsigned pitch, unsigned x, unsigned y) {
    const unsigned rows = 8u << BLK_LOG2;
    size_t block = (size_t)(y / rows) * (pitch / 64) + x / 64;
    return block * (512u << BLK_LOG2) + ((y % rows) / 8) * 512 + ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
           ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
}

static int pic_new(struct t_dev *d, struct pic *p, unsigned pitch, unsigned h16) {
    return t_bo_new(d, &p->luma, (size_t)pitch * ALIGN(h16, 8u << BLK_LOG2)) ||
           t_bo_new(d, &p->chroma, (size_t)pitch * ALIGN(h16 / 2, 8u << BLK_LOG2));
}

static void pic_free(struct t_dev *d, struct pic *p) {
    t_bo_free(d, &p->luma);
    t_bo_free(d, &p->chroma);
}

static void controls(nvenc_h264_me_control_s *me, nvenc_h264_md_control_s *md, nvenc_h264_quant_control_s *q,
                     int t8x8) {
    me->refinement_mode = 1; /* quarter pel */
    me->refine_on_search_enable = 1;
    me->fps_mvcost = 1;
    me->sps_mvcost = 1;
    me->sps_cost_func = 1;
    me->limit_mv.mv_limit_enable = 1;
    me->limit_mv.left_mvx_int = -2048;
    me->limit_mv.top_mvy_int = -512;
    me->limit_mv.right_mvx_int = 2047;
    me->limit_mv.bottom_mvy_int = 511;
    me->predsrc.self_temporal_search = 1;
    me->predsrc.self_temporal_enable = 1;
    me->predsrc.coloc_search = 1;
    me->predsrc.coloc_enable = 1;
    me->predsrc.self_spatial_stamp_l0 = 1;
    me->predsrc.self_spatial_search = 1;
    me->predsrc.self_spatial_enable = 1;
    me->predsrc.external_stamp_l0_refidx0_stamp = 1;
    me->predsrc.const_mv_stamp_l0 = 3;
    me->predsrc.const_mv_search = 1;
    me->predsrc.const_mv_enable = 1;
    nvenc_h264_me_stamp_s *shape[4] = {&me->shape0, &me->shape1, &me->shape2, &me->shape3};
    for (int i = 0; i < 4; i++) {
        shape[i]->bitmask[0] = i < 3 ? 0xff0c0c00 : 0xff1e0c0c;
        shape[i]->bitmask[1] = i < 3 ? 0x00000c0c : 0x000c0c1e;
        shape[i]->hor_adjust = shape[i]->ver_adjust = 1;
    }
    me->spatial_hint_pattern = 15;
    me->temporal_hint_pattern = 63;
    me->fbm_op_winner_num_p_frame = t8x8 ? 1 : 4;
    me->fbm_op_winner_num_b_frame_l0 = t8x8 ? 1 : 2;
    me->fbm_op_winner_num_b_frame_l1 = t8x8 ? 1 : 2;
    me->fbm_select_best_cu32_parttype_num = 3;
    me->sps_evaluate_merge_cand_num = 5;
    me->external_hint_order = 1;
    me->coloc_hint_order = 1;
    me->hint_type0 = 0;
    me->hint_type1 = 1;
    me->hint_type2 = 2;
    me->hint_type3 = 3;
    me->hint_type4 = 4;
    if (t8x8) {
        me->me_predictor_mode = 1;
        me->lambda_mode = 1;
        me->Cu16partDecisionMadeByFPP = 1;
        me->fps_quad_thresh_hold = 50;
    }

    md->intra_luma4x4_mode_enable = 0x1ff;
    md->intra_luma8x8_mode_enable = t8x8 ? 0x1ff : 0;
    md->intra_luma16x16_mode_enable = 0xf;
    md->intra_chroma_mode_enable = 0xf;
    md->l0_part_16x16_enable = md->l0_part_16x8_enable = md->l0_part_8x16_enable = md->l0_part_8x8_enable = 1;
    md->l0_part_8x4_enable = md->l0_part_4x8_enable = md->l0_part_4x4_enable = 1;
    md->l1_part_16x16_enable = md->l1_part_16x8_enable = md->l1_part_8x16_enable = md->l1_part_8x8_enable = 1;
    md->bi_part_16x16_enable = md->bi_part_16x8_enable = md->bi_part_8x16_enable = md->bi_part_8x8_enable = 1;
    md->bdirect_mode = 2;
    md->bskip_enable = 1;
    md->pskip_enable = 1;
    md->intra_nxn_bias_multiplier = 24;
    md->intra_most_prob_bias_multiplier = 4;
    md->tempmv_wt_spread_threshold = 0xffff;
    md->tempmv_wt_distort_threshold = 0xffff;
    md->mv_cost_enable = 1;
    md->early_intra_mode_type_16x16dc = md->early_intra_mode_type_16x16h = md->early_intra_mode_type_16x16v = 1;
    md->ip_search_mode = 7;
    md->intra_ssd_cnt_4x4 = t8x8 ? 1 : 9;
    md->intra_ssd_cnt_8x8 = t8x8 ? 2 : 9;
    md->intra_ssd_cnt_16x16 = t8x8 ? 2 : 4;
    md->skip_evaluate_enable = 1;
    md->rdo_level = 1;
    md->tu_search_num = 4;

    static const unsigned short dz_yi[16] = {1023, 878, 820, 682, 878, 820, 682, 512,
                                             820,  682, 512, 410, 682, 512, 410, 410};
    static const unsigned short dz_yp[16] = {682, 586, 546, 456, 586, 546, 456, 342,
                                             546, 456, 342, 292, 456, 342, 292, 274};
    q->qpp_run_vector_4x4 = 1387;
    q->qpp_run_vector_8x8[0] = 43775;
    q->qpp_run_vector_8x8[1] = 65450;
    q->qpp_run_vector_8x8[2] = 15;
    q->qpp_luma8x8_cost = q->qpp_luma16x16_cost = q->qpp_chroma_cost = 15;
    if (!t8x8) {
        q->quant_intra_sat_flag = q->quant_inter_sat_flag = 1;
        q->quant_intra_sat_limit = q->quant_inter_sat_limit = 2063;
    }
    memcpy(q->dz_4x4_YI, dz_yi, sizeof(dz_yi));
    memcpy(q->dz_4x4_YP, dz_yp, sizeof(dz_yp));
    q->dz_4x4_CI = 682;
    q->dz_4x4_CP = 342;
    for (int i = 0; i < 16; i++) {
        q->dz_8x8_YI[i] = 682;
        q->dz_8x8_YP[i] = 342;
    }
}

static unsigned gop_len(const struct h264enc *e) { return e->cfg.gop ? e->cfg.gop : 0xffffffffu; }

static double cpb_bits(const struct h264enc *e) { return e->cfg.bitrate * 2.56; /* as NVIDIA's driver */ }

/* Rate control state that persists over the stream, as NVIDIA's driver starts it. */
static void rc_init(struct h264enc *e) {
    memset(e->rc.ptr, 0, e->rc.size);
    if (!e->cfg.bitrate) return;
    nvenc_persistent_state_s *ps = e->rc.ptr;
    ps->nal_cpb_fullness = ps->vcl_cpb_fullness = (int)(cpb_bits(e) * 0.2 * 16);
    static const int wpbi[3] = {24, 19, 48};
    for (int i = 0; i < 3; i++) {
        ps->Wpbi[i] = wpbi[i];
        for (int t = 0; t < 3; t++) ps->Whist[i][t] = wpbi[i];
    }
    unsigned np = gop_len(e) > 4096 ? 4096 : gop_len(e);
    ps->np = (np - 1) * 16;
    ps->prevQS = 256;
    ps->prevQP = 26;
}

/* --- VIC input conversion --- */

static void vic_matrix(MatrixStruct *m, const struct h264enc_config *c) {
    /* RGB (0..1) -> Y'CbCr. Rows: Y, Cr, Cb; columns R, G, B, offset. With
     * r_shift 8, 1.0 is 1 << 16 and offsets are 10-bit values << 8. */
    double kr = 0.299, kb = 0.114;
    if (c->matrix == 1) { kr = 0.2126; kb = 0.0722; }       /* BT.709 */
    else if (c->matrix == 9) { kr = 0.2627; kb = 0.0593; }  /* BT.2020 */
    double kg = 1 - kr - kb;
    double ys = c->full_range ? 1.0 : 219.0 / 255, cs = c->full_range ? 1.0 : 224.0 / 255;
    double yo = c->full_range ? 0 : 16;
    double k[3][4] = {
            {kr * ys, kg * ys, kb * ys, yo},
            {0.5 * cs, -0.5 * kg / (1 - kr) * cs, -0.5 * kb / (1 - kr) * cs, 128},
            {-0.5 * kr / (1 - kb) * cs, -0.5 * kg / (1 - kb) * cs, 0.5 * cs, 128},
    };
    int v[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) v[i][j] = (int)(k[i][j] * 65536 + (k[i][j] < 0 ? -0.5 : 0.5));
        v[i][3] = (int)((k[i][3] * 4 + 2) * 256); /* + half an 8-bit step: round */
    }
    m->matrix_coeff00 = v[0][0] & 0xfffff; m->matrix_coeff01 = v[0][1] & 0xfffff;
    m->matrix_coeff02 = v[0][2] & 0xfffff; m->matrix_coeff03 = v[0][3] & 0xfffff;
    m->matrix_coeff10 = v[1][0] & 0xfffff; m->matrix_coeff11 = v[1][1] & 0xfffff;
    m->matrix_coeff12 = v[1][2] & 0xfffff; m->matrix_coeff13 = v[1][3] & 0xfffff;
    m->matrix_coeff20 = v[2][0] & 0xfffff; m->matrix_coeff21 = v[2][1] & 0xfffff;
    m->matrix_coeff22 = v[2][2] & 0xfffff; m->matrix_coeff23 = v[2][3] & 0xfffff;
    m->matrix_r_shift = 8;
    m->matrix_enable = 1;
}

static int vic_format(enum h264enc_format f, unsigned *cpp) {
    *cpp = 4;
    switch (f) {
    case H264ENC_RGBA: return VIC_FMT_A8B8G8R8;
    case H264ENC_RGBX: return VIC_FMT_X8B8G8R8;
    case H264ENC_BGRA: return VIC_FMT_A8R8G8B8;
    case H264ENC_BGRX: return VIC_FMT_X8R8G8B8;
    case H264ENC_NV12: *cpp = 1; return VIC_FMT_NV12;
    }
    return -1;
}

static int vic_convert(struct h264enc *e, const struct h264enc_frame *f) {
    const struct h264enc_config *c = &e->cfg;
    unsigned cpp;
    int fmt = vic_format(f->format, &cpp);
    if (!e->vic_ok || f->fd < 0 || fmt < 0) return -ENODEV;
    /* VIC writes nothing for odd sizes; pitch in pixels must cover the width */
    if ((c->width | c->height) & 1 || f->pitch % 64 || f->pitch % cpp || f->pitch / cpp < c->width)
        return -EINVAL;
    if ((size_t)f->pitch * c->height > f->size ||
        (fmt == VIC_FMT_NV12 && (size_t)f->chroma_offset + f->pitch * (c->height / 2) > f->size))
        return -EINVAL;
    struct t_bo src;
    if (t_bo_import(&e->vic, f->fd, f->size, &src)) return -EIO;

    ConfigStruct *cs = e->vic_config.ptr;
    memset(cs, 0, sizeof(*cs));
    cs->outputConfig.TargetRectRight = c->width - 1;
    cs->outputConfig.TargetRectBottom = c->height - 1;
    cs->outputConfig.BackgroundAlpha = 255;
    cs->outputSurfaceConfig.OutPixelFormat = VIC_FMT_NV12;
    cs->outputSurfaceConfig.OutBlkKind = VIC_BLK_KIND_GENERIC_16Bx2;
    cs->outputSurfaceConfig.OutBlkHeight = BLK_LOG2;
    cs->outputSurfaceConfig.OutSurfaceWidth = e->w16 - 1;
    cs->outputSurfaceConfig.OutSurfaceHeight = e->h16 - 1;
    cs->outputSurfaceConfig.OutLumaWidth = e->pitch - 1;
    cs->outputSurfaceConfig.OutLumaHeight = e->h16 - 1;
    cs->outputSurfaceConfig.OutChromaWidth = e->pitch / 2 - 1;
    cs->outputSurfaceConfig.OutChromaHeight = e->h16 / 2 - 1;
    if (fmt != VIC_FMT_NV12) vic_matrix(&cs->outColorMatrixStruct, c);

    SlotConfig *slot = &cs->slotStruct[0].slotConfig;
    slot->SlotEnable = 1;
    slot->CurrentFieldEnable = 1;
    slot->PlanarAlpha = 255;
    slot->ConstantAlpha = 1;
    slot->SourceRectRight = (c->width - 1) << 16;
    slot->SourceRectBottom = (c->height - 1) << 16;
    slot->DestRectRight = c->width - 1;
    slot->DestRectBottom = c->height - 1;
    slot->SoftClampHigh = 1023;
    SlotSurfaceConfig *ss = &cs->slotStruct[0].slotSurfaceConfig;
    ss->SlotPixelFormat = fmt;
    ss->SlotBlkKind = VIC_BLK_KIND_PITCH;
    ss->SlotCacheWidth = VIC_CACHE_WIDTH_64Bx4;
    ss->SlotChromaLocHORIZ = 1;
    ss->SlotChromaLocVert = 1;
    ss->SlotSurfaceWidth = c->width - 1;
    ss->SlotSurfaceHeight = c->height - 1;
    ss->SlotLumaWidth = f->pitch / cpp - 1;
    ss->SlotLumaHeight = c->height - 1;
    if (fmt == VIC_FMT_NV12) {
        ss->SlotChromaWidth = f->pitch / 2 - 1;
        ss->SlotChromaHeight = c->height / 2 - 1;
    }

    struct t_job j;
    t_job_init(&j);
    t_mthd(&j, VIC_APP_ID, 1);
    t_mthd(&j, VIC_SET_CONTROL_PARAMS, (sizeof(ConfigStruct) / 16) << 16);
    t_mthd_addr(&j, VIC_SET_CONFIG_STRUCT_OFFSET, &e->vic_config, 0);
    t_mthd_addr(&j, VIC_SET_FILTER_STRUCT_OFFSET, &e->vic_filter, 0);
    t_mthd_addr(&j, VIC_SET_OUTPUT_LUMA_OFFSET, &e->vic_luma, 0);
    t_mthd_addr(&j, VIC_SET_OUTPUT_CHROMA_U_OFFSET, &e->vic_chroma, 0);
    t_mthd_addr(&j, VIC_SET_SURFACE0_LUMA_OFFSET, &src, 0);
    if (fmt == VIC_FMT_NV12) t_mthd_addr(&j, VIC_SET_SURFACE0_CHROMA_U_OFFSET, &src, f->chroma_offset);
    t_mthd(&j, VIC_EXECUTE, EXECUTE_AWAKEN);
    t_syncpt_incr(&j, &e->vic);
    int err = t_submit_wait(&e->vic, &j, 1000, NULL) ? -ETIMEDOUT : 0;
    t_bo_release_import(&e->vic, &src);
    return err;
}

/* CPU fallback: YUV planes or packed RGB into the block-linear input surface. */
static void cpu_convert(struct h264enc *e, const struct h264enc_frame *f) {
    unsigned w = e->cfg.width, h = e->cfg.height, p = e->pitch;
    uint8_t *dl = e->cur.luma.ptr, *dc = e->cur.chroma.ptr;
    if (f->format == H264ENC_NV12) {
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++) dl[bl_offset(p, x, y)] = f->y[(size_t)y * f->y_stride + x];
        for (unsigned y = 0; y < h / 2; y++)
            for (unsigned x = 0; x < w / 2; x++) {
                size_t s = (size_t)y * f->uv_stride + x * f->uv_step;
                dc[bl_offset(p, 2 * x, y)] = f->u[s];
                dc[bl_offset(p, 2 * x + 1, y)] = f->v[s];
            }
        return;
    }
    /* packed RGB, BT.601/709 integer approximation */
    int bgr = f->format == H264ENC_BGRA || f->format == H264ENC_BGRX;
    int bt709 = e->cfg.matrix == 1, full = e->cfg.full_range;
    int yr = bt709 ? 47 : 66, yg = bt709 ? 157 : 129, yb = bt709 ? 16 : 25;
    int ur = bt709 ? -26 : -38, ug = bt709 ? -87 : -74, ub = 112;
    int vr = 112, vg = bt709 ? -102 : -94, vb = bt709 ? -10 : -18;
    if (full) {
        yr = yr * 255 / 219; yg = yg * 255 / 219; yb = yb * 255 / 219;
        ur = ur * 255 / 224; ug = ug * 255 / 224; ub = ub * 255 / 224;
        vr = vr * 255 / 224; vg = vg * 255 / 224; vb = vb * 255 / 224;
    }
    int yo = full ? 0 : 16;
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = f->y + (size_t)y * f->y_stride;
        for (unsigned x = 0; x < w; x++) {
            const uint8_t *px = row + 4 * x;
            int r = px[bgr ? 2 : 0], g = px[1], b = px[bgr ? 0 : 2];
            dl[bl_offset(p, x, y)] = (uint8_t)(((yr * r + yg * g + yb * b + 128) >> 8) + yo);
            if (!(x & 1) && !(y & 1)) {
                int u = ((ur * r + ug * g + ub * b + 128) >> 8) + 128;
                int v = ((vr * r + vg * g + vb * b + 128) >> 8) + 128;
                dc[bl_offset(p, x, y / 2)] = (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
                dc[bl_offset(p, x + 1, y / 2)] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
    }
}

/* --- encoder --- */

struct h264enc *h264enc_create(const char *node, uint32_t host1x_class, const struct h264enc_config *cfg) {
    if (cfg->width < 128 || cfg->height < 128 || cfg->width > 4096 || cfg->height > 4096 || (cfg->width | cfg->height) & 1)
        return NULL;
    if (cfg->profile_idc != 66 && cfg->profile_idc != 77 && cfg->profile_idc != 100) return NULL;
    struct h264enc *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->cfg = *cfg;
    if (!e->cfg.max_qp) e->cfg.max_qp = 51;
    if (!e->cfg.qp) e->cfg.qp = 28;
    if (e->cfg.fps <= 0) e->cfg.fps = 30;
    e->mbw = (cfg->width + 15) / 16;
    e->mbh = (cfg->height + 15) / 16;
    e->w16 = e->mbw * 16;
    e->h16 = e->mbh * 16;
    e->pitch = ALIGN(e->w16, 64);
    if (t_open(&e->dev, node, host1x_class)) {
        free(e);
        return NULL;
    }
    unsigned mbs = e->mbw * e->mbh;
    int err = t_bo_new(&e->dev, &e->setup, 16384) || pic_new(&e->dev, &e->cur, e->pitch, e->h16) ||
              t_bo_new(&e->dev, &e->status, ALIGN(128 + 80 * (mbs + 3), 4096)) ||
              t_bo_new(&e->dev, &e->bits, h264enc_max_output(e)) ||
              t_bo_new(&e->dev, &e->hist, ALIGN(mbs * 256, 4096)) || t_bo_new(&e->dev, &e->rc, 4096);
    for (int i = 0; i < 2 && !err; i++)
        err = pic_new(&e->dev, &e->ref[i], e->pitch, e->h16) ||
              t_bo_new(&e->dev, &e->coloc[i], ALIGN(mbs * 64, 4096)) ||
              t_bo_new(&e->dev, &e->mepred[i], ALIGN(mbs * 64, 4096));
    if (err) {
        h264enc_destroy(e);
        return NULL;
    }
    rc_init(e);
    if (!t_channel_open(&e->dev, &e->vic, VIC_CLASS)) {
        e->vic_ok = !t_bo_new(&e->vic, &e->vic_config, sizeof(ConfigStruct)) &&
                    !t_bo_new(&e->vic, &e->vic_filter, 16384) &&
                    !t_bo_map_into(&e->vic, &e->cur.luma, &e->vic_luma) &&
                    !t_bo_map_into(&e->vic, &e->cur.chroma, &e->vic_chroma);
    }
    return e;
}

void h264enc_destroy(struct h264enc *e) {
    if (!e) return;
    if (e->vic.fd > 0) {
        t_bo_unmap_from(&e->vic, &e->vic_luma);
        t_bo_unmap_from(&e->vic, &e->vic_chroma);
        t_bo_free(&e->vic, &e->vic_config);
        t_bo_free(&e->vic, &e->vic_filter);
        t_close(&e->vic);
    }
    pic_free(&e->dev, &e->cur);
    for (int i = 0; i < 2; i++) {
        pic_free(&e->dev, &e->ref[i]);
        t_bo_free(&e->dev, &e->coloc[i]);
        t_bo_free(&e->dev, &e->mepred[i]);
    }
    t_bo_free(&e->dev, &e->setup);
    t_bo_free(&e->dev, &e->status);
    t_bo_free(&e->dev, &e->bits);
    t_bo_free(&e->dev, &e->hist);
    t_bo_free(&e->dev, &e->rc);
    t_close(&e->dev);
    free(e);
}

size_t h264enc_max_output(const struct h264enc *e) {
    /* a worst-case I picture at low QP is around 1.5 bytes per pixel */
    return ALIGN((size_t)e->w16 * e->h16 * 3 / 2 + 65536, 4096);
}

void h264enc_set_bitrate(struct h264enc *e, unsigned bitrate) {
    if (!e->cfg.bitrate || !bitrate) return; /* the mode is fixed at creation */
    e->cfg.bitrate = bitrate;
}

int h264enc_encode(struct h264enc *e, const struct h264enc_frame *f, int force_idr, uint8_t *out, size_t cap,
                   int *idr_out) {
    const struct h264enc_config *c = &e->cfg;
    if (vic_convert(e, f)) {
        if (!f->y) return -EINVAL;
        cpu_convert(e, f);
    }

    int idr = force_idr || !e->have_ref || (c->gop && e->gop_pos >= c->gop);
    if (idr) {
        if (e->have_ref) e->idr_id++;
        e->gop_pos = 0;
    }
    unsigned n = e->gop_pos, o = e->last ^ 1, prev = e->last;
    unsigned mbs = e->mbw * e->mbh;

    memset(e->setup.ptr, 0, 8192);
    nvenc_h264_drv_pic_setup_s *s = e->setup.ptr;
    const unsigned off_me = 512, off_md = 768, off_slice = 1024, off_q = 5120;
    uint8_t *base = e->setup.ptr;
    nvenc_h264_slice_control_s *sl = (void *)(base + off_slice);
    int cabac = c->profile_idc != 66, t8x8 = c->profile_idc == 100;
    controls((void *)(base + off_me), (void *)(base + off_md), (void *)(base + off_q), t8x8);

    s->magic = MAGIC;
    s->input_cfg.frame_width_minus1 = e->w16 - 1;
    s->input_cfg.frame_height_minus1 = e->h16 - 1;
    s->input_cfg.sfc_pitch = e->pitch;
    s->input_cfg.sfc_pitch_chroma = e->pitch;
    s->input_cfg.block_height = BLK_FIELD;
    s->refpic_cfg = s->outputpic_cfg = s->input_cfg;

    s->sps_data.profile_idc = c->profile_idc;
    s->sps_data.level_idc = c->level_idc;
    s->sps_data.chroma_format_idc = 1;
    s->sps_data.log2_max_frame_num_minus4 = 4;
    s->sps_data.log2_max_pic_order_cnt_lsb_minus4 = 4;
    s->sps_data.frame_mbs_only = 1;
    s->pps_data.entropy_coding_mode_flag = cabac;
    s->pps_data.transform_8x8_mode_flag = t8x8;
    s->pps_data.deblocking_filter_control_present_flag = 1;

    nvenc_h264_rc_s *r = &s->rate_control;
    unsigned gop = gop_len(e);
    r->hrd_type = 2;
    r->maxQPD = 6;
    r->baseQPD = 3;
    r->rhopbi[0] = 286; r->rhopbi[1] = 307; r->rhopbi[2] = 256;
    r->framerate = (int)(c->fps * 256);
    r->gop_length = gop;
    r->Np = (gop > 4096 ? 4096 : gop) - 1;
    r->iSizeRatioX = 2;
    r->iSizeRatioY = 1;
    for (int i = 0; i < 3; i++) {
        r->minQP[i] = c->min_qp;
        r->maxQP[i] = c->max_qp;
    }
    int qp_i = c->qp, qp_p = c->qp;
    if (c->bitrate) {
        double pixels = (double)e->w16 * e->h16, cpb = cpb_bits(e);
        r->QP[0] = 28; r->QP[1] = 30; r->QP[2] = 25;
        r->buffersize = r->nal_cpb_size = (unsigned)cpb;
        r->nal_bitrate = c->bitrate;
        r->R = (int)(c->bitrate / c->fps / pixels * 256);
        r->Bmin = (int)(cpb / pixels * 256 / 5);
        qp_i = 25;
        qp_p = 28;
    } else {
        r->QP[0] = r->QP[1] = r->QP[2] = c->qp;
    }

    nvenc_h264_pic_control_s *pc = &s->pic_control;
    memset(pc->l0, 0xfe, sizeof(pc->l0));
    memset(pc->l1, 0xfe, sizeof(pc->l1));
    memset(pc->temp_dist_l0, 0xff, sizeof(pc->temp_dist_l0));
    memset(pc->temp_dist_l1, 0xff, sizeof(pc->temp_dist_l1));
    memset(pc->dist_scale_factor, 0xff, sizeof(pc->dist_scale_factor));
    if (!idr) {
        pc->l0[0] = 0; /* reference slot 0 */
        pc->temp_dist_l0[0] = 1;
        for (int i = 0; i < 8; i++) pc->dist_scale_factor[i][0] = 256;
    }
    pc->max_slice_size = mbs;
    pc->frame_num = n & 0xff;
    pc->pic_order_cnt_lsb = (2 * n) & 0xff;
    pc->idr_pic_id = e->idr_id & 0xffff;
    pc->slice_control_offset = off_slice;
    pc->me_control_offset = off_me;
    pc->md_control_offset = off_md;
    pc->q_control_offset = off_q;
    pc->hist_buf_size = e->hist.size;
    pc->bitstream_buf_size = e->bits.size;
    pc->pic_type = idr ? 3 : 0;
    pc->ref_pic_flag = 1;
    pc->cabac_zero_word_enable = cabac;
    pc->slice_stat_offset = 128;
    pc->mpec_stat_offset = 128 + 16 * (mbs + 3);

    sl->num_mb = mbs;
    sl->qp_avr = idr ? qp_i : qp_p;
    sl->qp_slice_min = 1;
    sl->qp_slice_max = 51;

    int rcmode = !c->bitrate ? RCMODE_CONSTQP : c->vbr ? RCMODE_VBR_MBROW : RCMODE_CBR_MBROW;
    nvenc_pic_stat_s *st = e->status.ptr;
    memset(st, 0, sizeof(*st));

    struct t_job j;
    t_job_init(&j);
    t_mthd(&j, M_CONTROL_PARAMS, CTRL_H264 | CTRL_FORCE_OUT_PIC | CTRL_SLICE_STAT_ON | CTRL_RCMODE(rcmode));
    t_mthd(&j, M_PICTURE_INDEX, e->frame);
    t_mthd(&j, M_APP_ID, 1);
    t_mthd_addr(&j, M_IN_DRV_PIC_SETUP, &e->setup, 0);
    t_mthd_addr(&j, M_OUT_ENC_STATUS, &e->status, 0);
    t_mthd_addr(&j, M_OUT_BITSTREAM, &e->bits, 0);
    t_mthd_addr(&j, M_IOHISTORY, &e->hist, 0);
    t_mthd_addr(&j, M_IO_RC_PROCESS, &e->rc, 0);
    t_mthd_addr(&j, M_IN_CUR_PIC, &e->cur.luma, 0);
    t_mthd_addr(&j, M_IN_CUR_CHROMA_U, &e->cur.chroma, 0);
    t_mthd_addr(&j, M_IN_CUR_CHROMA_V, &e->cur.chroma, 0);
    if (!idr) {
        t_mthd_addr(&j, M_IN_MEPRED, &e->mepred[prev], 0);
        t_mthd_addr(&j, M_OUT_MEPRED, &e->mepred[o], 0);
        t_mthd_addr(&j, M_IN_REF_LUMA(0), &e->ref[prev].luma, 0);
        t_mthd_addr(&j, M_IN_REF_CHROMA(0), &e->ref[prev].chroma, 0);
        t_mthd_addr(&j, M_IN_COLOC, &e->coloc[prev], 0);
    }
    t_mthd_addr(&j, M_OUT_COLOC, &e->coloc[o], 0);
    t_mthd_addr(&j, M_OUT_REF_LUMA, &e->ref[o].luma, 0);
    t_mthd_addr(&j, M_OUT_REF_CHROMA, &e->ref[o].chroma, 0);
    t_mthd(&j, M_EXECUTE, EXECUTE_AWAKEN);
    t_syncpt_incr(&j, &e->dev);
    if (t_submit_wait(&e->dev, &j, 1000, NULL)) {
        e->have_ref = 0; /* start over with an IDR */
        return -ETIMEDOUT;
    }

    size_t bytes = st->total_bit_count / 8;
    if (st->error_status != ENC_STATUS_DONE || !bytes || bytes > e->bits.size) {
        e->have_ref = 0;
        return -EIO;
    }
    if (bytes > cap) return -ENOSPC;
    memcpy(out, e->bits.ptr, bytes);
    e->frame++;
    e->gop_pos++;
    e->last = o;
    e->have_ref = 1;
    if (idr_out) *idr_out = idr;
    return (int)bytes;
}
