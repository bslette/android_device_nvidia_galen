/*
 * VIC conversion and CPU detiling of NVDEC output surfaces (see nvdec_common.h).
 */
#include "nvdec_common.h"
#include "vic42.h"

#include <errno.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define VIC_APP_ID                       0x200
#define VIC_EXECUTE                      0x300
#define VIC_SET_CONTROL_PARAMS           0x704
#define VIC_SET_CONFIG_STRUCT_OFFSET     0x708
#define VIC_SET_FILTER_STRUCT_OFFSET     0x70c
#define VIC_SET_OUTPUT_LUMA_OFFSET       0x720
#define VIC_SET_OUTPUT_CHROMA_U_OFFSET   0x724
#define VIC_SET_SURFACE0_LUMA_OFFSET     0x1200
#define VIC_SET_SURFACE0_CHROMA_U_OFFSET 0x1204
#define VIC_PIXEL_FORMAT_Y8_U8V8_N420    67 /* NV12 */
#define VIC_BLK_KIND_PITCH               0
#define VIC_BLK_KIND_GENERIC_16Bx2       1
#define VIC_CACHE_WIDTH_32Bx8            1
#define HOST1X_CLASS_VIC                 0x5d
#define EXECUTE_AWAKEN                   (1u << 8)

void nvdec_vic_open(struct t_dev *dev, struct nvdec_vic *vic) {
    memset(vic, 0, sizeof(*vic));
    if (t_channel_open(dev, &vic->ch, HOST1X_CLASS_VIC)) return;
    if (!t_bo_new(&vic->ch, &vic->config, sizeof(ConfigStruct)) && !t_bo_new(&vic->ch, &vic->filter, 16384))
        vic->ok = 1;
}

void nvdec_vic_close(struct nvdec_vic *vic) {
    if (vic->ch.fd <= 0) return;
    nvdec_vic_unmap(vic);
    t_bo_free(&vic->ch, &vic->config);
    t_bo_free(&vic->ch, &vic->filter);
    t_close(&vic->ch);
    vic->ok = 0;
}

int nvdec_vic_map_surface(struct nvdec_vic *vic, int i, const struct t_bo *surf, unsigned chroma_offset) {
    if (!vic->ok) return 0;
    t_bo_unmap_from(&vic->ch, &vic->surf[i]);
    vic->chroma_offset[i] = chroma_offset;
    return t_bo_map_into(&vic->ch, surf, &vic->surf[i]) ? -ENOMEM : 0;
}

void nvdec_vic_unmap_surface(struct nvdec_vic *vic, int i) {
    if (vic->ok) t_bo_unmap_from(&vic->ch, &vic->surf[i]);
}

int nvdec_vic_map(struct nvdec_vic *vic, const struct t_bo *surf, unsigned chroma_offset) {
    for (int i = 0; i < NVDEC_NSURF; i++)
        if (nvdec_vic_map_surface(vic, i, &surf[i], chroma_offset)) return -ENOMEM;
    return 0;
}

void nvdec_vic_unmap(struct nvdec_vic *vic) {
    for (int i = 0; i < NVDEC_NSURF; i++) nvdec_vic_unmap_surface(vic, i);
}

