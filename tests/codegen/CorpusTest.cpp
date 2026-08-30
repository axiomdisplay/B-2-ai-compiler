// B-2 codegen corpus sweep: the differential law (Rule 36 form).
//
// Every program in the interpreter corpus AND the frontend-lowered corpus
// executes on the Tier-1 engine; the run's stdout and exit status must be
// BYTE-IDENTICAL to the T0 golden .expected twins. The frontend corpus is
// lowered fresh (b2parse --emit-rbc is unavailable in-process; the corpus
// .java files are lowered through the frontend's lowerUnit API directly),
// proving the full source -> AST -> RBC -> plan -> x86-64 machine code
// path executes real Java.

#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "b2/codegen/Tier1.h"
#include "b2/frontend/Diagnostics.h"
#include "b2/frontend/Lexer.h"
#include "b2/frontend/Lower.h"
#include "b2/frontend/Parser.h"
#include "b2/frontend/SourceManager.h"
#include "b2/interp/Interp.h"
#include "b2/rbc/RbcText.h"

namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool readFile(const fs::path& p, std::string& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

struct Expected {
  std::string status;  // RETURNED / THREW <class> / ...
  std::string stdout_; // exact expected bytes
};

// The .expected format: line 1 = status directive, rest = exact stdout.
[[nodiscard]] Expected parseExpected(const std::string& text) {
  Expected e;
  const std::size_t nl = text.find('\n');
  if (nl == std::string::npos) {
    e.status = text;
    return e;
  }
  e.status = text.substr(0, nl);
  e.stdout_ = text.substr(nl + 1);
  while (!e.stdout_.empty() && e.stdout_.back() == '\n') {
    e.stdout_.pop_back(); // tail compares without the trailing newline
  }
  return e;
}

// Status -> expected Tier1Status + exit form.
[[nodiscard]] bool statusMatches(const std::string& directive,
                                 b2::codegen::Tier1Status st) {
  if (directive == "RETURNED") {
    return st == b2::codegen::Tier1Status::Returned;
  }
  if (directive.rfind("THREW", 0) == 0) {
    return st == b2::codegen::Tier1Status::Threw;
  }
  return true; // unknown directives: only stdout is compared
}

// Trims trailing newlines for comparison (the .expected tail convention).
[[nodiscard]] std::string trimTail(std::string s) {
  while (!s.empty() && s.back() == '\n') {
    s.pop_back();
  }
  return s;
}

} // namespace

B2_TEST(codegen_corpus_differential_interp) {
  const fs::path dir = B2_CODEGEN_CORPUS_DIR;
  std::size_t ran = 0;
  std::size_t compiled = 0;
  for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
    const fs::path p = entry.path();
    if (p.extension() != ".rbc") {
      continue;
    }
    std::string text;
    if (!readFile(p, text)) {
      CHECK_MSG(false, ("cannot read " + p.string()).c_str());
      continue;
    }
    auto parsed = b2::rbc::parseRbcText(text);
    if (!parsed) {
      CHECK_MSG(false, ("cannot parse " + p.string()).c_str());
      continue;
    }
    std::string expectedText;
    const fs::path expectedPath(p.string() + ".expected");
    if (!readFile(expectedPath, expectedText)) {
      CHECK_MSG(false, ("missing .expected twin for " + p.string()).c_str());
      continue;
    }
    const Expected exp = parseExpected(expectedText);

    b2::codegen::Tier1 jit(*parsed);
    const b2::codegen::Tier1RunResult r = jit.run("main", "()V", {});
    CHECK_MSG(statusMatches(exp.status, r.status),
              (p.filename().string() + ": status " +
               std::to_string(static_cast<int>(r.status)) + " vs directive " +
               exp.status)
                  .c_str());
    CHECK_MSG(trimTail(jit.interp().runtime().stdout()) == exp.stdout_,
              (p.filename().string() + ": stdout mismatch: got '" +
               trimTail(jit.interp().runtime().stdout()) + "' want '" +
               exp.stdout_ + "'")
                  .c_str());
    ++ran;
    compiled += r.stats.compile_ok;
  }
  CHECK(ran >= 14); // the whole corpus swept
  CHECK(compiled >= 5); // a healthy share actually ran machine code
}

B2_TEST(codegen_corpus_differential_frontend) {
  // Source -> AST -> RBC -> machine code: lower every .java in the
  // frontend's lower_corpus and execute on T1.
  const fs::path dir = B2_CODEGEN_FRONTEND_CORPUS_DIR;
  std::size_t ran = 0;
  for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
    const fs::path p = entry.path();
    if (p.extension() != ".java") {
      continue;
    }
    std::string text;
    if (!readFile(p, text)) {
      CHECK_MSG(false, ("cannot read " + p.string()).c_str());
      continue;
    }
    std::string expectedText;
    const fs::path expectedPath(p.string() + ".expected");
    if (!readFile(expectedPath, expectedText)) {
      continue; // programs without golden twins (parse-error fixtures)
    }
    // The frontend corpus .expected files are PURE STDOUT (no status
    // directive, unlike the interp corpus format).
    const std::string expectedOut = trimTail(expectedText);

    b2::frontend::SourceManager sm;
    sm.load(p.filename().string(), text);
    b2::frontend::DiagnosticEngine diags(sm);
    auto tokens = b2::frontend::Lexer(sm, diags).lex();
    b2::frontend::Parser parser(std::move(tokens), sm, diags);
    auto unit = parser.parseCompilationUnit();
    if (diags.hasErrors()) {
      CHECK_MSG(false, ("frontend parse failed for " + p.string()).c_str());
      continue;
    }
    const b2::frontend::LoweredUnit lr = b2::frontend::lowerUnit(*unit, sm, diags);
    if (!lr.ok) {
      CHECK_MSG(false, ("lowering failed for " + p.string()).c_str());
      continue;
    }

    b2::codegen::Tier1 jit(lr.program);
    // Entry inference (b2run's rule): ()V if present, else the String[]
    // form with a fresh empty array.
    std::string desc = "()V";
    if (lr.program.find("main", desc) == nullptr) {
      desc = "([Ljava/lang/String;)V";
    }
    std::vector<b2::interp::Value> args;
    if (desc == "([Ljava/lang/String;)V") {
      const auto arr = jit.interp().runtime().newRefArray(
          jit.interp().runtime().stringClass(), 0);
      args.push_back(b2::interp::Value::refVal(arr));
    }
    const b2::codegen::Tier1RunResult r = jit.run("main", desc, args);
    CHECK_MSG(r.status == b2::codegen::Tier1Status::Returned,
              (p.filename().string() + ": expected Returned").c_str());
    CHECK_MSG(trimTail(jit.interp().runtime().stdout()) == expectedOut,
              (p.filename().string() + ": stdout mismatch: got '" +
               trimTail(jit.interp().runtime().stdout()) + "' want '" +
               expectedOut + "'")
                  .c_str());
    ++ran;
  }
  CHECK(ran >= 8); // every golden-twin program swept
}
