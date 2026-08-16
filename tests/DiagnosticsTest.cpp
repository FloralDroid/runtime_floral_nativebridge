/*
 * Copyright 2026 FloralDroid
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <gtest/gtest.h>

#include <nativebridge/native_bridge_diagnostics.h>

namespace android::native_bridge_diagnostics {
namespace {

TEST(NativeBridgeDiagnosticsTest, MatchesMainAndChildProcesses) {
  EXPECT_TRUE(ProcessMatches("com.example.app", "com.example.app"));
  EXPECT_TRUE(ProcessMatches("com.example.app", "com.example.app:remote"));
  EXPECT_TRUE(
      ProcessMatches("com.example.app:remote", "com.example.app:remote"));
  EXPECT_FALSE(ProcessMatches("com.example.app", "com.example.application"));
  EXPECT_FALSE(ProcessMatches("com.example.app:remote", "com.example.app"));
}

TEST(NativeBridgeDiagnosticsTest, RequiresAnExplicitProcessFilter) {
  EXPECT_FALSE(ProcessMatches(nullptr, "com.example.app"));
  EXPECT_FALSE(ProcessMatches("", "com.example.app"));
  EXPECT_FALSE(ProcessMatches("com.example.app", nullptr));
  EXPECT_TRUE(ProcessMatches("*", "com.example.app"));
}

TEST(NativeBridgeDiagnosticsTest, MatchesLibraryPathOrBasename) {
  constexpr char kLibraryPath[] =
      "/data/app/com.example.app/lib/arm64/libsample.so";
  EXPECT_TRUE(LibraryMatches("libsample.so", kLibraryPath));
  EXPECT_TRUE(LibraryMatches(kLibraryPath, kLibraryPath));
  EXPECT_TRUE(LibraryMatches("", kLibraryPath));
  EXPECT_TRUE(LibraryMatches("*", kLibraryPath));
  EXPECT_FALSE(LibraryMatches("libother.so", kLibraryPath));
  EXPECT_FALSE(LibraryMatches("libsample.so", nullptr));
}

} // namespace
} // namespace android::native_bridge_diagnostics
