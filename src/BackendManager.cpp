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

#include <android-base/logging.h>
#include <android-base/properties.h>

namespace floral::nativebridge {
namespace {

constexpr char kInterfaceSymbol[] = "NativeBridgeItf";
constexpr uint32_t kRequiredNativeBridgeVersion = 3;
constexpr uint32_t kMaxNativeBridgeVersion = 6;

bool HasRequiredCallbacks(const android::NativeBridgeCallbacks *callbacks) {
  return callbacks->initialize != nullptr &&
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

} // namespace

BackendManager::BackendManager()
    : policy_path_(android::base::GetProperty(
          "ro.floral.nativebridge.policy", PolicyEngine::kDefaultPath)) {}

BackendManager::~BackendManager() {
  if (backend_handle_ != nullptr) {
    dlclose(backend_handle_);
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

bool BackendManager::LoadSelectedBackend(const BackendSelection &selection) {
  const std::vector<BackendKind> default_candidates = {
      BackendKind::kNdk, BackendKind::kHoudini};
  const std::vector<BackendKind> &candidates = selection.candidates.empty()
                                                    ? default_candidates
                                                    : selection.candidates;
  for (const BackendKind kind : candidates) {
    if (kind == BackendKind::kAuto) {
      continue;
    }
    const std::string path = BackendPath(kind);
    if (path.empty()) {
      continue;
    }

    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      const char *error = dlerror();
      if (selection.kind != BackendKind::kAuto && candidates.size() == 1) {
        SetError("cannot load " + std::string(BackendKindName(kind)) +
                 " backend " + path + ": " +
                 (error == nullptr ? "unknown error" : error));
      }
      continue;
    }

    auto *callbacks = reinterpret_cast<const android::NativeBridgeCallbacks *>(
        dlsym(handle, kInterfaceSymbol));
    if (callbacks == nullptr ||
        callbacks->version < kRequiredNativeBridgeVersion ||
        !HasRequiredCallbacks(callbacks) ||
        !callbacks->isCompatibleWith(kRequiredNativeBridgeVersion)) {
      SetError("backend " + path +
               " does not provide the Android 12 NativeBridge namespace "
               "interface");
      dlclose(handle);
      continue;
    }

    backend_handle_ = handle;
    callbacks_ = callbacks;
    selection_.kind = kind;
    selection_.name = BackendKindName(kind);
    selection_.reason = selection.reason;
    selected_path_ = path;
    LOG(INFO) << "Loaded " << selection_.name << " NativeBridge backend from "
              << selected_path_ << " (interface v" << callbacks_->version
              << ")";
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
    // Non-zygote callers retain a conservative fallback for diagnostics.
    const PolicyEngine policy = PolicyEngine::Load(policy_path_);
    selection_ = policy.Select(ProcessName());
    selection_prepared_ = true;
  }
  return LoadSelectedBackend(selection_);
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
  // ART asks this before InitializeNativeBridge(), while the process is still
  // the zygote. Do not load a backend here or all app children would inherit
  // one zygote-wide selection and process rules could never take effect.
  if (callbacks_ != nullptr && callbacks_->isCompatibleWith != nullptr) {
    return callbacks_->isCompatibleWith(bridge_version);
  }
  return callbacks_ == nullptr || callbacks_->version >= bridge_version;
}

void *BackendManager::LoadLibrary(const char *path, int flags) const {
  return callbacks_ != nullptr && callbacks_->loadLibrary != nullptr
             ? callbacks_->loadLibrary(path, flags)
             : nullptr;
}

void *BackendManager::GetTrampoline(void *handle, const char *name,
                                    const char *shorty, uint32_t len) const {
  return callbacks_ != nullptr && callbacks_->getTrampoline != nullptr
             ? callbacks_->getTrampoline(handle, name, shorty, len)
             : nullptr;
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

void BackendManager::PreZygoteFork() const {
  if (callbacks_ != nullptr && callbacks_->preZygoteFork != nullptr) {
    callbacks_->preZygoteFork();
  }
}

} // namespace floral::nativebridge
