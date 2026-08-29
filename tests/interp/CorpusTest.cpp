// Corpus test: every tests/interp/corpus/*.rbc must parse cleanly, verify
// cleanly, and execute under the Tier-0 interpreter with the EXACT expected
// stdout recorded in its .expected twin.
//
// .expected format (the corpus mechanism's STATUS directive):
//   line 1: "RETURNED"                     -> run must return normally
//           "THREW <internal-class-name>"  -> run must end RunStatus::Threw
//                                             with that exception class
//   rest:   the exact expected stdout bytes (Java println output).
//
// Mirrors tests/rbc/CorpusTest.cpp's mechanism (the corpus directory is
// located via the B2_INTERP_CORPUS_DIR compile definition so the test is
// independent of the process working directory); the executed oracle is
// the interpreter API itself, per the semantic-oracle charter (Rule 67).
//
// NOTE on descriptors: programs print through println with ONLY
// (I)(J)(F)(D)(Ljava/lang/String;)()V descriptors - the execBuiltin
// value-tag formatting pin makes (Z)/(C) print decimal (Runtime.cpp
// binding decision), so those are deliberately avoided here.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "b2/interp/Interp.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

#ifndef B2_INTERP_CORPUS_DIR
#define B2_INTERP_CORPUS_DIR "tests/interp/corpus"
#endif

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// Splits off the first line (the STATUS directive); the remainder is the
// expected stdout, byte for byte, WITHOUT the newline that ended line 1.
struct Expected {
  std::string status;  // "RETURNED" or "THREW <class>"
  std::string stdout;  // exact expected bytes
  bool ok = false;
};

Expected parseExpected(const std::string& content) {
  Expected e;
  const std::size_t nl = content.find('\n');
  if (nl == std::string::npos) {
    e.status = content;  // status-only file (no expected output)
  } else {
    e.status = content.substr(0, nl);
    e.stdout = content.substr(nl + 1);
  }
  // Tolerate CRLF in hand-edited files.
  while (!e.status.empty() && (e.status.back() == '\r')) {
    e.status.pop_back();
  }
  e.ok = e.status == "RETURNED" || e.status.starts_with("THREW ");
  return e;
}

void runCorpusFile(const fs::path& rbcPath, int& failures, int& ran) {
  const std::string text = readFile(rbcPath);
  const fs::path expectedPath =
      fs::path(rbcPath.string() + ".expected");

  const std::string expectedText = readFile(expectedPath);
  if (expectedText.empty()) {
    ++failures;
    b2::test::recordFailure(
        __FILE__, __LINE__,
        expectedPath.string() + " is missing or empty (every corpus "
                                "program needs a STATUS + stdout fixture)");
    return;
  }
  const Expected want = parseExpected(expectedText);
  if (!want.ok) {
    ++failures;
    b2::test::recordFailure(
        __FILE__, __LINE__,
        expectedPath.string() + ": first line must be \"RETURNED\" or "
                                "\"THREW <class>\", got \"" +
                                want.status + "\"");
    return;
  }

  ++ran;
  const auto parsed = b2::rbc::parseRbcText(text);
  if (!parsed) {
    ++failures;
    b2::test::recordFailure(
        __FILE__, __LINE__,
        rbcPath.filename().string() + " should parse cleanly, got: " +
            parsed.error().message);
    return;
  }
  for (const b2::rbc::Method& m : parsed->methods) {
    const b2::rbc::VerifyResult v = b2::rbc::verify(m);
    if (!v.ok) {
      ++failures;
      const std::string msg =
          v.diags.empty() ? "rejected without a diagnostic"
                          : v.diags.front().message;
      b2::test::recordFailure(
          __FILE__, __LINE__,
          rbcPath.filename().string() + " should verify cleanly (" +
              m.name + m.descriptor + "), got: " + msg);
      return;
    }
  }

  b2::interp::Interpreter interp(*parsed);
  const b2::interp::RunResult r = interp.run("main", "()V", {});
  const std::string got = interp.runtime().stdout();

  if (want.status == "RETURNED") {
    if (r.status != b2::interp::RunStatus::Returned) {
      ++failures;
      std::string what = rbcPath.filename().string() + ": expected RETURNED";
      if (r.status == b2::interp::RunStatus::Threw) {
        what += ", threw " +
                std::string(interp.runtime().classNameOf(r.exception)) +
                " \"" +
                std::string(interp.runtime().exceptionMessage(r.exception)) +
                "\"";
      } else {
        what += ", got VerifyFailed";
      }
      b2::test::recordFailure(__FILE__, __LINE__, what);
      return;
    }
  } else {
    const std::string wantClass = want.status.substr(6);
    if (r.status != b2::interp::RunStatus::Threw) {
      ++failures;
      b2::test::recordFailure(
          __FILE__, __LINE__,
          rbcPath.filename().string() + ": expected THREW " + wantClass +
              ", got status " +
              std::to_string(static_cast<int>(r.status)));
      return;
    }
    const std::string gotClass(interp.runtime().classNameOf(r.exception));
    if (gotClass != wantClass) {
      ++failures;
      b2::test::recordFailure(__FILE__, __LINE__,
                              rbcPath.filename().string() +
                                  ": expected exception class " + wantClass +
                                  ", got " + gotClass);
      return;
    }
  }

  if (got != want.stdout) {
    ++failures;
    b2::test::recordFailure(
        __FILE__, __LINE__,
        rbcPath.filename().string() + ": stdout mismatch\n  expected: \"" +
            want.stdout + "\"\n  got:      \"" + got + "\"");
  }
}

B2_TEST(interp_corpus) {
  const fs::path dir = B2_INTERP_CORPUS_DIR;
  if (!fs::exists(dir)) {
    CHECK_MSG(false, "corpus directory not found: " + dir.string());
    return;
  }
  int failures = 0;
  int ran = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const fs::path p = entry.path();
    if (p.extension() != ".rbc") continue;
    runCorpusFile(p, failures, ran);
  }
  std::printf("  corpus: %d program(s), %d failure(s)\n", ran, failures);
  CHECK_MSG(ran >= 10, "expected at least 10 corpus programs, ran " +
                           std::to_string(ran));
  CHECK_MSG(failures == 0,
            std::to_string(failures) +
                " corpus program(s) failed (see failures above)");
}

}  // namespace
