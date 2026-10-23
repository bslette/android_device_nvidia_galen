/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <SimpleC2Component.h>
#include <util/C2InterfaceHelper.h>

#include <vector>

#include "h264dec.h"
#include "hevcdec.h"
#include "vp9dec.h"

namespace android {

/*
 * H.264, H.265 and VP9 decoders on the Tegra NVDEC engine (upstream tegra-drm
 * channel UAPI). NVDEC decodes into block-linear surfaces; VIC converts each
 * picture into the NV12 graphic block that goes out (no CPU copy). H.264/H.265
 * pictures are held until display order is known (POC, reorder depth from the
 * SPS); VP9 pictures come out in display order.
 */
class C2NvdecDec : public SimpleC2Component {
  public:
    enum Codec { AVC, HEVC, VP9 };
    class IntfImpl;

    C2NvdecDec(const char *name, c2_node_id_t id, Codec codec, const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2NvdecDec();

    // From SimpleC2Component
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(const std::unique_ptr<C2Work> &work,
                 const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(uint32_t drainMode, const std::shared_ptr<C2BlockPool> &pool) override;

  private:
    struct Pending {
        int period, poc;
        uint64_t frameIndex;
        std::shared_ptr<C2GraphicBlock> block;
    };

    static int onPicture(void *opaque, const struct nvdec_picture *pic);
    int handlePicture(const struct nvdec_picture *pic);
    void resetDecoder();
    int reorderDepth() const;
    int vicCopy(const struct nvdec_picture *pic, int fd, size_t size, unsigned pitch, unsigned chromaOffset);
    void outputUpTo(size_t keep, const std::unique_ptr<C2Work> &work);
    void outputOne(Pending p, const std::unique_ptr<C2Work> &work);
    bool updateSize(uint32_t width, uint32_t height, const std::unique_ptr<C2Work> &work);

    const Codec mCodec;
    std::shared_ptr<IntfImpl> mIntf;
    struct h264dec *mAvc = nullptr;
    struct hevcdec *mHevc = nullptr;
    struct vp9dec *mVp9 = nullptr;
    std::vector<Pending> mPending;
    uint32_t mWidth = 0, mHeight = 0;
    int mOutputDelay = -1;
    bool mSignalledError = false;
    bool mSignalledOutputEos = false;
    bool mVicFailed = false;

    // valid while the decode call runs process()'s work
    const std::unique_ptr<C2Work> *mCurWork = nullptr;
    std::shared_ptr<C2BlockPool> mCurPool;
    bool mCurDecoded = false;

    C2_DO_NOT_COPY(C2NvdecDec);
};

C2ComponentFactory *CreateNvdecDecFactory(std::shared_ptr<C2ReflectorHelper> helper, C2NvdecDec::Codec codec);
const char *NvdecDecName(C2NvdecDec::Codec codec);

}  // namespace android
