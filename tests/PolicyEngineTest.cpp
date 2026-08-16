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
        "com.example.game:remote": "houdini"
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
          "selected_backend": "houdini"
        }
      }
    })";
  }
  const floral::nativebridge::PolicyEngine reloaded =
      floral::nativebridge::PolicyEngine::Load(path);
  const auto refreshed = reloaded.Select("com.example.game");
  const auto preferred = reloaded.Select("com.other.app");

  {
    std::ofstream state(path, std::ios::trunc);
    state << R"({
      "version": 1,
      "packages": {
        "com.example.game": {"selected_backend": "houdini"},
        "com.exhausted.game": {"exhausted": true}
      }
    })";
  }
  const auto recovered = floral::nativebridge::PolicyEngine::ApplyRuntimeState(
      exact, "com.example.game", path);
  const auto locked = floral::nativebridge::PolicyEngine::ApplyRuntimeState(
      process, "com.example.game", path);
  const auto recovered_process =
      floral::nativebridge::PolicyEngine::ApplyRuntimeState(
          exact, "com.example.game:worker", path);
  const auto exhausted = floral::nativebridge::PolicyEngine::ApplyRuntimeState(
      exact, "com.exhausted.game", path);
  unlink(path);

  const bool ok =
      engine.loaded() &&
      Check(exact.kind == floral::nativebridge::BackendKind::kAuto,
            "exact process rule") &&
      Check(exact.candidates.size() == 4 &&
                exact.candidates[0].backend == floral::nativebridge::BackendKind::kHoudini &&
                exact.candidates[0].mode == floral::nativebridge::LoaderMode::kHybrid &&
                exact.candidates[1].mode == floral::nativebridge::LoaderMode::kCompat,
            "candidate order") &&
      Check(process.kind == floral::nativebridge::BackendKind::kHoudini,
            "process rule") &&
      Check(fallback.kind == floral::nativebridge::BackendKind::kHoudini,
            "default rule") &&
      Check(executable.kind == floral::nativebridge::BackendKind::kHoudini,
            "process executable rule") &&
      Check(path_only.kind == floral::nativebridge::BackendKind::kHoudini,
            "path executable rule") &&
      Check(global_executable.kind == floral::nativebridge::BackendKind::kNdk,
            "global executable rule") &&
      Check(refreshed.kind == floral::nativebridge::BackendKind::kHoudini &&
                refreshed.candidates.size() == 2 &&
                refreshed.candidates[0].backend == floral::nativebridge::BackendKind::kHoudini &&
                refreshed.candidates[0].mode == floral::nativebridge::LoaderMode::kHybrid,
            "selected backend reload") &&
      Check(preferred.kind == floral::nativebridge::BackendKind::kAuto &&
                preferred.candidates.size() == 4 &&
                preferred.candidates[0].backend == floral::nativebridge::BackendKind::kNdk &&
                preferred.candidates[0].mode == floral::nativebridge::LoaderMode::kHybrid &&
                preferred.candidates[1].backend == floral::nativebridge::BackendKind::kNdk &&
                preferred.candidates[1].mode == floral::nativebridge::LoaderMode::kCompat &&
                preferred.candidates[2].backend == floral::nativebridge::BackendKind::kHoudini,
            "preferred backend keeps automatic fallback") &&
      Check(recovered.kind == floral::nativebridge::BackendKind::kHoudini &&
                recovered.loader_mode == floral::nativebridge::LoaderMode::kHybrid &&
                recovered.reason == "Android runtime state",
            "runtime recovery state") &&
      Check(locked.kind == floral::nativebridge::BackendKind::kHoudini &&
                locked.reason == "process rule",
            "explicit host rule remains locked") &&
      Check(recovered_process.kind == floral::nativebridge::BackendKind::kAuto,
            "runtime state remains process-specific") &&
      Check(exhausted.exhausted && exhausted.candidates.empty(),
            "runtime circuit breaker");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
