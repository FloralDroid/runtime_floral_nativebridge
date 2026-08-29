# Build the AOSP guest userspace used by NativeBridge translations.
$(call inherit-product, frameworks/libs/native_bridge_support/native_bridge_support.mk)

# Install the aggregate entry point used by ART's NativeBridge loader.
PRODUCT_PACKAGES += \
    floral_nativebridge_runner \
    libmixbridge \
    floral_msa_compat

PRODUCT_PROPERTY_OVERRIDES += \
    ro.dalvik.vm.native.bridge=libmixbridge.so \
    ro.floral.nb.default_backend=auto \
    ro.floral.bridge.abi_virtualization=1 \
    ro.floral.bridge.hybrid_elf=0 \
    ro.floral.nb.ndk64=/system/lib64/libndk_translation.so \
    ro.floral.nb.houdini32=/system/floral/houdini/lib/libhoudini.so \
    ro.floral.nb.houdini64=/system/floral/houdini/lib64/libhoudini.so \
    ro.floral.nb.runner.ndk64=/system/bin/ndk_translation_program_runner_binfmt_misc_arm64 \
    ro.floral.nb.runner.houdini32=/system/floral/houdini/bin/houdini \
    ro.floral.nb.runner.houdini64=/system/floral/houdini/bin/houdini64
