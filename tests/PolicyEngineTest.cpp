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
        "com.example.game": "ndk",
        "com.example.game:remote": "houdini"
      }
    })";
  }

  const floral::nativebridge::PolicyEngine engine =
      floral::nativebridge::PolicyEngine::Load(path);
  const auto exact = engine.Select("com.example.game");
  const auto process = engine.Select("com.example.game:remote");
  const auto fallback = engine.Select("com.other.app");
  unlink(path);

  const bool ok =
      engine.loaded() &&
      Check(exact.kind == floral::nativebridge::BackendKind::kNdk,
            "exact process rule") &&
      Check(process.kind == floral::nativebridge::BackendKind::kHoudini,
            "process rule") &&
      Check(fallback.kind == floral::nativebridge::BackendKind::kHoudini,
            "default rule");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
