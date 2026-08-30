// b2jit - the B-2 Tier 1 execution driver.
//
// WHY THIS FILE EXISTS:
// The end-to-end surface of the T1 instantiation milestone: parse an RBC
// text program, verify it (the hard gate), compile + instantiate + execute
// it as x86-64 stencil code on the Tier1 engine, and report the result with
// EXACTLY b2run's launcher shape - the differential contract (Rule 36 form)
// is that b2run and b2jit are observationally indistinguishable on every
// program both can run.
//
// Usage:
//   b2jit program.rbc [--entry NAME DESC] [--stats] [--quiet] [--code NAME]
//                     [--bench N] [--no-fusion]
//
//   --entry NAME DESC   entry method (default: main; descriptor inferred
//                       like b2run: ()V if present, else the String[] form)
//   --stats             print tier counters to stderr after the run
//   --quiet             suppress the "[b2jit] ..." status lines
//   --code NAME         dump the compiled code of every method whose name
//                       contains NAME (the golden/inspection path)
//   --bench N           run N additional times on the same engine and report
//                       steady-state timing to stderr (first run compiles;
//                       the rest reuse the code cache - measures execution)
//   --no-fusion         plan with superinstructions off (the diagnostic
//                       mode; the A/B side of the fusion-set measurements)
//
// Exit status: 0 = normal return, 1 = uncaught Java exception or failure.

#include <csignal>
#include <ucontext.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "b2/codegen/Tier1.h"
#include "b2/interp/Interp.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

