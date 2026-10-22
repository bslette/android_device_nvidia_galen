/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <SimpleC2Component.h>
#include <util/C2InterfaceHelper.h>

#include <vector>

#include "h264dec.h"

namespace android {

/*
 * H.264 decoder on the Tegra NVDEC engine (upstream tegra-drm channel UAPI).
 * NVDEC decodes into block-linear surfaces; VIC converts each picture into the
 * NV12 graphic block that goes out (no CPU copy). Pictures are held until
 * display order is known (POC, reorder depth from the SPS).
 */
class C2NvdecAvcDec : public SimpleC2Component {
  public:
    class IntfImpl;

    C2NvdecAvcDec(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2NvdecAvcDec();

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

    static int onPicture(void *opaque, const struct h264dec_picture *pic);
    int handlePicture(const struct h264dec_picture *pic);
    void outputUpTo(size_t keep, const std::unique_ptr<C2Work> &work);
    void outputOne(Pending p, const std::unique_ptr<C2Work> &work);
    bool updateSize(uint32_t width, uint32_t height, const std::unique_ptr<C2Work> &work);

    std::shared_ptr<IntfImpl> mIntf;
    struct h264dec *mDec = nullptr;
    std::vector<Pending> mPending;
    uint32_t mWidth = 0, mHeight = 0;
    int mOutputDelay = -1;
    bool mSignalledError = false;
    bool mSignalledOutputEos = false;
    bool mVicFailed = false;

    // valid while h264dec_decode() runs process()'s work
    const std::unique_ptr<C2Work> *mCurWork = nullptr;
    std::shared_ptr<C2BlockPool> mCurPool;
    bool mCurDecoded = false;

    C2_DO_NOT_COPY(C2NvdecAvcDec);
};

}  // namespace android
