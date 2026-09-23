#include "harness.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dpu::fabric::test {
namespace {

struct TestCase {
  std::string suite;
  std::string name;
  TestFn fn;
};

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct RunState {
  const TestCase* current{nullptr};
  std::size_t failures{0};
  bool current_failed{false};
  std::vector<std::string> notes;
};

RunState& run_state() {
  static RunState state;
  return state;
}

}  // namespace

void register_test(const char* suite, const char* name, TestFn fn) {
  registry().push_back(TestCase{suite, name, fn});
}

bool evaluate(bool value) { return value; }

void report_failure(const char* file, int line, const std::string& message) {
  RunState& state = run_state();
  state.failures += 1;
  state.current_failed = true;
  const char* suite = state.current != nullptr ? state.current->suite.c_str() : "<none>";
  const char* name = state.current != nullptr ? state.current->name.c_str() : "<none>";
  std::printf("FAIL %s.%s\n  %s:%d\n  %s\n", suite, name, file, line, message.c_str());
  std::fflush(stdout);
}

void report_note(const std::string& note) {
  RunState& state = run_state();
  if (state.current_failed) return;
  state.notes.push_back(note);
}

int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--list") {
      list_only = true;
    } else if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else {
      std::printf("unknown argument: %s\n", argument.c_str());
      return 2;
    }
  }

  std::vector<TestCase>& tests = registry();
  std::sort(tests.begin(), tests.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (lhs.suite != rhs.suite) return lhs.suite < rhs.suite;
    return lhs.name < rhs.name;
  });

  if (list_only) {
    for (const TestCase& test : tests) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }

  std::size_t executed = 0;
  std::size_t failed_tests = 0;
  for (const TestCase& test : tests) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) continue;
    RunState& state = run_state();
    state.current = &test;
    state.current_failed = false;
    state.notes.clear();
    std::printf("[ RUN  ] %s\n", full.c_str());
    std::fflush(stdout);
    test.fn();
    ++executed;
    if (state.current_failed) {
      ++failed_tests;
      std::printf("[ FAIL ] %s\n", full.c_str());
    } else {
      for (const std::string& note : state.notes) {
        std::printf("  note: %s\n", note.c_str());
      }
      std::printf("[  OK  ] %s\n", full.c_str());
    }
    std::fflush(stdout);
  }

  std::printf("\n%d test(s) executed, %d failed, %zu check failure(s)\n", static_cast<int>(executed),
              static_cast<int>(failed_tests), run_state().failures);
  if (executed == 0) {
    std::printf("no tests matched the filter\n");
    return 1;
  }
  return failed_tests == 0 ? 0 : 1;
}

}  // namespace dpu::fabric::test

int main(int argc, char** argv) { return dpu::fabric::test::run_all(argc, argv); }
