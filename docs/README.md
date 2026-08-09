# Floral Mixed NativeBridge

`libmixbridge.so` is a single Android NativeBridge entry point for x86 Android
images that need to run ARM applications. It selects one backend per process
and forwards the Android 12 NativeBridge v6 callbacks without placing
translation binaries in this repository.

Include `system/floral/nativebridge/nativebridge.mk` from an x86 Floral device
product to install the library and set `ro.dalvik.vm.native.bridge` to
`libmixbridge.so`. ARM-only products must not include that product fragment.

The Android 12 source tree also needs the companion patch
`redroid-patches/android-12.0.0_r32/art/0001-expose-nativebridge-headers-to-floral.patch`
before Soong can consume the ART callback header from this system library.

## Policy

The optional policy file is `/mnt/vendor/floral_stream/nativebridge.json`.
Missing or malformed files use the built-in `auto` policy. `auto` tries NDK
Translation first and Houdini second. A process rule matches the full process
name first and then the package portion before `:`.

```json
{
  "default_backend": "auto",
  "packages": {
    "com.example.game": "houdini",
    "com.example.game:remote": "ndk"
  }
}
```

Backend paths can be overridden with the read-only properties
`ro.floral.nativebridge.ndk` and `ro.floral.nativebridge.houdini`. The default
paths are architecture-aware `/system/lib64/...` or `/system/lib/...` paths.

The router deliberately does not fall back after a backend has initialized in
a process. A failed load or initialization is reported to ART, preventing two
translation runtimes from sharing one process state.

## Scope

This first version selects by process name. Android 12 does not pass package,
certificate, or version metadata to NativeBridge callbacks, so those rules
require a later ART integration. The router also forwards the backend's ABI
environment rather than changing global `ro.product.cpu.*` properties.

## Checks

`floral_nativebridge_policy_test` validates policy precedence. The Android
`floral_nativebridge_probe` only opens a library and checks its exported
`NativeBridgeItf`; it never initializes two backends in one process.
