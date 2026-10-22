/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Codec2 IComponentStore "default" for galen: the NVDEC H.264 decoder.
 * Based on AOSP's sample vendor service (frameworks/av/media/codec2/hal/services).
 */

#define LOG_TAG "android.hardware.media.c2-rey-service"

#include <android-base/logging.h>
#include <minijail.h>

#include <C2Component.h>
#include <C2Config.h>
#include <C2PlatformSupport.h>
#include <util/C2InterfaceHelper.h>

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <codec2/aidl/ComponentStore.h>

#include <atomic>
#include <signal.h>

namespace android {
C2ComponentFactory *CreateNvdecAvcDecFactory(std::shared_ptr<C2ReflectorHelper> helper);
const char *NvdecAvcDecName();
}  // namespace android

static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/android.hardware.media.c2-default-seccomp_policy";
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/android.hardware.media.c2-extended-seccomp_policy";
static constexpr int kThreadCount = 8;

class StoreImpl : public C2ComponentStore {
  public:
    StoreImpl()
        : mReflectorHelper(std::make_shared<C2ReflectorHelper>()),
          mInterface(mReflectorHelper),
          mFactory(android::CreateNvdecAvcDecFactory(mReflectorHelper)) {
        auto traits = std::make_shared<C2Component::Traits>();
        traits->name = android::NvdecAvcDecName();
        traits->domain = C2Component::DOMAIN_VIDEO;
        traits->kind = C2Component::KIND_DECODER;
        traits->rank = 0x80; // ahead of the software decoders (0x200)
        traits->mediaType = "video/avc";
        traits->owner = "default";
        mTraits = traits;
    }

    C2String getName() const override { return "default"; }

    c2_status_t createComponent(C2String name, std::shared_ptr<C2Component> *const component) override {
        if (name != mTraits->name) return C2_NOT_FOUND;
        return mFactory->createComponent(mNodeId++, component,
                                         std::default_delete<C2Component>());
    }

    c2_status_t createInterface(C2String name,
                                std::shared_ptr<C2ComponentInterface> *const interface) override {
        if (name != mTraits->name) return C2_NOT_FOUND;
        return mFactory->createInterface(mNodeId++, interface,
                                         std::default_delete<C2ComponentInterface>());
    }

    std::vector<std::shared_ptr<const C2Component::Traits>> listComponents() override {
        return {mTraits};
    }

    c2_status_t copyBuffer(std::shared_ptr<C2GraphicBuffer>, std::shared_ptr<C2GraphicBuffer>) override {
        return C2_OMITTED;
    }

    c2_status_t query_sm(const std::vector<C2Param *> &stackParams,
                         const std::vector<C2Param::Index> &heapParamIndices,
                         std::vector<std::unique_ptr<C2Param>> *const heapParams) const override {
        return mInterface.query(stackParams, heapParamIndices, C2_MAY_BLOCK, heapParams);
    }

    c2_status_t config_sm(const std::vector<C2Param *> &params,
                          std::vector<std::unique_ptr<C2SettingResult>> *const failures) override {
        return mInterface.config(params, C2_MAY_BLOCK, failures);
    }

    std::shared_ptr<C2ParamReflector> getParamReflector() const override { return mReflectorHelper; }

    c2_status_t querySupportedParams_nb(
            std::vector<std::shared_ptr<C2ParamDescriptor>> *const params) const override {
        return mInterface.querySupportedParams(params);
    }

    c2_status_t querySupportedValues_sm(std::vector<C2FieldSupportedValuesQuery> &fields) const override {
        return mInterface.querySupportedValues(fields, C2_MAY_BLOCK);
    }

  private:
    class Interface : public C2InterfaceHelper {
      public:
        explicit Interface(const std::shared_ptr<C2ReflectorHelper> &helper) : C2InterfaceHelper(helper) {
            setDerivedInstance(this);
            addParameter(DefineParam(mDmaBufUsageInfo, "dmabuf-usage")
                                 .withDefault(C2StoreDmaBufUsageInfo::AllocShared(128))
                                 .withFields({
                                         C2F(mDmaBufUsageInfo, m.usage)
                                                 .flags({C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE}),
                                         C2F(mDmaBufUsageInfo, m.capacity).inRange(0, UINT32_MAX, 1024),
                                         C2F(mDmaBufUsageInfo, m.allocFlags).flags({}),
                                         C2F(mDmaBufUsageInfo, m.heapName).any(),
                                 })
                                 .withSetter(SetDmaBufUsage)
                                 .build());
        }

      private:
        static C2R SetDmaBufUsage(bool /* mayBlock */, C2P<C2StoreDmaBufUsageInfo> &me) {
            strncpy(me.set().m.heapName, "system", me.v.flexCount());
            me.set().m.allocFlags = 0;
            return C2R::Ok();
        }

        std::shared_ptr<C2StoreDmaBufUsageInfo> mDmaBufUsageInfo;
    };

    std::shared_ptr<C2ReflectorHelper> mReflectorHelper;
    Interface mInterface;
    std::unique_ptr<C2ComponentFactory> mFactory;
    std::shared_ptr<C2Component::Traits> mTraits;
    std::atomic<c2_node_id_t> mNodeId{0};
};

int main(int /* argc */, char ** /* argv */) {
    signal(SIGPIPE, SIG_IGN);
    android::SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    ABinderProcess_setThreadPoolMaxThreadCount(kThreadCount);
    ABinderProcess_startThreadPool();

    using namespace ::aidl::android::hardware::media::c2;
    std::shared_ptr<IComponentStore> store =
            ::ndk::SharedRefBase::make<utils::ComponentStore>(std::make_shared<StoreImpl>());
    const std::string serviceName = std::string(IComponentStore::descriptor) + "/default";
    if (AServiceManager_addService(store->asBinder().get(), serviceName.c_str()) != EX_NONE) {
        LOG(ERROR) << "cannot register " << serviceName;
        return 1;
    }
    LOG(INFO) << serviceName << " registered (" << android::NvdecAvcDecName() << ")";
    ABinderProcess_joinThreadPool();
    return 0;
}
