/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Parameter interface and work handling follow AOSP's C2SoftAvcEnc.
 */

#define LOG_TAG "C2NvencEnc"
#include <log/log.h>

#include <media/stagefright/foundation/MediaDefs.h>

#include <C2AllocatorGralloc.h>
#include <C2Debug.h>
#include <C2PlatformSupport.h>
#include <SimpleC2Interface.h>

#include <algorithm>
#include <unistd.h>

#include "C2NvencEnc.h"

namespace android {

namespace {

constexpr char kName[] = "c2.rey.avc.encoder";
constexpr const char *kRenderNode = "/dev/dri/renderD128"; // tegra-drm (host1x)
constexpr uint32_t kNvencClass = 0x21;                     // NVENC0

void ParseGop(const C2StreamGopTuning::output &gop, uint32_t *syncInterval) {
    uint32_t syncInt = 1;
    for (size_t i = 0; i < gop.flexCount(); ++i) {
        const C2GopLayerStruct &layer = gop.m.values[i];
        if (layer.count == UINT32_MAX)
            syncInt = 0;
        else if (syncInt <= UINT32_MAX / (layer.count + 1))
            syncInt *= (layer.count + 1);
    }
    *syncInterval = syncInt;
}

// C2 colour aspects -> ISO/IEC 23091-2 code points for the VUI
int isoPrimaries(C2Color::primaries_t p) {
    switch (p) {
    case C2Color::PRIMARIES_BT709: return 1;
    case C2Color::PRIMARIES_BT470_M: return 4;
    case C2Color::PRIMARIES_BT601_625: return 5;
    case C2Color::PRIMARIES_BT601_525: return 6;
    case C2Color::PRIMARIES_GENERIC_FILM: return 8;
    case C2Color::PRIMARIES_BT2020: return 9;
    default: return 2;
    }
}

int isoTransfer(C2Color::transfer_t t) {
    switch (t) {
    case C2Color::TRANSFER_LINEAR: return 8;
    case C2Color::TRANSFER_SRGB: return 13;
    case C2Color::TRANSFER_170M: return 6;
    case C2Color::TRANSFER_GAMMA22: return 4;
    case C2Color::TRANSFER_GAMMA28: return 5;
    case C2Color::TRANSFER_ST2084: return 16;
    case C2Color::TRANSFER_HLG: return 18;
    case C2Color::TRANSFER_240M: return 7;
    default: return 2;
    }
}

int isoMatrix(C2Color::matrix_t m) {
    switch (m) {
    case C2Color::MATRIX_BT709: return 1;
    case C2Color::MATRIX_FCC47_73_682: return 4;
    case C2Color::MATRIX_BT601: return 6;
    case C2Color::MATRIX_240M: return 7;
    case C2Color::MATRIX_BT2020: return 9;
    case C2Color::MATRIX_BT2020_CONSTANT: return 10;
    default: return 2;
    }
}

}  // namespace

class C2NvencEnc::IntfImpl : public SimpleInterface<void>::BaseParams {
  public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        : SimpleInterface<void>::BaseParams(helper, kName, C2Component::KIND_ENCODER, C2Component::DOMAIN_VIDEO,
                                            MEDIA_MIMETYPE_VIDEO_AVC) {
        noPrivateBuffers();
        noInputReferences();
        noOutputReferences();
        noInputLatency();
        noTimeStretch();
        setDerivedInstance(this);

        addParameter(DefineParam(mUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
                             .withConstValue(new C2StreamUsageTuning::input(0u, (uint64_t)C2MemoryUsage::CPU_READ))
                             .build());

        addParameter(DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                             .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL))
                             .build());

        addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                             .withDefault(new C2StreamPictureSizeInfo::input(0u, 320, 240))
                             .withFields({
                                     C2F(mSize, width).inRange(128, 4096, 2),
                                     C2F(mSize, height).inRange(128, 4096, 2),
                             })
                             .withSetter(SizeSetter)
                             .build());

        addParameter(DefineParam(mGop, C2_PARAMKEY_GOP)
                             .withDefault(C2StreamGopTuning::output::AllocShared(0 /* flexCount */, 0u /* stream */))
                             .withFields({C2F(mGop, m.values[0].type_).any(), C2F(mGop, m.values[0].count).any()})
                             .withSetter(GopSetter)
                             .build());

