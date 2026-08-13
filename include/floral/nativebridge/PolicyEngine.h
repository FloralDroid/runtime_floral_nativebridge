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

struct BackendSelection {
  BackendKind kind = BackendKind::kAuto;
  std::string name = "auto";
  std::string reason = "default";
  std::vector<BackendKind> candidates;
};

class PolicyEngine {
public:
  static constexpr const char *kDefaultPath =
      "/data/system/floral/nativebridge-policy.json";

  static PolicyEngine Load(std::string path = kDefaultPath);

  BackendSelection Select(std::string_view process_name) const;

  static BackendSelection ApplyRuntimeState(
      BackendSelection selection, std::string_view process_name,
      std::string path = "/data/system/floral/nativebridge-state.json");

  const std::string &path() const { return path_; }
  bool loaded() const { return loaded_; }

private:
  std::string path_;
  BackendSelection default_selection_;
  std::unordered_map<std::string, BackendSelection> process_selections_;
  bool loaded_ = false;
};

const char *BackendKindName(BackendKind kind);
BackendKind ParseBackendKind(std::string_view value);

} // namespace floral::nativebridge

#endif // FLORAL_NATIVEBRIDGE_POLICY_ENGINE_H_
