# Floral Mixed NativeBridge

`libmixbridge.so` is a single Android NativeBridge entry point for x86 Android
images that need to run ARM applications. It selects one backend per process
without placing translation binaries in this repository. Hybrid mode forwards
Android 12 NativeBridge v6 callbacks; direct mode asks ART to bind the selected
backend callback table itself. A process never keeps both translation runtimes
resident.

Include `system/floral/nativebridge/nativebridge.mk` from an x86 Floral device
product to install the library and set `ro.dalvik.vm.native.bridge` to
`libmixbridge.so`. ARM-only products must not include that product fragment.

The Android 12 source tree also needs the companion ART patch set under
`redroid-patches/android-12.0.0_r32/art/`. It exposes the ART callback header,
routes native ELF files from a bridged classloader, passes the real process
identity before the zygote child drops privileges, and limits mixed loading to
processes that explicitly select `hybrid`.
A companion frameworks/base patch persists selection and handles early native
and JNI-loader failures.

## Policy

The optional host policy is `/ipc/floral_stream/nativebridge.json`. Init copies
it into an Android-private cache at every boot.
Missing or malformed files use the built-in `auto` policy. A 64-bit process tries
NDK Translation first and Houdini second; a 32-bit process uses Houdini only.
Package rules take precedence over same-package process rules, and helper
processes inherit the package selection.
Restart the container after
changing the host file so init refreshes the cache. Android application UIDs do
not need access to the host file.

```json
{
  "version": 1,
  "preferred_backend": "ndk",
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
`houdini` first in the backend order. Automatic recovery keeps the enhanced
loader model for its complete pass; for example, the default order is
`ndk/hybrid`, `houdini/hybrid`. `direct` is available only to an explicit
object rule that sets both `selected_backend` and `selected_loader_mode`;
it only selects the backend;
NativeLoader namespaces, library ownership, and process identity retain the
original AOSP and backend behavior while ART invokes that backend's callbacks
without a `libmixbridge` forwarding hop. `default_backend` remains an
explicit default and wins when both fields are present. The `abi.public` lists are the ABI view exposed through
`Build.SUPPORTED_ABIS` to applications. Framework package loading keeps its
build-owned ABI list separately, so an x86 host ABI is not removed from the
native loader path. Missing `abi.public` uses the ARM compatibility default.

String rules remain supported. Object rules retain candidate order and an
optional explicit `selected_backend`. Explicit selections are never overridden.
An explicit object rule may set `selected_loader_mode` to `hybrid` or `direct`;
it defaults to `hybrid`. Direct selections are not monitored, learned, or
entered by automatic recovery.
For `auto`, system_server keeps the package version and candidate results in a
bounded in-memory table. A native crash or the translated-JNI/ELF loader
failure within 60 seconds selects the next untried candidate. Only a foreground main process
restores its task; push and other helper/service processes restart independently
and no longer tear down the foreground task. Other Java crashes, ANRs, native
processes, and later crashes are unaffected. Each hybrid backend candidate is
attempted once per application and system version. The successful candidate is
cached for 10 minutes only after every process in the package has exited. The
cache is bounded, is not written to `/data/system`, and is cleared with
system_server.

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
`ro.floral.nativebridge.runner.ndk64`, `houdini32`, and `houdini64`.
The `ndk32` runner is intentionally not a supported candidate.

The router does not monitor processes or switch a backend inside a live process.
Android itself owns early native-crash recovery and task restart without a host
agent. The selected backend is passed once from system_server through the zygote
specialization arguments; no runtime state file is used. Backends must
provide the NativeBridge v3 namespace interface used
by Android 12. A failed load, interface check, or initialization is reported to
ART, preventing two translation runtimes from sharing one process state.
Backend DSOs are loaded only after zygote fork with ART's system linker namespace
and `RTLD_NOW | RTLD_LOCAL`. A failed candidate is closed before the next one is
tried. Policy selection remains deferred until after zygote fork, and only the
selected backend is initialized. ART explicitly passes the nice name, app data
directory, and one-shot backend selection before privileges are dropped.

The product fragment enables `ro.floral.bridge.hybrid_elf=1`. With the
companion ART patch, a bridged classloader selected for `hybrid` owns both native
and bridged linker namespaces. System-provided x86/x86_64 ELF files retain host
ownership. PackageManager first scans the complete package for a public ARM ABI.
If any base or split APK contains ARM native libraries, only ARM ABIs are selected
and extracted. Host x86/x86_64 is selected only for packages with no ARM native
libraries. Every app-private ELF in a bridged process stays with the selected
translation backend, even if its ELF header advertises x86; it never falls back
to the host namespace. The returned handle records which owner must perform JNI
and unload operations. `direct` creates no Floral host namespace, performs no
Floral ELF split, and enables no Floral guest identity; ART owns the selected
backend handle and callback table exactly as it does for a standalone NativeBridge.
A single ELF dependency graph still cannot mix architectures.

NativeLoader links a guest classloader to the backend namespace using only the
public-library contract computed by Android. It does not preload
`libart.so`, `libnativeloader.so`, or `libdl_android.so` by soname as a
substitute for a namespace relationship. Backend runtime libraries remain
private to the backend's linker topology.

The framework integration reserves 256 MiB for 32-bit WebView RELRO creation
by default. Products with an unusually large provider can override the byte
counts with `ro.floral.webview.vmsize32` and
`ro.floral.webview.vmsize64`; non-positive values fall back to the defaults.

The router forwards the backend's ABI environment rather than changing global
`ro.product.cpu.*` properties.

## Namespace audit

The companion ART patch provides a persistent, opt-in namespace audit for
userdebug and eng builds. It is disabled by default and performs no additional
`dlopen`, JNI interception, stack collection, thread creation, or TLS tracking.
When enabled it records exported namespace handles, classloader namespace
creation, host and guest link ownership, list sizes, load ownership, and the
original linker error on failure.

Enable it before starting the process under test:

```sh
adb shell setprop persist.floral.nb.audit 1
adb logcat -c
adb shell am force-stop PACKAGE
adb shell monkey -p PACKAGE 1
adb logcat -d -v threadtime -s nativeloader:I
```

Disable it after collection because the property and its log volume persist
across reboot:

```sh
adb shell setprop persist.floral.nb.audit 0
```

## Checks

`floral_nativebridge_policy_test` validates policy precedence. The Android
`floral_nativebridge_probe` only opens a library and checks its exported
`NativeBridgeItf`; it never initializes two backends in one process.
