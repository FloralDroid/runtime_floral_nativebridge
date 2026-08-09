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
  if (!value.isString()) {
    selection.reason = std::move(reason);
    return selection;
  }

  const std::string backend = value.asString();
  selection.kind = ParseBackendKind(backend);
  selection.name = BackendKindName(selection.kind);
  selection.reason = std::move(reason);
  if (selection.kind == BackendKind::kAuto && backend != "auto") {
    LOG(WARNING) << "Unknown Floral NativeBridge backend '" << backend
                 << "'; using auto selection";
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

} // namespace floral::nativebridge
