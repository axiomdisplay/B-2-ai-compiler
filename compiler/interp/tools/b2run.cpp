// b2run - the B-2 Tier 0 execution driver.
//
// WHY THIS FILE EXISTS:
// The end-to-end surface of the backend kickoff: parse an RBC text program,
// verify it (the hard gate - the interpreter refuses unverified input), run
// it in T0, and report the result exactly like a Java launcher: program
// stdout to stdout, an uncaught exception to stderr with the JVM's report
// shape and exit status 1. Mirrors b2parse (frontend) and b2rbc (middle end).
//
// Usage:
//   b2run program.rbc [--entry NAME DESC] [--trace] [--stats] [--quiet]
//
//   --entry NAME DESC   entry method (default: main; descriptor is inferred:
//                       ()V if present, else ([Ljava/lang/String;)V which is
//                       passed an empty String[])
//   --trace             print T0 frame-state dumps at every safepoint poll
//                       (the deopt fixture stream) to stderr
//   --stats             print interpreter counters to stderr after the run
//   --quiet             suppress the "[b2run] ..." status line on failure
//
// Exit status: 0 = normal return (including a caught exception path), 1 =
// uncaught Java exception or verify/parse failure.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "b2/interp/Interp.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

namespace {

// WHY: reading via rdbuf into a string keeps the whole program in one
// contiguous buffer so TextError offsets index the exact input bytes.
[[nodiscard]] bool readFile(const std::filesystem::path& p, std::string& out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

void reportVerifyFailure(const b2::interp::RunResult& r) {
  std::fprintf(stderr, "[b2run] verification failed (%zu diagnostics):\n",
               r.verifyDiags.size());
  for (const auto& d : r.verifyDiags) {
    std::fprintf(stderr, "  pc %u: %s\n", d.pc, d.message.c_str());
  }
}

// Java launcher shape: Exception in thread "main" <dotted class>: <message>
void reportUncaught(b2::interp::Runtime& rt, b2::interp::ObjRef exc) {
  std::string cls(rt.classNameOf(exc));
  for (auto& c : cls) if (c == '/') c = '.';
  std::string msg(rt.exceptionMessage(exc));
  if (msg.empty()) {
    std::fprintf(stderr, "Exception in thread \"main\" %s\n", cls.c_str());
  } else {
    std::fprintf(stderr, "Exception in thread \"main\" %s: %s\n", cls.c_str(),
                 msg.c_str());
  }
}

void reportStats(const b2::interp::InterpStats& s) {
  std::fprintf(stderr,
               "[b2run] instructions=%llu polls=%llu calls=%llu icHits=%llu "
               "icMisses=%llu exceptions=%llu allocations=%llu\n",
               static_cast<unsigned long long>(s.instructions),
               static_cast<unsigned long long>(s.polls),
               static_cast<unsigned long long>(s.calls),
               static_cast<unsigned long long>(s.icHits),
               static_cast<unsigned long long>(s.icMisses),
               static_cast<unsigned long long>(s.exceptions),
               static_cast<unsigned long long>(s.allocations));
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: b2run program.rbc [--entry NAME DESC] "
                         "[--trace] [--stats] [--quiet]\n");
    return 1;
  }

  std::filesystem::path path;
  std::string entryName = "main";
  std::string entryDesc;
  bool trace = false, stats = false, quiet = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--entry" && i + 2 < argc) {
      entryName = argv[++i];
      entryDesc = argv[++i];
    } else if (arg == "--trace") {
      trace = true;
    } else if (arg == "--stats") {
      stats = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (path.empty()) {
      path = arg;
    } else {
      std::fprintf(stderr, "[b2run] unexpected argument: %s\n", argv[i]);
      return 1;
    }
  }
  if (path.empty()) {
    std::fprintf(stderr, "usage: b2run program.rbc\n");
    return 1;
  }

  std::string text;
  if (!readFile(path, text)) {
    std::fprintf(stderr, "[b2run] cannot read %s\n", path.string().c_str());
    return 1;
  }

  auto parsed = b2::rbc::parseRbcText(text);
  if (!parsed) {
    std::fprintf(stderr, "[b2run] parse error at byte %u: %s\n",
                 parsed.error().offset, parsed.error().message.c_str());
    return 1;
  }

  b2::interp::InterpConfig cfg;
  cfg.traceSafepoints = trace;

  b2::interp::Interpreter interp(*parsed, cfg);

  // Entry descriptor inference (see file header).
  if (entryDesc.empty()) {
    entryDesc = "()V";
    if (!parsed->find(entryName, entryDesc)) {
      entryDesc = "([Ljava/lang/String;)V";
    }
  }

  // Build the argument vector: a fresh empty String[] for the array form,
  // nothing for the plain form.
  std::vector<b2::interp::Value> args;
  if (entryDesc == "([Ljava/lang/String;)V") {
    auto arr = interp.runtime().newRefArray(interp.runtime().stringClass(), 0);
    args.push_back(b2::interp::Value::refVal(arr));
  }

  b2::interp::RunResult r = interp.run(entryName, entryDesc, args);

  // Program-visible output first (Java ordering discipline: the program's
  // own output precedes any launcher diagnostics).
  std::fwrite(interp.runtime().stdout().data(), 1,
              interp.runtime().stdout().size(), stdout);
  std::fflush(stdout);
  std::fwrite(interp.runtime().stderr().data(), 1,
              interp.runtime().stderr().size(), stderr);

  switch (r.status) {
  case b2::interp::RunStatus::Returned:
    break;
  case b2::interp::RunStatus::Threw:
    if (!quiet) reportUncaught(interp.runtime(), r.exception);
    break;
  case b2::interp::RunStatus::VerifyFailed:
    if (!quiet) reportVerifyFailure(r);
    break;
  }

  if (trace) {
    std::fwrite(r.safepointTrace.data(), 1, r.safepointTrace.size(), stderr);
  }
  if (stats) reportStats(r.stats);

  return r.status == b2::interp::RunStatus::Returned ? 0 : 1;
}
