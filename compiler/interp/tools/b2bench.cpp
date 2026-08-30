// b2bench - the Tier-0 dispatch micro-benchmark.
//
// WHY THIS FILE EXISTS:
// The computed-goto milestone (interp_contract.md SS4 v1, MSG-20260830-005)
// needs an observable, reproducible dispatch-overhead measurement: run one
// corpus program R times on the interpreter, report retired instructions,
// wall time, and instructions/second. The twin binary b2bench-portable
// (same source, linked against b2_interp_portable - the strict-c++26 switch
// core) answers the same questions; the two throughputs together are the
// Law-36-style dispatch A/B (same program, same semantics, two dispatchers).
//
// Usage:
//   b2bench program.rbc [--runs N] [--entry NAME DESC] [--quiet]
//
//   --runs N            timed runs after one warmup (default 200)
//   --entry NAME DESC   entry method (default: main; ()V if present, else
//                       the String[] form, like b2run)
//   --quiet             suppress the banner; print only the summary line
//
// Output summary line (stable format, tools may parse):
//   bench program=<name> runs=<N> instructions=<total> wall_ms=<ms>
//   ns_per_run=<mean> mips=<instructions per second / 1e6>
// Exit status: 0 = all runs returned/threw identically, 1 = any run failed
// or diverged from the first run's status/output shape (a bench must never
// paper over a behavioral difference).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "b2/interp/Interp.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

namespace {

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

void usage() {
  std::fprintf(stderr,
               "usage: b2bench program.rbc [--runs N] [--entry NAME DESC] "
               "[--quiet]\n");
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 1;
  }
  std::vector<std::string> args(argv + 1, argv + argc);
  std::filesystem::path programPath;
  std::string entryName = "main";
  std::string entryDesc;
  std::uint32_t runs = 200;
  bool quiet = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--runs" && i + 1 < args.size()) {
      runs = static_cast<std::uint32_t>(
          std::strtoul(args[++i].c_str(), nullptr, 10));
      if (runs == 0) {
        runs = 1;
      }
    } else if (args[i] == "--entry" && i + 2 < args.size()) {
      entryName = args[++i];
      entryDesc = args[++i];
    } else if (args[i] == "--quiet") {
      quiet = true;
    } else if (args[i][0] == '-') {
      usage();
      return 1;
    } else {
      programPath = args[i];
    }
  }
  if (programPath.empty()) {
    usage();
    return 1;
  }

  std::string text;
  if (!readFile(programPath, text)) {
    std::fprintf(stderr, "b2bench: cannot read %s\n",
                 programPath.string().c_str());
    return 1;
  }
  const auto parsed = b2::rbc::parseRbcText(text);
  if (!parsed) {
    std::fprintf(stderr, "b2bench: parse error at byte %u: %s\n",
                 parsed.error().offset, parsed.error().message.c_str());
    return 1;
  }
  const b2::rbc::Program& program = *parsed;

  // Entry descriptor inference, b2run-style: ()V if present, else the
  // String[] form.
  if (entryDesc.empty()) {
    entryDesc = "()V";
    for (const b2::rbc::Method& m : program.methods) {
      if (m.name == entryName && m.descriptor == "([Ljava/lang/String;)V") {
        entryDesc = "([Ljava/lang/String;)V";
        break;
      }
    }
  }

  b2::interp::InterpConfig cfg;
  cfg.collectStats = true;
  b2::interp::Interpreter interp(program, cfg);
  const std::vector<b2::interp::Value> noArgs;

  // Warmup + behavioral stability check: every run must reach the same
  // status (a bench number from diverging runs is meaningless).
  std::uint64_t instructions = 0;
  b2::interp::RunStatus firstStatus = b2::interp::RunStatus::VerifyFailed;
  auto runOnce = [&]() {
    const b2::interp::RunResult r =
        interp.run(entryName, entryDesc, noArgs);
    instructions += r.stats.instructions;
    return r.status;
  };
  firstStatus = runOnce();
  const auto t0 = std::chrono::steady_clock::now();
  for (std::uint32_t i = 0; i < runs; ++i) {
    const b2::interp::RunStatus s = runOnce();
    if (s != firstStatus) {
      std::fprintf(stderr,
                   "b2bench: run %u diverged (status %d != %d); refusing to "
                   "report\n",
                   i, static_cast<int>(s), static_cast<int>(firstStatus));
      return 1;
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double wallNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  const double wallMs = wallNs / 1.0e6;
  const double nsPerRun = wallNs / static_cast<double>(runs);
  const double mips =
      static_cast<double>(instructions) / (wallNs / 1.0e3);

  if (!quiet) {
    std::fprintf(stderr, "[b2bench] dispatch=%s program=%s runs=%u\n",
                 B2_BENCH_DISPATCH_NAME, programPath.filename().c_str(), runs);
  }
  std::printf(
      "bench program=%s runs=%u instructions=%llu wall_ms=%.3f "
      "ns_per_run=%.1f mips=%.2f\n",
      programPath.filename().c_str(), runs,
      static_cast<unsigned long long>(instructions), wallMs, nsPerRun, mips);
  return 0;
}
