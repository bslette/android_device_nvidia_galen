/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Parameter interface and work handling follow AOSP's C2SoftAvcDec.
 */

#define LOG_TAG "C2NvdecDec"
#include <log/log.h>

#include <media/stagefright/foundation/MediaDefs.h>

#include <C2AllocatorGralloc.h>
#include <C2Debug.h>
#include <C2PlatformSupport.h>
#include <SimpleC2Interface.h>

#include <algorithm>
#include <unistd.h>

#include "C2NvdecDec.h"

namespace android {

namespace {

constexpr char kAvcName[] = "c2.rey.avc.decoder";
constexpr char kHevcName[] = "c2.rey.hevc.decoder";
constexpr char kVp9Name[] = "c2.rey.vp9.decoder";
constexpr const char *kRenderNode = "/dev/dri/renderD128"; // tegra-drm (host1x)
constexpr uint32_t kNvdecClass = 0xf0;                     // NVDEC0
constexpr uint32_t kDefaultOutputDelay = 8;
constexpr uint32_t kMaxOutputDelay = 16;
constexpr size_t kMinInputBufferSize = 2 * 1024 * 1024;

const char *mediaType(C2NvdecDec::Codec codec) {
    return codec == C2NvdecDec::HEVC  ? MEDIA_MIMETYPE_VIDEO_HEVC
           : codec == C2NvdecDec::VP9 ? MEDIA_MIMETYPE_VIDEO_VP9
                                      : MEDIA_MIMETYPE_VIDEO_AVC;
}

}  // namespace

class C2NvdecDec::IntfImpl : public SimpleInterface<void>::BaseParams {
  public:
    IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper, Codec codec)
        : SimpleInterface<void>::BaseParams(helper, NvdecDecName(codec), C2Component::KIND_DECODER,
                                            C2Component::DOMAIN_VIDEO, mediaType(codec)) {
        noPrivateBuffers();
        noInputReferences();
        noOutputReferences();
        noInputLatency();
        noTimeStretch();

        addParameter(DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
                             .withDefault(new C2PortActualDelayTuning::output(kDefaultOutputDelay))
                             .withFields({C2F(mActualOutputDelay, value).inRange(0, kMaxOutputDelay)})
                             .withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
                             .build());

        addParameter(DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                             .withConstValue(new C2ComponentAttributesSetting(
                                     C2Component::ATTRIB_IS_TEMPORAL))
                             .build());

        addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                             .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
                             .withFields({
                                     C2F(mSize, width).inRange(16, 4096, codec == VP9 ? 1 : 2),
                                     C2F(mSize, height).inRange(16, 4096, codec == VP9 ? 1 : 2),
                             })
                             .withSetter(SizeSetter)
                             .build());

        addParameter(DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
                             .withDefault(new C2StreamMaxPictureSizeTuning::output(0u, 320, 240))
                             .withFields({
                                     C2F(mSize, width).inRange(16, 4096, codec == VP9 ? 1 : 2),
                                     C2F(mSize, height).inRange(16, 4096, codec == VP9 ? 1 : 2),
                             })
                             .withSetter(MaxPictureSizeSetter, mSize)
                             .build());

