#
# Copyright (C) 2020 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Only include Shield apps for first party targets
ifneq ($(filter $(word 2,$(subst _, ,$(TARGET_PRODUCT))), galen galen_tab),)
include device/nvidia/shield-common/shield.mk
endif

TARGET_REFERENCE_DEVICE ?= galen
TARGET_TEGRA_VARIANT    ?= common

TARGET_TEGRA_MODELS := $(shell awk -F, '/tegra_init::devices/{ f = 1; next } /};/{ f = 0 } f{ gsub(/"/, "", $$3); gsub(/ /, "", $$3); print $$3 }' device/nvidia/$(TARGET_REFERENCE_DEVICE)/init/init_$(TARGET_REFERENCE_DEVICE).cpp |sort |uniq)

TARGET_KERNEL_VERSION ?= 6.18
TARGET_BOOT_HAL       ?= smd
TARGET_LIGHT_HAL      ?= tegra

TARGET_HAS_BATTERY    ?= false

# Root console + crash logcat on the debug UART (bring-up).
TARGET_CONSOLE_AS_ROOT         ?= true
TARGET_ENABLE_LOGCAT_TO_SERIAL ?= true

include device/nvidia/t194-common/t194.mk

# System properties
include device/nvidia/galen/system_prop.mk

PRODUCT_CHARACTERISTICS   := tv
PRODUCT_AAPT_PREBUILT_DPI := xxhdpi xhdpi hdpi mdpi hdpi tvdpi
PRODUCT_AAPT_PREF_CONFIG  := xhdpi

$(call inherit-product, frameworks/native/build/tablet-10in-xhdpi-2048-dalvik-heap.mk)

PRODUCT_USE_DYNAMIC_PARTITIONS := true

include device/nvidia/galen/vendor/galen-vendor.mk

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += device/nvidia/galen

# Init related
PRODUCT_COPY_FILES += \
    $(foreach model,$(TARGET_TEGRA_MODELS),device/nvidia/galen/initfiles/fstab.galen:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.$(model)) \
    $(foreach model,$(TARGET_TEGRA_MODELS),device/nvidia/galen/initfiles/fstab.galen:$(TARGET_COPY_OUT_RAMDISK)/fstab.$(model)) \
    $(foreach model,$(TARGET_TEGRA_MODELS),device/nvidia/galen/initfiles/init.$(model).rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.$(model).rc) \
    $(foreach model,$(TARGET_TEGRA_MODELS),device/nvidia/galen/initfiles/init.recovery.galen.rc:$(TARGET_COPY_OUT_RECOVERY)/root/init.recovery.$(model).rc) \
    $(foreach model,$(TARGET_TEGRA_MODELS),device/nvidia/galen/initfiles/power.galen.rc:$(TARGET_COPY_OUT_ODM)/etc/power.$(model).rc) \
    device/nvidia/galen/initfiles/init.galen_common.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.galen_common.rc

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.audio.low_latency.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.audio.low_latency.xml \
    frameworks/native/data/etc/android.hardware.ethernet.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.ethernet.xml

# Audio
ifeq ($(TARGET_AUDIO_HAL),baylibre)
PRODUCT_COPY_FILES += \
    device/nvidia/tegra-common/audio/primary_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/primary_audio_policy_configuration.xml
endif

# Fingerprint override
PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildFingerprint=NVIDIA/galen/galen:11/RQ1A.210105.003/13961456_3871.0251:user/release-keys

# Loadable kernel modules
PRODUCT_PACKAGES += \
    lkm_loader
PRODUCT_COPY_FILES += \
    device/nvidia/tegra-common/initfiles/init.lkm.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.lkm.rc \
    device/nvidia/galen/initfiles/lkm.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/lkm.rc

# Power
ifeq ($(TARGET_POWER_HAL),perfmgr-lineage)
PRODUCT_PACKAGES += \
    powerhint.galen.json
endif

# Shipping API
PRODUCT_SHIPPING_API_LEVEL := 37

# Thermal
ifeq ($(TARGET_THERMAL_HAL),tegra)
PRODUCT_COPY_FILES += \
    $(foreach model,$(TARGET_TEGRA_MODELS),device/nvidia/galen/thermal/thermalhal.$(model).xml:$(TARGET_COPY_OUT_VENDOR)/etc/thermalhal.$(model).xml)
endif

# Updater
ifneq ($(TARGET_BOOT_HAL),)
AB_OTA_PARTITIONS += \
    boot \
    product \
    recovery \
    system \
    system_dlkm \
    system_ext \
    vbmeta \
    vbmeta_system \
    vendor \
    vendor_boot \
    odm
ifeq ($(TARGET_BOOT_HAL),smd)
AB_OTA_POSTINSTALL_CONFIG += \
    FILESYSTEM_TYPE_system=ext4 \
    POSTINSTALL_OPTIONAL_system=true \
    POSTINSTALL_PATH_system=system/bin/otapreopt_script \
    RUN_POSTINSTALL_system=true \
    FILESYSTEM_TYPE_product=ext4 \
    POSTINSTALL_OPTIONAL_product=true \
    POSTINSTALL_PATH_product=bin/nv_bootloader_payload_updater \
    RUN_POSTINSTALL_product=true
PRODUCT_PACKAGES += \
    nv_bootloader_payload_updater.product
endif
endif

# GV11B (Xavier GPU) firmware for nouveau, repacked from NVIDIA L4T R35
# (nouveau-gv11b repack-gv11b-fw.py). Local bring-up only: NVIDIA firmware,
# not in git (see .gitignore).
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,device/nvidia/galen/firmware-gv11b,$(TARGET_COPY_OUT_VENDOR)/firmware/nvidia/gv11b)

# Vulkan via NVK. NVK refuses Tegra unless opted in (it is not conformance-tested there); Mesa reads
# NVK_I_WANT_A_BROKEN_VULKAN_DRIVER from this vendor property on Android. The UI keeps using GL
# (RenderEngine pinned; HWUI already defaults to GL: ro.hwui.use_vulkan is set empty elsewhere) and no android.hardware.vulkan.* features are declared yet, so only
# programs that ask for Vulkan explicitly get NVK. Volta tensor cores (VK_KHR_cooperative_matrix)
# are opt-in in NVK. nouveau.uvmm_sysmem_vol=0 (BoardConfig.mk) maps VM_BIND system memory
# L2-cacheable, which needs the NVK L2 flushes around every exec; apps cannot read the module
# parameter under SELinux, so the property turns the flushes on.
PRODUCT_VENDOR_PROPERTIES += \
    ro.hardware.vulkan=nouveau \
    vendor.mesa.nvk.i.want.a.broken.vulkan.driver=1 \
    vendor.mesa.nvk.volta.cmat=1 \
    vendor.mesa.nvk.l2.sysmem.maint=1 \
    debug.renderengine.backend=skiaglthreaded

# llama.cpp with the Vulkan backend (llama/Android.bp), in /system_ext/bin
PRODUCT_PACKAGES += \
    llama-bench \
    llama-cli \
    llama-perplexity \
    llama-server \
    test-backend-ops

# Codec2: NVDEC H.264 decoder (codec2/)
PRODUCT_PACKAGES += \
    android.hardware.media.c2-default-service
