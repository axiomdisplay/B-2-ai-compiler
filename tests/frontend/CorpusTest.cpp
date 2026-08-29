// Corpus test: every tests/frontend/corpus/*.java must parse cleanly; every
// *.java.errors file describes diagnostics its .java twin must produce.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ParseUtil.h"
#include "TestHarness.h"

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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

void runCorpusFile(const fs::path& javaPath, bool negative, int& failures,
                   int& positives, int& negatives) {
  const std::string src = readFile(javaPath);
  b2::test::ParseSession session;
  session.run(javaPath.filename().string(), src);

  if (!negative) {
    ++positives;
    if (session.hasErrors()) {
      ++failures;
      b2::test::recordFailure(__FILE__, __LINE__,
                              javaPath.filename().string() +
                                  " should parse cleanly, got:\n" +
                                  session.diagnosticsText());
    }
    return;
  }

  ++negatives;
  const fs::path errorsPath = javaPath.string() + ".errors";
  const std::vector<std::string> expected = splitLines(readFile(errorsPath));
  if (expected.empty()) {
    b2::test::recordFailure(__FILE__, __LINE__,
                            errorsPath.string() + " is empty or missing");
    ++failures;
    return;
  }
  if (!session.hasErrors()) {
    ++failures;
    b2::test::recordFailure(__FILE__, __LINE__,
                            javaPath.filename().string() +
                                " should produce diagnostics: [" +
                                [&] {
                                  std::string joined;
                                  for (const auto& e : expected) joined += e + "; ";
                                  return joined;
                                }() +
                                "] but parsed cleanly");
    return;
  }
  const std::string text = session.diagnosticsText();
  for (const std::string& want : expected) {
    if (text.find(want) == std::string::npos) {
      ++failures;
      b2::test::recordFailure(__FILE__, __LINE__,
                              javaPath.filename().string() + ": expected diagnostic \"" +
                                  want + "\" not found in:\n" + text);
    }
  }
}

B2_TEST(corpus) {
  const fs::path dir = "tests/frontend/corpus";
  if (!fs::exists(dir)) {
    CHECK_MSG(false, "corpus directory not found (run tests from the repo root)");
    return;
  }
  int failures = 0;
  int positives = 0;
  int negatives = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const fs::path p = entry.path();
    if (p.extension() != ".java") continue;
    const bool negative = fs::exists(fs::path(p.string() + ".errors"));
    runCorpusFile(p, negative, failures, positives, negatives);
  }
  std::printf("  corpus: %d positive file(s), %d negative file(s), %d failure(s)\n",
              positives, negatives, failures);
  CHECK_MSG(failures == 0,
            std::to_string(failures) + " corpus file(s) failed (see failures above)");
}

}  // namespace
