# Install the aggregate entry point used by ART's NativeBridge loader.
PRODUCT_PACKAGES += \
    floral_nativebridge_runner \
    libmixbridge \
    floral_msa_compat

PRODUCT_PROPERTY_OVERRIDES += \
    ro.dalvik.vm.native.bridge=libmixbridge.so \
    ro.floral.nativebridge.default_backend=auto \
    ro.floral.bridge.abi_virtualization=1 \
    ro.floral.bridge.hybrid_elf=1 \
    ro.floral.nativebridge.ndk64=/system/floral/ndk/lib64/libndk_translation.so \
    ro.floral.nativebridge.houdini32=/system/floral/houdini/lib/libhoudini.so \
    ro.floral.nativebridge.houdini64=/system/floral/houdini/lib64/libhoudini.so \
    ro.floral.nativebridge.runner.ndk64=/system/floral/ndk/bin/ndk_translation_program_runner_binfmt_misc_arm64 \
    ro.floral.nativebridge.runner.houdini32=/system/floral/houdini/bin/houdini \
    ro.floral.nativebridge.runner.houdini64=/system/floral/houdini/bin/houdini64
