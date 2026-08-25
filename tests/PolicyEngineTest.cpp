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
      "executables": {
        "/vendor/bin/arm-helper": "ndk"
      },
      "packages": {
        "com.example.game": {
          "mode": "auto",
          "candidates": ["houdini", "ndk"],
          "executables": {
            "libzs-local.so": "houdini"
          }
        },
        "com.example.game:remote": "ndk"
      }
    })";
  }

  const floral::nativebridge::PolicyEngine engine =
      floral::nativebridge::PolicyEngine::Load(path);
  const auto exact = engine.Select("com.example.game");
  const auto process = engine.Select("com.example.game:remote");
  const auto fallback = engine.Select("com.other.app");
  const auto executable = engine.SelectExecutable(
      "com.example.game", "/data/app/com.example.game-1/lib/arm64/libzs-local.so");
  const auto path_only = engine.SelectExecutable(
      "", "/data/app/com.example.game-1/lib/arm64/libzs-local.so");
  const auto global_executable =
      engine.SelectExecutable("", "/vendor/bin/arm-helper");

  {
    std::ofstream policy(path, std::ios::trunc);
    policy << R"({
      "preferred_backend": "ndk",
      "packages": {
        "com.example.game": {
          "mode": "auto",
          "candidates": ["ndk", "houdini"],
          "selected_backend": "houdini",
          "selected_loader_mode": "direct"
        }
      }
    })";
  }
  const floral::nativebridge::PolicyEngine reloaded =
      floral::nativebridge::PolicyEngine::Load(path);
  const auto refreshed = reloaded.Select("com.example.game");
  const auto preferred = reloaded.Select("com.other.app");

  unlink(path);

  const bool ok =
      engine.loaded() &&
      Check(exact.kind == floral::nativebridge::BackendKind::kAuto,
            "exact process rule") &&
      Check(exact.candidates.size() == 2 &&
                exact.candidates[0].backend == floral::nativebridge::BackendKind::kHoudini &&
                exact.candidates[0].mode == floral::nativebridge::LoaderMode::kHybrid &&
                exact.candidates[1].backend == floral::nativebridge::BackendKind::kNdk &&
                exact.candidates[1].mode == floral::nativebridge::LoaderMode::kHybrid,
            "automatic selection contains hybrid candidates only") &&
      Check(process.kind == floral::nativebridge::BackendKind::kAuto &&
                process.candidates.size() == exact.candidates.size() &&
                process.candidates[0].backend == exact.candidates[0].backend &&
                process.candidates[1].backend == exact.candidates[1].backend,
            "package rule has authority over child process rule") &&
      Check(fallback.kind == floral::nativebridge::BackendKind::kHoudini,
            "default rule") &&
      Check(executable.kind == floral::nativebridge::BackendKind::kHoudini,
            "process executable rule") &&
      Check(path_only.kind == floral::nativebridge::BackendKind::kHoudini,
            "path executable rule") &&
      Check(global_executable.kind == floral::nativebridge::BackendKind::kNdk,
            "global executable rule") &&
      Check(refreshed.kind == floral::nativebridge::BackendKind::kHoudini &&
                refreshed.loader_mode == floral::nativebridge::LoaderMode::kDirect &&
                refreshed.candidates.size() == 1 &&
                refreshed.candidates[0].backend == floral::nativebridge::BackendKind::kHoudini &&
                refreshed.candidates[0].mode == floral::nativebridge::LoaderMode::kDirect,
            "selected backend and loader mode reload") &&
      Check(preferred.kind == floral::nativebridge::BackendKind::kAuto &&
                preferred.candidates.size() == 2 &&
                preferred.candidates[0].backend == floral::nativebridge::BackendKind::kNdk &&
                preferred.candidates[0].mode == floral::nativebridge::LoaderMode::kHybrid &&
                preferred.candidates[1].backend == floral::nativebridge::BackendKind::kHoudini &&
                preferred.candidates[1].mode == floral::nativebridge::LoaderMode::kHybrid,
             "preferred backend keeps automatic fallback");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
