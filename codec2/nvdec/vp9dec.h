/*
 * VP9 decoding on the Tegra NVDEC engine through the upstream tegra-drm
 * channel UAPI. Profile 0 (8-bit 4:2:0). VP9 has no display reordering:
 * pictures come back through the callback in output order, including
 * show_existing_frame repeats; hidden frames are decoded but not returned.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nvdec_common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vp9dec;

/* node: tegra-drm render node; host1x_class: 0xf0 (NVDEC0) or 0xf5 (NVDEC1) */
struct vp9dec *vp9dec_create(const char *node, uint32_t host1x_class,
                             nvdec_picture_cb cb, void *opaque);
void vp9dec_destroy(struct vp9dec *dec);

/*
 * Decode one compressed VP9 frame or superframe (a WebM/IVF packet, as
 * MediaCodec delivers them). Returns 0, or a negative errno; -ENOTSUP for
 * profiles other than 0.
 */
int vp9dec_decode(struct vp9dec *dec, const uint8_t *data, size_t len, uint64_t timestamp);

/* Forget references (flush/seek); decoding restarts at the next key frame. */
void vp9dec_reset(struct vp9dec *dec);

/* nvdec_vic_copy() for this decoder's surfaces; only valid inside the picture callback. */
int vp9dec_vic_copy(struct vp9dec *dec, const struct nvdec_picture *pic, int dmabuf_fd,
                    size_t dmabuf_size, unsigned pitch, unsigned chroma_offset);

#ifdef __cplusplus
}
#endif
