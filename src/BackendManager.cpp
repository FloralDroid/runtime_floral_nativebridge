/*
 * Copyright 2026 FloralDroid
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "floral/nativebridge/BackendManager.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <utility>

#include <android/dlext.h>
#include <android-base/logging.h>
#include <android-base/properties.h>

namespace floral::nativebridge {
namespace {

constexpr char kInterfaceSymbol[] = "NativeBridgeItf";
constexpr uint32_t kRequiredNativeBridgeVersion = 3;
constexpr uint32_t kMaxNativeBridgeVersion = 6;

extern "C" void __floral_nativebridge_set_guest_arch(int arch)
    __attribute__((weak));
extern "C" android_namespace_t *android_get_exported_namespace(const char *name)
    __attribute__((weak));

enum FloralGuestArchitecture : int {
  kFloralGuestArm = 1,
  kFloralGuestArm64 = 2,
};

constexpr int GuestArchitecture() {
#if defined(__LP64__)
  return kFloralGuestArm64;
#else
  return kFloralGuestArm;
#endif
}

bool HasRequiredCallbacks(const android::NativeBridgeCallbacks *callbacks) {
  return callbacks != nullptr && callbacks->initialize != nullptr &&
         callbacks->loadLibrary != nullptr &&
         callbacks->getTrampoline != nullptr &&
         callbacks->isSupported != nullptr && callbacks->getAppEnv != nullptr &&
         callbacks->isCompatibleWith != nullptr &&
         callbacks->getSignalHandler != nullptr &&
         callbacks->unloadLibrary != nullptr && callbacks->getError != nullptr &&
         callbacks->isPathSupported != nullptr &&
         callbacks->initAnonymousNamespace != nullptr &&
         callbacks->createNamespace != nullptr &&
         callbacks->linkNamespaces != nullptr &&
         callbacks->loadLibraryExt != nullptr;
}

std::string ReadProcessName() {
  char buffer[512] = {};
  const int fd =
      TEMP_FAILURE_RETRY(open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC));
  if (fd < 0) {
    return "unknown";
  }
  const ssize_t size = TEMP_FAILURE_RETRY(read(fd, buffer, sizeof(buffer) - 1));
  close(fd);
  if (size <= 0) {
    return "unknown";
  }
  buffer[size] = '\0';
  return std::string(buffer);
}

std::string ArchitectureLibraryDirectory() {
#if defined(__LP64__)
  return "/system/lib64/";
#else
  return "/system/lib/";
#endif
}

void *OpenBackendLibrary(const std::string &path) {
  // Match ART's NativeBridge loading namespace. Falling back to the caller's
  // namespace would make the router observably different from libhoudini or
  // libndk_translation used directly by ART.
  if (android_get_exported_namespace != nullptr) {
    android_namespace_t *library_namespace =
        android_get_exported_namespace("system");
    if (library_namespace == nullptr) {
      library_namespace = android_get_exported_namespace("default");
    }
    if (library_namespace != nullptr) {
      android_dlextinfo extinfo = {};
      extinfo.flags = ANDROID_DLEXT_USE_NAMESPACE;
      extinfo.library_namespace = library_namespace;
      return android_dlopen_ext(path.c_str(), RTLD_LAZY, &extinfo);
    }
    LOG(WARNING) << "Floral NativeBridge: no exported system linker namespace "
                 << "while preloading " << path;
  } else {
    LOG(WARNING) << "Floral NativeBridge: exported namespace lookup unavailable "
                 << "while preloading " << path;
  }

  // This is retained for older incremental images whose libdl does not expose
  // the namespace lookup symbol. Production Android 12 images use the branch
  // above and therefore follow ART's system-namespace behavior.
  return dlopen(path.c_str(), RTLD_LAZY);
}

} // namespace

BackendManager::BackendManager()
    : policy_path_(android::base::GetProperty(
          "ro.floral.nativebridge.policy", PolicyEngine::kDefaultPath)) {}

BackendManager::~BackendManager() {
  for (const LoadedBackend &backend : loaded_backends_) {
    if (backend.handle != nullptr) {
      dlclose(backend.handle);
    }
  }
}

void BackendManager::SetError(std::string message) const {
  last_error_ = std::move(message);
  LOG(ERROR) << "Floral NativeBridge: " << last_error_;
}

const char *BackendManager::GetError() const {
  if (callbacks_ != nullptr && callbacks_->getError != nullptr) {
    const char *backend_error = callbacks_->getError();
    if (backend_error != nullptr && *backend_error != '\0') {
      last_error_ = backend_error;
    }
  }
  return last_error_.c_str();
}

std::string BackendManager::ProcessName() const { return ReadProcessName(); }

std::string BackendManager::PackageNameFromDataDir(const char *app_data_dir) {
  if (app_data_dir == nullptr || *app_data_dir == '\0') {
    return {};
  }
  std::string path(app_data_dir);
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  const size_t separator = path.find_last_of('/');
  return separator == std::string::npos ? path : path.substr(separator + 1);
}

void BackendManager::ConfigureProcessContext(const char *process_name,
                                             const char *app_data_dir) {
  if (callbacks_ != nullptr || initialized_) {
    return;
  }
  process_name_ = process_name == nullptr ? "" : process_name;
  package_name_ = PackageNameFromDataDir(app_data_dir);

  // This runs in the forked zygote child before dropping privileges. It can
  // safely read Android-owned 0600 state without exposing it to applications.
  const PolicyEngine policy = PolicyEngine::Load(policy_path_);
  selection_ = policy.Select(process_name_.empty() ? package_name_ : process_name_);
  selection_ = PolicyEngine::ApplyRuntimeState(
      std::move(selection_), process_name_.empty() ? package_name_ : process_name_);
  selection_prepared_ = true;

  // Guest identity is part of Floral's hybrid enhancement. Direct mode must
  // preserve the selected backend's original process identity and loader
  // behavior. Weak binding keeps hybrid usable on older incremental images.
  if (selection_.loader_mode == LoaderMode::kHybrid) {
    if (__floral_nativebridge_set_guest_arch != nullptr) {
      __floral_nativebridge_set_guest_arch(GuestArchitecture());
    } else {
      LOG(WARNING) << "Floral NativeBridge: libc guest identity hook unavailable";
    }
  }
}

std::string BackendManager::BackendPath(BackendKind kind) const {
  const std::string default_path = ArchitectureLibraryDirectory();
  switch (kind) {
  case BackendKind::kNdk:
    return android::base::GetProperty("ro.floral.nativebridge.ndk",
                                      default_path + "libndk_translation.so");
  case BackendKind::kHoudini:
    return android::base::GetProperty("ro.floral.nativebridge.houdini",
                                      default_path + "libhoudini.so");
  case BackendKind::kAuto:
    return {};
  }
  return {};
}

bool BackendManager::IsBackendCompatible(const LoadedBackend &backend,
                                         uint32_t bridge_version) const {
  if (bridge_version == 0 || bridge_version > kMaxNativeBridgeVersion ||
      backend.callbacks == nullptr || backend.callbacks->version < bridge_version ||
      backend.callbacks->version > kMaxNativeBridgeVersion) {
    return false;
  }
  return backend.callbacks->isCompatibleWith == nullptr ||
         backend.callbacks->isCompatibleWith(bridge_version);
}

bool BackendManager::LoadBackend(BackendKind kind, const std::string &path) {
  if (path.empty() || FindLoadedBackend(kind) != nullptr) {
    return !path.empty();
  }

  void *handle = OpenBackendLibrary(path);
  if (handle == nullptr) {
    const char *error = dlerror();
    LOG(WARNING) << "Floral NativeBridge: cannot preload "
                 << BackendKindName(kind) << " backend " << path << ": "
                 << (error == nullptr ? "unknown error" : error);
    return false;
  }

  auto *callbacks = reinterpret_cast<const android::NativeBridgeCallbacks *>(
      dlsym(handle, kInterfaceSymbol));
  const LoadedBackend backend{kind, path, handle, callbacks};
  if (!HasRequiredCallbacks(callbacks) ||
      !IsBackendCompatible(backend, kRequiredNativeBridgeVersion)) {
    LOG(WARNING) << "Floral NativeBridge: rejecting " << BackendKindName(kind)
                 << " backend " << path
                 << " because it does not provide a compatible Android 12 "
                    "NativeBridge interface";
    dlclose(handle);
    return false;
  }

  loaded_backends_.push_back(backend);
  LOG(INFO) << "Preloaded " << BackendKindName(kind)
            << " NativeBridge backend from " << path << " (interface v"
            << callbacks->version << ")";
  return true;
}

bool BackendManager::PreloadBackends() {
  if (backends_preloaded_) {
    return !loaded_backends_.empty();
  }
  backends_preloaded_ = true;

  // Backends may perform protected loader or JNI preparation from constructors.
  // Keeping both resident before the zygote forks preserves the original
  // NativeBridge startup order while selection remains per-process.
  constexpr BackendKind kBackends[] = {
      BackendKind::kNdk,
      BackendKind::kHoudini,
  };
  for (const BackendKind kind : kBackends) {
    LoadBackend(kind, BackendPath(kind));
  }
  return !loaded_backends_.empty();
}

const BackendManager::LoadedBackend *BackendManager::FindLoadedBackend(
    BackendKind kind) const {
  for (const LoadedBackend &backend : loaded_backends_) {
    if (backend.kind == kind) {
      return &backend;
    }
  }
  return nullptr;
}

bool BackendManager::LoadSelectedBackend(const BackendSelection &selection) {
  if (selection.exhausted) {
    SetError("NativeBridge backend candidates exhausted by Android runtime state");
    return false;
  }
  if (selection.loader_mode == LoaderMode::kDirect) {
    // Direct is an explicit, single-backend contract. Never walk another
    // candidate here: doing so would reintroduce recovery and make the
    // backend observable as a Floral-managed loader.
    if (selection.candidates.size() != 1 ||
        selection.candidates.front().mode != LoaderMode::kDirect ||
        selection.candidates.front().backend == BackendKind::kAuto) {
      SetError("direct NativeBridge policy must contain one explicit backend");
      return false;
    }
    const BackendCandidate candidate = selection.candidates.front();
    if (FindLoadedBackend(candidate.backend) == nullptr &&
        !LoadBackend(candidate.backend, BackendPath(candidate.backend))) {
      SetError("explicit direct NativeBridge backend could not be loaded");
      return false;
    }
  } else if (!PreloadBackends()) {
    SetError("no usable NativeBridge backend could be preloaded");
    return false;
  }
  const std::vector<BackendCandidate> default_candidates = {
      {BackendKind::kNdk, LoaderMode::kHybrid},
      {BackendKind::kHoudini, LoaderMode::kHybrid},
      {BackendKind::kNdk, LoaderMode::kDirect},
      {BackendKind::kHoudini, LoaderMode::kDirect},
  };
  const std::vector<BackendCandidate> &candidates = selection.candidates.empty()
                                                        ? default_candidates
                                                        : selection.candidates;
  for (const BackendCandidate candidate : candidates) {
    if (candidate.backend == BackendKind::kAuto) {
      continue;
    }
    const LoadedBackend *backend = FindLoadedBackend(candidate.backend);
    if (backend == nullptr) {
      continue;
    }

    callbacks_ = backend->callbacks;
    selection_.kind = candidate.backend;
    selection_.loader_mode = candidate.mode;
    selection_.name = BackendKindName(candidate.backend);
    selection_.reason = selection.reason;
    selected_path_ = backend->path;
    LOG(INFO) << "Selected " << selection_.name << " NativeBridge backend in "
              << LoaderModeName(selection_.loader_mode) << " mode from "
              << selected_path_ << " (interface v" << callbacks_->version << ")";
    return true;
  }

  SetError("no usable NativeBridge backend for policy " + selection.name);
  return false;
}

bool BackendManager::EnsureLoaded() {
  if (callbacks_ != nullptr) {
    return true;
  }
  if (!selection_prepared_) {
    // Non-zygote callers retain a conservative policy fallback.
    const PolicyEngine policy = PolicyEngine::Load(policy_path_);
    selection_ = policy.Select(ProcessName());
    selection_prepared_ = true;
  }
  return LoadSelectedBackend(selection_);
}

bool BackendManager::PrepareDirectBackend() {
  if (!selection_prepared_ || selection_.loader_mode != LoaderMode::kDirect) {
    return false;
  }
  return EnsureLoaded();
}

bool BackendManager::Initialize(
    const android::NativeBridgeRuntimeCallbacks *runtime_callbacks,
    const char *private_dir, const char *instruction_set) {
  if (initialized_) {
    return true;
  }
  if (!EnsureLoaded() || callbacks_->initialize == nullptr) {
    return false;
  }
  if (!callbacks_->initialize(runtime_callbacks, private_dir,
                              instruction_set)) {
    SetError("selected backend initialization failed");
    return false;
  }
  initialized_ = true;
  return true;
}

bool BackendManager::IsCompatibleWith(uint32_t bridge_version) {
  if (bridge_version == 0 ||
      bridge_version > kMaxNativeBridgeVersion) {
    return false;
  }
  if (callbacks_ != nullptr) {
    const LoadedBackend *backend = FindLoadedBackend(selection_.kind);
    return backend != nullptr && IsBackendCompatible(*backend, bridge_version);
  }
  if (!PreloadBackends()) {
    return false;
  }
  for (const LoadedBackend &backend : loaded_backends_) {
    if (IsBackendCompatible(backend, bridge_version)) {
      return true;
    }
  }
  return false;
}

void *BackendManager::LoadLibrary(const char *path, int flags) const {
  return callbacks_ != nullptr && callbacks_->loadLibrary != nullptr
             ? callbacks_->loadLibrary(path, flags)
             : nullptr;
}

void *BackendManager::GetTrampoline(void *handle, const char *name,
                                    const char *shorty, uint32_t len) const {
  void *symbol = callbacks_ != nullptr && callbacks_->getTrampoline != nullptr
                     ? callbacks_->getTrampoline(handle, name, shorty, len)
                     : nullptr;
  return symbol;
}

bool BackendManager::IsSupported(const char *path) const {
  return callbacks_ != nullptr && callbacks_->isSupported != nullptr &&
         callbacks_->isSupported(path);
}

const android::NativeBridgeRuntimeValues *
BackendManager::GetAppEnv(const char *instruction_set) const {
  return callbacks_ != nullptr && callbacks_->getAppEnv != nullptr
             ? callbacks_->getAppEnv(instruction_set)
             : nullptr;
}

android::NativeBridgeSignalHandlerFn
BackendManager::GetSignalHandler(int signal) const {
  return callbacks_ != nullptr && callbacks_->getSignalHandler != nullptr
             ? callbacks_->getSignalHandler(signal)
             : nullptr;
}

int BackendManager::UnloadLibrary(void *handle) const {
  return callbacks_ != nullptr && callbacks_->unloadLibrary != nullptr
             ? callbacks_->unloadLibrary(handle)
             : -1;
}

bool BackendManager::IsPathSupported(const char *path) const {
  return callbacks_ != nullptr && callbacks_->isPathSupported != nullptr &&
         callbacks_->isPathSupported(path);
}

bool BackendManager::InitAnonymousNamespace(
    const char *public_ns_sonames, const char *anon_ns_library_path) const {
  return callbacks_ != nullptr &&
         callbacks_->initAnonymousNamespace != nullptr &&
         callbacks_->initAnonymousNamespace(public_ns_sonames,
                                            anon_ns_library_path);
}

android::native_bridge_namespace_t *BackendManager::CreateNamespace(
    const char *name, const char *ld_library_path,
    const char *default_library_path, uint64_t type,
    const char *permitted_when_isolated_path,
    android::native_bridge_namespace_t *parent_ns) const {
  return callbacks_ != nullptr && callbacks_->createNamespace != nullptr
             ? callbacks_->createNamespace(
                   name, ld_library_path, default_library_path, type,
                   permitted_when_isolated_path, parent_ns)
             : nullptr;
}

bool BackendManager::LinkNamespaces(android::native_bridge_namespace_t *from,
                                    android::native_bridge_namespace_t *to,
                                    const char *shared_libs_sonames) const {
  return callbacks_ != nullptr && callbacks_->linkNamespaces != nullptr &&
         callbacks_->linkNamespaces(from, to, shared_libs_sonames);
}

void *
BackendManager::LoadLibraryExt(const char *path, int flags,
                               android::native_bridge_namespace_t *ns) const {
  return callbacks_ != nullptr && callbacks_->loadLibraryExt != nullptr
             ? callbacks_->loadLibraryExt(path, flags, ns)
             : nullptr;
}

android::native_bridge_namespace_t *BackendManager::GetVendorNamespace() const {
  return callbacks_ != nullptr && callbacks_->getVendorNamespace != nullptr
             ? callbacks_->getVendorNamespace()
             : nullptr;
}

android::native_bridge_namespace_t *
BackendManager::GetExportedNamespace(const char *name) const {
  return callbacks_ != nullptr && callbacks_->getExportedNamespace != nullptr
             ? callbacks_->getExportedNamespace(name)
             : nullptr;
}

void BackendManager::PreZygoteFork() {
  if (!PreloadBackends()) {
    return;
  }
  // Every resident v6 backend receives zygote preparation. Only the selected
  // backend is initialized in each child process.
  for (const LoadedBackend &backend : loaded_backends_) {
    if (backend.callbacks->version >= 6 &&
        backend.callbacks->preZygoteFork != nullptr) {
      backend.callbacks->preZygoteFork();
    }
  }
}

} // namespace floral::nativebridge
