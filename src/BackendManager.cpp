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

constexpr char kGuestSystemNamespaceName[] = "floral-guest-system";

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
  // A backend payload has a guest linker topology. Loading it through the
  // caller namespace can bind guest dependencies to host x86 libraries.
  if (android_get_exported_namespace == nullptr) {
    LOG(ERROR) << "Floral NativeBridge: exported namespace lookup unavailable "
               << "for " << path;
    return nullptr;
  }
  android_namespace_t *library_namespace =
      android_get_exported_namespace("system");
  if (library_namespace == nullptr) {
    // Android 12 names the /system namespace "default" outside the Runtime
    // APEX. Both are explicit linker namespaces; neither is the caller scope.
    library_namespace = android_get_exported_namespace("default");
  }
  if (library_namespace == nullptr) {
    LOG(ERROR) << "Floral NativeBridge: system linker namespace unavailable for "
               << path;
    return nullptr;
  }
  android_dlextinfo extinfo = {};
  extinfo.flags = ANDROID_DLEXT_USE_NAMESPACE;
  extinfo.library_namespace = library_namespace;
  return android_dlopen_ext(path.c_str(), RTLD_NOW | RTLD_LOCAL, &extinfo);
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
                                             const char *app_data_dir,
                                             const char *selected_backend) {
  if (callbacks_ != nullptr || initialized_) {
    return;
  }
  process_name_ = process_name == nullptr ? "" : process_name;
  package_name_ = PackageNameFromDataDir(app_data_dir);
  selected_backend_override_ =
      selected_backend == nullptr ? "" : selected_backend;

  // This runs in the forked zygote child before dropping privileges. It can
  // safely read Android-owned 0600 state without exposing it to applications.
  const PolicyEngine policy = PolicyEngine::Load(policy_path_);
  selection_ = policy.Select(process_name_.empty() ? package_name_ : process_name_);
  if (!selected_backend_override_.empty()) {
    const size_t separator = selected_backend_override_.find('/');
    const std::string backend_name = separator == std::string::npos
                                         ? selected_backend_override_
                                         : selected_backend_override_.substr(0, separator);
    const BackendKind selected = ParseBackendKind(backend_name);
    if (selected != BackendKind::kAuto) {
      selection_.kind = selected;
      selection_.loader_mode = LoaderMode::kHybrid;
      selection_.name = BackendKindName(selected);
      selection_.reason = "system server process selection";
      selection_.candidates = {{selected, LoaderMode::kHybrid}};
    } else {
      LOG(WARNING) << "Floral NativeBridge: ignoring invalid process backend "
                   << selected_backend_override_;
    }
  }
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

std::string BackendManager::GuestSystemLibraryDirectory(
    const char *instruction_set) {
  if (instruction_set != nullptr &&
      (strcmp(instruction_set, "arm64") == 0 ||
       strcmp(instruction_set, "arm64-v8a") == 0)) {
    return "/system/lib64/arm64";
  }
  return "/system/lib/arm";
}

bool BackendManager::CandidateSupportsInstructionSet(
    BackendKind kind, const char *instruction_set) {
  if (instruction_set == nullptr || *instruction_set == '\0') {
    return true;
  }
  if (strcmp(instruction_set, "arm") == 0 ||
      strcmp(instruction_set, "armeabi") == 0 ||
      strcmp(instruction_set, "armeabi-v7a") == 0) {
    return kind == BackendKind::kHoudini;
  }
  return kind == BackendKind::kNdk || kind == BackendKind::kHoudini;
}

bool BackendManager::EnsureGuestSystemNamespace(const char *instruction_set) {
  if (!UseHybridLoader() || guest_system_namespace_ready_) {
    return guest_system_namespace_ready_;
  }
  if (callbacks_ == nullptr || callbacks_->createNamespace == nullptr) {
    LOG(WARNING) << "Floral NativeBridge: selected backend cannot create a "
                    "guest system namespace";
    return false;
  }

  const std::string library_directory =
      GuestSystemLibraryDirectory(instruction_set);
  // This handle is created by the selected backend and is never passed to the
  // host linker. Keeping the guest sysroot in one backend-owned namespace is
  // required when a backend does not export Android's named namespaces.
  guest_system_namespace_ = callbacks_->createNamespace(
      kGuestSystemNamespaceName, nullptr, library_directory.c_str(), 0,
      library_directory.c_str(), nullptr);
  if (guest_system_namespace_ == nullptr) {
    LOG(WARNING) << "Floral NativeBridge: failed to create guest system "
                    "namespace for "
                 << library_directory;
    return false;
  }

  guest_system_namespace_ready_ = true;
  LOG(INFO) << "Floral NativeBridge: created guest system namespace at "
            << library_directory;
  return true;
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
  if (path.empty()) {
    return false;
  }
  if (!loaded_backends_.empty()) {
    return FindLoadedBackend(kind) != nullptr;
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
  LOG(INFO) << "Loaded " << BackendKindName(kind)
            << " NativeBridge backend from " << path << " (interface v"
            << callbacks->version << ")";
  return true;
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

bool BackendManager::LoadSelectedBackend(const BackendSelection &selection,
                                         const char *instruction_set) {
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
    if (!CandidateSupportsInstructionSet(candidate.backend, instruction_set)) {
      SetError("explicit direct NativeBridge backend does not support instruction set");
      return false;
    }
    if (FindLoadedBackend(candidate.backend) == nullptr &&
        !LoadBackend(candidate.backend, BackendPath(candidate.backend))) {
      SetError("explicit direct NativeBridge backend could not be loaded");
      return false;
    }
    const LoadedBackend *backend = FindLoadedBackend(candidate.backend);
    if (backend == nullptr) {
      SetError("explicit direct NativeBridge backend disappeared after loading");
      return false;
    }
    callbacks_ = backend->callbacks;
    selection_.kind = candidate.backend;
    selection_.loader_mode = LoaderMode::kDirect;
    selection_.name = BackendKindName(candidate.backend);
    selection_.reason = selection.reason;
    selected_path_ = backend->path;
    LOG(INFO) << "Selected " << selection_.name
              << " NativeBridge backend in direct mode from " << selected_path_
              << " (interface v" << callbacks_->version << ")";
    // Direct mode is a transparent single-backend contract. Do not continue
    // into the hybrid candidate loop or initialize Floral guest namespaces.
    return true;
  }
  const std::vector<BackendCandidate> default_candidates = {
      {BackendKind::kNdk, LoaderMode::kHybrid},
      {BackendKind::kHoudini, LoaderMode::kHybrid},
  };
  const std::vector<BackendCandidate> &candidates = selection.candidates.empty()
                                                        ? default_candidates
                                                        : selection.candidates;
  for (const BackendCandidate candidate : candidates) {
    if (candidate.backend == BackendKind::kAuto) {
      continue;
    }
    if (!CandidateSupportsInstructionSet(candidate.backend, instruction_set)) {
      LOG(INFO) << "Skipping " << BackendKindName(candidate.backend)
                << " for instruction set "
                << (instruction_set == nullptr ? "<unknown>" : instruction_set);
      continue;
    }
    const LoadedBackend *backend = FindLoadedBackend(candidate.backend);
    if (backend == nullptr &&
        !LoadBackend(candidate.backend, BackendPath(candidate.backend))) {
      continue;
    }
    backend = FindLoadedBackend(candidate.backend);
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
  return LoadSelectedBackend(selection_, instruction_set_.c_str());
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
  instruction_set_ = instruction_set == nullptr ? "" : instruction_set;
  if (!EnsureLoaded() || callbacks_->initialize == nullptr) {
    return false;
  }
  if (!callbacks_->initialize(runtime_callbacks, private_dir,
                              instruction_set)) {
    SetError("selected backend initialization failed");
    return false;
  }
  initialized_ = true;
  // Hybrid routing needs an explicit guest system parent when the backend
  // does not export a named system/default namespace. A failure is
  // non-fatal so the original NativeBridge fallback remains available.
  EnsureGuestSystemNamespace(instruction_set);
  return true;
}

bool BackendManager::IsCompatibleWith(uint32_t bridge_version) {
  if (bridge_version == 0 ||
      bridge_version > kMaxNativeBridgeVersion) {
    return false;
  }
  // ART asks this before process context and instruction set are available.
  // The aggregate bridge owns the complete callback table; payload validation
  // is deferred to the one backend selected during Initialize().
  return true;
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
  if (callbacks_ != nullptr && callbacks_->getExportedNamespace != nullptr) {
    android::native_bridge_namespace_t *exported =
        callbacks_->getExportedNamespace(name);
    if (exported != nullptr) {
      return exported;
    }
  }
  if (UseHybridLoader() && guest_system_namespace_ready_ && name != nullptr &&
      (strcmp(name, "system") == 0 || strcmp(name, "default") == 0)) {
    return guest_system_namespace_;
  }
  return nullptr;
}

void BackendManager::PreZygoteFork() {
  // Payloads are deliberately not loaded in zygote. Their constructors and
  // callback state must belong to exactly one application process.
}

} // namespace floral::nativebridge
