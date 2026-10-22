/*
 * H.264 decoding on the Tegra NVDEC engine through the upstream tegra-drm
 * channel UAPI. Progressive 8-bit 4:2:0 streams, POC types 0 and 2, no
 * scaling matrices. Pictures come back in decode order through a callback,
 * with their POC and IDR period so the caller can reorder them for display.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct h264dec;

struct h264dec_picture {
    /* NV12 in Tegra block linear (TBL, 64x8 GOBs, blocks of 2 GOBs) */
    const uint8_t *luma, *chroma;
    unsigned pitch;            /* bytes, both planes */
    unsigned coded_width, coded_height;
    unsigned crop_left, crop_top, width, height; /* visible rectangle */
    int poc, period;           /* display order: by period, then poc */
    uint64_t timestamp;        /* as passed to h264dec_decode() */
    unsigned mbs_in_error;
    int surface;               /* engine surface, for h264dec_vic_copy() */
};

/* Return non-zero to abort decoding. */
typedef int (*h264dec_picture_cb)(void *opaque, const struct h264dec_picture *pic);

/* node: tegra-drm render node; host1x_class: 0xf0 (NVDEC0) or 0xf5 (NVDEC1) */
struct h264dec *h264dec_create(const char *node, uint32_t host1x_class,
                               h264dec_picture_cb cb, void *opaque);
void h264dec_destroy(struct h264dec *dec);

/*
 * Decode Annex B data: parameter sets and/or whole pictures (an access unit
 * per call, as MediaCodec delivers them). Returns 0, or a negative errno.
 */
int h264dec_decode(struct h264dec *dec, const uint8_t *data, size_t len, uint64_t timestamp);

/* Frames the stream needs buffered for display reordering (VUI
 * max_num_reorder_frames, or max_num_ref_frames without VUI); -1 before the
 * first SPS. */
int h264dec_reorder_depth(const struct h264dec *dec);

/* Forget references and POC state (flush/seek); the next picture must be an IDR. */
void h264dec_reset(struct h264dec *dec);

/*
 * Convert the visible part of a picture with the VIC engine into a pitch-linear
 * NV12 dma-buf: luma at offset 0, interleaved chroma at chroma_offset. Only
 * valid inside the picture callback. pitch and chroma_offset must be multiples
 * of 256. Returns 0, or a negative errno (use h264dec_copy_out instead).
 */
int h264dec_vic_copy(struct h264dec *dec, const struct h264dec_picture *pic, int dmabuf_fd,
                     size_t dmabuf_size, unsigned pitch, unsigned chroma_offset);

/* Copy the visible part of a picture to planar or semi-planar 4:2:0.
 * u/v may point into one interleaved plane (uv_step 2) or two planes (uv_step 1). */
void h264dec_copy_out(const struct h264dec_picture *pic,
                      uint8_t *y, int y_stride,
                      uint8_t *u, uint8_t *v, int uv_stride, int uv_step);

#ifdef __cplusplus
}
#endif
