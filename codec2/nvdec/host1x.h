/* Minimal Tegra DRM channel (host1x) helpers for engines driven through the THI method interface. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>

#define HOST1X_CLASS_NVDEC  0xf0
#define HOST1X_CLASS_NVDEC1 0xf5

#define HOST1X_OPCODE_INCR(offset, count)    ((0x1u << 28) | (((offset) & 0xfff) << 16) | ((count) & 0xffff))
#define HOST1X_OPCODE_NONINCR(offset, count) ((0x2u << 28) | (((offset) & 0xfff) << 16) | ((count) & 0xffff))
#define THI_METHOD0 0x10            /* method offset >> 2, then data to METHOD1 */
#define SYNC_COND_OP_DONE 1

struct t_bo {
    uint32_t handle, mapping;
    size_t size;
    void *ptr;
};

struct t_dev {
    int fd;
    uint32_t context, version, syncpt;
    int shared_fd; /* opened with t_channel_open: fd belongs to another t_dev */
};

struct t_job {
    uint32_t words[4096];
    unsigned n;
    struct { uint32_t mapping, gather_off; uint64_t target_off; uint32_t shift; } relocs[64];
    unsigned nrelocs;
    unsigned incrs;
};

int t_open(struct t_dev *d, const char *node, uint32_t host1x_class);
int t_bo_new(struct t_dev *d, struct t_bo *bo, size_t size);
void t_bo_free(struct t_dev *d, struct t_bo *bo);
/* another channel on the same DRM fd (GEM handles are shared) */
int t_channel_open(struct t_dev *base, struct t_dev *ch, uint32_t host1x_class);
/* map an existing bo of the same fd into another channel: dst gets the new mapping */
int t_bo_map_into(struct t_dev *ch, const struct t_bo *src, struct t_bo *dst);
void t_bo_unmap_from(struct t_dev *ch, struct t_bo *bo);
/* import a dma-buf into a channel (no CPU mapping) */
int t_bo_import(struct t_dev *ch, int dmabuf_fd, size_t size, struct t_bo *bo);
void t_bo_release_import(struct t_dev *ch, struct t_bo *bo);
void t_close(struct t_dev *d);
void t_job_init(struct t_job *j);
void t_mthd(struct t_job *j, uint32_t method, uint32_t value);
void t_mthd_addr(struct t_job *j, uint32_t method, struct t_bo *bo, uint64_t offset); /* value = iova >> 8 */
void t_mthd_reloc(struct t_job *j, uint32_t method, struct t_bo *bo, uint64_t offset, unsigned shift); /* value = iova >> shift */
void t_syncpt_incr(struct t_job *j, struct t_dev *d);
int t_submit_wait(struct t_dev *d, struct t_job *j, int64_t timeout_ms, uint32_t *fence);

#ifdef __cplusplus
}
#endif
