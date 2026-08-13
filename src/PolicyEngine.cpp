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

BackendSelection SelectionFromJson(const Json::Value &value,
                                   std::string reason) {
  BackendSelection selection;
  if (value.isString()) {
    const std::string backend = value.asString();
    selection.kind = ParseBackendKind(backend);
    selection.name = BackendKindName(selection.kind);
    selection.reason = std::move(reason);
    if (selection.kind == BackendKind::kAuto) {
      selection.candidates = {BackendKind::kNdk, BackendKind::kHoudini};
    } else {
      selection.candidates = {selection.kind};
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
  std::string backend = "auto";
  if (value["selected_backend"].isString()) {
    backend = value["selected_backend"].asString();
  } else if (value["backend"].isString()) {
    backend = value["backend"].asString();
  } else if (value["mode"].isString()) {
    backend = value["mode"].asString();
  }
  selection.kind = ParseBackendKind(backend);
  selection.name = BackendKindName(selection.kind);
  selection.reason = std::move(reason);
  if (value["candidates"].isArray()) {
    for (const Json::Value &candidate : value["candidates"]) {
      if (!candidate.isString()) {
        continue;
      }
      const BackendKind kind = ParseBackendKind(candidate.asString());
      if (kind != BackendKind::kAuto) {
        selection.candidates.push_back(kind);
      }
    }
  }
  if (selection.kind != BackendKind::kAuto) {
    selection.candidates = {selection.kind};
  } else if (selection.candidates.empty()) {
    selection.candidates = {BackendKind::kNdk, BackendKind::kHoudini};
  }
  return selection;
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
      .candidates = {BackendKind::kNdk, BackendKind::kHoudini},
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
  }
  const Json::Value packages = root["packages"];
  if (packages.isObject()) {
    for (const std::string &process_name : packages.getMemberNames()) {
      engine.process_selections_.emplace(
          process_name,
          SelectionFromJson(packages[process_name], "process rule"));
    }
  }
  engine.loaded_ = true;
  return engine;
}

BackendSelection PolicyEngine::Select(std::string_view process_name) const {
  const auto exact = process_selections_.find(std::string(process_name));
  if (exact != process_selections_.end()) {
    return exact->second;
  }

  const size_t separator = process_name.find(':');
  if (separator != std::string_view::npos) {
    const auto package = process_selections_.find(
        std::string(process_name.substr(0, separator)));
    if (package != process_selections_.end()) {
      return package->second;
    }
  }
  return default_selection_;
}

BackendSelection PolicyEngine::ApplyRuntimeState(BackendSelection selection,
                                                 std::string_view process_name,
                                                 std::string path) {
  // Explicit host rules are authoritative. Runtime recovery only advances an
  // auto rule after Android has observed an early native crash.
  if (selection.kind != BackendKind::kAuto || process_name.empty()) {
    return selection;
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    return selection;
  }
  std::stringstream contents;
  contents << file.rdbuf();
  const std::string input = contents.str();
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(input.data(), input.data() + input.size(), &root,
                     &errors) ||
      !root.isObject()) {
    LOG(WARNING) << "Cannot parse Floral NativeBridge state " << path << ": "
                 << errors;
    return selection;
  }

  const Json::Value package = root["packages"][std::string(process_name)];
  if (!package.isObject() || !package["selected_backend"].isString()) {
    return selection;
  }
  const std::string backend = package["selected_backend"].asString();
  const BackendKind kind = ParseBackendKind(backend);
  if (kind == BackendKind::kAuto) {
    return selection;
  }
  selection.kind = kind;
  selection.name = BackendKindName(kind);
  selection.reason = "Android runtime state";
  selection.candidates = {kind};
  return selection;
}

} // namespace floral::nativebridge