        if (codec == HEVC) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                            .withDefault(new C2StreamProfileLevelInfo::input(
                                    0u, C2Config::PROFILE_HEVC_MAIN, C2Config::LEVEL_HEVC_MAIN_5_1))
                            .withFields({C2F(mProfileLevel, profile)
                                                 .oneOf({C2Config::PROFILE_HEVC_MAIN,
                                                         C2Config::PROFILE_HEVC_MAIN_STILL}),
                                         C2F(mProfileLevel, level)
                                                 .oneOf({C2Config::LEVEL_HEVC_MAIN_1,
                                                         C2Config::LEVEL_HEVC_MAIN_2,
                                                         C2Config::LEVEL_HEVC_MAIN_2_1,
                                                         C2Config::LEVEL_HEVC_MAIN_3,
                                                         C2Config::LEVEL_HEVC_MAIN_3_1,
                                                         C2Config::LEVEL_HEVC_MAIN_4,
                                                         C2Config::LEVEL_HEVC_MAIN_4_1,
                                                         C2Config::LEVEL_HEVC_MAIN_5,
                                                         C2Config::LEVEL_HEVC_MAIN_5_1,
                                                         C2Config::LEVEL_HEVC_HIGH_4,
                                                         C2Config::LEVEL_HEVC_HIGH_4_1,
                                                         C2Config::LEVEL_HEVC_HIGH_5,
                                                         C2Config::LEVEL_HEVC_HIGH_5_1})})
                            .withSetter(ProfileLevelSetter, mSize)
                            .build());
        } else if (codec == VP9) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                            .withDefault(new C2StreamProfileLevelInfo::input(
                                    0u, C2Config::PROFILE_VP9_0, C2Config::LEVEL_VP9_5))
                            .withFields({C2F(mProfileLevel, profile).oneOf({C2Config::PROFILE_VP9_0}),
                                         C2F(mProfileLevel, level)
                                                 .oneOf({C2Config::LEVEL_VP9_1, C2Config::LEVEL_VP9_1_1,
                                                         C2Config::LEVEL_VP9_2, C2Config::LEVEL_VP9_2_1,
                                                         C2Config::LEVEL_VP9_3, C2Config::LEVEL_VP9_3_1,
                                                         C2Config::LEVEL_VP9_4, C2Config::LEVEL_VP9_4_1,
                                                         C2Config::LEVEL_VP9_5, C2Config::LEVEL_VP9_5_1})})
                            .withSetter(ProfileLevelSetter, mSize)
                            .build());
        } else {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                            .withDefault(new C2StreamProfileLevelInfo::input(
                                    0u, C2Config::PROFILE_AVC_CONSTRAINED_BASELINE,
                                    C2Config::LEVEL_AVC_5_1))
                            .withFields({C2F(mProfileLevel, profile)
                                                 .oneOf({C2Config::PROFILE_AVC_CONSTRAINED_BASELINE,
                                                         C2Config::PROFILE_AVC_BASELINE,
                                                         C2Config::PROFILE_AVC_MAIN,
                                                         C2Config::PROFILE_AVC_CONSTRAINED_HIGH,
                                                         C2Config::PROFILE_AVC_PROGRESSIVE_HIGH,
                                                         C2Config::PROFILE_AVC_HIGH}),
                                         C2F(mProfileLevel, level)
                                                 .oneOf({C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B,
                                                         C2Config::LEVEL_AVC_1_1, C2Config::LEVEL_AVC_1_2,
                                                         C2Config::LEVEL_AVC_1_3, C2Config::LEVEL_AVC_2,
                                                         C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2,
                                                         C2Config::LEVEL_AVC_3, C2Config::LEVEL_AVC_3_1,
                                                         C2Config::LEVEL_AVC_3_2, C2Config::LEVEL_AVC_4,
                                                         C2Config::LEVEL_AVC_4_1, C2Config::LEVEL_AVC_4_2,
                                                         C2Config::LEVEL_AVC_5, C2Config::LEVEL_AVC_5_1})})
                            .withSetter(ProfileLevelSetter, mSize)
                            .build());
        }

        addParameter(DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
                             .withDefault(new C2StreamMaxBufferSizeInfo::input(0u, kMinInputBufferSize))
                             .withFields({C2F(mMaxInputSize, value).any()})
                             .calculatedAs(MaxInputSizeSetter, mMaxSize)
                             .build());

        std::shared_ptr<C2StreamColorInfo::output> defaultColorInfo =
                C2StreamColorInfo::output::AllocShared({C2ChromaOffsetStruct::ITU_YUV_420_0()}, 0u,
                                                       8u /* bitDepth */, C2Color::YUV_420);
        helper->addStructDescriptors<C2ChromaOffsetStruct>();
        addParameter(DefineParam(mColorInfo, C2_PARAMKEY_CODED_COLOR_INFO)
                             .withConstValue(defaultColorInfo)
                             .build());

        addParameter(DefineParam(mDefaultColorAspects, C2_PARAMKEY_DEFAULT_COLOR_ASPECTS)
                             .withDefault(new C2StreamColorAspectsTuning::output(
                                     0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                                     C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                             .withFields({C2F(mDefaultColorAspects, range)
                                                  .inRange(C2Color::RANGE_UNSPECIFIED,
                                                           C2Color::RANGE_OTHER),
                                          C2F(mDefaultColorAspects, primaries)
                                                  .inRange(C2Color::PRIMARIES_UNSPECIFIED,
                                                           C2Color::PRIMARIES_OTHER),
                                          C2F(mDefaultColorAspects, transfer)
                                                  .inRange(C2Color::TRANSFER_UNSPECIFIED,
                                                           C2Color::TRANSFER_OTHER),
                                          C2F(mDefaultColorAspects, matrix)
                                                  .inRange(C2Color::MATRIX_UNSPECIFIED,
                                                           C2Color::MATRIX_OTHER)})
                             .withSetter(DefaultColorAspectsSetter)
                             .build());

        addParameter(DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
                             .withDefault(new C2StreamColorAspectsInfo::output(
                                     0u, C2Color::RANGE_LIMITED, C2Color::PRIMARIES_UNSPECIFIED,
                                     C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                             .withFields({C2F(mColorAspects, range)
                                                  .inRange(C2Color::RANGE_UNSPECIFIED,
                                                           C2Color::RANGE_OTHER),
                                          C2F(mColorAspects, primaries)
                                                  .inRange(C2Color::PRIMARIES_UNSPECIFIED,
                                                           C2Color::PRIMARIES_OTHER),
                                          C2F(mColorAspects, transfer)
                                                  .inRange(C2Color::TRANSFER_UNSPECIFIED,
                                                           C2Color::TRANSFER_OTHER),
                                          C2F(mColorAspects, matrix)
                                                  .inRange(C2Color::MATRIX_UNSPECIFIED,
                                                           C2Color::MATRIX_OTHER)})
                             .withSetter(ColorAspectsSetter, mDefaultColorAspects)
                             .build());

        // VIC writes NV12; YCbCr_420_888 is what the gralloc pool hands out as NV12
        addParameter(DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
                             .withConstValue(new C2StreamPixelFormatInfo::output(
                                     0u, HAL_PIXEL_FORMAT_YCBCR_420_888))
                             .build());
    }

    static C2R SizeSetter(bool /* mayBlock */, const C2P<C2StreamPictureSizeInfo::output> &oldMe,
                          C2P<C2StreamPictureSizeInfo::output> &me) {
        C2R res = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            me.set().width = oldMe.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().height = oldMe.v.height;
        }
        return res;
    }

    static C2R MaxPictureSizeSetter(bool /* mayBlock */, C2P<C2StreamMaxPictureSizeTuning::output> &me,
                                    const C2P<C2StreamPictureSizeInfo::output> &size) {
        me.set().width = c2_min(c2_max(me.v.width, size.v.width), 4096u);
        me.set().height = c2_min(c2_max(me.v.height, size.v.height), 4096u);
        return C2R::Ok();
    }

    static C2R MaxInputSizeSetter(bool /* mayBlock */, C2P<C2StreamMaxBufferSizeInfo::input> &me,
                                  const C2P<C2StreamMaxPictureSizeTuning::output> &maxSize) {
        // assume compression ratio of 2, as the software decoder does
        me.set().value = c2_max((((maxSize.v.width + 15) / 16) * ((maxSize.v.height + 15) / 16) * 192),
                                kMinInputBufferSize);
        return C2R::Ok();
    }

    static C2R ProfileLevelSetter(bool /* mayBlock */, C2P<C2StreamProfileLevelInfo::input> & /* me */,
                                  const C2P<C2StreamPictureSizeInfo::output> & /* size */) {
        return C2R::Ok();
    }

    static C2R DefaultColorAspectsSetter(bool /* mayBlock */,
                                         C2P<C2StreamColorAspectsTuning::output> &me) {
        if (me.v.range > C2Color::RANGE_OTHER) me.set().range = C2Color::RANGE_OTHER;
        if (me.v.primaries > C2Color::PRIMARIES_OTHER) me.set().primaries = C2Color::PRIMARIES_OTHER;
        if (me.v.transfer > C2Color::TRANSFER_OTHER) me.set().transfer = C2Color::TRANSFER_OTHER;
        if (me.v.matrix > C2Color::MATRIX_OTHER) me.set().matrix = C2Color::MATRIX_OTHER;
        return C2R::Ok();
    }

    static C2R ColorAspectsSetter(bool /* mayBlock */, C2P<C2StreamColorAspectsInfo::output> &me,
                                  const C2P<C2StreamColorAspectsTuning::output> &def) {
        // VUI color description is not parsed yet: take the defaults, limited range
        me.set().range = def.v.range == RANGE_UNSPECIFIED ? C2Color::RANGE_LIMITED : def.v.range;
        me.set().primaries = def.v.primaries;
        me.set().transfer = def.v.transfer;
        me.set().matrix = def.v.matrix;
        return C2R::Ok();
    }

    std::shared_ptr<C2StreamColorAspectsInfo::output> getColorAspects_l() { return mColorAspects; }

  private:
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
};

C2NvdecDec::C2NvdecDec(const char *name, c2_node_id_t id, Codec codec,
                       const std::shared_ptr<IntfImpl> &intfImpl)
    : SimpleC2Component(std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)),
      mCodec(codec),
      mIntf(intfImpl) {}