namespace {

void segvHandler(int, siginfo_t* info, void* ctx) {
  const auto* uc = static_cast<const ucontext_t*>(ctx);
  std::fprintf(stderr, "[b2jit] SIGSEGV addr=%p rip=%llx rbp=%llx rdi=%llx\n",
               info->si_addr,
               (unsigned long long)uc->uc_mcontext.gregs[REG_RIP],
               (unsigned long long)uc->uc_mcontext.gregs[REG_RBP],
               (unsigned long long)uc->uc_mcontext.gregs[REG_RDI]);
  _exit(139);
}

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

void reportUncaught(b2::interp::Runtime& rt, b2::interp::ObjRef exc) {
  std::string cls(rt.classNameOf(exc));
  for (auto& c : cls) {
    if (c == '/') {
      c = '.';
    }
  }
  std::string msg(rt.exceptionMessage(exc));
  if (msg.empty()) {
    std::fprintf(stderr, "Exception in thread \"main\" %s\n", cls.c_str());
  } else {
    std::fprintf(stderr, "Exception in thread \"main\" %s: %s\n", cls.c_str(),
                 msg.c_str());
  }
}

void reportStats(const b2::codegen::Tier1Stats& s) {
  std::fprintf(stderr,
               "[b2jit] attempts=%u ok=%u planRefused=%u instRefused=%u "
               "deopts(trap=%u callExc=%u guard=%u) t0Fallback=%u "
               "entries=%llu helperCalls=%llu codeBytes=%llu\n",
               s.compile_attempts, s.compile_ok, s.plan_refusals,
               s.instantiation_refusals, s.deopt_trap, s.deopt_call_exception,
               s.deopt_guard, s.t0_fallback_executions,
               static_cast<unsigned long long>(s.t1_entries),
               static_cast<unsigned long long>(s.helper_calls),
               static_cast<unsigned long long>(s.code_bytes));
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: b2jit program.rbc [--entry NAME DESC] [--stats] "
                 "[--quiet] [--code NAME] [--bench N] [--no-fusion]\n");
    return 1;
  }

  std::filesystem::path path;
  std::string entryName = "main";
  std::string entryDesc;
  bool stats = false, quiet = false;
  std::string codeFilter;
  // --bench N (MSG-20260830-005): run the program N times on ONE engine and
  // report steady-state timing (first run compiles; the rest reuse the code
  // cache - the measurement is execution, not compilation). --no-fusion:
  // plan with superinstructions off (the diagnostic mode; the A/B side of the
  // superinstruction set extension measurement).
  std::uint32_t benchRuns = 0;
  bool noFusion = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--entry" && i + 2 < argc) {
      entryName = argv[++i];
      entryDesc = argv[++i];
    } else if (arg == "--stats") {
      stats = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "--code" && i + 1 < argc) {
      codeFilter = argv[++i];
    } else if (arg == "--bench" && i + 1 < argc) {
      benchRuns = static_cast<std::uint32_t>(
          std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--no-fusion") {
      noFusion = true;
    } else if (path.empty()) {
      path = arg;
    } else {
      std::fprintf(stderr, "[b2jit] unexpected argument: %s\n", argv[i]);
      return 1;
    }
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: b2jit program.rbc\n");
    return 1;
  }

  if (getenv("B2JIT_SEGV") != nullptr) {
    struct sigaction sa{};
    sa.sa_sigaction = segvHandler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
  }
  std::string text;
  if (!readFile(path, text)) {
    std::fprintf(stderr, "[b2jit] cannot read %s\n", path.string().c_str());
    return 1;
  }

  auto parsed = b2::rbc::parseRbcText(text);
  if (!parsed) {
    std::fprintf(stderr, "[b2jit] parse error at byte %u: %s\n",
                 parsed.error().offset, parsed.error().message.c_str());
    return 1;
  }

  b2::codegen::Tier1Config cfg;
  if (noFusion) {
    cfg.plan_options.superinstructions = false;
  }
  b2::codegen::Tier1 jit(*parsed, cfg);

  if (entryDesc.empty()) {
    entryDesc = "()V";
    if (!parsed->find(entryName, entryDesc)) {
      entryDesc = "([Ljava/lang/String;)V";
    }
  }

  std::vector<b2::interp::Value> args;
  if (entryDesc == "([Ljava/lang/String;)V") {
    auto arr =
        jit.interp().runtime().newRefArray(jit.interp().runtime().stringClass(), 0);
    args.push_back(b2::interp::Value::refVal(arr));
  }

  const b2::codegen::Tier1RunResult r = [&] {
    if (benchRuns == 0) {
      return jit.run(entryName, entryDesc, args);
    }
    // Warmup run: compiles + instantiates; also the behavioral reference.
    const b2::codegen::Tier1RunResult first =
        jit.run(entryName, entryDesc, args);
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < benchRuns; ++i) {
      const b2::codegen::Tier1RunResult rr =
          jit.run(entryName, entryDesc, args);
      if (rr.status != first.status) {
        std::fprintf(stderr,
                     "[b2jit] bench run %u diverged (status %d != %d); "
                     "refusing to report\n",
                     i, static_cast<int>(rr.status),
                     static_cast<int>(first.status));
        std::exit(1);
      }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double wallNs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    std::fprintf(stderr,
                 "[b2jit] bench fusion=%s runs=%u wall_ms=%.3f "
                 "ns_per_run=%.1f t1_entries=%llu\n",
                 noFusion ? "off" : "on", benchRuns, wallNs / 1.0e6,
                 wallNs / static_cast<double>(benchRuns),
                 static_cast<unsigned long long>(first.stats.t1_entries));
    return first;
  }();

  // Program-visible output first (Java ordering discipline, as b2run).
  std::fwrite(jit.interp().runtime().stdout().data(), 1,
              jit.interp().runtime().stdout().size(), stdout);
  std::fflush(stdout);
  std::fwrite(jit.interp().runtime().stderr().data(), 1,
              jit.interp().runtime().stderr().size(), stderr);

  switch (r.status) {
    case b2::codegen::Tier1Status::Returned:
      break;
    case b2::codegen::Tier1Status::Threw:
      if (!quiet) {
        reportUncaught(jit.interp().runtime(), r.exception);
      }
      break;
    case b2::codegen::Tier1Status::VerifyFailed:
      if (!quiet) {
        std::fprintf(stderr, "[b2jit] verification failed (%zu diagnostics):\n",
                     r.verify_diags.size());
        for (const auto& d : r.verify_diags) {
          std::fprintf(stderr, "  pc %u: %s\n", d.pc, d.message.c_str());
        }
      }
      break;
    case b2::codegen::Tier1Status::NoSuchMethod:
      if (!quiet) {
        std::fprintf(stderr, "[b2jit] entry method not found\n");
      }
      break;
  }

  if (stats) {
    reportStats(r.stats);
  }
  if (!codeFilter.empty()) {
    for (std::size_t i = 0; i < parsed->methods.size(); ++i) {
      if (parsed->methods[i].name.find(codeFilter) == std::string::npos) {
        continue;
      }
      const b2::codegen::CompiledCode* cc = jit.codeFor(
          static_cast<std::uint32_t>(i));
      if (cc == nullptr) {
        std::fprintf(stderr, "[%s] %s%s: not compiled\n",
                     parsed->methods[i].name.c_str(),
                     parsed->methods[i].name.c_str(),
                     parsed->methods[i].descriptor.c_str());
        continue;
      }
      std::fprintf(stderr, "[%s] %s%s compiled:\n%s", codeFilter.c_str(),
                   parsed->methods[i].name.c_str(),
                   parsed->methods[i].descriptor.c_str(),
                   b2::codegen::dumpCode(*cc).c_str());
    }
  }

  return r.status == b2::codegen::Tier1Status::Returned ? 0 : 1;
}
