#pragma once
// B-2 interpreter tests - minimal self-registration harness.
//
// Mirrors tests/rbc/TestHarness.h exactly (same B2_TEST semantics, same
// registry/failure model) so the whole test tree shares one discipline:
// tests self-register at static-init time, TestMain runs the registry in
// registration order, and the process exit code is the number-of-failures
// discriminator (0 = all green). Test names must be unique across the whole
// binary; every test here is prefixed interp_.
//
// This suite is the semantic oracle for Tier 0 (Rules 67, 72, 133): every
// assertion encodes Java semantics as pinned by docs/rbc_spec.md SS3 and the
// normative comments in include/b2/interp/{Interp,Frame,Runtime,Value}.h.

#include <cstdio>
#include <string>
#include <vector>

namespace b2::test {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

struct Failure {
  std::string file;
  int line;
  std::string what;
};

inline std::vector<Failure>& failures() {
  static std::vector<Failure> f;
  return f;
}

inline void recordFailure(const char* file, int line, const std::string& what) {
  Failure f;
  f.file = file;
  f.line = line;
  f.what = what;
  failures().push_back(std::move(f));
}

inline int runAll() {
  std::printf("running %zu test(s)\n", registry().size());
  for (const TestCase& tc : registry()) {
    const std::size_t before = failures().size();
    tc.fn();
    if (failures().size() == before) {
      std::printf("  PASS %s\n", tc.name);
    } else {
      std::printf("  FAIL %s\n", tc.name);
    }
  }
  if (failures().empty()) {
    std::printf("all %zu test(s) passed\n", registry().size());
    return 0;
  }
  std::printf("\n%zu failure(s):\n", failures().size());
  for (const Failure& f : failures()) {
    std::printf("  %s:%d: %s\n", f.file.c_str(), f.line, f.what.c_str());
  }
  return 1;
}

}  // namespace b2::test

#define B2_CONCAT_(a, b) a##b
#define B2_CONCAT(a, b) B2_CONCAT_(a, b)

#define B2_TEST(name)                                                            \
  static void B2_CONCAT(b2_test_fn_, name)();                                    \
  static const bool B2_CONCAT(b2_test_reg_, name) = [] {                         \
    b2::test::registry().push_back({#name, &B2_CONCAT(b2_test_fn_, name)});      \
    return true;                                                                 \
  }();                                                                            \
  static void B2_CONCAT(b2_test_fn_, name)()

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      b2::test::recordFailure(__FILE__, __LINE__, "CHECK(" #cond ") failed");    \
    }                                                                            \
  } while (false)

#define CHECK_MSG(cond, msg)                                                     \
  do {                                                                           \
    if (!(cond)) {                                                               \
      b2::test::recordFailure(__FILE__, __LINE__, (msg));                        \
    }                                                                            \
  } while (false)