        addParameter(DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
                             .withDefault(new C2StreamFrameRateInfo::output(0u, 30.))
                             .withFields({C2F(mFrameRate, value).greaterThan(0.)})
                             .withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps)
                             .build());

        addParameter(DefineParam(mBitrateMode, C2_PARAMKEY_BITRATE_MODE)
                             .withDefault(new C2StreamBitrateModeTuning::output(0u, C2Config::BITRATE_VARIABLE))
                             .withFields({C2F(mBitrateMode, value)
                                                  .oneOf({C2Config::BITRATE_CONST, C2Config::BITRATE_VARIABLE,
                                                          C2Config::BITRATE_IGNORE})})
                             .withSetter(Setter<decltype(*mBitrateMode)>::StrictValueWithNoDeps)
                             .build());

        addParameter(DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
                             .withDefault(new C2StreamBitrateInfo::output(0u, 2000000))
                             .withFields({C2F(mBitrate, value).inRange(16000, 120000000)})
                             .withSetter(BitrateSetter)
                             .build());

        addParameter(DefineParam(mIntraRefresh, C2_PARAMKEY_INTRA_REFRESH)
                             .withConstValue(new C2StreamIntraRefreshTuning::output(
                                     0u, C2Config::INTRA_REFRESH_DISABLED, 0.))
                             .build());

        addParameter(DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                             .withDefault(new C2StreamProfileLevelInfo::output(
                                     0u, PROFILE_AVC_CONSTRAINED_BASELINE, LEVEL_AVC_4_1))
                             .withFields({
                                     C2F(mProfileLevel, profile)
                                             .oneOf({PROFILE_AVC_BASELINE, PROFILE_AVC_CONSTRAINED_BASELINE,
                                                     PROFILE_AVC_MAIN, PROFILE_AVC_CONSTRAINED_HIGH,
                                                     PROFILE_AVC_PROGRESSIVE_HIGH, PROFILE_AVC_HIGH}),
                                     C2F(mProfileLevel, level)
                                             .oneOf({LEVEL_AVC_1, LEVEL_AVC_1B, LEVEL_AVC_1_1, LEVEL_AVC_1_2,
                                                     LEVEL_AVC_1_3, LEVEL_AVC_2, LEVEL_AVC_2_1, LEVEL_AVC_2_2,
                                                     LEVEL_AVC_3, LEVEL_AVC_3_1, LEVEL_AVC_3_2, LEVEL_AVC_4,
                                                     LEVEL_AVC_4_1, LEVEL_AVC_4_2, LEVEL_AVC_5, LEVEL_AVC_5_1,
                                                     LEVEL_AVC_5_2}),
                             })
                             .withSetter(ProfileLevelSetter, mSize, mFrameRate, mBitrate)
                             .build());

        addParameter(DefineParam(mRequestSync, C2_PARAMKEY_REQUEST_SYNC_FRAME)
                             .withDefault(new C2StreamRequestSyncFrameTuning::output(0u, C2_FALSE))
                             .withFields({C2F(mRequestSync, value).oneOf({C2_FALSE, C2_TRUE})})
                             .withSetter(Setter<decltype(*mRequestSync)>::NonStrictValueWithNoDeps)
                             .build());

        addParameter(DefineParam(mSyncFramePeriod, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
                             .withDefault(new C2StreamSyncFrameIntervalTuning::output(0u, 1000000))
                             .withFields({C2F(mSyncFramePeriod, value).any()})
                             .withSetter(Setter<decltype(*mSyncFramePeriod)>::StrictValueWithNoDeps)
                             .build());

        addParameter(DefineParam(mPrependHeader, C2_PARAMKEY_PREPEND_HEADER_MODE)
                             .withDefault(new C2PrependHeaderModeSetting(C2Config::PREPEND_HEADER_TO_NONE))
                             .withFields({C2F(mPrependHeader, value)
                                                  .oneOf({C2Config::PREPEND_HEADER_TO_NONE,
                                                          C2Config::PREPEND_HEADER_TO_ALL_SYNC})})
                             .withSetter(Setter<decltype(*mPrependHeader)>::StrictValueWithNoDeps)
                             .build());

        addParameter(DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
                             .withDefault(new C2StreamColorAspectsInfo::input(
                                     0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                                     C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                             .withFields({C2F(mColorAspects, range)
                                                  .inRange(C2Color::RANGE_UNSPECIFIED, C2Color::RANGE_OTHER),
                                          C2F(mColorAspects, primaries)
                                                  .inRange(C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                                          C2F(mColorAspects, transfer)
                                                  .inRange(C2Color::TRANSFER_UNSPECIFIED, C2Color::TRANSFER_OTHER),
                                          C2F(mColorAspects, matrix)
                                                  .inRange(C2Color::MATRIX_UNSPECIFIED, C2Color::MATRIX_OTHER)})
                             .withSetter(ColorAspectsSetter)
                             .build());

        addParameter(DefineParam(mCodedColorAspects, C2_PARAMKEY_VUI_COLOR_ASPECTS)
                             .withDefault(new C2StreamColorAspectsInfo::output(
                                     0u, C2Color::RANGE_LIMITED, C2Color::PRIMARIES_UNSPECIFIED,
                                     C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                             .withFields({C2F(mCodedColorAspects, range)
                                                  .inRange(C2Color::RANGE_UNSPECIFIED, C2Color::RANGE_OTHER),
                                          C2F(mCodedColorAspects, primaries)
                                                  .inRange(C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                                          C2F(mCodedColorAspects, transfer)
                                                  .inRange(C2Color::TRANSFER_UNSPECIFIED, C2Color::TRANSFER_OTHER),
                                          C2F(mCodedColorAspects, matrix)
                                                  .inRange(C2Color::MATRIX_UNSPECIFIED, C2Color::MATRIX_OTHER)})
                             .withSetter(CodedColorAspectsSetter, mColorAspects)
                             .build());
    }

    static C2R GopSetter(bool /* mayBlock */, C2P<C2StreamGopTuning::output> & /* me */) {
        return C2R::Ok(); // B pictures are not supported; the sync interval is all that is used
    }

    static C2R BitrateSetter(bool /* mayBlock */, C2P<C2StreamBitrateInfo::output> &me) {
        if (me.v.value < 16000) me.set().value = 16000;
        return C2R::Ok();
    }

    static C2R SizeSetter(bool /* mayBlock */, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
                          C2P<C2StreamPictureSizeInfo::input> &me) {
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

    static C2R ProfileLevelSetter(bool /* mayBlock */, C2P<C2StreamProfileLevelInfo::output> &me,
                                  const C2P<C2StreamPictureSizeInfo::input> &size,
                                  const C2P<C2StreamFrameRateInfo::output> &frameRate,
                                  const C2P<C2StreamBitrateInfo::output> &bitrate) {
        if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) me.set().profile = PROFILE_AVC_CONSTRAINED_BASELINE;

        struct LevelLimits {
            C2Config::level_t level;
            float mbsPerSec;
            uint64_t mbs;
            uint32_t bitrate;
        };
        constexpr LevelLimits kLimits[] = {
                {LEVEL_AVC_1, 1485, 99, 64000},          {LEVEL_AVC_1_1, 3000, 396, 192000},
                {LEVEL_AVC_1_2, 6000, 396, 384000},      {LEVEL_AVC_1_3, 11880, 396, 768000},
                {LEVEL_AVC_2, 11880, 396, 2000000},      {LEVEL_AVC_2_1, 19800, 792, 4000000},
                {LEVEL_AVC_2_2, 20250, 1620, 4000000},   {LEVEL_AVC_3, 40500, 1620, 10000000},
                {LEVEL_AVC_3_1, 108000, 3600, 14000000}, {LEVEL_AVC_3_2, 216000, 5120, 20000000},
                {LEVEL_AVC_4, 245760, 8192, 20000000},   {LEVEL_AVC_4_1, 245760, 8192, 50000000},
                {LEVEL_AVC_4_2, 522240, 8704, 50000000}, {LEVEL_AVC_5, 589824, 22080, 135000000},
                {LEVEL_AVC_5_1, 983040, 36864, 240000000}, {LEVEL_AVC_5_2, 2073600, 36864, 240000000},
        };

        // the lowest level that covers the configuration, if the given one does not
        uint64_t mbs = uint64_t((size.v.width + 15) / 16) * ((size.v.height + 15) / 16);
        float mbsPerSec = float(mbs) * frameRate.v.value;
        bool needsUpdate = me.v.level == LEVEL_AVC_1B || !me.F(me.v.level).supportsAtAll(me.v.level);
        bool found = false;
        for (const LevelLimits &limit : kLimits) {
            if (mbs <= limit.mbs && mbsPerSec <= limit.mbsPerSec && bitrate.v.value <= limit.bitrate) {
                if (needsUpdate) me.set().level = limit.level;
                found = true;
                break;
            }
            if (me.v.level == limit.level) needsUpdate = true;
        }
        if (!found) me.set().level = LEVEL_AVC_5_2;
        return C2R::Ok();
    }

    static C2R ColorAspectsSetter(bool /* mayBlock */, C2P<C2StreamColorAspectsInfo::input> &me) {
        if (me.v.range > C2Color::RANGE_OTHER) me.set().range = C2Color::RANGE_OTHER;
        if (me.v.primaries > C2Color::PRIMARIES_OTHER) me.set().primaries = C2Color::PRIMARIES_OTHER;
        if (me.v.transfer > C2Color::TRANSFER_OTHER) me.set().transfer = C2Color::TRANSFER_OTHER;
        if (me.v.matrix > C2Color::MATRIX_OTHER) me.set().matrix = C2Color::MATRIX_OTHER;
        return C2R::Ok();
    }

    static C2R CodedColorAspectsSetter(bool /* mayBlock */, C2P<C2StreamColorAspectsInfo::output> &me,
                                       const C2P<C2StreamColorAspectsInfo::input> &coded) {
        me.set().range = coded.v.range;
        me.set().primaries = coded.v.primaries;
        me.set().transfer = coded.v.transfer;
        me.set().matrix = coded.v.matrix;
        return C2R::Ok();
    }

    int profileIdc_l() const {
        switch (mProfileLevel->profile) {
        case PROFILE_AVC_MAIN: return 77;
        case PROFILE_AVC_CONSTRAINED_HIGH:
        case PROFILE_AVC_PROGRESSIVE_HIGH:
        case PROFILE_AVC_HIGH: return 100;
        default: return 66;
        }
    }

    int levelIdc_l() const {
        struct Level {
            C2Config::level_t c2Level;
            int idc;
        };
        constexpr Level levels[] = {
                {LEVEL_AVC_1, 10},   {LEVEL_AVC_1B, 9},   {LEVEL_AVC_1_1, 11}, {LEVEL_AVC_1_2, 12},
                {LEVEL_AVC_1_3, 13}, {LEVEL_AVC_2, 20},   {LEVEL_AVC_2_1, 21}, {LEVEL_AVC_2_2, 22},
                {LEVEL_AVC_3, 30},   {LEVEL_AVC_3_1, 31}, {LEVEL_AVC_3_2, 32}, {LEVEL_AVC_4, 40},
                {LEVEL_AVC_4_1, 41}, {LEVEL_AVC_4_2, 42}, {LEVEL_AVC_5, 50},   {LEVEL_AVC_5_1, 51},
                {LEVEL_AVC_5_2, 52},
        };
        for (const Level &level : levels)
            if (mProfileLevel->level == level.c2Level) return level.idc;
        return 41;
    }

    // IDR interval in frames, 0 for none after the first
    uint32_t syncFramePeriod_l() const {
        uint32_t gop = 0;
        ParseGop(*mGop, &gop);
        if (mGop->flexCount() && gop) return gop;
        if (mSyncFramePeriod->value < 0 || mSyncFramePeriod->value == INT64_MAX) return 0;
        double period = mSyncFramePeriod->value / 1e6 * mFrameRate->value;
        return (uint32_t)c2_max(c2_min(period + 0.5, double(UINT32_MAX)), 1.);
    }

    std::shared_ptr<C2StreamPictureSizeInfo::input> getSize_l() const { return mSize; }
    std::shared_ptr<C2StreamFrameRateInfo::output> getFrameRate_l() const { return mFrameRate; }
    std::shared_ptr<C2StreamBitrateModeTuning::output> getBitrateMode_l() const { return mBitrateMode; }
    std::shared_ptr<C2StreamBitrateInfo::output> getBitrate_l() const { return mBitrate; }
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> getRequestSync_l() const { return mRequestSync; }
    std::shared_ptr<C2StreamColorAspectsInfo::output> getCodedColorAspects_l() const { return mCodedColorAspects; }
    std::shared_ptr<C2PrependHeaderModeSetting> getPrependHeader_l() const { return mPrependHeader; }

  private:
    std::shared_ptr<C2StreamUsageTuning::input> mUsage;
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;
    std::shared_ptr<C2StreamIntraRefreshTuning::output> mIntraRefresh;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamBitrateModeTuning::output> mBitrateMode;
    std::shared_ptr<C2StreamProfileLevelInfo::output> mProfileLevel;
    std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncFramePeriod;
    std::shared_ptr<C2StreamGopTuning::output> mGop;
    std::shared_ptr<C2PrependHeaderModeSetting> mPrependHeader;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mCodedColorAspects;
};

C2NvencEnc::C2NvencEnc(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl)
    : SimpleC2Component(std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)), mIntf(intfImpl) {}

C2NvencEnc::~C2NvencEnc() {
    onRelease();
}

c2_status_t C2NvencEnc::onInit() {
    return C2_OK; // the engine is opened with the first picture, when the format is final
}

void C2NvencEnc::releaseEncoder() {
    h264enc_destroy(mEnc);
    mEnc = nullptr;
    mHeaders.clear();
    mHeadersSent = false;
    mInputLogged = false;
}

c2_status_t C2NvencEnc::onStop() {
    releaseEncoder();
    mSignalledError = false;
    mSignalledOutputEos = false;
    return C2_OK;
}

void C2NvencEnc::onReset() {
    (void)onStop();
}

void C2NvencEnc::onRelease() {
    releaseEncoder();
}

c2_status_t C2NvencEnc::onFlush_sm() {
    // no pictures are held; start the next stream with an IDR and parameter sets
    releaseEncoder();
    mSignalledOutputEos = false;
    return C2_OK;
}

c2_status_t C2NvencEnc::createEncoder() {
    struct h264enc_config cfg = {};
    {
        IntfImpl::Lock lock = mIntf->lock();
        auto size = mIntf->getSize_l();
        auto aspects = mIntf->getCodedColorAspects_l();
        cfg.width = size->width;
        cfg.height = size->height;
        cfg.profile_idc = mIntf->profileIdc_l();
        cfg.level_idc = mIntf->levelIdc_l();
        cfg.fps = mIntf->getFrameRate_l()->value;
        auto mode = mIntf->getBitrateMode_l()->value;
        if (mode != C2Config::BITRATE_IGNORE) cfg.bitrate = mIntf->getBitrate_l()->value;
        cfg.vbr = mode == C2Config::BITRATE_VARIABLE;
        cfg.gop = mIntf->syncFramePeriod_l();
        cfg.full_range = aspects->range == C2Color::RANGE_FULL;
        cfg.primaries = isoPrimaries(aspects->primaries);
        cfg.transfer = isoTransfer(aspects->transfer);
        cfg.matrix = isoMatrix(aspects->matrix);
        mBitrate = mIntf->getBitrate_l();
        mRequestSync = mIntf->getRequestSync_l();
    }
    mEnc = h264enc_create(kRenderNode, kNvencClass, &cfg);
    if (!mEnc) {
        ALOGE("cannot open NVENC for %ux%u profile %d", cfg.width, cfg.height, cfg.profile_idc);
        return C2_CORRUPTED;
    }
    uint8_t hdr[256];
    int n = h264enc_headers(mEnc, hdr, sizeof(hdr));
    if (n <= 0) return C2_CORRUPTED;
    mHeaders.assign(hdr, hdr + n);
    ALOGI("NVENC %ux%u %.1f fps, profile %d level %d, %s %u bit/s, IDR every %u", cfg.width, cfg.height, cfg.fps,
          cfg.profile_idc, cfg.level_idc, !cfg.bitrate ? "const QP" : cfg.vbr ? "VBR" : "CBR", cfg.bitrate, cfg.gop);
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

// Describe the input picture for h264enc: dma-buf + layout for VIC, CPU view as fallback.
static bool describeInput(const C2Block2D &block, const C2GraphicView &view, struct h264enc_frame *f) {
    const C2PlanarLayout &layout = view.layout();
    const C2Handle *handle = block.handle();
    f->fd = handle->numFds > 0 ? handle->data[0] : -1;
    off_t size = f->fd >= 0 ? lseek(f->fd, 0, SEEK_END) : -1;
    if (size <= 0) f->fd = -1;
    f->size = size > 0 ? size : 0;
    switch (layout.type) {
    case C2PlanarLayout::TYPE_RGB:
    case C2PlanarLayout::TYPE_RGBA: {
        const C2PlaneInfo &r = layout.planes[C2PlanarLayout::PLANE_R];
        if (r.colInc != 4) return false;
        const uint8_t *pr = view.data()[C2PlanarLayout::PLANE_R], *pb = view.data()[C2PlanarLayout::PLANE_B];
        bool bgr = pb < pr;
        f->format = bgr ? H264ENC_BGRA : H264ENC_RGBA;
        f->y = bgr ? pb : pr; // first byte of the pixel
        f->y_stride = r.rowInc;
        f->pitch = r.rowInc;
        return true;
    }
    case C2PlanarLayout::TYPE_YUV: {
        const uint8_t *y = view.data()[C2PlanarLayout::PLANE_Y];
        const uint8_t *u = view.data()[C2PlanarLayout::PLANE_U];
        const uint8_t *v = view.data()[C2PlanarLayout::PLANE_V];
        const C2PlaneInfo &py = layout.planes[C2PlanarLayout::PLANE_Y];
        const C2PlaneInfo &pu = layout.planes[C2PlanarLayout::PLANE_U];
        const C2PlaneInfo &pv = layout.planes[C2PlanarLayout::PLANE_V];
        if (py.colInc != 1 || pu.colInc != pv.colInc || pu.rowInc != pv.rowInc) return false;
        f->format = H264ENC_NV12;
        f->y = y;
        f->u = u;
        f->v = v;
        f->y_stride = py.rowInc;
        f->uv_stride = pu.rowInc;
        f->uv_step = pu.colInc;
        // VIC reads NV12 (U first, interleaved, same pitch as luma); anything else takes the CPU path
        f->pitch = py.rowInc;
        if (pu.colInc == 2 && v == u + 1 && pu.rowInc == py.rowInc)
            f->chroma_offset = u - y;
        else
            f->fd = -1;
        return true;
    }
    default:
        return false;
    }
}

void C2NvencEnc::process(const std::unique_ptr<C2Work> &work, const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;
    if (mSignalledError || mSignalledOutputEos) {
        work->result = C2_BAD_VALUE;
        return;
    }
    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
    if (!mEnc && createEncoder() != C2_OK) {
        mSignalledError = true;
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }
    if (!mHeadersSent) {
        std::unique_ptr<C2StreamInitDataInfo::output> csd =
                C2StreamInitDataInfo::output::AllocUnique(mHeaders.size(), 0u);
        if (!csd) {
            mSignalledError = true;
            work->result = C2_NO_MEMORY;
            work->workletsProcessed = 1u;
            return;
        }
        memcpy(csd->m.value, mHeaders.data(), mHeaders.size());
        work->worklets.front()->output.configUpdate.push_back(std::move(csd));
        mHeadersSent = true;
    }

    // dynamic parameters
    bool forceIdr = false, prepend = false;
    {
        IntfImpl::Lock lock = mIntf->lock();
        std::shared_ptr<C2StreamBitrateInfo::output> bitrate = mIntf->getBitrate_l();
        std::shared_ptr<C2StreamRequestSyncFrameTuning::output> requestSync = mIntf->getRequestSync_l();
        prepend = mIntf->getPrependHeader_l()->value == C2Config::PREPEND_HEADER_TO_ALL_SYNC;
        lock.unlock();
        if (bitrate != mBitrate) {
            mBitrate = bitrate;
            h264enc_set_bitrate(mEnc, bitrate->value);
        }
        if (requestSync != mRequestSync) {
            if (requestSync->value) {
                C2StreamRequestSyncFrameTuning::output clearSync(0u, C2_FALSE);
                std::vector<std::unique_ptr<C2SettingResult>> failures;
                mIntf->config({&clearSync}, C2_MAY_BLOCK, &failures);
                forceIdr = true;
            }
            mRequestSync = requestSync;
        }
    }

    if (work->input.buffers.empty()) {
        fillEmptyWork(work);
        if (eos) mSignalledOutputEos = true;
        return;
    }

    std::shared_ptr<C2Buffer> input = work->input.buffers[0];
    C2ConstGraphicBlock block = input->data().graphicBlocks().front();
    C2GraphicView view = block.map().get();
    if (view.error() != C2_OK) {
        ALOGE("graphic view map err = %d", view.error());
        mSignalledError = true;
        work->result = view.error();
        work->workletsProcessed = 1u;
        return;
    }
    struct h264enc_frame frame = {};
    if (!describeInput(block, view, &frame)) {
        ALOGE("unsupported input layout (type %d)", view.layout().type);
        mSignalledError = true;
        work->result = C2_BAD_VALUE;
        work->workletsProcessed = 1u;
        return;
    }
    if (!mInputLogged) {
        ALOGI("input %ux%u: %s, pitch %u, %s", view.width(), view.height(),
              frame.format == H264ENC_NV12 ? "NV12" : frame.format == H264ENC_BGRA ? "BGRA" : "RGBA", frame.pitch,
              frame.fd >= 0 ? "VIC" : "CPU conversion");
        mInputLogged = true;
    }

    size_t cap = h264enc_max_output(mEnc) + mHeaders.size();
    std::shared_ptr<C2LinearBlock> outBlock;
    C2MemoryUsage usage = {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
    c2_status_t err = pool->fetchLinearBlock(cap, usage, &outBlock);
    if (err != C2_OK) {
        ALOGE("fetchLinearBlock failed: %d", err);
        work->result = err;
        work->workletsProcessed = 1u;
        return;
    }
    C2WriteView wView = outBlock->map().get();
    if (wView.error() != C2_OK) {
        work->result = wView.error();
        work->workletsProcessed = 1u;
        return;
    }

    int idr = 0;
    size_t hdr = prepend ? mHeaders.size() : 0;
    int n = h264enc_encode(mEnc, &frame, forceIdr, wView.base() + hdr, wView.capacity() - hdr, &idr);
    if (n < 0) {
        ALOGE("encoding failed: %d", n);
        mSignalledError = true;
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }
    size_t offset = hdr;
    if (prepend && idr) {
        memcpy(wView.base(), mHeaders.data(), hdr);
        offset = 0;
        n += hdr;
    }
    std::shared_ptr<C2Buffer> buffer = createLinearBuffer(outBlock, offset, n);
    if (idr)
        buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(0u, C2Config::SYNC_FRAME));
    work->worklets.front()->output.flags = eos ? C2FrameData::FLAG_END_OF_STREAM : (C2FrameData::flags_t)0;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.buffers.push_back(buffer);
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
    if (eos) mSignalledOutputEos = true;
}

c2_status_t C2NvencEnc::drain(uint32_t drainMode, const std::shared_ptr<C2BlockPool> & /* pool */) {
    if (drainMode == NO_DRAIN) return C2_OK;
    if (drainMode == DRAIN_CHAIN) return C2_OMITTED;
    return C2_OK; // nothing is held
}

class C2NvencEncFactory : public C2ComponentFactory {
  public:
    explicit C2NvencEncFactory(std::shared_ptr<C2ReflectorHelper> helper) : mHelper(std::move(helper)) {}

    c2_status_t createComponent(c2_node_id_t id, std::shared_ptr<C2Component> *const component,
                                std::function<void(C2Component *)> deleter) override {
        *component = std::shared_ptr<C2Component>(
                new C2NvencEnc(kName, id, std::make_shared<C2NvencEnc::IntfImpl>(mHelper)), deleter);
        return C2_OK;
    }

    c2_status_t createInterface(c2_node_id_t id, std::shared_ptr<C2ComponentInterface> *const interface,
                                std::function<void(C2ComponentInterface *)> deleter) override {
        *interface = std::shared_ptr<C2ComponentInterface>(
                new SimpleInterface<C2NvencEnc::IntfImpl>(kName, id, std::make_shared<C2NvencEnc::IntfImpl>(mHelper)),
                deleter);
        return C2_OK;
    }

  private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
};

C2ComponentFactory *CreateNvencEncFactory(std::shared_ptr<C2ReflectorHelper> helper) {
    return new C2NvencEncFactory(std::move(helper));
}

const char *NvencEncName() {
    return kName;
}

}  // namespace android
