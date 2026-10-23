/* VP9 probability context, laid out like libvpx's FRAME_CONTEXT so its default tables apply as-is. */
#pragma once

#include <stdint.h>

struct vp9_tx_probs {
    uint8_t p32x32[2][3];
    uint8_t p16x16[2][2];
    uint8_t p8x8[2][1];
};

struct vp9_mv_comp_probs {
    uint8_t sign;
    uint8_t classes[10];
    uint8_t class0[1];
    uint8_t bits[10];
    uint8_t class0_fp[2][3];
    uint8_t fp[3];
    uint8_t class0_hp;
    uint8_t hp;
};

struct vp9_mv_probs {
    uint8_t joints[3];
    struct vp9_mv_comp_probs comps[2]; /* 0: vertical, 1: horizontal */
};

struct vp9_frame_ctx {
    uint8_t y_mode[4][9];
    uint8_t uv_mode[10][9];
    uint8_t partition[16][3];
    uint8_t coef[4][2][2][6][6][3]; /* [tx size][plane][ref][band][context][node] */
    uint8_t interp[4][2];
    uint8_t inter_mode[7][3];
    uint8_t intra_inter[4];
    uint8_t comp_inter[5];
    uint8_t single_ref[5][2];
    uint8_t comp_ref[5];
    struct vp9_tx_probs tx;
    uint8_t skip[3];
    struct vp9_mv_probs mv;
};
