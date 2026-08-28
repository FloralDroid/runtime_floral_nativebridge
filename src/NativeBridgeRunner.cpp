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

#include "floral/nativebridge/PolicyEngine.h"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits.h>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/properties.h>

namespace {

constexpr char kNdk32RunnerProperty[] = "ro.floral.nativebridge.runner.ndk32";
constexpr char kNdk64RunnerProperty[] = "ro.floral.nativebridge.runner.ndk64";
constexpr char kHoudini32RunnerProperty[] =
    "ro.floral.nativebridge.runner.houdini32";
constexpr char kHoudini64RunnerProperty[] =
    "ro.floral.nativebridge.runner.houdini64";
constexpr char kDefaultBackendProperty[] =
    "ro.floral.nativebridge.default_backend";
constexpr char kDefaultNdk32Runner[] =
    "/system/floral/ndk/bin/ndk_translation_program_runner_binfmt_misc";
constexpr char kDefaultNdk64Runner[] =
    "/system/floral/ndk/bin/ndk_translation_program_runner_binfmt_misc_arm64";
constexpr char kDefaultHoudini32Runner[] = "/system/floral/houdini/bin/houdini";
constexpr char kDefaultHoudini64Runner[] = "/system/floral/houdini/bin/houdini64";

enum class ElfClass {
  kUnknown,
  k32,
  k64,
};

ElfClass ReadElfClass(const char *path) {
  std::ifstream file(path, std::ios::binary);
  std::array<unsigned char, 5> header = {};
  if (!file.read(reinterpret_cast<char *>(header.data()), header.size()) ||
      header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' ||
      header[3] != 'F') {
    return ElfClass::kUnknown;
  }
  if (header[4] == 1) {
    return ElfClass::k32;
  }
  return header[4] == 2 ? ElfClass::k64 : ElfClass::kUnknown;
}

int FindElfArgument(int argc, char **argv) {
  for (int index = 1; index < argc; ++index) {
    if (ReadElfClass(argv[index]) != ElfClass::kUnknown) {
      return index;
    }
  }
  return -1;
}

std::string RunnerPath(floral::nativebridge::BackendKind backend,
                       ElfClass elf_class) {
  switch (backend) {
  case floral::nativebridge::BackendKind::kNdk:
    return elf_class == ElfClass::k32
               ? android::base::GetProperty(kNdk32RunnerProperty,
                                            kDefaultNdk32Runner)
               : android::base::GetProperty(kNdk64RunnerProperty,
                                            kDefaultNdk64Runner);
  case floral::nativebridge::BackendKind::kHoudini:
    return elf_class == ElfClass::k32
               ? android::base::GetProperty(kHoudini32RunnerProperty,
                                            kDefaultHoudini32Runner)
               : android::base::GetProperty(kHoudini64RunnerProperty,
                                            kDefaultHoudini64Runner);
  case floral::nativebridge::BackendKind::kAuto:
    return {};
  }
  return {};
}

const char *BackendLibraryDirectory(
    floral::nativebridge::BackendKind backend, ElfClass elf_class) {
  switch (backend) {
  case floral::nativebridge::BackendKind::kNdk:
    return elf_class == ElfClass::k32 ? "/system/floral/ndk/lib"
                                      : "/system/floral/ndk/lib64";
  case floral::nativebridge::BackendKind::kHoudini:
    return elf_class == ElfClass::k32 ? "/system/floral/houdini/lib"
                                      : "/system/floral/houdini/lib64";
  case floral::nativebridge::BackendKind::kAuto:
    return nullptr;
  }
  return nullptr;
}

floral::nativebridge::BackendKind DefaultBackend(ElfClass elf_class) {
  if (elf_class == ElfClass::k32) {
    return floral::nativebridge::BackendKind::kHoudini;
  }

  const char *process_backend = getenv("FLORAL_NATIVEBRIDGE_BACKEND");
  const auto process_kind = process_backend == nullptr
                                ? floral::nativebridge::BackendKind::kAuto
                                : floral::nativebridge::ParseBackendKind(process_backend);
  if (process_kind != floral::nativebridge::BackendKind::kAuto) {
    return process_kind;
  }

  const std::string configured = android::base::GetProperty(
      kDefaultBackendProperty, "auto");
  if (configured == "auto") {
    return elf_class == ElfClass::k64
               ? floral::nativebridge::BackendKind::kNdk
               : floral::nativebridge::BackendKind::kHoudini;
  }
  return floral::nativebridge::ParseBackendKind(configured) ==
                 floral::nativebridge::BackendKind::kNdk
             ? floral::nativebridge::BackendKind::kNdk
             : floral::nativebridge::BackendKind::kHoudini;
}

bool AutoBackendConfigured() {
  return getenv("FLORAL_NATIVEBRIDGE_BACKEND") == nullptr &&
         android::base::GetProperty(kDefaultBackendProperty, "auto") == "auto";
}

floral::nativebridge::BackendKind AlternateBackend(
    floral::nativebridge::BackendKind backend) {
  return backend == floral::nativebridge::BackendKind::kNdk
             ? floral::nativebridge::BackendKind::kHoudini
             : floral::nativebridge::BackendKind::kNdk;
}

bool IsSelf(const std::string &path) {
  char executable[PATH_MAX] = {};
  const ssize_t size = readlink("/proc/self/exe", executable,
                                sizeof(executable) - 1);
  return size > 0 && path == std::string(executable, size);
}

int ExecBackend(const std::string &runner, int argc, char **argv,
                floral::nativebridge::BackendKind backend,
                ElfClass elf_class) {
  const char *backend_name = floral::nativebridge::BackendKindName(backend);
  if (runner.empty() || IsSelf(runner)) {
    LOG(WARNING) << "Floral NativeBridge runner unavailable for " << backend_name
                 << ": " << runner;
    return -1;
  }
  if (access(runner.c_str(), X_OK) != 0) {
    LOG(WARNING) << "Floral NativeBridge runner unavailable for " << backend_name
                 << ": " << runner << " (" << strerror(errno) << ")";
    return -1;
  }

  const char *library_directory = BackendLibraryDirectory(backend, elf_class);
  if (library_directory == nullptr ||
      setenv("LD_LIBRARY_PATH", library_directory, 1) != 0) {
    LOG(ERROR) << "Unable to configure Floral NativeBridge library path for "
               << backend_name << ": " << strerror(errno);
    return -1;
  }

  std::vector<char *> arguments;
  arguments.reserve(static_cast<size_t>(argc));
  arguments.push_back(const_cast<char *>(runner.c_str()));
  for (int index = 1; index < argc; ++index) {
    arguments.push_back(argv[index]);
  }
  arguments.push_back(nullptr);

  setenv("FLORAL_NATIVEBRIDGE_BACKEND", backend_name, 1);
  execv(runner.c_str(), arguments.data());
  LOG(ERROR) << "Unable to exec Floral NativeBridge runner " << runner
             << ": " << strerror(errno);
  return -1;
}

} // namespace

