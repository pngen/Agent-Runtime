// Agent Runtime - test framework implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include "test_framework.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace artest {

void TestContext::fail(const std::string& message) { failures.push_back(message); }

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

TestContext& context() {
  static TestContext instance;
  return instance;
}

Registrar::Registrar(const char* suite, const char* name, TestFunction function) {
  registry().push_back(TestCase{suite, name, function});
}

void check_impl(bool condition, const char* expression, const char* file, int line) {
  ++context().checks;
  if (!condition) {
    context().fail(std::string(file) + ":" + std::to_string(line) + ": check failed: " +
                   expression);
  }
}

void require_impl(bool condition, const char* expression, const char* file, int line) {
  ++context().checks;
  if (!condition) {
    context().fail(std::string(file) + ":" + std::to_string(line) + ": requirement failed: " +
                   expression);
  }
}

int run_all(int argc, char** argv) {
  const char* filter = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
      filter = argv[i + 1];
      ++i;
    }
  }
  std::uint32_t total = 0;
  std::uint32_t failed = 0;
  for (const TestCase& test : registry()) {
    const std::string full = std::string(test.suite) + "." + test.name;
    if (filter != nullptr && full.find(filter) == std::string::npos) {
      continue;
    }
    ++total;
    TestContext& ctx = context();
    ctx.failures.clear();
    ctx.checks = 0;
    ctx.current_test = full;
    try {
      test.function();
    } catch (const RequireFailure&) {
      // The requirement already recorded a deterministic failure message.
    }
    if (ctx.failures.empty()) {
      std::printf("[ PASS ] %s (%u checks)\n", full.c_str(), ctx.checks);
    } else {
      ++failed;
      std::printf("[ FAIL ] %s (%u checks)\n", full.c_str(), ctx.checks);
      for (const std::string& failure : ctx.failures) {
        std::printf("         %s\n", failure.c_str());
      }
    }
    std::fflush(stdout);
  }
  std::printf("\n%u tests, %u failed\n", total, failed);
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

}  // namespace artest
