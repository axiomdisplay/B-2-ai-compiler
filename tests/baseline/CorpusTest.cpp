// Corpus test: every tests/interp/corpus/*.rbc must parse cleanly, and for
// EVERY method in every program the T1 plan builder must uphold the
// charter's honest-outcome contract:
//
//   result.ok   => verifyPlan(plan, method, set).ok AND the instances fully
//                  tile [0, code.size()) (checked independently, not just
//                  via the auditor);
//   !result.ok  => the refusal is NoStencilForOp or MissingBackedgePoll -
//                  the documented v0 reality for interpreter-oriented
//                  corpus programs (nearly every main ends in an
//                  un-quickened println invoke, whose stencil waits on IC /
//                  ABI patching, and sum_loop-shaped loops lower with the
//                  poll-before-backedge convention instead of the
//                  poll-at-loop-head convention the default options check).
//                  NEVER UnverifiableMethod (the corpus verifies), NEVER
//                  BudgetExceeded (defaults are generous), NEVER
//                  InternalInvariant (a builder bug would fail the suite).
//
// Mirrors tests/interp/CorpusTest.cpp's mechanism: the corpus directory is
// located via the B2_BASELINE_CORPUS_DIR compile definition (an absolute
// path baked at configure time) so the test is independent of the process
// working directory; files are visited in sorted filename order so the
// per-file outcome report is deterministic.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "b2/baseline/Compiler.h"
#include "b2/baseline/Plan.h"
#include "b2/baseline/StencilSet.h"
#include "b2/rbc/Rbc.h"
#include "b2/rbc/RbcText.h"

#ifndef B2_BASELINE_CORPUS_DIR
#define B2_BASELINE_CORPUS_DIR "tests/interp/corpus"
#endif

namespace {

namespace fs = std::filesystem;

using b2::baseline::compilePlan;
using b2::baseline::compilePlanFor;
using b2::baseline::CompileOptions;
using b2::baseline::PlanResult;
using b2::baseline::RefuseReason;
using b2::baseline::StencilSet;
using b2::baseline::verifyPlan;

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// Independent full-coverage walk (plan invariant 1): the instances tile
// [0, code.size()) in rbc order with no gaps or overlaps. Deliberately not
// delegated to verifyPlan - the corpus must not merely trust the auditor.
bool fullCoverage(const b2::baseline::StencilPlan& plan,
                  const b2::rbc::Method& m, std::string& why) {
  std::uint32_t expected = 0;
  for (const b2::baseline::StencilInstance& inst : plan.instances) {
    if (inst.rbc_pc_start != expected) {
      why = "instance starts at rbc pc " + std::to_string(inst.rbc_pc_start) +
            ", expected " + std::to_string(expected);
      return false;
    }
    if (inst.rbc_pc_end <= inst.rbc_pc_start) {
      why = "instance has empty rbc range at pc " +
            std::to_string(inst.rbc_pc_start);
      return false;
    }
    expected = inst.rbc_pc_end;
  }
  if (expected != m.code.size()) {
    why = "instances end at rbc pc " + std::to_string(expected) +
          " but code size is " + std::to_string(m.code.size());
    return false;
  }
  return true;
}

struct FileOutcome {
  std::string file;
  std::vector<std::string> lines; // one per method, deterministic
};

void runCorpusFile(const fs::path& rbcPath, const StencilSet& set,
                   FileOutcome& outcome, int& plannedOk, int& refused) {
  const std::string text = readFile(rbcPath);
  outcome.file = rbcPath.filename().string();
  const auto parsed = b2::rbc::parseRbcText(text);
  if (!parsed) {
    outcome.lines.push_back("PARSE FAILED: " + parsed.error().message);
    b2::test::recordFailure(__FILE__, __LINE__,
                            rbcPath.filename().string() +
                                " should parse cleanly, got: " +
                                parsed.error().message);
    return;
  }
  for (std::size_t mi = 0; mi < parsed->methods.size(); ++mi) {
    const b2::rbc::Method& m = parsed->methods[mi];
    const PlanResult r =
        compilePlan(*parsed, static_cast<std::uint32_t>(mi), set, CompileOptions{});
    if (r.ok) {
      ++plannedOk;
      const auto check = verifyPlan(r.plan, m, set);
      std::string why;
      const bool covered = fullCoverage(r.plan, m, why);
      outcome.lines.push_back(m.name + m.descriptor + ": planned (" +
                              std::to_string(r.plan.instances.size()) +
                              " instances, code_size " +
                              std::to_string(r.plan.code_size) + ")");
      if (!check.ok) {
        b2::test::recordFailure(
            __FILE__, __LINE__,
            rbcPath.filename().string() + " " + m.name + m.descriptor +
                ": planned but verifyPlan rejected it: " + check.error);
      }
      if (!covered) {
        b2::test::recordFailure(
            __FILE__, __LINE__,
            rbcPath.filename().string() + " " + m.name + m.descriptor +
                ": planned without full instruction coverage: " + why);
      }
    } else {
      ++refused;
      outcome.lines.push_back(m.name + m.descriptor + ": refused " +
                              std::string(b2::baseline::refuseReasonName(
                                  r.reason)));
      const bool allowed = r.reason == RefuseReason::NoStencilForOp ||
                           r.reason == RefuseReason::MissingBackedgePoll;
      if (!allowed) {
        b2::test::recordFailure(
            __FILE__, __LINE__,
            rbcPath.filename().string() + " " + m.name + m.descriptor +
                ": refused with reason " +
                std::string(b2::baseline::refuseReasonName(r.reason)) +
                " (" + r.detail +
                ") - only no-stencil-for-op / missing-backedge-poll are "
                "documented v0 outcomes");
      }
      if (r.detail.empty()) {
        b2::test::recordFailure(
            __FILE__, __LINE__,
            rbcPath.filename().string() + " " + m.name + m.descriptor +
                ": refusal carries no diagnostic (Compiler.h: never empty)");
      }
    }
  }
}

B2_TEST(baseline_corpus) {
  const fs::path dir = B2_BASELINE_CORPUS_DIR;
  if (!fs::exists(dir)) {
    CHECK_MSG(false, "corpus directory not found: " + dir.string());
    return;
  }
  // Sorted order: deterministic sweep + deterministic failure report.
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const fs::path p = entry.path();
    if (p.extension() != ".rbc") continue;
    files.push_back(p);
  }
  std::sort(files.begin(), files.end());