C2NvdecDec::~C2NvdecDec() {
    onRelease();
}

c2_status_t C2NvdecDec::onInit() {
    if (mCodec == HEVC)
        mHevc = hevcdec_create(kRenderNode, kNvdecClass, onPicture, this);
    else if (mCodec == VP9)
        mVp9 = vp9dec_create(kRenderNode, kNvdecClass, onPicture, this);
    else
        mAvc = h264dec_create(kRenderNode, kNvdecClass, onPicture, this);
    if (!mAvc && !mHevc && !mVp9) {
        ALOGE("cannot open NVDEC on %s", kRenderNode);
        return C2_CORRUPTED;
    }
    return C2_OK;
}

void C2NvdecDec::resetDecoder() {
    if (mAvc) h264dec_reset(mAvc);
    if (mHevc) hevcdec_reset(mHevc);
    if (mVp9) vp9dec_reset(mVp9);
}

int C2NvdecDec::reorderDepth() const {
    if (mVp9) return 0; // VP9 has no display reordering
    return mHevc ? hevcdec_reorder_depth(mHevc) : h264dec_reorder_depth(mAvc);
}

int C2NvdecDec::vicCopy(const struct nvdec_picture *pic, int fd, size_t size, unsigned pitch,
                        unsigned chromaOffset) {
    if (mVp9) return vp9dec_vic_copy(mVp9, pic, fd, size, pitch, chromaOffset);
    return mHevc ? hevcdec_vic_copy(mHevc, pic, fd, size, pitch, chromaOffset)
                 : h264dec_vic_copy(mAvc, pic, fd, size, pitch, chromaOffset);
}

