// Agent Runtime - minimal deterministic test framework.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef AGENT_RUNTIME_TESTS_TEST_FRAMEWORK_HPP
#define AGENT_RUNTIME_TESTS_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace artest {

using TestFunction = void (*)();

struct TestCase {
  const char* suite;
  const char* name;
  TestFunction function;
};

/// Thrown by AR_REQUIRE/AR_REQUIRE_MSG so that requirements work inside
/// value-returning functions and lambdas as well as void functions.
struct RequireFailure {};

struct TestContext {
  std::uint32_t checks = 0;
  std::vector<std::string> failures;
  std::string current_test;

  void fail(const std::string& message);
};

std::vector<TestCase>& registry();
TestContext& context();
int run_all(int argc, char** argv);

struct Registrar {
  Registrar(const char* suite, const char* name, TestFunction function);
};

/// Reports a failure unless the condition holds. Execution continues so that a
/// single test reports every failure it can observe.
void check_impl(bool condition, const char* expression, const char* file, int line);

/// Aborts the current test when the condition does not hold.
void require_impl(bool condition, const char* expression, const char* file, int line);

template <class Left, class Right>
void check_eq_impl(const Left& left, const Right& right, const char* expression, const char* file,
                   int line) {
  if (!(left == right)) {
    std::ostringstream message;
    message << expression << ": " << left << " != " << right;
    context().fail(std::string(file) + ":" + std::to_string(line) + ": " + message.str());
  }
  ++context().checks;
}

}  // namespace artest

#define AR_TEST(suite_name, test_name)                                                     \
  static void suite_name##_##test_name##_body();                                           \
  namespace {                                                                              \
  const ::artest::Registrar suite_name##_##test_name##_registrar(#suite_name, #test_name,  \
                                                                &suite_name##_##test_name##_body); \
  }                                                                                        \
  static void suite_name##_##test_name##_body()

#define AR_CHECK(expression) ::artest::check_impl((expression), #expression, __FILE__, __LINE__)
#define AR_REQUIRE(expression)                                                              \
  do {                                                                                      \
    if (!static_cast<bool>(expression)) {                                                   \
      ++::artest::context().checks;                                                         \
      ::artest::context().fail(std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                               ": requirement failed: " #expression);                        \
      throw ::artest::RequireFailure{};                                                     \
    }                                                                                       \
    ++::artest::context().checks;                                                           \
  } while (false)
#define AR_CHECK_EQ(left, right) ::artest::check_eq_impl((left), (right), #left " == " #right, __FILE__, __LINE__)
#define AR_CHECK_MSG(expression, message)                                                   \
  do {                                                                                      \
    if (!(expression)) {                                                                    \
      ::artest::context().fail(std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                               ": " #expression " -- " + std::string(message));             \
    }                                                                                       \
    ++::artest::context().checks;                                                           \
  } while (false)
#define AR_REQUIRE_MSG(expression, message)                                                 \
  do {                                                                                      \
    if (!static_cast<bool>(expression)) {                                                   \
      ++::artest::context().checks;                                                         \
      ::artest::context().fail(std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                               ": requirement failed: " #expression " -- " +                \
                               std::string(message));                                       \
      throw ::artest::RequireFailure{};                                                     \
    }                                                                                       \
    ++::artest::context().checks;                                                           \
  } while (false)

#endif  // AGENT_RUNTIME_TESTS_TEST_FRAMEWORK_HPP