  const StencilSet set = b2::baseline::builtinStencilSetV0();
  std::vector<FileOutcome> outcomes;
  int plannedOk = 0;
  int refused = 0;
  const std::size_t failuresBefore = b2::test::failures().size();
  for (const fs::path& p : files) {
    FileOutcome outcome;
    runCorpusFile(p, set, outcome, plannedOk, refused);
    outcomes.push_back(std::move(outcome));
  }

  // Per-file outcomes on failure (and a summary line either way).
  std::printf("  corpus: %zu program(s), %d method(s) planned, %d refused\n",
              files.size(), plannedOk, refused);
  if (b2::test::failures().size() != failuresBefore) {
    for (const FileOutcome& o : outcomes) {
      std::printf("  [%s]\n", o.file.c_str());
      for (const std::string& line : o.lines) {
        std::printf("    %s\n", line.c_str());
      }
    }
  }

  // Sanity floors (set v2, MSG-20260830-004): the un-quickened invoke*/ldc
  // flip made most corpus methods plannable - only the no-poll loop pair
  // (fib_loop/sum_loop: MissingBackedgePoll) and any indy/multianewarray
  // holdouts refuse. Floors (not exact pins) so corpus additions do not
  // break the sweep.
  CHECK_MSG(files.size() >= 14,
            "expected at least 14 corpus programs, found " +
                std::to_string(files.size()));
  CHECK_MSG(plannedOk >= 15,
            "expected at least 15 plannable corpus methods, got " +
                std::to_string(plannedOk));
  CHECK_MSG(refused >= 2,
            "expected at least 2 refused corpus methods, got " +
                std::to_string(refused));

  // The known-positive: uncaught.rbc's main plans under the default entry.
  const fs::path uncaught = dir / "uncaught.rbc";
  if (!fs::exists(uncaught)) {
    CHECK_MSG(false, "uncaught.rbc missing from the corpus");
    return;
  }
  const auto parsed = b2::rbc::parseRbcText(readFile(uncaught));
  CHECK_MSG(static_cast<bool>(parsed), "uncaught.rbc must parse");
  if (parsed) {
    const PlanResult r =
        compilePlanFor(*parsed, "main", "()V", set, CompileOptions{});
    CHECK_MSG(r.ok, "uncaught.rbc main ()V must plan (the known-positive): " +
                        r.detail);
    if (r.ok) {
      const auto check =
          verifyPlan(r.plan, parsed->methods[0], set);
      CHECK_MSG(check.ok,
                "uncaught.rbc plan failed verifyPlan: " + check.error);
      std::string why;
      CHECK_MSG(fullCoverage(r.plan, parsed->methods[0], why),
                "uncaught.rbc plan lacks full coverage: " + why);
      CHECK(r.plan.instances.size() == 4); // iconst, iconst, idiv, return
    }
  }
}

}  // namespace