int nvdec_vic_copy(struct nvdec_vic *vic, const struct nvdec_picture *pic, int dmabuf_fd,
                   size_t dmabuf_size, unsigned pitch, unsigned chroma_offset) {
    if (!vic->ok) return -ENODEV;
    /* VIC completes NV12 jobs with odd target sizes (VP9 allows them) without writing anything */
    if ((pic->width | pic->height) & 1) return -EINVAL;
    if (pitch % 256 || chroma_offset % 256 || chroma_offset < pitch * pic->height ||
        (size_t)chroma_offset + pitch * ((pic->height + 1) / 2) > dmabuf_size)
        return -EINVAL;
    struct t_bo dst;
    if (t_bo_import(&vic->ch, dmabuf_fd, dmabuf_size, &dst)) return -EIO;

    ConfigStruct *c = vic->config.ptr;
    memset(c, 0, sizeof(*c));
    c->outputConfig.TargetRectRight = pic->width - 1;
    c->outputConfig.TargetRectBottom = pic->height - 1;
    c->outputConfig.BackgroundAlpha = 255;
    c->outputSurfaceConfig.OutPixelFormat = VIC_PIXEL_FORMAT_Y8_U8V8_N420;
    c->outputSurfaceConfig.OutBlkKind = VIC_BLK_KIND_PITCH;
    c->outputSurfaceConfig.OutSurfaceWidth = pic->width - 1;
    c->outputSurfaceConfig.OutSurfaceHeight = pic->height - 1;
    c->outputSurfaceConfig.OutLumaWidth = pitch - 1;
    c->outputSurfaceConfig.OutLumaHeight = pic->height - 1;
    c->outputSurfaceConfig.OutChromaWidth = pitch / 2 - 1;
    c->outputSurfaceConfig.OutChromaHeight = (pic->height + 1) / 2 - 1; /* odd heights (VP9) */

    SlotConfig *slot = &c->slotStruct[0].slotConfig;
    slot->SlotEnable = 1;
    slot->CurrentFieldEnable = 1;
    slot->PlanarAlpha = 255;
    slot->ConstantAlpha = 1;
    slot->SourceRectLeft = pic->crop_left << 16;
    slot->SourceRectRight = (pic->crop_left + pic->width - 1) << 16;
    slot->SourceRectTop = pic->crop_top << 16;
    slot->SourceRectBottom = (pic->crop_top + pic->height - 1) << 16;
    slot->DestRectRight = pic->width - 1;
    slot->DestRectBottom = pic->height - 1;
    slot->SoftClampHigh = 1023;

    SlotSurfaceConfig *surf = &c->slotStruct[0].slotSurfaceConfig;
    surf->SlotPixelFormat = VIC_PIXEL_FORMAT_Y8_U8V8_N420;
    surf->SlotChromaLocHORIZ = 1;
    surf->SlotChromaLocVert = 1;
    surf->SlotBlkKind = VIC_BLK_KIND_GENERIC_16Bx2;
    surf->SlotBlkHeight = 1; /* blocks of two GOBs, as NVDEC wrote them */
    surf->SlotCacheWidth = VIC_CACHE_WIDTH_32Bx8;
    surf->SlotSurfaceWidth = pic->coded_width - 1;
    surf->SlotSurfaceHeight = pic->coded_height - 1;
    surf->SlotLumaWidth = pic->pitch - 1;
    surf->SlotLumaHeight = pic->coded_height - 1;
    surf->SlotChromaWidth = pic->pitch / 2 - 1;
    surf->SlotChromaHeight = (pic->coded_height + 1) / 2 - 1;

    struct t_job j;
    t_job_init(&j);
    t_mthd(&j, VIC_APP_ID, 1);
    t_mthd(&j, VIC_SET_CONTROL_PARAMS, (sizeof(ConfigStruct) / 16) << 16);
    t_mthd_addr(&j, VIC_SET_CONFIG_STRUCT_OFFSET, &vic->config, 0);
    t_mthd_addr(&j, VIC_SET_FILTER_STRUCT_OFFSET, &vic->filter, 0);
    t_mthd_addr(&j, VIC_SET_OUTPUT_LUMA_OFFSET, &dst, 0);
    t_mthd_addr(&j, VIC_SET_OUTPUT_CHROMA_U_OFFSET, &dst, chroma_offset);
    t_mthd_addr(&j, VIC_SET_SURFACE0_LUMA_OFFSET, &vic->surf[pic->surface], 0);
    t_mthd_addr(&j, VIC_SET_SURFACE0_CHROMA_U_OFFSET, &vic->surf[pic->surface], vic->chroma_offset[pic->surface]);
    t_mthd(&j, VIC_EXECUTE, EXECUTE_AWAKEN);
    t_syncpt_incr(&j, &vic->ch);
    int err = t_submit_wait(&vic->ch, &j, 1000, NULL) ? -ETIMEDOUT : 0;
    t_bo_release_import(&vic->ch, &dst);
    return err;
}

/* TBL: GOB of 64 B x 8 rows in 16Bx2 sectors, blocks of 2 GOBs stacked vertically. */
static inline size_t tbl_offset(unsigned pitch, unsigned x, unsigned y) {
    size_t block = (size_t)(y / 16) * (pitch / 64) + x / 64;
    return block * 1024 + ((y % 16) / 8) * 512 + ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 +
           ((x % 32) / 16) * 32 + (y % 2) * 16;
}

void nvdec_copy_out(const struct nvdec_picture *pic, uint8_t *y, int y_stride,
                    uint8_t *u, uint8_t *v, int uv_stride, int uv_step) {
    unsigned x0 = pic->crop_left, y0 = pic->crop_top, w = pic->width, h = pic->height;
    for (unsigned row = 0; row < h; row++) {
        uint8_t *dst = y + (size_t)row * y_stride;
        unsigned sy = y0 + row, x = 0;
        while (x < w) { /* 16-byte sector runs */
            unsigned sx = x0 + x, n = 16 - (sx % 16);
            if (n > w - x) n = w - x;
            memcpy(dst + x, pic->luma + tbl_offset(pic->pitch, sx & ~15u, sy) + (sx % 16), n);
            x += n;
        }
    }
    for (unsigned row = 0; row < (h + 1) / 2; row++) {
        uint8_t *du = u + (size_t)row * uv_stride, *dv = v + (size_t)row * uv_stride;
        unsigned sy = y0 / 2 + row, x = 0;
#if defined(__aarch64__)
        /* whole 16-byte sectors (8 UV pairs) when the crop is sector aligned */
        if (x0 % 16 == 0)
            for (; x + 8 <= (w + 1) / 2; x += 8) {
                uint8x8x2_t uv = vld2_u8(pic->chroma + tbl_offset(pic->pitch, x0 + 2 * x, sy));
                if (uv_step == 1) {
                    vst1_u8(du + x, uv.val[0]);
                    vst1_u8(dv + x, uv.val[1]);
                } else {
                    for (int k = 0; k < 8; k++) {
                        du[(x + k) * uv_step] = vget_lane_u8(uv.val[0], 0);
                        dv[(x + k) * uv_step] = vget_lane_u8(uv.val[1], 0);
                        uv.val[0] = vext_u8(uv.val[0], uv.val[0], 1);
                        uv.val[1] = vext_u8(uv.val[1], uv.val[1], 1);
                    }
                }
            }
#endif
        for (; x < (w + 1) / 2; x++) {
            unsigned sx = x0 + 2 * x; /* byte offset of the U sample in the NV12 row */
            const uint8_t *src = pic->chroma + tbl_offset(pic->pitch, sx & ~15u, sy) + (sx % 16);
            du[x * uv_step] = src[0];
            dv[x * uv_step] = src[1];
        }
    }
}