int main(int argc, char **argv) {
  const int elf_index = FindElfArgument(argc, argv);
  if (elf_index < 0) {
    LOG(ERROR) << "Floral NativeBridge runner requires an ELF path";
    return 127;
  }

  const ElfClass elf_class = ReadElfClass(argv[elf_index]);
  if (elf_class == ElfClass::kUnknown) {
    LOG(ERROR) << "Floral NativeBridge runner cannot identify "
               << argv[elf_index];
    return 126;
  }

  const floral::nativebridge::BackendKind backend = DefaultBackend(elf_class);
  const bool allow_fallback = AutoBackendConfigured();
  if (ExecBackend(RunnerPath(backend, elf_class), argc, argv, backend,
                  elf_class) == 0) {
    return 0;
  }

  if (allow_fallback) {
    const auto fallback = AlternateBackend(backend);
    if (elf_class != ElfClass::k32 ||
        fallback != floral::nativebridge::BackendKind::kNdk) {
      if (ExecBackend(RunnerPath(fallback, elf_class), argc, argv, fallback,
                      elf_class) == 0) {
        return 0;
      }
    }
  }

  LOG(ERROR) << "No usable Floral NativeBridge executable backend for "
             << argv[elf_index] << " (backend="
             << floral::nativebridge::BackendKindName(backend) << ")";
  return 127;
}
