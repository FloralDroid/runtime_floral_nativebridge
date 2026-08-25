/*
 * Copyright 2026 FloralDroid
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <dlfcn.h>
#include <cstdint>
#include <cstdio>

namespace {

// The version field is the first member of Android's NativeBridgeCallbacks.
struct NativeBridgeCallbacksPrefix {
  uint32_t version;
};

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s LIBRARY [LIBRARY ...]\n", argv[0]);
    return 2;
  }

  int failures = 0;
  for (int i = 1; i < argc; ++i) {
    void *handle = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      std::fprintf(stderr, "FAIL %s: %s\n", argv[i], dlerror());
      ++failures;
      continue;
    }
    auto *callbacks = reinterpret_cast<const NativeBridgeCallbacksPrefix *>(
        dlsym(handle, "NativeBridgeItf"));
    if (callbacks == nullptr) {
      std::fprintf(stderr, "FAIL %s: NativeBridgeItf is missing\n", argv[i]);
      ++failures;
    } else {
      std::printf("OK %s: NativeBridgeItf v%u\n", argv[i], callbacks->version);
    }
    dlclose(handle);
  }
  return failures == 0 ? 0 : 1;
}
