# Floral Mixed NativeBridge

`libmixbridge.so` is a single Android NativeBridge entry point for x86 Android
images that need to run ARM applications. It selects one backend per process
and forwards the Android 12 NativeBridge v6 callbacks without placing
translation binaries in this repository.

Include `system/floral/nativebridge/nativebridge.mk` from an x86 Floral device
product to install the library and set `ro.dalvik.vm.native.bridge` to
`libmixbridge.so`. ARM-only products must not include that product fragment.

The Android 12 source tree also needs the companion ART patch set under
`redroid-patches/android-12.0.0_r32/art/`. It exposes the ART callback header,
routes native ELF files from a bridged classloader, passes the real process
identity before the zygote child drops privileges, exposes the per-process
compatibility loader mode, and provides package-scoped JNI loading diagnostics.
A companion frameworks/base patch persists selection and handles early native
and JNI-loader failures.

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
`houdini` first in the candidate list. Each backend contributes two internal
candidates in order: `hybrid`, then `compat`. `compat` gives the selected
backend first ownership of app-private native ELF and only falls back to the
host linker when that backend rejects the library. `default_backend` remains an
explicit default and wins when both fields are present. The `abi.public` lists are the ABI view exposed through
`Build.SUPPORTED_ABIS` to applications. Framework package loading keeps its
build-owned ABI list separately, so an x86 host ABI is not removed from the
native loader path. Missing `abi.public` uses the ARM compatibility default.

String rules remain supported. Object rules retain candidate order and an
optional explicit `selected_backend`. Explicit selections are never overridden.
For `auto`, Android records the process version and candidate results. A native
crash or the narrow translated-JNI `UnsatisfiedLinkError` failure within 60
seconds selects the next untried candidate. Only a foreground main process
restores its task; push and other helper/service processes restart independently
and no longer tear down the foreground task. Other Java crashes, ANRs, native
processes, and later crashes are unaffected. Each backend/mode candidate is
attempted once per application and system version. A candidate that survives the
60-second probation window is kept, so a later ordinary application crash cannot
poison the learned result.

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
namespaces. System-provided x86/x86_64 ELF files remain in the native namespace.
In `hybrid` mode, system-provided x86/x86_64 ELF files remain in the native
namespace and app-private native ELF follows the existing host-first routing. In
`compat` mode, platform paths remain host-owned, while app-private native ELF
first uses the selected translation backend and falls back to the native
namespace only when that backend rejects the library. The returned handle
records which owner must perform JNI and unload operations. ARM/ARM64 files and
paths whose ELF type cannot be read continue through the selected translation
backend. A single ELF dependency graph still cannot mix architectures.

The framework integration reserves 256 MiB for 32-bit WebView RELRO creation
by default. Products with an unusually large provider can override the byte
counts with `ro.floral.webview.vmsize32` and
`ro.floral.webview.vmsize64`; non-positive values fall back to the defaults.

The router forwards the backend's ABI environment rather than changing global
`ro.product.cpu.*` properties.

## Diagnostics

Unified diagnostics are disabled by default. A full package name is required;
the optional library filter accepts either a full path or a basename. The
package filter also covers child processes such as `:remote`. These `debug.*`
properties do not survive a reboot.

```shell
adb shell setprop debug.floral.nbdiag.package com.example.app
adb shell setprop debug.floral.nbdiag.library libsample.so
adb logcat -s FloralNBDiag
```

Events cover backend selection and initialization, `loadLibrary` and
`loadLibraryExt`, trampolines, the `JNI_OnLoad` result, and every method passed
to `RegisterNatives`. Clear the package property to stop tracing immediately:

```shell
adb shell setprop debug.floral.nbdiag.package ''
adb shell setprop debug.floral.nbdiag.library ''
```

`RegisterNatives` normally runs on the `JNI_OnLoad` thread and retains its
library attribution. A protected loader that registers from another thread is
reported with an `<unknown>` library while the package filter still captures
the registration.

## Checks

`floral_nativebridge_policy_test` validates policy precedence and
`floral_nativebridge_diagnostics_test` validates package and library filters.
The Android
`floral_nativebridge_probe` only opens a library and checks its exported
`NativeBridgeItf`; it never initializes two backends in one process.
