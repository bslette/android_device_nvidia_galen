/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <SimpleC2Component.h>
#include <util/C2InterfaceHelper.h>

#include "h264enc.h"

namespace android {

/*
 * H.264 encoder on the Tegra NVENC engine (upstream tegra-drm channel UAPI).
 * Input graphic buffers (RGBA from an input surface, or NV12) are converted by
 * VIC into the block-linear surface NVENC reads; I and P pictures only, so every
 * input produces its output right away.
 */
class C2NvencEnc : public SimpleC2Component {
  public:
    class IntfImpl;

    C2NvencEnc(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2NvencEnc();

    // From SimpleC2Component
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(const std::unique_ptr<C2Work> &work, const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(uint32_t drainMode, const std::shared_ptr<C2BlockPool> &pool) override;

  private:
    c2_status_t createEncoder();
    void releaseEncoder();

    std::shared_ptr<IntfImpl> mIntf;
    struct h264enc *mEnc = nullptr;
    std::vector<uint8_t> mHeaders;
    bool mHeadersSent = false;
    bool mSignalledError = false;
    bool mSignalledOutputEos = false;
    bool mInputLogged = false;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;

    C2_DO_NOT_COPY(C2NvencEnc);
};

C2ComponentFactory *CreateNvencEncFactory(std::shared_ptr<C2ReflectorHelper> helper);
const char *NvencEncName();

}  // namespace android