void C2NvdecDec::resetOutputOrder() {
    mHaveLast = false;
    mOddPoc = false;
    mPicsSeen = 0;
}

c2_status_t C2NvdecDec::onStop() {
    mPending.clear();
    resetDecoder();
    resetOutputOrder();
    mSignalledError = false;
    mSignalledOutputEos = false;
    return C2_OK;
}

void C2NvdecDec::onReset() {
    (void)onStop();
}

void C2NvdecDec::onRelease() {
    mPending.clear();
    h264dec_destroy(mAvc);
    hevcdec_destroy(mHevc);
    vp9dec_destroy(mVp9);
    mAvc = nullptr;
    mHevc = nullptr;
    mVp9 = nullptr;
}

c2_status_t C2NvdecDec::onFlush_sm() {
    mPending.clear();
    resetDecoder();
    resetOutputOrder();
    mSignalledOutputEos = false;
    return C2_OK;
}

static void fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    uint32_t flags = 0;
    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) flags |= C2FrameData::FLAG_END_OF_STREAM;
    work->worklets.front()->output.flags = (C2FrameData::flags_t)flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

bool C2NvdecDec::updateSize(uint32_t width, uint32_t height, const std::unique_ptr<C2Work> &work) {
    if (width == mWidth && height == mHeight) return true;
    C2StreamPictureSizeInfo::output size(0u, width, height);
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    if (mIntf->config({&size}, C2_MAY_BLOCK, &failures) != C2_OK) {
        ALOGE("cannot set picture size %ux%u", width, height);
        return false;
    }
    work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(size));
    mWidth = width;
    mHeight = height;
    return true;
}

