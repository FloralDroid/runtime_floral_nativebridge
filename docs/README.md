# Floral Mixed NativeBridge

`libmixbridge.so` is a single Android NativeBridge entry point for x86 Android
images that need to run ARM applications. It selects one backend per process
without placing translation binaries in this repository. ART binds the selected
backend callback table directly; the router is only used to choose the backend
and is released before application initialization. A process never keeps both
translation runtimes resident.

Include `system/floral/nativebridge/nativebridge.mk` from an x86 Floral device
product to install the library and set `ro.dalvik.vm.native.bridge` to
`libmixbridge.so`. ARM-only products must not include that product fragment.

The Android 12 source tree also needs the companion ART, frameworks/base,
system/core, and linkerconfig changes from the Floral Android 12 patch series.
They route bridged native ELF files, pass the selected backend before the zygote
child drops privileges, mount backend-private linker configuration and
Houdini's process-private guest sysroot, and handle early native and JNI-loader
failures. NDK Translation uses the ARM guest userspace produced by the AOSP
build.

## Backend selection

No host policy or runtime JSON state file is used. For ARM64 applications,
`ro.floral.nb.default_backend=auto` starts with NDK Translation and
uses Houdini as the fallback. Setting the read-only property to `houdini`
reverses that order. ARM32 applications always use Houdini because the Android
12 NDK payload is ARM64-only.

Before each translated application fork, system_server selects one
`backend/direct` candidate. It keeps package version, failed candidates, and the
successful candidate in a bounded in-memory table. A native crash or supported
JNI/ELF loader failure within 60 seconds selects the next untried candidate for
the next fresh zygote launch. Only a foreground main process restores its task;
helper and service processes restart independently. Other Java crashes, ANRs,
native processes, and later crashes are unaffected. The successful candidate is
cached for 10 minutes after every process in the package exits. This state is
not persisted and is cleared with system_server.

The image generator routes ARM32 and ARM64 `binfmt_misc` registrations through
`/system/bin/floral_nativebridge_runner`. The runner honors the backend inherited
from a translated zygote application; otherwise ARM64 follows the global default
and ARM32 uses Houdini. Its `execv()` fallback only covers failure to start a
backend runner. It cannot switch runtimes after a backend runner has started.

Backend paths can be overridden with the read-only architecture-specific
properties `ro.floral.nb.ndk64`, `ro.floral.nb.houdini32`, and
`ro.floral.nb.houdini64`. The
NDK host payload uses its canonical `/system` paths and keeps its linker
configuration under `/system/floral/ndk`; Houdini remains under
`/system/floral/houdini`.
Standalone runner paths can be overridden with
`ro.floral.nb.runner.ndk64`, `ro.floral.nb.runner.houdini32`, and
`ro.floral.nb.runner.houdini64`.
The `ndk32` runner is intentionally not a supported candidate.

The router does not monitor processes or switch a backend inside a live process.
Android itself owns early native-crash recovery and task restart without a host
agent. The selected backend is passed once from system_server through the zygote
specialization arguments; no runtime state file is used. Backends must
provide the NativeBridge v3 namespace interface used
by Android 12. A failed load, interface check, or initialization is reported to
ART, preventing two translation runtimes from sharing one process state.
Backend DSOs are loaded only after zygote fork with ART's system linker namespace
and `RTLD_NOW | RTLD_LOCAL`. Only the selected backend is initialized. ART
explicitly passes the nice name, app data directory, and one-shot backend
selection before privileges are dropped. NDK uses the canonical AOSP guest
userspace and receives only its private linker configuration in the application's
mount namespace. For Houdini, zygote bind-mounts its guest libraries, binaries,
linker config, and cpuinfo over pre-created canonical targets in the
application's private mount namespace.

The product fragment disables `ro.floral.bridge.hybrid_elf` and selects direct
backend ownership. System-provided x86/x86_64 ELF files retain host ownership.
PackageManager first scans the complete package for a public ARM ABI.
If any base or split APK contains ARM native libraries, only ARM ABIs are selected
and extracted. Host x86/x86_64 is selected only for packages with no ARM native
libraries. Every app-private ELF in a bridged process stays with the selected
translation backend, even if its ELF header advertises x86; it never falls back
to the host namespace. The returned handle records which owner must perform JNI
and unload operations. Direct mode creates no Floral host namespace, performs no
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

`floral_nativebridge_policy_test` retains focused tests for the shared backend
selection data types. The Android `floral_nativebridge_probe` only opens a
library and checks its exported `NativeBridgeItf`; it never initializes two
backends in one process.
