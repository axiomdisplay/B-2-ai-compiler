// b2plan - the B-2 Tier 1 stencil-plan driver.
//
// WHY THIS FILE EXISTS:
// The end-to-end surface of the T1 plan stage: parse an RBC text program
// (the same parseRbcText call b2run uses), build the v0 target-neutral
// StencilSet, compile the entry method into a StencilPlan, print the
// deterministic golden dump (dumpPlan) to stdout, and re-audit the plan with
// verifyPlan as a final assertion. Mirrors b2parse/b2rbc/b2run in CLI shape
// so the toolchain reads uniformly.
//
// Usage:
//   b2plan file.rbc [--entry NAME DESC] [--no-fusion] [--no-backedge-check]
//                   [--check-only] [--stats] [--quiet]
//
//   --entry NAME DESC     entry method (default: main ()V)
//   --no-fusion           disable superinstruction fusion (the diagnostic
//                         mode that pins opcode-stencil-only plans)
//   --no-backedge-check   disable the require_backedge_polls membership
//                         check (for programs lowered with the
//                         poll-before-backedge convention instead of the
//                         poll-at-loop-head convention CompileOptions checks)
//   --check-only          compile + verifyPlan only; no dump on stdout
//   --stats               print the stats summary line (it is also the
//                         default behavior; the flag is accepted for CLI
//                         symmetry with b2run and is idempotent)
//   --quiet               suppress the stats summary line on success
//
// Exit status: 0 = plan compiled and verified; 1 = refusal (printed as
// "refused: <reason> <detail>" to stderr); 2 = usage or parse error.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "b2/baseline/Compiler.h"
#include "b2/rbc/RbcText.h"

namespace {

// WHY: reading via rdbuf keeps the whole program in one contiguous buffer so
// TextError offsets index the exact input bytes (same discipline as b2run).
[[nodiscard]] bool readFile(const std::filesystem::path& p, std::string& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

void reportStats(const b2::baseline::StencilPlan& plan) {
  // Time-free counters only (Rule 124: the summary is deterministic).
  std::fprintf(stderr,
               "[b2plan] instances=%llu fusions=%llu patches=%llu deopt_points=%llu "
               "safepoints=%llu code_bytes=%llu\n",
               static_cast<unsigned long long>(plan.instances.size()),
               static_cast<unsigned long long>(plan.fusion_count),
               static_cast<unsigned long long>(plan.patch_count),
               static_cast<unsigned long long>(plan.deopt_points.size()),
               static_cast<unsigned long long>(plan.safepoint_count),
               static_cast<unsigned long long>(plan.code_size));
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: b2plan file.rbc [--entry NAME DESC] [--no-fusion] "
                 "[--no-backedge-check] [--check-only] [--stats] [--quiet]\n");
    return 2;
  }

  std::filesystem::path path;
  std::string entryName = "main";
  std::string entryDesc = "()V";
  bool fusion = true;
  bool backedgeCheck = true;
  bool checkOnly = false;
  bool stats = false;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--entry" && i + 2 < argc) {
      entryName = argv[++i];
      entryDesc = argv[++i];
    } else if (arg == "--no-fusion") {
      fusion = false;
    } else if (arg == "--no-backedge-check") {
      backedgeCheck = false;
    } else if (arg == "--check-only") {
      checkOnly = true;
    } else if (arg == "--stats") {
      stats = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (path.empty() && arg.size() > 0 && arg[0] != '-') {
      path = argv[i];
    } else {
      std::fprintf(stderr, "[b2plan] unexpected argument: %s\n", argv[i]);
      return 2;
    }
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: b2plan file.rbc [--entry NAME DESC] ...\n");
    return 2;
  }

  std::string text;
  if (!readFile(path, text)) {
    std::fprintf(stderr, "[b2plan] cannot read %s\n", path.string().c_str());
    return 2;
  }

  auto parsed = b2::rbc::parseRbcText(text);
  if (!parsed) {
    const auto& err = parsed.error();
    std::fprintf(stderr, "[b2plan] parse error at byte %u: %s\n", err.offset,
                 err.message.c_str());
    return 2;
  }

  const b2::baseline::StencilSet set = b2::baseline::builtinStencilSetV0();

  b2::baseline::CompileOptions options;
  options.superinstructions = fusion;
  options.require_backedge_polls = backedgeCheck;

  const b2::baseline::PlanResult result = b2::baseline::compilePlanFor(
      *parsed, entryName, entryDesc, set, options);
  if (!result.ok) {
    std::fprintf(stderr, "refused: %s %s\n",
                 b2::baseline::refuseReasonName(result.reason).data(),
                 result.detail.c_str());
    return 1;
  }

  if (!checkOnly) {
    std::fputs(b2::baseline::dumpPlan(result.plan, set).c_str(), stdout);
  }

  // Final assertion (belt and suspenders): compilePlan already self-audits,
  // so a failure here is an internal error, never a plan-quality signal.
  if (result.plan.method_index >= parsed->methods.size()) {
    std::fprintf(stderr, "[b2plan] internal error: plan method index out of range\n");
    return 1;
  }
  const b2::baseline::PlanCheckResult check = b2::baseline::verifyPlan(
      result.plan, parsed->methods[result.plan.method_index], set);
  if (!check.ok) {
    std::fprintf(stderr, "[b2plan] internal error: post-compile verifyPlan failed: %s\n",
                 check.error.c_str());
    return 1;
  }

  if (!quiet || stats) {
    reportStats(result.plan);
  }
  return 0;
}
