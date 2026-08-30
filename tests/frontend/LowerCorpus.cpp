// B-2 Frontend lowering corpus: every tests/frontend/lower_corpus/*.java is
// parsed, lowered, VERIFIED (every method), and EXECUTED on the T0
// interpreter; the program's stdout must match its .java.expected twin
// byte-for-byte. This is the source -> RBC -> execution pipeline test: the
// same three stages the b2parse --emit-rbc | b2run shell pipeline runs.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "LowerUtil.h"
#include "TestHarness.h"

namespace {

namespace fs = std::filesystem;

std::string readFileText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

struct CorpusCase {
  std::string name;
  fs::path java;
  fs::path expected;
};

std::vector<CorpusCase> collectCases() {
  std::vector<CorpusCase> out;
  const fs::path dir = B2_LOWER_CORPUS_DIR;
  if (!fs::exists(dir)) {
    return out;
  }
  for (const fs::directory_entry& e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) {
      continue;
    }
    const fs::path p = e.path();
    if (p.extension() != ".java") {
      continue;
    }
    // Skip corpus files whose parse is expected to fail (they carry an
    // .errors twin and belong to the parse-only corpus).
    if (fs::exists(p.string() + ".errors")) {
      continue;
    }
    CorpusCase c;
    c.name = p.stem().string();
    c.java = p;
    c.expected = p.string() + ".expected";
    out.push_back(std::move(c));
  }
  std::sort(out.begin(), out.end(),
            [](const CorpusCase& a, const CorpusCase& b) { return a.name < b.name; });
  return out;
}

void runCorpus() {
  const std::vector<CorpusCase> cases = collectCases();
  CHECK_MSG(!cases.empty(), "lowering corpus is empty (missing dir?)");
  std::size_t ran = 0;
  std::size_t executed = 0;
  for (const CorpusCase& c : cases) {
    const std::string src = readFileText(c.java);
    CHECK_MSG(!src.empty(), "cannot read " + c.java.string());
    b2::test::LowerSession s;
    s.runLower(c.java.string(), src);
    const bool loweredOk = s.lowerOk();
    CHECK_MSG(loweredOk, "lowering failed for " + c.java.string());
    if (!loweredOk) {
      continue;
    }
    const bool verified = b2::test::verifyAll(s.lowered->program);
    CHECK_MSG(verified, "verification failed for " + c.java.string());
    if (!verified) {
      continue;
    }
    ++ran;
    if (!fs::exists(c.expected)) {
      // No .expected twin: the program must still lower + verify.
      continue;
    }
    const b2::test::ExecOutcome o = s.executeMain();
    CHECK_MSG(o.ok, "execution failed to start for " + c.java.string());
    const std::string want = readFileText(c.expected);
    CHECK_MSG(o.stdoutText == want,
              c.java.string() + ": stdout mismatch\n  expected: " +
                  want + "\n  actual:   " + o.stdoutText);
    ++executed;
  }
  CHECK_MSG(ran == cases.size(),
            "only " + std::to_string(ran) + "/" + std::to_string(cases.size()) +
                " corpus programs lowered+verified");
  CHECK_MSG(executed >= 7,
            "expected at least 7 executed corpus programs, got " +
                std::to_string(executed));
}

B2_TEST(lowerCorpusPipeline) { runCorpus(); }

}  // namespace
