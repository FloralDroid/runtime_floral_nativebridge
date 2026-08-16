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

#ifndef FLORAL_NATIVEBRIDGE_POLICY_ENGINE_H_
#define FLORAL_NATIVEBRIDGE_POLICY_ENGINE_H_

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace floral::nativebridge {

enum class BackendKind {
  kAuto,
  kNdk,
  kHoudini,
};

// Hybrid keeps the existing machine-based split. Direct gives the selected
// backend ownership of every bridged ELF load without a host-linker fallback.
enum class LoaderMode {
  kHybrid,
  kDirect,
  // Kept as a source compatibility alias for older callers.
  kCompat = kDirect,
};

struct BackendCandidate {
  BackendKind backend = BackendKind::kAuto;
  LoaderMode mode = LoaderMode::kHybrid;
};

struct BackendSelection {
  BackendKind kind = BackendKind::kAuto;
  LoaderMode loader_mode = LoaderMode::kHybrid;
  std::string name = "auto";
  std::string reason = "default";
  std::vector<BackendCandidate> candidates;
  bool exhausted = false;
};

struct ExecutableRule {
  std::string owner;
  std::string pattern;
  BackendSelection selection;
};

class PolicyEngine {
public:
  static constexpr const char *kDefaultPath =
      "/data/system/floral/nativebridge-policy.json";

  static PolicyEngine Load(std::string path = kDefaultPath);

  BackendSelection Select(std::string_view process_name) const;
  BackendSelection SelectExecutable(std::string_view process_name,
                                    std::string_view executable_path) const;

  static BackendSelection ApplyRuntimeState(
      BackendSelection selection, std::string_view process_name,
      std::string path = "/data/system/floral/nativebridge-state.json");

  const std::string &path() const { return path_; }
  bool loaded() const { return loaded_; }

private:
  std::string path_;
  BackendSelection default_selection_;
  std::unordered_map<std::string, BackendSelection> process_selections_;
  std::vector<ExecutableRule> executable_rules_;
  bool loaded_ = false;
};

const char *BackendKindName(BackendKind kind);
BackendKind ParseBackendKind(std::string_view value);
const char *LoaderModeName(LoaderMode mode);
LoaderMode ParseLoaderMode(std::string_view value);
std::string BackendCandidateName(const BackendCandidate &candidate);

} // namespace floral::nativebridge

#endif // FLORAL_NATIVEBRIDGE_POLICY_ENGINE_H_
