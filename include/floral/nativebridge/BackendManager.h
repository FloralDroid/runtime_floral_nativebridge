/*
 * Copyright 2026 FloralDroid
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FLORAL_NATIVEBRIDGE_BACKEND_MANAGER_H_
#define FLORAL_NATIVEBRIDGE_BACKEND_MANAGER_H_

#include <string>
#include <vector>

#include "nativebridge/native_bridge.h"

#include "floral/nativebridge/PolicyEngine.h"

namespace floral::nativebridge {

class BackendManager {
public:
  BackendManager();
  ~BackendManager();

  void ConfigureProcessContext(const char *process_name,
                               const char *app_data_dir);
  bool EnsureLoaded();
  // Direct mode must be selected before ART pre-initializes the backend so
  // ART can bind the real backend callback table for the whole app lifetime.
  bool PrepareDirectBackend();
  bool
  Initialize(const android::NativeBridgeRuntimeCallbacks *runtime_callbacks,
             const char *private_dir, const char *instruction_set);

  bool IsCompatibleWith(uint32_t bridge_version);
  const android::NativeBridgeCallbacks *callbacks() const { return callbacks_; }
  const char *GetError() const;

  void *LoadLibrary(const char *path, int flags) const;
  void *GetTrampoline(void *handle, const char *name, const char *shorty,
                      uint32_t len) const;
  bool IsSupported(const char *path) const;
  const android::NativeBridgeRuntimeValues *
  GetAppEnv(const char *instruction_set) const;
  android::NativeBridgeSignalHandlerFn GetSignalHandler(int signal) const;
  int UnloadLibrary(void *handle) const;
  bool IsPathSupported(const char *path) const;
  bool InitAnonymousNamespace(const char *public_ns_sonames,
                              const char *anon_ns_library_path) const;
  android::native_bridge_namespace_t *
  CreateNamespace(const char *name, const char *ld_library_path,
                  const char *default_library_path, uint64_t type,
                  const char *permitted_when_isolated_path,
                  android::native_bridge_namespace_t *parent_ns) const;
  bool LinkNamespaces(android::native_bridge_namespace_t *from,
                      android::native_bridge_namespace_t *to,
                      const char *shared_libs_sonames) const;
  void *LoadLibraryExt(const char *path, int flags,
                       android::native_bridge_namespace_t *ns) const;
  android::native_bridge_namespace_t *GetVendorNamespace() const;
  android::native_bridge_namespace_t *
  GetExportedNamespace(const char *name) const;
  void PreZygoteFork();

  BackendKind selected_kind() const { return selection_.kind; }
  LoaderMode selected_loader_mode() const { return selection_.loader_mode; }
  bool UseHybridLoader() const {
    return selection_.loader_mode == LoaderMode::kHybrid;
  }
  const std::string &selected_path() const { return selected_path_; }

private:
  struct LoadedBackend {
    BackendKind kind = BackendKind::kAuto;
    std::string path;
    void *handle = nullptr;
    const android::NativeBridgeCallbacks *callbacks = nullptr;
  };

  std::string ProcessName() const;
  static std::string PackageNameFromDataDir(const char *app_data_dir);
  std::string BackendPath(BackendKind kind) const;
  bool PreloadBackends();
  bool LoadBackend(BackendKind kind, const std::string &path);
  const LoadedBackend *FindLoadedBackend(BackendKind kind) const;
  bool IsBackendCompatible(const LoadedBackend &backend,
                           uint32_t bridge_version) const;
  bool LoadSelectedBackend(const BackendSelection &selection);
  void SetError(std::string message) const;

  std::string policy_path_;
  std::string process_name_;
  std::string package_name_;
  std::vector<LoadedBackend> loaded_backends_;
  const android::NativeBridgeCallbacks *callbacks_ = nullptr;
  BackendSelection selection_;
  std::string selected_path_;
  mutable std::string last_error_;
  bool selection_prepared_ = false;
  bool backends_preloaded_ = false;
  bool initialized_ = false;
};

} // namespace floral::nativebridge

#endif // FLORAL_NATIVEBRIDGE_BACKEND_MANAGER_H_
