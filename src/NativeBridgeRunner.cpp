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

constexpr char kRunnerPolicyProperty[] =
    "ro.floral.nativebridge.runner.policy";
constexpr char kNdk32RunnerProperty[] = "ro.floral.nativebridge.runner.ndk32";
constexpr char kNdk64RunnerProperty[] = "ro.floral.nativebridge.runner.ndk64";
constexpr char kHoudini32RunnerProperty[] =
    "ro.floral.nativebridge.runner.houdini32";
constexpr char kHoudini64RunnerProperty[] =
    "ro.floral.nativebridge.runner.houdini64";
constexpr char kDefaultRunnerPolicy[] = "/ipc/floral_stream/nativebridge.json";
constexpr char kDefaultNdk32Runner[] =
    "/system/bin/ndk_translation_program_runner_binfmt_misc";
constexpr char kDefaultNdk64Runner[] =
    "/system/bin/ndk_translation_program_runner_binfmt_misc_arm64";
constexpr char kDefaultHoudini32Runner[] = "/system/bin/houdini";
constexpr char kDefaultHoudini64Runner[] = "/system/bin/houdini64";

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

std::string PolicyPath() {
  const std::string configured =
      android::base::GetProperty(kRunnerPolicyProperty, "");
  return configured.empty() ? kDefaultRunnerPolicy : configured;
}

bool IsSelf(const std::string &path) {
  char executable[PATH_MAX] = {};
  const ssize_t size = readlink("/proc/self/exe", executable,
                                sizeof(executable) - 1);
  return size > 0 && path == std::string(executable, size);
}

int ExecBackend(const std::string &runner, int argc, char **argv,
                const char *backend) {
  if (runner.empty() || IsSelf(runner)) {
    LOG(WARNING) << "Floral NativeBridge runner unavailable for " << backend
                 << ": " << runner;
    return -1;
  }
  if (access(runner.c_str(), X_OK) != 0) {
    LOG(WARNING) << "Floral NativeBridge runner unavailable for " << backend
                 << ": " << runner << " (" << strerror(errno) << ")";
    return -1;
  }

  std::vector<char *> arguments;
  arguments.reserve(static_cast<size_t>(argc));
  arguments.push_back(const_cast<char *>(runner.c_str()));
  for (int index = 1; index < argc; ++index) {
    arguments.push_back(argv[index]);
  }
  arguments.push_back(nullptr);

  setenv("FLORAL_NATIVEBRIDGE_BACKEND", backend, 1);
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

  const floral::nativebridge::PolicyEngine policy =
      floral::nativebridge::PolicyEngine::Load(PolicyPath());
  const auto selection = policy.SelectExecutable("", argv[elf_index]);
  for (const auto backend : selection.candidates) {
    if (backend == floral::nativebridge::BackendKind::kAuto) {
      continue;
    }
    ExecBackend(RunnerPath(backend, elf_class), argc, argv,
                floral::nativebridge::BackendKindName(backend));
  }

  LOG(ERROR) << "No usable Floral NativeBridge executable backend for "
             << argv[elf_index] << " (policy=" << selection.name << ")";
  return 127;
}