int C2NvdecDec::onPicture(void *opaque, const struct nvdec_picture *pic) {
    return static_cast<C2NvdecDec *>(opaque)->handlePicture(pic);
}

int C2NvdecDec::handlePicture(const struct nvdec_picture *pic) {
    const std::unique_ptr<C2Work> &work = *mCurWork;
    mCurDecoded = true;
    if (pic->mbs_in_error) ALOGW("picture %d: %u macroblocks in error", pic->poc, pic->mbs_in_error);
    if (!updateSize(pic->width, pic->height, work)) return -1;

    std::shared_ptr<C2GraphicBlock> block;
    C2MemoryUsage usage = {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
    c2_status_t err = mCurPool->fetchGraphicBlock(pic->width, pic->height,
                                                  HAL_PIXEL_FORMAT_YCBCR_420_888, usage, &block);
    if (err != C2_OK) {
        ALOGE("fetchGraphicBlock failed: %d", err);
        return -1;
    }

    // Plane layout of the gralloc buffer (NV12 from minigbm): map once to read it.
    C2GraphicView view = block->map().get();
    if (view.error()) {
        ALOGE("graphic view map failed: %d", view.error());
        return -1;
    }
    const C2PlanarLayout &layout = view.layout();
    uint8_t *y = view.data()[C2PlanarLayout::PLANE_Y];
    uint8_t *u = view.data()[C2PlanarLayout::PLANE_U];
    uint8_t *v = view.data()[C2PlanarLayout::PLANE_V];
    int yStride = layout.planes[C2PlanarLayout::PLANE_Y].rowInc;
    int uvStride = layout.planes[C2PlanarLayout::PLANE_U].rowInc;
    int uvStep = layout.planes[C2PlanarLayout::PLANE_U].colInc;

    bool done = false;
    if (!mVicFailed && uvStep == 2 && v == u + 1 && uvStride == yStride) {
        const C2Handle *handle = block->handle();
        int fd = handle->numFds > 0 ? handle->data[0] : -1;
        off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
        if (size > 0) {
            int verr = vicCopy(pic, fd, size, yStride, u - y);
            if (verr == 0) {
                done = true;
            } else {
                ALOGW("VIC copy failed (%d), using CPU copies", verr);
                mVicFailed = verr != -EINVAL;
            }
        }
    }
    if (!done) nvdec_copy_out(pic, y, yStride, u, v, uvStride, uvStep);

    if (mCodec == AVC) {
        mOddPoc |= (pic->poc & 1) != 0;
        mPicsSeen++;
    }
    mPending.push_back({pic->period, pic->poc, pic->leader != 0, pic->timestamp, std::move(block)});
    int depth = reorderDepth();
    if (depth >= 0 && depth != mOutputDelay) {
        C2PortActualDelayTuning::output outputDelay(std::min<int>(depth + 1, kMaxOutputDelay));
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        if (mIntf->config({&outputDelay}, C2_MAY_BLOCK, &failures) == C2_OK)
            work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(outputDelay));
        mOutputDelay = depth;
    }
    outputUpTo(depth < 0 ? 0 : depth, work);
    return 0;
}

