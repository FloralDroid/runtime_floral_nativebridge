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
#include <nativebridge/native_bridge_diagnostics.h>

namespace floral::nativebridge {
namespace {

constexpr char kInterfaceSymbol[] = "NativeBridgeItf";
constexpr uint32_t kRequiredNativeBridgeVersion = 3;
constexpr uint32_t kMaxNativeBridgeVersion = 6;

extern "C" void __floral_nativebridge_set_guest_arch(int arch)
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

void BackendManager::RememberLibrary(void *handle, const char *path) const {
  if (handle == nullptr || path == nullptr || *path == '\0') {
    return;
  }
  std::lock_guard<std::mutex> lock(library_mutex_);
  library_paths_[handle] = path;
}

void BackendManager::ForgetLibrary(void *handle) const {
  if (handle == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(library_mutex_);
  library_paths_.erase(handle);
}

std::string BackendManager::LibraryPath(void *handle) const {
  std::lock_guard<std::mutex> lock(library_mutex_);
  const auto library = library_paths_.find(handle);
  return library == library_paths_.end() ? std::string() : library->second;
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
                                             const char *app_data_dir) {
  if (callbacks_ != nullptr || initialized_) {
    return;
  }
  process_name_ = process_name == nullptr ? "" : process_name;
  package_name_ = PackageNameFromDataDir(app_data_dir);

  // The router runs only for an ARM NativeBridge child. New Floral libc builds
  // expose this private hook, while weak binding keeps the router loadable on an
  // older system image during an incremental update.
  if (__floral_nativebridge_set_guest_arch != nullptr) {
    __floral_nativebridge_set_guest_arch(GuestArchitecture());
  } else {
    LOG(WARNING) << "Floral NativeBridge: libc guest identity hook unavailable";
  }

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
  if (selection.exhausted) {
    SetError("NativeBridge backend candidates exhausted by Android runtime state");
    return false;
  }
  const std::vector<BackendCandidate> default_candidates = {
      {BackendKind::kNdk, LoaderMode::kHybrid},
      {BackendKind::kNdk, LoaderMode::kCompat},
      {BackendKind::kHoudini, LoaderMode::kHybrid},
      {BackendKind::kHoudini, LoaderMode::kCompat},
  };
  const std::vector<BackendCandidate> &candidates = selection.candidates.empty()
                                                        ? default_candidates
                                                        : selection.candidates;
  for (const BackendCandidate candidate : candidates) {
    if (candidate.backend == BackendKind::kAuto) {
      continue;
    }
    const std::string path = BackendPath(candidate.backend);
    if (path.empty()) {
      continue;
    }

    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      const char *error = dlerror();
      if (selection.kind != BackendKind::kAuto) {
        SetError("cannot load " +
                 std::string(BackendKindName(candidate.backend)) +
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
    selection_.kind = candidate.backend;
    selection_.loader_mode = candidate.mode;
    selection_.name = BackendKindName(candidate.backend);
    selection_.reason = selection.reason;
    selected_path_ = path;
    LOG(INFO) << "Loaded " << selection_.name << " NativeBridge backend in "
              << LoaderModeName(selection_.loader_mode) << " mode from "
              << selected_path_ << " (interface v" << callbacks_->version << ")";
    const std::string &diagnostic_process =
        process_name_.empty() ? package_name_ : process_name_;
    if (android::native_bridge_diagnostics::ShouldTraceProcess(
            diagnostic_process.c_str())) {
      Dl_info callback_info = {};
      const char *callback_owner =
          dladdr(reinterpret_cast<void *>(callbacks_->loadLibrary),
                 &callback_info) != 0
              ? callback_info.dli_fname
              : nullptr;
      android::native_bridge_diagnostics::Trace(
          "stage=backend_selected process=%s package=%s backend=%s mode=%s path=%s "
          "backend_handle=%p callbacks=%p load_callback=%p callback_owner=%s "
          "version=%u",
          diagnostic_process.c_str(), package_name_.c_str(),
          selection_.name.c_str(), LoaderModeName(selection_.loader_mode),
          selected_path_.c_str(), backend_handle_,
          const_cast<void *>(static_cast<const void *>(callbacks_)),
          reinterpret_cast<void *>(callbacks_->loadLibrary),
          callback_owner == nullptr ? "<unknown>" : callback_owner,
          callbacks_->version);
    }
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
  const std::string &diagnostic_process =
      process_name_.empty() ? package_name_ : process_name_;
  const bool trace = android::native_bridge_diagnostics::ShouldTraceProcess(
      diagnostic_process.c_str());
  if (trace) {
    android::native_bridge_diagnostics::Trace(
        "stage=backend_initialize_begin process=%s backend=%s private_dir=%s "
        "instruction_set=%s",
        diagnostic_process.c_str(), selection_.name.c_str(),
        private_dir == nullptr ? "<null>" : private_dir,
        instruction_set == nullptr ? "<null>" : instruction_set);
  }
  if (!callbacks_->initialize(runtime_callbacks, private_dir,
                              instruction_set)) {
    SetError("selected backend initialization failed");
    if (trace) {
      android::native_bridge_diagnostics::Trace(
          "stage=backend_initialize_end process=%s backend=%s result=error "
          "error=%s",
          diagnostic_process.c_str(), selection_.name.c_str(), GetError());
    }
    return false;
  }
  initialized_ = true;
  if (trace) {
    android::native_bridge_diagnostics::Trace(
        "stage=backend_initialize_end process=%s backend=%s result=ok",
        diagnostic_process.c_str(), selection_.name.c_str());
  }
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
  void *handle = callbacks_ != nullptr && callbacks_->loadLibrary != nullptr
                     ? callbacks_->loadLibrary(path, flags)
                     : nullptr;
  RememberLibrary(handle, path);
  const std::string &diagnostic_process =
      process_name_.empty() ? package_name_ : process_name_;
  if (android::native_bridge_diagnostics::ShouldTraceLibrary(
          diagnostic_process.c_str(), path)) {
    android::native_bridge_diagnostics::Trace(
        "stage=backend_load_library process=%s backend=%s path=%s flags=0x%x "
        "handle=%p error=%s",
        diagnostic_process.c_str(), selection_.name.c_str(),
        path == nullptr ? "<null>" : path, flags, handle,
        handle == nullptr ? GetError() : "<none>");
  }
  return handle;
}

void *BackendManager::GetTrampoline(void *handle, const char *name,
                                    const char *shorty, uint32_t len) const {
  void *symbol = callbacks_ != nullptr && callbacks_->getTrampoline != nullptr
                     ? callbacks_->getTrampoline(handle, name, shorty, len)
                     : nullptr;
  const std::string path = LibraryPath(handle);
  const std::string &diagnostic_process =
      process_name_.empty() ? package_name_ : process_name_;
  if (android::native_bridge_diagnostics::ShouldTraceLibrary(
          diagnostic_process.c_str(), path.empty() ? nullptr : path.c_str())) {
    Dl_info symbol_info = {};
    const char *owner = symbol != nullptr && dladdr(symbol, &symbol_info) != 0
                            ? symbol_info.dli_fname
                            : nullptr;
    android::native_bridge_diagnostics::Trace(
        "stage=backend_get_trampoline process=%s backend=%s path=%s handle=%p "
        "symbol=%s shorty=%s length=%u result=%p owner=%s error=%s",
        diagnostic_process.c_str(), selection_.name.c_str(),
        path.empty() ? "<unknown>" : path.c_str(), handle,
        name == nullptr ? "<null>" : name,
        shorty == nullptr ? "<null>" : shorty, len, symbol,
        owner == nullptr ? "<unknown>" : owner,
        symbol == nullptr ? GetError() : "<none>");
  }
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
  const int result =
      callbacks_ != nullptr && callbacks_->unloadLibrary != nullptr
          ? callbacks_->unloadLibrary(handle)
          : -1;
  if (result == 0) {
    ForgetLibrary(handle);
  }
  return result;
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
  void *handle = callbacks_ != nullptr && callbacks_->loadLibraryExt != nullptr
                     ? callbacks_->loadLibraryExt(path, flags, ns)
                     : nullptr;
  RememberLibrary(handle, path);
  const std::string &diagnostic_process =
      process_name_.empty() ? package_name_ : process_name_;
  if (android::native_bridge_diagnostics::ShouldTraceLibrary(
          diagnostic_process.c_str(), path)) {
    android::native_bridge_diagnostics::Trace(
        "stage=backend_load_library_ext process=%s backend=%s path=%s "
        "flags=0x%x "
        "namespace=%p handle=%p error=%s",
        diagnostic_process.c_str(), selection_.name.c_str(),
        path == nullptr ? "<null>" : path, flags, static_cast<void *>(ns), handle,
        handle == nullptr ? GetError() : "<none>");
  }
  return handle;
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
