# Install the aggregate entry point used by ART's NativeBridge loader.
PRODUCT_PACKAGES += libmixbridge

PRODUCT_PROPERTY_OVERRIDES += \
    ro.dalvik.vm.native.bridge=libmixbridge.so
