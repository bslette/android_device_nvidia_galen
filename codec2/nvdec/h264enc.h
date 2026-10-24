/*
 * H.264 encoder on the Tegra194 NVENC engine (upstream tegra-drm channel UAPI).
 * Input pictures go through VIC (RGB or NV12, pitch linear, from a dma-buf) into
 * the block-linear NV12 surface NVENC reads; I and P pictures only, one reference.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct h264enc;

struct h264enc_config {
    unsigned width, height;     /* even, 128..4096 x 128..4096 */
    int profile_idc;            /* 66 constrained Baseline, 77 Main (CABAC), 100 High (+ 8x8 transform) */
    int level_idc;              /* e.g. 41 */
    double fps;
    unsigned bitrate;           /* bit/s; 0: constant QP */
    int vbr;                    /* with bitrate: VBR instead of CBR */
    int qp;                     /* constant QP mode (I and P) */
    int min_qp, max_qp;         /* rate control limits, 0/0: 0..51 */
    unsigned gop;               /* IDR interval in frames, 0: only the first picture */
    /* VUI colour description (ISO/IEC 23091-2 values), and the RGB -> YUV matrix */
    int full_range, primaries, transfer, matrix;
};

enum h264enc_format {
    H264ENC_NV12,
    H264ENC_RGBA, /* bytes R, G, B, A */
    H264ENC_RGBX,
    H264ENC_BGRA,
    H264ENC_BGRX,
};

/* One input picture: a dma-buf (VIC path) and/or CPU pointers (fallback path). */
struct h264enc_frame {
    enum h264enc_format format;
    int fd;                     /* -1: no dma-buf */
    size_t size;                /* of the dma-buf */
    unsigned pitch;             /* bytes, luma / packed RGB */
    unsigned chroma_offset;     /* NV12: offset of the interleaved UV plane in the dma-buf */
    /* CPU view: Y/U/V planes (uv_step 1 planar, 2 interleaved) or packed RGB in y */
    const uint8_t *y, *u, *v;
    int y_stride, uv_stride, uv_step;
};

struct h264enc *h264enc_create(const char *node, uint32_t host1x_class, const struct h264enc_config *cfg);
void h264enc_destroy(struct h264enc *enc);

/* SPS + PPS, Annex B. Returns the length, or a negative errno. */
int h264enc_headers(struct h264enc *enc, uint8_t *buf, size_t cap);

/* Change the bit rate (rate-controlled modes) from the next picture on. */
void h264enc_set_bitrate(struct h264enc *enc, unsigned bitrate);

/*
 * Encode one picture into out (Annex B slice NAL units). force_idr starts a new
 * GOP. *idr is set when the picture is an IDR. Returns the length or a negative
 * errno (-ENOSPC: out too small).
 */
int h264enc_encode(struct h264enc *enc, const struct h264enc_frame *frame, int force_idr, uint8_t *out,
                   size_t cap, int *idr);

/* Largest output of one picture. */
size_t h264enc_max_output(const struct h264enc *enc);

#ifdef __cplusplus
}
#endif
