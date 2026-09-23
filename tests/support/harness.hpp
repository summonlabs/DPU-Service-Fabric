#pragma once

// Minimal, dependency-free test harness.
//
// Each test registers itself at static-initialisation time. A failing check
// records file, line and message and marks the test failed, but does not abort
// it: the remaining checks in the test still run so one run reports every
// problem. The process exits non-zero when any test failed.
//
// There are deliberately no timeouts anywhere in the suite. A test that would
// need one is a defect: synchronisation is by join, barrier or protocol
// completion.

#include <cstdint>
#include <sstream>
#include <type_traits>
#include <string>
#include <string_view>
#include <vector>

#include "dpu/fabric/core/digest.hpp"
#include "dpu/fabric/core/result.hpp"
#include "dpu/fabric/core/types.hpp"

namespace dpu::fabric::test {

using TestFn = void (*)();

void register_test(const char* suite, const char* name, TestFn fn);

/// Records a failed check for the currently running test.
void report_failure(const char* file, int line, const std::string& message);

/// Opaque wrapper around a check condition. It is deliberately not constexpr:
/// a check on a compile-time constant is still a check, and a compiler that
/// folds it into `if (true)` would raise a constant-condition diagnostic for
/// perfectly ordinary test code.
[[nodiscard]] bool evaluate(bool value);

/// Records an informational note that is printed for a passing run.
void report_note(const std::string& note);

[[nodiscard]] int run_all(int argc, char** argv);

/// Renders a value for a failed-check message. Types the harness cannot render
/// fall back to a placeholder rather than failing to compile: a diagnostic is
/// never allowed to break the suite.
template <class T>
concept Streamable = requires(std::ostream& stream, const T& value) { stream << value; };

[[nodiscard]] inline std::string describe(std::string_view value) {
  return std::string{"\""} + std::string{value} + "\"";
}

[[nodiscard]] inline std::string describe(bool value) { return value ? "true" : "false"; }

template <class T>
  requires Streamable<T>
[[nodiscard]] std::string describe(const T& value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

template <class Tag>
[[nodiscard]] std::string describe(const StrongId<Tag>& value) { return value.str(); }

template <class Tag>
[[nodiscard]] std::string describe(const Counter<Tag>& value) {
  return std::to_string(value.value());
}

[[nodiscard]] inline std::string describe(const Digest& value) { return value.hex(); }

template <class T>
[[nodiscard]] std::string describe(const std::vector<T>& value) {
  return "sequence[" + std::to_string(value.size()) + "]";
}

[[nodiscard]] inline std::string describe(const LogicalInstant& value) {
  return "t" + std::to_string(value.ticks);
}

[[nodiscard]] inline std::string describe(ReasonCode code) { return std::string{to_string(code)}; }

template <class T>
  requires std::is_enum_v<T>
[[nodiscard]] std::string describe(T value) {
  return std::string{to_string(value)};
}

[[nodiscard]] inline std::string describe(const Status& status) { return status.message(); }

template <class T>
[[nodiscard]] std::string describe(const Result<T>& result) {
  if (result) return std::string{"Result{value}"};
  return std::string{"Result{"} + result.status().message() + "}";
}

}  // namespace dpu::fabric::test

#define DPUF_TEST(suite_name, test_name)                                                  \
  static void suite_name##_##test_name##_body();                                          \
  namespace {                                                                             \
  const bool suite_name##_##test_name##_registered =                                      \
      (::dpu::fabric::test::register_test(#suite_name, #test_name,                        \
                                          &suite_name##_##test_name##_body),              \
       true);                                                                             \
  }                                                                                       \
  static void suite_name##_##test_name##_body()

#define DPUF_CHECK(condition)                                                             \
  do {                                                                                    \
    if (!::dpu::fabric::test::evaluate(static_cast<bool>(condition))) {                    \
      ::dpu::fabric::test::report_failure(__FILE__, __LINE__, "expected: " #condition);    \
    }                                                                                     \
  } while (false)

#define DPUF_CHECK_EQ(actual, expected)                                                   \
  do {                                                                                    \
    const auto dpu_test_actual = (actual);                                                \
    const auto dpu_test_expected = (expected);                                            \
    if (!::dpu::fabric::test::evaluate(dpu_test_actual == dpu_test_expected)) {            \
      ::dpu::fabric::test::report_failure(                                                \
          __FILE__, __LINE__,                                                             \
          std::string{#actual " == " #expected " (actual="} +                             \
              ::dpu::fabric::test::describe(dpu_test_actual) + ", expected=" +            \
              ::dpu::fabric::test::describe(dpu_test_expected) + ")");                    \
    }                                                                                     \
  } while (false)

#define DPUF_CHECK_NE(actual, unexpected)                                                 \
  do {                                                                                    \
    const auto dpu_test_actual = (actual);                                                \
    const auto dpu_test_unexpected = (unexpected);                                        \
    if (::dpu::fabric::test::evaluate(dpu_test_actual == dpu_test_unexpected)) {           \
      ::dpu::fabric::test::report_failure(                                                \
          __FILE__, __LINE__,                                                             \
          std::string{#actual " != " #unexpected " (both="} +                             \
              ::dpu::fabric::test::describe(dpu_test_actual) + ")");                      \
    }                                                                                     \
  } while (false)

/// Asserts a Status carries exactly the expected reason code. A mismatch prints
/// the observed code and detail, which is the usual way a refusal is diagnosed.
///
/// The status is taken by value on purpose: a reference would bind to a member of
/// a temporary Result and dangle as soon as the statement ended.
#define DPUF_CHECK_CODE(status_value, expected_code)                                      \
  do {                                                                                    \
    const ::dpu::fabric::Status dpu_test_status = (status_value);                         \
    if (!::dpu::fabric::test::evaluate(dpu_test_status.code() == (expected_code))) {       \
      ::dpu::fabric::test::report_failure(                                                \
          __FILE__, __LINE__,                                                             \
          std::string{#status_value " has code "} +                                       \
              std::string{::dpu::fabric::to_string(dpu_test_status.code())} +             \
              ", expected " + std::string{::dpu::fabric::to_string(expected_code)} +      \
              " (" + dpu_test_status.detail() + ")");                                     \
    }                                                                                     \
  } while (false)

#define DPUF_CHECK_OK(status_value)                                                       \
  do {                                                                                    \
    const ::dpu::fabric::Status dpu_test_status = (status_value);                         \
    if (!::dpu::fabric::test::evaluate(dpu_test_status.ok())) {                            \
      ::dpu::fabric::test::report_failure(                                                \
          __FILE__, __LINE__,                                                             \
          std::string{#status_value " is not ok: "} + dpu_test_status.message());         \
    }                                                                                     \
  } while (false)

/// Asserts a Result carries a value, printing the refusal otherwise.
#define DPUF_CHECK_RESULT_OK(result_value)                                                \
  do {                                                                                    \
    const auto& dpu_test_result = (result_value);                                         \
    if (!::dpu::fabric::test::evaluate(dpu_test_result.has_value())) {                     \
      ::dpu::fabric::test::report_failure(                                                \
          __FILE__, __LINE__,                                                             \
          std::string{#result_value " refused: "} + dpu_test_result.status().message());  \
    }                                                                                     \
  } while (false)

/// Asserts an lvalue Result holds a value and returns from the test body when it
/// does not, so a following .value() call can never dereference a refusal.
#define DPUF_REQUIRE(result_lvalue)                                                       \
  do {                                                                                    \
    if (!::dpu::fabric::test::evaluate((result_lvalue).has_value())) {                     \
      ::dpu::fabric::test::report_failure(                                                \
          __FILE__, __LINE__,                                                             \
          std::string{#result_lvalue " refused: "} + (result_lvalue).status().message()); \
      return;                                                                             \
    }                                                                                     \
  } while (false)

/// Asserts a Result was refused, with the expected reason code.
#define DPUF_CHECK_RESULT_CODE(result_value, expected_code)                               \
  do {                                                                                    \
    const auto& dpu_test_result = (result_value);                                         \
    if (::dpu::fabric::test::evaluate(dpu_test_result.has_value())) {                      \
      ::dpu::fabric::test::report_failure(__FILE__, __LINE__,                             \
                                          #result_value " unexpectedly succeeded");        \
    } else if (!::dpu::fabric::test::evaluate(dpu_test_result.status().code() ==           \
                                              (expected_code))) {                          \
      ::dpu::fabric::test::report_failure(                                                \
          __FILE__, __LINE__,                                                             \
          std::string{#result_value " refused with "} +                                   \
              std::string{::dpu::fabric::to_string(dpu_test_result.status().code())} +    \
              ", expected " + std::string{::dpu::fabric::to_string(expected_code)});      \
    }                                                                                     \
  } while (false)

#define DPUF_CHECK_FAILS(status_value)                                                    \
  do {                                                                                    \
    const ::dpu::fabric::Status dpu_test_status = (status_value);                         \
    if (::dpu::fabric::test::evaluate(dpu_test_status.ok())) {                             \
      ::dpu::fabric::test::report_failure(__FILE__, __LINE__, #status_value " unexpectedly ok"); \
    }                                                                                     \
  } while (false)

/// Unwraps a Result inside a test. Terminates the current test body on failure
/// by returning a default value; use only where the value is not further
/// inspected after failure.
#define DPUF_REQUIRE_VALUE(result_value, default_value)                                    \
  ([&]() {                                                                                 \
    auto&& dpu_test_result = (result_value);                                               \
    if (!dpu_test_result) {                                                                \
      ::dpu::fabric::test::report_failure(__FILE__, __LINE__,                              \
                                          std::string{#result_value " failed: "} +         \
                                              dpu_test_result.status().message());         \
      return (default_value);                                                              \
    }                                                                                      \
    return std::move(dpu_test_result).value();                                             \
  }())
