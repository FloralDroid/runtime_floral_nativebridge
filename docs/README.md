# Floral Mixed NativeBridge

`libmixbridge.so` is a single Android NativeBridge entry point for x86 Android
images that need to run ARM applications. It selects one backend per process
and forwards the Android 12 NativeBridge v6 callbacks without placing
translation binaries in this repository.

Include `system/floral/nativebridge/nativebridge.mk` from an x86 Floral device
product to install the library and set `ro.dalvik.vm.native.bridge` to
`libmixbridge.so`. ARM-only products must not include that product fragment.

The Android 12 source tree also needs three companion ART patches,
`redroid-patches/android-12.0.0_r32/art/0001-expose-nativebridge-headers-to-floral.patch`
through `0003-pass-Floral-NativeBridge-process-context.patch`. They expose the ART
callback header, route native ELF files from a bridged classloader, and pass the
real process identity before the zygote child drops privileges. A companion
frameworks/base patch persists selection and handles early native crashes.

## Policy

The optional host policy is `/ipc/floral_stream/nativebridge.json`. Init copies
it into an Android-private cache at every boot.
Missing or malformed files use the built-in `auto` policy. `auto` tries NDK
Translation first and Houdini second. A process rule matches the full process
name first and then the package portion before `:`. Restart the container after
changing the host file so init refreshes the cache. Android application UIDs do
not need access to the host file.

```json
{
  "version": 1,
  "preferred_backend": "houdini",
  "abi": {
    "public": {
      "64": ["arm64-v8a"],
      "32": ["armeabi-v7a", "armeabi"]
    }
  },
  "executables": {
    "/vendor/bin/arm-helper": "ndk"
  },
  "packages": {
    "com.example.game": {
      "mode": "auto",
      "candidates": ["houdini", "ndk"],
      "executables": {
        "libzs-local.so": "houdini"
      }
    },
    "com.example.game:remote": "ndk"
  }
}
```

`preferred_backend` keeps automatic recovery enabled and places `ndk` or
`houdini` first in the candidate list. `default_backend` remains an explicit
default and wins when both fields are present. The `abi.public` lists are the ABI view exposed through
`Build.SUPPORTED_ABIS` to applications. Framework package loading keeps its
build-owned ABI list separately, so an x86 host ABI is not removed from the
native loader path. Missing `abi.public` uses the ARM compatibility default.

String rules remain supported. Object rules retain candidate order and an
optional explicit `selected_backend`. Explicit selections are never overridden.
For `auto`, Android records the process version and candidate results. A native
crash in a translated ARM process within 60 seconds selects the next untried
candidate. Only a foreground main process restores its task; push and other
helper/service processes restart independently and no longer tear down the
foreground task. Java crashes, ANRs, native processes, and later crashes are
unaffected. Each candidate is attempted once per application and system
version. A backend that survives the 60-second probation window is kept, so a
later ordinary application crash cannot poison the learned result.

Top-level `executables` entries are system-wide full-path rules. Entries inside
a package object accept a full path, basename, or `*`, and package rules take
precedence. The Android 12 image generator routes ARM32 and ARM64
`binfmt_misc` registrations through `/system/bin/floral_nativebridge_runner`,
which inspects the ELF class and executes the configured NDK or Houdini runner.
A package rule is not used when ownership cannot be determined uniquely from
the executable path.

Backend paths can be overridden with the read-only properties
`ro.floral.nativebridge.ndk` and `ro.floral.nativebridge.houdini`. The default
paths are architecture-aware `/system/lib64/...` or `/system/lib/...` paths.
Standalone runner paths can be overridden with
`ro.floral.nativebridge.runner.ndk32`, `ndk64`, `houdini32`, and `houdini64`.

The router does not monitor processes or switch a backend inside a live process.
Android itself owns early native-crash recovery and task restart without a host
agent. Recovery state is written with `AtomicFile` to
`/data/system/floral/nativebridge-state.json`; SQLite is not used. Backends must
provide the NativeBridge v3 namespace interface used
by Android 12. A failed load, interface check, or initialization is reported to
ART, preventing two translation runtimes from sharing one process state.
Policy and backend loading are deferred until after zygote fork. ART explicitly
passes the nice name and app data directory before privileges are dropped, so
selection no longer depends on an early `/proc/self/cmdline` value.

The product fragment enables `ro.floral.nativebridge.hybrid_elf=1`. With the
companion ART patch, a bridged classloader owns both native and bridged linker
namespaces. Dynamically extracted x86/x86_64 ELF files loaded by absolute path
use the native namespace; ARM/ARM64 files and paths whose ELF type cannot be
read continue through the selected translation backend. A single ELF
dependency graph still cannot mix architectures.

The router forwards the backend's ABI environment rather than changing global
`ro.product.cpu.*` properties.

## Checks

`floral_nativebridge_policy_test` validates policy precedence. The Android
`floral_nativebridge_probe` only opens a library and checks its exported
`NativeBridgeItf`; it never initializes two backends in one process.
