/*
 * Pieces shared by the NVDEC decoders (h264dec, hevcdec): the decoded picture
 * description, VIC conversion of block-linear surfaces into pitch-linear NV12
 * dma-bufs, and a CPU detiler for when VIC is unavailable.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "host1x.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVDEC_NSURF 17 /* picture slots the engine addresses */

struct nvdec_picture {
    /* NV12 in Tegra block linear (TBL, 64x8 GOBs, blocks of 2 GOBs) */
    const uint8_t *luma, *chroma;
    unsigned pitch;            /* bytes, both planes */
    unsigned coded_width, coded_height;
    unsigned crop_left, crop_top, width, height; /* visible rectangle */
    int poc, period;           /* display order: by period, then poc */
    uint64_t timestamp;        /* as passed to the decode call */
    unsigned mbs_in_error;
    int surface;               /* engine surface, for the vic_copy calls */
};

/* Return non-zero to abort decoding. */
typedef int (*nvdec_picture_cb)(void *opaque, const struct nvdec_picture *pic);

struct nvdec_vic {
    struct t_dev ch;
    int ok;
    struct t_bo config, filter, surf[NVDEC_NSURF];
    unsigned chroma_offset[NVDEC_NSURF]; /* of the mapped decoder surfaces */
};

/* Open VIC on the decoder's DRM fd; failure leaves vic->ok == 0. */
void nvdec_vic_open(struct t_dev *dev, struct nvdec_vic *vic);
void nvdec_vic_close(struct nvdec_vic *vic);
/* Map the decoder surfaces (chroma plane at chroma_offset) into VIC. */
int nvdec_vic_map(struct nvdec_vic *vic, const struct t_bo *surf, unsigned chroma_offset);
void nvdec_vic_unmap(struct nvdec_vic *vic);
/* The same for one surface, for decoders whose surfaces differ in size (VP9). */
int nvdec_vic_map_surface(struct nvdec_vic *vic, int i, const struct t_bo *surf, unsigned chroma_offset);
void nvdec_vic_unmap_surface(struct nvdec_vic *vic, int i);
/*
 * Convert the visible part of a picture into a pitch-linear NV12 dma-buf: luma
 * at offset 0, interleaved chroma at chroma_offset. pitch and chroma_offset
 * must be multiples of 256. Returns 0, or a negative errno.
 */
int nvdec_vic_copy(struct nvdec_vic *vic, const struct nvdec_picture *pic, int dmabuf_fd,
                   size_t dmabuf_size, unsigned pitch, unsigned chroma_offset);

/* Copy the visible part of a picture to planar or semi-planar 4:2:0.
 * u/v may point into one interleaved plane (uv_step 2) or two planes (uv_step 1). */
void nvdec_copy_out(const struct nvdec_picture *pic, uint8_t *y, int y_stride,
                    uint8_t *u, uint8_t *v, int uv_stride, int uv_step);

#ifdef __cplusplus
}
#endif
