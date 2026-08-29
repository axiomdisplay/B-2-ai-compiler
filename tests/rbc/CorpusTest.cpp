// Corpus test: every tests/rbc/corpus/*.rbc must parse cleanly AND every
// method in it must pass b2::rbc::verify(); every *.rbc.errors file
// describes an error its .rbc twin must produce (parse error or first
// verifier diagnostic), pinned as a substring of the reported message.
//
// Mirrors tests/frontend/CorpusTest.cpp's mechanism (negative files are
// detected by a sibling .errors file), except the corpus directory is located
// via the B2_RBC_CORPUS_DIR compile definition so the test is independent of
// the process working directory.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

#ifndef B2_RBC_CORPUS_DIR
#define B2_RBC_CORPUS_DIR "tests/rbc/corpus"
#endif

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      if (!cur.empty() && cur.back() == '\r') cur.pop_back();
      lines.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) lines.push_back(cur);
  return lines;
}

// Verifies every method of a parsed program; returns "" when all pass and
// the first failing diagnostic's message otherwise.
std::string firstVerifyFailure(const b2::rbc::Program& p) {
  for (const b2::rbc::Method& m : p.methods) {
    const b2::rbc::VerifyResult r = b2::rbc::verify(m);
    if (!r.ok) {
      if (r.diags.empty()) {
        return "rejected without a diagnostic";
      }
      return r.diags.front().message;
    }
  }
  return "";
}

void runCorpusFile(const fs::path& rbcPath, bool negative, int& failures,
                   int& positives, int& negatives) {
  const std::string text = readFile(rbcPath);
  const auto parsed = b2::rbc::parseRbcText(text);

  if (!negative) {
    ++positives;
    if (!parsed) {
      ++failures;
      b2::test::recordFailure(
          __FILE__, __LINE__,
          rbcPath.filename().string() + " should parse cleanly, got: " +
              parsed.error().message);
      return;
    }
    const std::string verifyErr = firstVerifyFailure(*parsed);
    if (!verifyErr.empty()) {
      ++failures;
      b2::test::recordFailure(
          __FILE__, __LINE__,
          rbcPath.filename().string() + " should verify cleanly, got: " +
              verifyErr);
    }
    return;
  }

  ++negatives;
  const fs::path errorsPath = rbcPath.string() + ".errors";
  const std::vector<std::string> expected = splitLines(readFile(errorsPath));
  if (expected.empty() || expected.front().empty()) {
    ++failures;
    b2::test::recordFailure(__FILE__, __LINE__,
                            errorsPath.string() + " is empty or missing");
    return;
  }
  const std::string want = expected.front();

  // The failure may be a parse error or (if parsing succeeds) the first
  // verifier diagnostic.
  std::string reported;
  if (!parsed) {
    reported = parsed.error().message;
  } else {
    reported = firstVerifyFailure(*parsed);
  }
  if (reported.empty()) {
    ++failures;
    b2::test::recordFailure(
        __FILE__, __LINE__,
        rbcPath.filename().string() + " should fail with \"" + want +
            "\" but parsed and verified cleanly");
    return;
  }
  if (reported.find(want) == std::string::npos) {
    ++failures;
    b2::test::recordFailure(__FILE__, __LINE__,
                            rbcPath.filename().string() +
                                ": expected error \"" + want +
                                "\" not found in: " + reported);
  }
}

B2_TEST(rbc_corpus) {
  const fs::path dir = B2_RBC_CORPUS_DIR;
  if (!fs::exists(dir)) {
    CHECK_MSG(false, "corpus directory not found: " + dir.string());
    return;
  }
  int failures = 0;
  int positives = 0;
  int negatives = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const fs::path p = entry.path();
    if (p.extension() != ".rbc") continue;
    const bool negative = fs::exists(fs::path(p.string() + ".errors"));
    runCorpusFile(p, negative, failures, positives, negatives);
  }
  std::printf("  corpus: %d positive file(s), %d negative file(s), %d failure(s)\n",
              positives, negatives, failures);
  CHECK_MSG(failures == 0,
            std::to_string(failures) + " corpus file(s) failed (see failures above)");
}

}  // namespace