// The smallest POC step that can separate two pictures in display order. HEVC
// counts pictures; H.264 counts fields, so frames advance by 2 unless the stream
// has shown an odd POC (only trusted after a DPB's worth of pictures).
int C2NvdecDec::pocStep() const {
    return mCodec == AVC && !mOddPoc && mPicsSeen >= 16 ? 2 : 1;
}

// Output while more than `keep` pictures wait (the stream's reorder bound), and
// also whenever the next picture in display order is already decoded: one that
// starts a new period (no leading pictures), or one whose POC directly follows
// the last output, so that no picture still to come can go between them.
void C2NvdecDec::outputUpTo(size_t keep, const std::unique_ptr<C2Work> &work) {
    while (!mPending.empty()) {
        auto first = std::min_element(mPending.begin(), mPending.end(), [](const Pending &a, const Pending &b) {
            return a.period != b.period ? a.period < b.period : a.poc < b.poc;
        });
        bool next = mHaveLast && first->period == mLastPeriod && first->poc == mLastPoc + pocStep();
        bool lead = first->leader && (!mHaveLast || first->period > mLastPeriod);
        if (mPending.size() <= keep && !next && !lead) break;
        Pending p = std::move(*first);
        mPending.erase(first);
        mHaveLast = true;
        mLastPeriod = p.period;
        mLastPoc = p.poc;
        outputOne(std::move(p), work);
    }
}

void C2NvdecDec::outputOne(Pending p, const std::unique_ptr<C2Work> &work) {
    std::shared_ptr<C2Buffer> buffer = createGraphicBuffer(std::move(p.block), C2Rect(mWidth, mHeight));
    {
        IntfImpl::Lock lock = mIntf->lock();
        buffer->setInfo(mIntf->getColorAspects_l());
    }
    auto fillWork = [buffer](const std::unique_ptr<C2Work> &w) {
        w->worklets.front()->output.flags = (C2FrameData::flags_t)0;
        w->worklets.front()->output.buffers.clear();
        w->worklets.front()->output.buffers.push_back(buffer);
        w->worklets.front()->output.ordinal = w->input.ordinal;
        w->workletsProcessed = 1u;
    };
    if (work && c2_cntr64_t(p.frameIndex) == work->input.ordinal.frameIndex) {
        // The running work would only be delivered when process() returns, after
        // older works finished later in this call: send its picture now in a clone
        // (with any size update), and the work itself completes empty.
        C2WorkOrdinalStruct ordinal = work->input.ordinal;
        std::vector<std::unique_ptr<C2Param>> configUpdate =
                std::move(work->worklets.front()->output.configUpdate);
        cloneAndSend(p.frameIndex, work, [buffer, ordinal, &configUpdate](const std::unique_ptr<C2Work> &w) {
            w->worklets.front()->output.flags = C2FrameData::FLAG_INCOMPLETE;
            w->worklets.front()->output.buffers.clear();
            w->worklets.front()->output.buffers.push_back(buffer);
            w->worklets.front()->output.configUpdate = std::move(configUpdate);
            w->worklets.front()->output.ordinal = ordinal;
            w->workletsProcessed = 1u;
            w->result = C2_OK;
        });
        work->worklets.front()->output.configUpdate.clear();
        mCurSent = true;
    } else {
        finish(p.frameIndex, fillWork);
    }
}

