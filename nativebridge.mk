# Install the aggregate entry point used by ART's NativeBridge loader.
PRODUCT_PACKAGES += \
    floral_nativebridge_runner \
    libmixbridge \
    floral_msa_compat

PRODUCT_PROPERTY_OVERRIDES += \
    ro.dalvik.vm.native.bridge=libmixbridge.so \
    ro.floral.bridge.hybrid_elf=1
