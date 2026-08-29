/*
 * Copyright 2026 FloralDroid
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "floral/nativebridge/PolicyEngine.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

#include <android-base/logging.h>
#include <json/json.h>

namespace floral::nativebridge {
namespace {

std::vector<BackendCandidate> DirectCandidates(
    const std::vector<BackendKind> &backends) {
  std::vector<BackendCandidate> candidates;
  for (const BackendKind backend : backends) {
    if (backend == BackendKind::kAuto) {
      continue;
    }
    candidates.push_back({backend, LoaderMode::kDirect});
  }
  return candidates;
}

BackendSelection SelectionFromJson(const Json::Value &value,
                                   std::string reason) {
  BackendSelection selection;
  if (value.isString()) {
    const std::string backend = value.asString();
    selection.kind = ParseBackendKind(backend);
    selection.name = BackendKindName(selection.kind);
    selection.reason = std::move(reason);
    if (selection.kind == BackendKind::kAuto) {
      selection.candidates = DirectCandidates(
          {BackendKind::kNdk, BackendKind::kHoudini});
    } else {
      selection.candidates = DirectCandidates({selection.kind});
    }
    if (selection.kind == BackendKind::kAuto && backend != "auto") {
      LOG(WARNING) << "Unknown Floral NativeBridge backend '" << backend
                   << "'; using auto selection";
    }
    return selection;
  }
  if (!value.isObject()) {
    selection.reason = std::move(reason);
    return selection;
  }

  // The policy file may keep candidate order and an explicit selected backend
  // in one object. The selected value is preferred for the next process.
  const bool has_selected_backend = value["selected_backend"].isString();
  std::string backend = "auto";
  if (has_selected_backend) {
    backend = value["selected_backend"].asString();
  } else if (value["backend"].isString()) {
    backend = value["backend"].asString();
  } else if (value["mode"].isString()) {
    backend = value["mode"].asString();
  }
  selection.kind = ParseBackendKind(backend);
  selection.name = BackendKindName(selection.kind);
  selection.reason = std::move(reason);
  if (has_selected_backend && value["selected_loader_mode"].isString()) {
    selection.loader_mode =
        ParseLoaderMode(value["selected_loader_mode"].asString());
  }
  std::vector<BackendKind> backend_candidates;
  if (value["candidates"].isArray()) {
    for (const Json::Value &candidate : value["candidates"]) {
      if (!candidate.isString()) {
        continue;
      }
      const BackendKind kind = ParseBackendKind(candidate.asString());
      if (kind != BackendKind::kAuto) {
        backend_candidates.push_back(kind);
      }
    }
  }
  if (selection.kind != BackendKind::kAuto) {
    selection.candidates = {{selection.kind, selection.loader_mode}};
    return selection;
  } else if (backend_candidates.empty()) {
    backend_candidates = {BackendKind::kNdk, BackendKind::kHoudini};
  }
  selection.candidates = DirectCandidates(backend_candidates);
  return selection;
}

BackendSelection PreferredSelectionFromJson(const Json::Value &value,
                                            std::string reason) {
  BackendSelection selection{
      .kind = BackendKind::kAuto,
      .name = BackendKindName(BackendKind::kAuto),
      .reason = std::move(reason),
      .candidates = DirectCandidates({BackendKind::kNdk, BackendKind::kHoudini}),
  };
  if (!value.isString()) {
    LOG(WARNING) << "Floral NativeBridge preferred_backend must be ndk or houdini";
    return selection;
  }

  const BackendKind preferred = ParseBackendKind(value.asString());
  if (preferred == BackendKind::kHoudini) {
    selection.candidates = DirectCandidates(
        {BackendKind::kHoudini, BackendKind::kNdk});
  } else if (preferred != BackendKind::kNdk) {
    LOG(WARNING) << "Unknown Floral NativeBridge preferred backend '"
                 << value.asString() << "'; using ndk first";
  }
  return selection;
}

std::string Basename(std::string_view path) {
  const size_t separator = path.find_last_of('/');
  return separator == std::string_view::npos
             ? std::string(path)
             : std::string(path.substr(separator + 1));
}

bool MatchesExecutable(std::string_view pattern, std::string_view path) {
  return pattern == "*" || pattern == path || pattern == Basename(path);
}

bool PathBelongsToOwner(std::string_view path, std::string_view owner) {
  if (owner.empty()) {
    return true;
  }
  const size_t separator = owner.find(':');
  if (separator != std::string_view::npos) {
    owner = owner.substr(0, separator);
  }
  const std::string package_marker = "/" + std::string(owner);
  if (path.find(package_marker + "/") != std::string_view::npos) {
    return true;
  }
  // Installed APK directories append a generated suffix to the package name.
  return path.find(package_marker + "-") != std::string_view::npos;
}

} // namespace

const char *BackendKindName(BackendKind kind) {
  switch (kind) {
  case BackendKind::kNdk:
    return "ndk";
  case BackendKind::kHoudini:
    return "houdini";
  case BackendKind::kAuto:
    return "auto";
  }
  return "auto";
}

const char *LoaderModeName(LoaderMode mode) {
  switch (mode) {
  case LoaderMode::kHybrid:
    return "hybrid";
  case LoaderMode::kDirect:
    return "direct";
  }
  return "hybrid";
}

LoaderMode ParseLoaderMode(std::string_view value) {
  // "compat" and "hybrid" were names used by older state formats. They are
  // normalized to direct so stale state cannot re-enable router forwarding.
  (void)value;
  return LoaderMode::kDirect;
}

std::string BackendCandidateName(const BackendCandidate &candidate) {
  return std::string(BackendKindName(candidate.backend)) + "/" +
         LoaderModeName(candidate.mode);
}

BackendKind ParseBackendKind(std::string_view value) {
  if (value == "ndk") {
    return BackendKind::kNdk;
  }
  if (value == "houdini") {
    return BackendKind::kHoudini;
  }
  return BackendKind::kAuto;
}

PolicyEngine PolicyEngine::Load(std::string path) {
  PolicyEngine engine;
  engine.path_ = std::move(path);
  engine.default_selection_ = {
      .kind = BackendKind::kAuto,
      .name = BackendKindName(BackendKind::kAuto),
      .reason = "built-in default",
      .candidates = DirectCandidates({BackendKind::kNdk, BackendKind::kHoudini}),
  };

  std::ifstream file(engine.path_);
  if (!file.is_open()) {
    return engine;
  }

  std::stringstream contents;
  contents << file.rdbuf();
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  const std::string input = contents.str();
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(input.data(), input.data() + input.size(), &root,
                     &errors) ||
      !root.isObject()) {
    LOG(WARNING) << "Cannot parse Floral NativeBridge policy " << engine.path_
                 << ": " << errors;
    return engine;
  }

  if (root.isMember("default_backend")) {
    engine.default_selection_ =
        SelectionFromJson(root["default_backend"], "policy default");
  } else if (root.isMember("preferred_backend")) {
    engine.default_selection_ = PreferredSelectionFromJson(
        root["preferred_backend"], "policy preferred backend");
  }

  const auto add_executable_rules = [&engine](const Json::Value &rules,
                                               std::string owner) {
    if (!rules.isObject()) {
      return;
    }
    for (const std::string &pattern : rules.getMemberNames()) {
      engine.executable_rules_.push_back({
          owner,
          pattern,
          SelectionFromJson(rules[pattern], owner.empty()
                                                ? "global executable rule"
                                                : "executable rule"),
      });
    }
  };
  add_executable_rules(root["executables"], "");

  const Json::Value packages = root["packages"];
  if (packages.isObject()) {
    for (const std::string &process_name : packages.getMemberNames()) {
      engine.process_selections_.emplace(
          process_name,
          SelectionFromJson(packages[process_name], "process rule"));

      const Json::Value &rule = packages[process_name];
      if (rule.isObject()) {
        add_executable_rules(rule["executables"], process_name);
      }
    }
  }
  engine.loaded_ = true;
  return engine;
}

BackendSelection PolicyEngine::Select(std::string_view process_name) const {
  const size_t separator = process_name.find(':');
  if (separator != std::string_view::npos) {
    // A package rule is the authority for every process in that package. This
    // prevents a helper process from silently selecting another backend.
    const auto package = process_selections_.find(
        std::string(process_name.substr(0, separator)));
    if (package != process_selections_.end()) {
      return package->second;
    }
  }

  const auto exact = process_selections_.find(std::string(process_name));
  if (exact != process_selections_.end()) {
    return exact->second;
  }
  return default_selection_;
}

BackendSelection PolicyEngine::SelectExecutable(
    std::string_view process_name, std::string_view executable_path) const {
  if (executable_path.empty()) {
    return Select(process_name);
  }

  const std::string package = [&process_name] {
    const size_t separator = process_name.find(':');
    return std::string(process_name.substr(0, separator));
  }();

  // Package-owned executable rules are authoritative for every process in the
  // package. A path rule is only accepted when its package marker is present.
  for (const ExecutableRule &rule : executable_rules_) {
    if (rule.owner.empty() || package.empty() || rule.owner != package ||
        !MatchesExecutable(rule.pattern, executable_path) ||
        !PathBelongsToOwner(executable_path, package)) {
      continue;
    }
    return rule.selection;
  }

  for (const ExecutableRule &rule : executable_rules_) {
    if (rule.owner.empty() || rule.owner != process_name ||
        !MatchesExecutable(rule.pattern, executable_path) ||
        !PathBelongsToOwner(executable_path, rule.owner)) {
      continue;
    }
    return rule.selection;
  }

  for (const ExecutableRule &rule : executable_rules_) {
    if (rule.owner.empty() && MatchesExecutable(rule.pattern, executable_path)) {
      return rule.selection;
    }
  }

  // A binfmt dispatcher does not always retain the Android process name. If
  // the path identifies exactly one configured executable rule, use it.
  const ExecutableRule *path_match = nullptr;
  for (const ExecutableRule &rule : executable_rules_) {
    if (rule.owner.empty() || !MatchesExecutable(rule.pattern, executable_path) ||
        !PathBelongsToOwner(executable_path, rule.owner)) {
      continue;
    }
    if (path_match != nullptr) {
      return Select(process_name);
    }
    path_match = &rule;
  }
  return path_match == nullptr ? Select(process_name) : path_match->selection;
}

} // namespace floral::nativebridge
