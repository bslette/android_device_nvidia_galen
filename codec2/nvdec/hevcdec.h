/*
 * H.265 decoding on the Tegra NVDEC engine through the upstream tegra-drm
 * channel UAPI. Main profile (8-bit 4:2:0). Pictures come back in decode order
 * through a callback, with their POC and IRAP period so the caller can reorder
 * them for display; pictures with pic_output_flag 0 and skipped RASL pictures
 * are not returned.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nvdec_common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct hevcdec;

/* node: tegra-drm render node; host1x_class: 0xf0 (NVDEC0) or 0xf5 (NVDEC1) */
struct hevcdec *hevcdec_create(const char *node, uint32_t host1x_class,
                               nvdec_picture_cb cb, void *opaque);
void hevcdec_destroy(struct hevcdec *dec);

/*
 * Decode Annex B data: parameter sets and/or whole pictures (an access unit
 * per call, as MediaCodec delivers them). Returns 0, or a negative errno.
 */
int hevcdec_decode(struct hevcdec *dec, const uint8_t *data, size_t len, uint64_t timestamp);

/* Frames the stream needs buffered for display reordering
 * (sps_max_num_reorder_pics); -1 before the first SPS. */
int hevcdec_reorder_depth(const struct hevcdec *dec);

/* Forget references and POC state (flush/seek); decoding restarts at the next IRAP picture. */
void hevcdec_reset(struct hevcdec *dec);

/* nvdec_vic_copy() for this decoder's surfaces; only valid inside the picture callback. */
int hevcdec_vic_copy(struct hevcdec *dec, const struct nvdec_picture *pic, int dmabuf_fd,
                     size_t dmabuf_size, unsigned pitch, unsigned chroma_offset);

#ifdef __cplusplus
}
#endif
