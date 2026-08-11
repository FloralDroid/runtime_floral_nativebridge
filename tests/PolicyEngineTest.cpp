/*
 * Copyright 2026 FloralDroid
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "floral/nativebridge/PolicyEngine.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>

namespace {

bool Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

} // namespace

int main() {
  char path[] = "/tmp/floral-nativebridge-policy-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) {
    std::cerr << "unable to create temporary policy\n";
    return EXIT_FAILURE;
  }
  close(fd);

  {
    std::ofstream policy(path);
    policy << R"({
      "default_backend": "houdini",
      "packages": {
        "com.example.game": {
          "mode": "auto",
          "candidates": ["houdini", "ndk"]
        },
        "com.example.game:remote": "houdini"
      }
    })";
  }

  const floral::nativebridge::PolicyEngine engine =
      floral::nativebridge::PolicyEngine::Load(path);
  const auto exact = engine.Select("com.example.game");
  const auto process = engine.Select("com.example.game:remote");
  const auto fallback = engine.Select("com.other.app");

  {
    std::ofstream policy(path, std::ios::trunc);
    policy << R"({
      "default_backend": "ndk",
      "packages": {
        "com.example.game": {
          "mode": "auto",
          "candidates": ["ndk", "houdini"],
          "selected_backend": "houdini"
        }
      }
    })";
  }
  const floral::nativebridge::PolicyEngine reloaded =
      floral::nativebridge::PolicyEngine::Load(path);
  const auto refreshed = reloaded.Select("com.example.game");
  unlink(path);

  const bool ok =
      engine.loaded() &&
      Check(exact.kind == floral::nativebridge::BackendKind::kAuto,
            "exact process rule") &&
      Check(exact.candidates.size() == 2 &&
                exact.candidates[0] == floral::nativebridge::BackendKind::kHoudini,
            "candidate order") &&
      Check(process.kind == floral::nativebridge::BackendKind::kHoudini,
            "process rule") &&
      Check(fallback.kind == floral::nativebridge::BackendKind::kHoudini,
            "default rule") &&
      Check(refreshed.kind == floral::nativebridge::BackendKind::kHoudini &&
                refreshed.candidates.size() == 1 &&
                refreshed.candidates[0] == floral::nativebridge::BackendKind::kHoudini,
            "selected backend reload");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