void C2NvdecDec::process(const std::unique_ptr<C2Work> &work,
                            const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;
    if (mSignalledError || mSignalledOutputEos) {
        work->result = C2_BAD_VALUE;
        return;
    }

    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
    if (mVp9 && (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG)) {
        fillEmptyWork(work); // codec private data (vpcC), not a frame
        return;
    }
    mCurDecoded = false;
    mCurSent = false;
    if (!work->input.buffers.empty()) {
        C2ReadView rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        if (rView.capacity() && rView.error()) {
            ALOGE("read view map failed %d", rView.error());
            work->result = rView.error();
            return;
        }
        mCurWork = &work;
        mCurPool = pool;
        uint64_t ts = work->input.ordinal.frameIndex.peeku();
        int err = mVp9    ? vp9dec_decode(mVp9, rView.data(), rView.capacity(), ts)
                  : mHevc ? hevcdec_decode(mHevc, rView.data(), rView.capacity(), ts)
                          : h264dec_decode(mAvc, rView.data(), rView.capacity(), ts);
        mCurWork = nullptr;
        mCurPool.reset();
        if (err == -ENOTSUP || err == -ECANCELED || err == -ENOMEM || err == -ETIMEDOUT) {
            ALOGE("decoding failed: %d", err);
            mSignalledError = true;
            work->workletsProcessed = 1u;
            work->result = C2_CORRUPTED;
            return;
        } else if (err) {
            ALOGW("bad access unit (%d), skipped", err);
        }
    }

    if (eos) {
        outputUpTo(0, work);
        fillEmptyWork(work);
        mSignalledOutputEos = true;
    } else if (!mCurDecoded || mCurSent) {
        fillEmptyWork(work); // parameter sets only, a skipped access unit, or the picture went out already
    } else if (work->workletsProcessed == 0 &&
               !work->worklets.front()->output.configUpdate.empty()) {
        // the picture is held for reordering: send the config update now
        auto fill = [&work](const std::unique_ptr<C2Work> &clone) {
            clone->worklets.front()->output.flags = C2FrameData::FLAG_INCOMPLETE;
            clone->worklets.front()->output.buffers.clear();
            clone->worklets.front()->output.configUpdate =
                    std::move(work->worklets.front()->output.configUpdate);
            clone->worklets.front()->output.ordinal = work->input.ordinal;
            clone->workletsProcessed = 1u;
            clone->result = C2_OK;
        };
        cloneAndSend(work->input.ordinal.frameIndex.peeku(), work, fill);
    }
    work->input.buffers.clear();
}

c2_status_t C2NvdecDec::drain(uint32_t drainMode, const std::shared_ptr<C2BlockPool> & /* pool */) {
    if (drainMode == NO_DRAIN) return C2_OK;
    if (drainMode == DRAIN_CHAIN) return C2_OMITTED;
    outputUpTo(0, nullptr);
    return C2_OK;
}

class C2NvdecDecFactory : public C2ComponentFactory {
  public:
    C2NvdecDecFactory(std::shared_ptr<C2ReflectorHelper> helper, C2NvdecDec::Codec codec)
        : mHelper(std::move(helper)), mCodec(codec) {}

    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component> *const component,
                                std::function<void(C2Component *)> deleter) override {
        *component = std::shared_ptr<C2Component>(
                new C2NvdecDec(NvdecDecName(mCodec), id, mCodec,
                               std::make_shared<C2NvdecDec::IntfImpl>(mHelper, mCodec)),
                deleter);
        return C2_OK;
    }

    c2_status_t createInterface(c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *const interface,
                                std::function<void(C2ComponentInterface *)> deleter) override {
        *interface = std::shared_ptr<C2ComponentInterface>(
                new SimpleInterface<C2NvdecDec::IntfImpl>(
                        NvdecDecName(mCodec), id, std::make_shared<C2NvdecDec::IntfImpl>(mHelper, mCodec)),
                deleter);
        return C2_OK;
    }

  private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
    C2NvdecDec::Codec mCodec;
};

C2ComponentFactory *CreateNvdecDecFactory(std::shared_ptr<C2ReflectorHelper> helper, C2NvdecDec::Codec codec) {
    return new C2NvdecDecFactory(std::move(helper), codec);
}

const char *NvdecDecName(C2NvdecDec::Codec codec) {
    return codec == C2NvdecDec::HEVC ? kHevcName : codec == C2NvdecDec::VP9 ? kVp9Name : kAvcName;
}

}  // namespace android
