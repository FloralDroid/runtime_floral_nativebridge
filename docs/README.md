# Floral Mixed NativeBridge

`libmixbridge.so` is a single Android NativeBridge entry point for x86 Android
images that need to run ARM applications. It selects one backend per process
and forwards the Android 12 NativeBridge v6 callbacks without placing
translation binaries in this repository.

Include `system/floral/nativebridge/nativebridge.mk` from an x86 Floral device
product to install the library and set `ro.dalvik.vm.native.bridge` to
`libmixbridge.so`. ARM-only products must not include that product fragment.

The Android 12 source tree also needs the companion patches
`redroid-patches/android-12.0.0_r32/art/0001-expose-nativebridge-headers-to-floral.patch`
and `0002-support-hybrid-nativebridge-elf-loading.patch` before Soong can consume
the ART callback header and route native ELF files from a bridged classloader.

## Policy

The optional policy file is `/ipc/floral_stream/nativebridge.json`.
Missing or malformed files use the built-in `auto` policy. `auto` tries NDK
Translation first and Houdini second. A process rule matches the full process
name first and then the package portion before `:`. Each application process
reloads the policy while initializing NativeBridge, so a changed policy takes
effect after that application process is stopped and restarted. The host must
make the file readable but not writable by application UIDs, for example mode
`0644`.

```json
{
  "default_backend": {
    "mode": "auto",
    "candidates": ["ndk", "houdini"]
  },
  "packages": {
    "com.example.game": {
      "mode": "auto",
      "candidates": ["houdini", "ndk"],
      "selected_backend": "houdini"
    },
    "com.example.game:remote": "ndk"
  }
}
```

String rules remain supported. Object rules retain candidate order and an
optional explicit `selected_backend`. The router uses `selected_backend` when
present; otherwise an `auto` rule tries `candidates` in order while the app
process initializes.

Backend paths can be overridden with the read-only properties
`ro.floral.nativebridge.ndk` and `ro.floral.nativebridge.houdini`. The default
paths are architecture-aware `/system/lib64/...` or `/system/lib/...` paths.

The router deliberately does not monitor the process or fall back after a
backend has initialized. Android remains responsible for process lifecycle,
crash handling, and ANR handling. Backends must provide the NativeBridge v3 namespace interface used
by Android 12. A failed load, interface check, or initialization is reported to
ART, preventing two translation runtimes from sharing one process state.
Policy loading and backend loading are deferred until `initialize`, after
zygote fork, so process rules are evaluated in the application process rather
than being fixed by the zygote.

The product fragment enables `ro.floral.nativebridge.hybrid_elf=1`. With the
companion ART patch, a bridged classloader owns both native and bridged linker
namespaces. Dynamically extracted x86/x86_64 ELF files loaded by absolute path
use the native namespace; ARM/ARM64 files and paths whose ELF type cannot be
read continue through the selected translation backend. A single ELF
dependency graph still cannot mix architectures.

## Scope

This first version selects by process name. Android 12 does not pass package,
certificate, or version metadata to NativeBridge callbacks, so those rules
require a later ART integration. The router also forwards the backend's ABI
environment rather than changing global `ro.product.cpu.*` properties.

## Checks

`floral_nativebridge_policy_test` validates policy precedence. The Android
`floral_nativebridge_probe` only opens a library and checks its exported
`NativeBridgeItf`; it never initializes two backends in one process.
