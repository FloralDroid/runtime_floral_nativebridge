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
#include <cstdlib>
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
extern "C" android_namespace_t *android_get_exported_namespace(const char *name);

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
constexpr char kAuditProperty[] = "persist.floral.nb.audit";
constexpr char kProcessBackendEnvironment[] = "FLORAL_NATIVEBRIDGE_BACKEND";

bool AuditEnabled() {
  return android::base::GetBoolProperty(kAuditProperty, false);
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

void *OpenBackendLibrary(const std::string &path) {
  // A backend payload has a guest linker topology. Loading it through the
  // caller namespace can bind guest dependencies to host x86 libraries.
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

BackendManager::BackendManager() = default;

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

std::string BackendManager::EffectiveInstructionSet(
    const char *instruction_set) const {
  const std::string requested = instruction_set == nullptr ? "" : instruction_set;
  if (!UseHybridLoader() ||
      (requested != "x86" && requested != "x86_64")) {
    return requested;
  }
  return sizeof(void *) == 8 ? "arm64" : "arm";
}

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
  if (AuditEnabled()) {
    LOG(INFO) << "Floral NativeBridge audit: ConfigureProcessContext process="
              << (process_name == nullptr ? "<null>" : process_name)
              << " data_dir=" << (app_data_dir == nullptr ? "<null>" : app_data_dir)
              << " selected="
              << (selected_backend == nullptr ? "<null>" : selected_backend)
              << " callbacks=" << (callbacks_ != nullptr)
              << " initialized=" << initialized_;
  }
  if (callbacks_ != nullptr || initialized_) {
    if (AuditEnabled()) {
      LOG(INFO) << "Floral NativeBridge audit: ConfigureProcessContext ignored"
                << " reason=already_initialized";
    }
    return;
  }
  process_name_ = process_name == nullptr ? "" : process_name;
  package_name_ = PackageNameFromDataDir(app_data_dir);
  selected_backend_override_ =
      selected_backend == nullptr ? "" : selected_backend;

  selection_ = {};
  selection_.reason = selected_backend_override_.empty()
                          ? "installed backend"
                          : "system server process selection";
  if (!selected_backend_override_.empty()) {
    const size_t separator = selected_backend_override_.find('/');
    const std::string backend_name = separator == std::string::npos
                                         ? selected_backend_override_
                                         : selected_backend_override_.substr(0, separator);
    const BackendKind selected = ParseBackendKind(backend_name);
    if (selected != BackendKind::kAuto) {
      const LoaderMode selected_mode = separator == std::string::npos
                                           ? LoaderMode::kHybrid
                                           : ParseLoaderMode(
                                                 selected_backend_override_.substr(
                                                     separator + 1));
      selection_.kind = selected;
      selection_.loader_mode = selected_mode;
      selection_.name = BackendKindName(selected);
      selection_.reason = "system server process selection";
      selection_.candidates = {{selected, selected_mode}};
    } else {
      LOG(WARNING) << "Floral NativeBridge: ignoring invalid process backend "
                   << selected_backend_override_;
    }
  }
  if (selection_.candidates.empty()) {
    selection_.kind = InstalledBackend(nullptr);
    selection_.name = BackendKindName(selection_.kind);
    selection_.loader_mode = LoaderMode::kHybrid;
    selection_.candidates = {{selection_.kind, LoaderMode::kHybrid}};
  }
  if (selection_.kind != BackendKind::kAuto) {
    setenv(kProcessBackendEnvironment, BackendKindName(selection_.kind), 1);
  }
  selection_prepared_ = true;
  if (AuditEnabled()) {
    LOG(INFO) << "Floral NativeBridge audit: selection backend="
              << BackendKindName(selection_.kind)
              << " mode=" << LoaderModeName(selection_.loader_mode)
              << " candidates=" << selection_.candidates.size();
  }
}

std::string BackendManager::BackendPath(BackendKind kind) const {
  const bool is_64_bit = sizeof(void *) == 8;
  const char *backend_name = nullptr;
  const char *generic_property = nullptr;
  const char *arch_property = nullptr;
  const char *library_name = nullptr;
  switch (kind) {
  case BackendKind::kNdk:
    backend_name = "ndk";
    generic_property = "ro.floral.nativebridge.ndk";
    arch_property = is_64_bit ? "ro.floral.nativebridge.ndk64"
                              : "ro.floral.nativebridge.ndk32";
    library_name = "libndk_translation.so";
    break;
  case BackendKind::kHoudini:
    backend_name = "houdini";
    generic_property = "ro.floral.nativebridge.houdini";
    arch_property = is_64_bit ? "ro.floral.nativebridge.houdini64"
                              : "ro.floral.nativebridge.houdini32";
    library_name = "libhoudini.so";
    break;
  case BackendKind::kAuto:
    return {};
  }
  const std::string default_path =
      std::string("/system/floral/") + backend_name +
      (is_64_bit ? "/lib64/" : "/lib/") + library_name;
  const std::string configured =
      android::base::GetProperty(arch_property, "");
  return configured.empty()
             ? android::base::GetProperty(generic_property, default_path)
             : configured;
}

bool BackendManager::BackendAvailable(BackendKind kind) const {
  const std::string path = BackendPath(kind);
  return !path.empty() && access(path.c_str(), R_OK) == 0;
}

BackendKind BackendManager::InstalledBackend(const char *instruction_set) const {
  const bool arm32 = instruction_set != nullptr &&
                     (strcmp(instruction_set, "arm") == 0 ||
                      strcmp(instruction_set, "armeabi") == 0 ||
                      strcmp(instruction_set, "armeabi-v7a") == 0);
  if (arm32) {
    return BackendKind::kHoudini;
  }
  if (BackendAvailable(BackendKind::kNdk)) {
    return BackendKind::kNdk;
  }
  return BackendKind::kHoudini;
}

std::string BackendManager::GuestSystemLibraryDirectory(
    BackendKind kind, const char *instruction_set) const {
  const bool guest_arm64 =
      instruction_set != nullptr &&
      (strcmp(instruction_set, "arm64") == 0 ||
       strcmp(instruction_set, "arm64-v8a") == 0);
  const std::string backend_path = BackendPath(kind);
  const size_t separator = backend_path.find_last_of('/');
  if (separator == std::string::npos) {
    return {};
  }

  std::string backend_library_directory = backend_path.substr(0, separator);
  const std::string host_library_directory = sizeof(void *) == 8
                                                  ? "/lib64"
                                                  : "/lib";
  if (backend_library_directory.size() < host_library_directory.size() ||
      backend_library_directory.compare(
          backend_library_directory.size() - host_library_directory.size(),
          host_library_directory.size(), host_library_directory) != 0) {
    return {};
  }
  backend_library_directory.erase(
      backend_library_directory.size() - host_library_directory.size());
  if (backend_library_directory.empty()) {
    return {};
  }

  return backend_library_directory +
         (guest_arm64 ? "/lib64/arm64" : "/lib/arm");
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
      GuestSystemLibraryDirectory(selection_.kind, instruction_set);
  if (library_directory.empty() ||
      access(library_directory.c_str(), R_OK | X_OK) != 0) {
    LOG(WARNING) << "Floral NativeBridge: guest system library directory is "
                    "unavailable for "
                 << BackendKindName(selection_.kind) << ": "
                 << (library_directory.empty() ? "<empty>" : library_directory)
                 << " (errno=" << errno << ")";
    return false;
  }
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
  if (AuditEnabled()) {
    LOG(INFO) << "Floral NativeBridge audit: LoadBackend requested="
              << BackendKindName(kind) << " path=" << path
              << " loaded_count=" << loaded_backends_.size();
  }
  if (path.empty()) {
    return false;
  }
  if (FindLoadedBackend(kind) != nullptr) {
    if (AuditEnabled()) {
      LOG(INFO) << "Floral NativeBridge audit: LoadBackend reused_existing=true";
    }
    return true;
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
    SetError("NativeBridge backend unavailable in installed Android 12 payload");
    return false;
  }
  if (selection.loader_mode == LoaderMode::kDirect) {
    // Direct is an explicit, single-backend contract. Never walk another
    // candidate here: doing so would reintroduce recovery and make the
    // backend observable as a Floral-managed loader.
    if (selection.candidates.size() != 1 ||
        selection.candidates.front().mode != LoaderMode::kDirect ||
        selection.candidates.front().backend == BackendKind::kAuto) {
      SetError("direct NativeBridge selection must contain one explicit backend");
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

  SetError("no usable NativeBridge backend for installed Android 12 payload");
  return false;
}

bool BackendManager::EnsureLoaded() {
  if (callbacks_ != nullptr) {
    return true;
  }
  if (!selection_prepared_) {
    // Non-zygote callers use the installed Android 12 default backend. The
    // backend is selected explicitly by system_server for normal app launches.
    selection_ = {};
    selection_.kind = InstalledBackend(instruction_set_.c_str());
    selection_.name = BackendKindName(selection_.kind);
    selection_.reason = "installed backend";
    selection_.loader_mode = LoaderMode::kHybrid;
    selection_.candidates = {{selection_.kind, LoaderMode::kHybrid}};
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
  if (AuditEnabled()) {
    LOG(INFO) << "Floral NativeBridge audit: Initialize instruction_set="
              << (instruction_set == nullptr ? "<null>" : instruction_set)
              << " selection_prepared=" << selection_prepared_
              << " callbacks=" << (callbacks_ != nullptr)
              << " initialized=" << initialized_;
  }
  if (initialized_) {
    return true;
  }
  instruction_set_ = instruction_set == nullptr ? "" : instruction_set;
  instruction_set_ = EffectiveInstructionSet(instruction_set_.c_str());
  if (!EnsureLoaded() || callbacks_->initialize == nullptr) {
    return false;
  }
  if (!callbacks_->initialize(runtime_callbacks, private_dir,
                              instruction_set_.c_str())) {
    SetError("selected backend initialization failed");
    return false;
  }
  initialized_ = true;
  // Hybrid routing needs an explicit guest system parent when the backend
  // does not export a named system/default namespace. A failure is
  // non-fatal so the original NativeBridge fallback remains available.
  const bool guest_namespace_ready =
      EnsureGuestSystemNamespace(instruction_set_.c_str());
  // Keep the host identity while the backend initializes. NDK translation
  // resolves libc symbols during initialize(), so exposing the guest identity
  // earlier can make it resolve against the wrong libc namespace.
  if (selection_.loader_mode == LoaderMode::kHybrid && guest_namespace_ready) {
    if (__floral_nativebridge_set_guest_arch != nullptr) {
      __floral_nativebridge_set_guest_arch(GuestArchitecture());
    } else {
      LOG(WARNING) << "Floral NativeBridge: libc guest identity hook unavailable";
    }
  }
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
             ? callbacks_->getAppEnv(EffectiveInstructionSet(instruction_set).c_str())
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
  if (AuditEnabled()) {
    LOG(INFO) << "Floral NativeBridge audit: PreZygoteFork callbacks="
              << (callbacks_ != nullptr) << " initialized=" << initialized_
              << " loaded_count=" << loaded_backends_.size();
  }
  // Payloads are deliberately not loaded in zygote. Their constructors and
  // callback state must belong to exactly one application process.
}

} // namespace floral::nativebridge
