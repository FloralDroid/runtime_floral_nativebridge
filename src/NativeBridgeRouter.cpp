/*
 * Copyright 2026 FloralDroid
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "floral/nativebridge/BackendManager.h"

namespace floral::nativebridge {
namespace {

BackendManager &Manager() {
  // ART calls NativeBridge callbacks from a single initialized bridge instance.
  static BackendManager manager;
  return manager;
}

void ConfigureProcessContext(const char *process_name,
                             const char *app_data_dir,
                             const char *selected_backend) {
  Manager().ConfigureProcessContext(process_name, app_data_dir,
                                    selected_backend);
}

bool Initialize(const android::NativeBridgeRuntimeCallbacks *runtime_callbacks,
                const char *private_dir, const char *instruction_set) {
  return Manager().Initialize(runtime_callbacks, private_dir, instruction_set);
}

void *LoadLibrary(const char *path, int flags) {
  return Manager().LoadLibrary(path, flags);
}

void *GetTrampoline(void *handle, const char *name, const char *shorty,
                    uint32_t len) {
  return Manager().GetTrampoline(handle, name, shorty, len);
}

bool IsSupported(const char *path) { return Manager().IsSupported(path); }

const android::NativeBridgeRuntimeValues *
GetAppEnv(const char *instruction_set) {
  return Manager().GetAppEnv(instruction_set);
}

bool IsCompatibleWith(uint32_t bridge_version) {
  return Manager().IsCompatibleWith(bridge_version);
}

android::NativeBridgeSignalHandlerFn GetSignalHandler(int signal) {
  return Manager().GetSignalHandler(signal);
}

int UnloadLibrary(void *handle) { return Manager().UnloadLibrary(handle); }

const char *GetError() { return Manager().GetError(); }

bool IsPathSupported(const char *path) {
  return Manager().IsPathSupported(path);
}

bool InitAnonymousNamespace(const char *public_ns_sonames,
                            const char *anon_ns_library_path) {
  return Manager().InitAnonymousNamespace(public_ns_sonames,
                                          anon_ns_library_path);
}

android::native_bridge_namespace_t *
CreateNamespace(const char *name, const char *ld_library_path,
                const char *default_library_path, uint64_t type,
                const char *permitted_when_isolated_path,
                android::native_bridge_namespace_t *parent_ns) {
  return Manager().CreateNamespace(name, ld_library_path, default_library_path,
                                   type, permitted_when_isolated_path,
                                   parent_ns);
}

bool LinkNamespaces(android::native_bridge_namespace_t *from,
                    android::native_bridge_namespace_t *to,
                    const char *shared_libs_sonames) {
  return Manager().LinkNamespaces(from, to, shared_libs_sonames);
}

void *LoadLibraryExt(const char *path, int flags,
                     android::native_bridge_namespace_t *ns) {
  return Manager().LoadLibraryExt(path, flags, ns);
}

android::native_bridge_namespace_t *GetVendorNamespace() {
  return Manager().GetVendorNamespace();
}

android::native_bridge_namespace_t *GetExportedNamespace(const char *name) {
  return Manager().GetExportedNamespace(name);
}

void PreZygoteFork() { Manager().PreZygoteFork(); }

bool UseHybridLoader() { return Manager().UseHybridLoader(); }

const char *PrepareDirectBackend() {
  if (!Manager().PrepareDirectBackend()) {
    return nullptr;
  }
  return Manager().selected_path().c_str();
}

} // namespace
} // namespace floral::nativebridge

// ART discovers this optional Floral extension before the zygote child drops
// privileges. It is intentionally separate from the stable NativeBridge ABI.
extern "C" void FloralNativeBridgeSetProcessContext(const char *process_name,
                                                    const char *app_data_dir,
                                                    const char *selected_backend) {
  floral::nativebridge::ConfigureProcessContext(process_name, app_data_dir,
                                                selected_backend);
}

// ART uses this per-process bit only to enable Floral's hybrid enhancement.
// A false result leaves the platform NativeLoader behavior unchanged.
extern "C" bool FloralNativeBridgeUseHybridLoader() {
  return floral::nativebridge::UseHybridLoader();
}

// ART calls this after process context selection and before pre-initializing
// the bridge. The returned path identifies the backend ART must bind directly.
extern "C" const char *FloralNativeBridgePrepareDirectBackend() {
  return floral::nativebridge::PrepareDirectBackend();
}

// Keep the symbol name exactly as expected by ART's libnativebridge loader.
extern "C" android::NativeBridgeCallbacks NativeBridgeItf{
    .version = 6,
    .initialize = &floral::nativebridge::Initialize,
    .loadLibrary = &floral::nativebridge::LoadLibrary,
    .getTrampoline = &floral::nativebridge::GetTrampoline,
    .isSupported = &floral::nativebridge::IsSupported,
    .getAppEnv = &floral::nativebridge::GetAppEnv,
    .isCompatibleWith = &floral::nativebridge::IsCompatibleWith,
    .getSignalHandler = &floral::nativebridge::GetSignalHandler,
    .unloadLibrary = &floral::nativebridge::UnloadLibrary,
    .getError = &floral::nativebridge::GetError,
    .isPathSupported = &floral::nativebridge::IsPathSupported,
    .initAnonymousNamespace = &floral::nativebridge::InitAnonymousNamespace,
    .createNamespace = &floral::nativebridge::CreateNamespace,
    .linkNamespaces = &floral::nativebridge::LinkNamespaces,
    .loadLibraryExt = &floral::nativebridge::LoadLibraryExt,
    .getVendorNamespace = &floral::nativebridge::GetVendorNamespace,
    .getExportedNamespace = &floral::nativebridge::GetExportedNamespace,
    .preZygoteFork = &floral::nativebridge::PreZygoteFork,
};
