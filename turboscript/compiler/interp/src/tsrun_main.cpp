// tsrun — TurboScript Tier 0 driver.
// Usage: tsrun <file.tsbc> [--verify-only] [--dump] [--dump-feedback]
//               [--stats] [--time] [--check <expected.out>]
// Exit codes: 0 ok; 1 runtime failure / check mismatch; 2 usage/compile error.
// --time measures the run() execution phase only (assembly, verification and
// module load are excluded) and prints "elapsed_ms=<x.xx>" on stderr.
// Host environment: the native global `print` (ToString of args joined with
// a single space, newline-terminated) — driver contract,
// docs/interp_contract.md. Program output is buffered and flushed once, so
// --check can compare it exactly against an expected file (Rule 52:
// self-contained, reproducible tests).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ts_assembler.h"
#include "ts_interpreter.h"
#include "ts_verifier.h"

namespace {

// Buffered program output (Rule 52: deterministic, diffable runs).
std::string g_output;

ts::JsResult<ts::Value> nativePrint(ts::Isolate& isolate, ts::Value,
                                    const ts::Value* args, uint32_t argc) {
  std::u16string line;
  for (uint32_t i = 0; i < argc; i++) {
    if (i > 0) line += u" ";
    ts::JsResult<std::u16string> s = isolate.displayString(args[i]);
    if (!s) return std::unexpected(s.error());
    line += *s;
  }
  line += u"\n";
  g_output += ts::utf16ToUtf8(line);
  return ts::Value::undefined();
}

std::string readFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void printDiag(const ts::TsDiagnostic& d) {
  std::fprintf(stderr, "%s:%u: error: %s\n", d.file.c_str(), d.line,
               d.message.c_str());
  if (!d.expected.empty() || !d.actual.empty()) {
    std::fprintf(stderr, "  expected: %s\n  actual:   %s\n",
                 d.expected.c_str(), d.actual.c_str());
  }
  if (!d.hint.empty()) {
    std::fprintf(stderr, "  hint: %s\n", d.hint.c_str());
  }
}

void flushOutput() {
  std::fputs(g_output.c_str(), stdout);
  g_output.clear();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: tsrun <file.tsbc> [--verify-only] [--dump] "
                 "[--dump-feedback] [--stats] [--time] [--check <expected.out>]\n");
    return 2;
  }
  std::string path = argv[1];
  bool verifyOnly = false, dump = false, dumpFeedback = false, stats = false;
  bool timing = false;
  std::string checkPath;
  for (int i = 2; i < argc; i++) {
    if (std::strcmp(argv[i], "--verify-only") == 0) {
      verifyOnly = true;
    } else if (std::strcmp(argv[i], "--dump") == 0) {
      dump = true;
    } else if (std::strcmp(argv[i], "--dump-feedback") == 0) {
      dumpFeedback = true;
    } else if (std::strcmp(argv[i], "--stats") == 0) {
      stats = true;
    } else if (std::strcmp(argv[i], "--time") == 0) {
      timing = true;
    } else if (std::strcmp(argv[i], "--check") == 0 && i + 1 < argc) {
      checkPath = argv[++i];
    } else {
      std::fprintf(stderr, "tsrun: unknown option %s\n", argv[i]);
      return 2;
    }
  }

  std::string source = readFile(path.c_str());
  if (source.empty()) {
    std::fprintf(stderr, "tsrun: cannot read %s (empty or missing)\n",
                 path.c_str());
    return 2;
  }

  ts::SymbolTable symbols;
  ts::Assembler assembler(path, symbols);
  ts::TsResult<std::unique_ptr<ts::Module>> module = assembler.assemble(source);
  if (!module) {
    printDiag(module.error());
    return 2;
  }
  ts::TsResult<bool> verified = ts::Verifier::verify(**module, symbols, path);
  if (!verified) {
    printDiag(verified.error());
    return 2;
  }
  if (verifyOnly) {
    std::printf("OK\n");
    return 0;
  }
  if (dump) {
    std::fputs(ts::disassembleModule(**module, symbols).c_str(), stdout);
  }

  ts::Isolate isolate;
  if (stats) isolate.setCountOpcodes(true);
  if (ts::TsResult<bool> loaded = isolate.loadModule(**module, symbols); !loaded) {
    printDiag(loaded.error());
    return 2;
  }
  isolate.registerNative("print", nativePrint);

  const std::chrono::steady_clock::time_point runStart =
      std::chrono::steady_clock::now();
  ts::JsResult<ts::Value> result = isolate.run();
  if (timing) {
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - runStart)
            .count();
    std::fprintf(stderr, "elapsed_ms=%.2f\n", elapsedMs);
  }
  const bool failed = !result;
  if (failed) {
    const ts::Value& thrown = result.error().thrown;
    std::u16string text = isolate.formatUncaught(thrown);
    std::fprintf(stderr, "Uncaught %s\n", ts::utf16ToUtf8(text).c_str());
  }
  const std::string produced = g_output;  // snapshot for --check (Rule 52)
  flushOutput();
  if (failed) return 1;

  if (dumpFeedback) {
    for (const auto& fn : (*module)->functions) {
      std::printf("fn@%u feedback:\n", fn->index);
      const std::vector<ts::FeedbackSlot>& slots =
          isolate.feedbackFor(fn.get());
      for (size_t i = 0; i < slots.size(); i++) {
        const ts::FeedbackSlot& s = slots[i];
        const char* kind = s.kind == ts::FeedbackKind::Property   ? "prop"
                           : s.kind == ts::FeedbackKind::Binary   ? "bin"
                           : s.kind == ts::FeedbackKind::Branch   ? "br"
                           : s.kind == ts::FeedbackKind::Element  ? "elem"
                                                                  : "call";
        std::printf("  slot %zu (%s):", i, kind);
        if (s.kind == ts::FeedbackKind::Property) {
          std::printf(" distinct=%u hits=%u megamorphic=%d",
                      s.distinctShapes, s.propertyHits,
                      s.distinctShapes >= ts::kMegamorphicThreshold ? 1 : 0);
        } else if (s.kind == ts::FeedbackKind::Element) {
          // Kind ids: 0 = non-array receiver, 1..6 = ElementsKind + 1.
          static const char* kElemKindNames[] = {
              "non-array", "packed-smi", "holey-smi", "packed-double",
              "holey-double", "packed-tagged", "holey-tagged"};
          std::printf(" distinct=%u hits=%u kinds=[", s.distinctShapes,
                      s.propertyHits);
          for (uint32_t k = 0; k < s.distinctShapes; k++) {
            uint32_t id = s.shapeIds[k];
            const char* n = id < 7 ? kElemKindNames[id] : "?";
            std::printf("%s%s x%u", k ? ", " : "", n, s.shapeCounts[k]);
          }
          std::printf("] megamorphic=%d",
                      s.distinctShapes >= ts::kMegamorphicThreshold ? 1 : 0);
        } else if (s.kind == ts::FeedbackKind::Binary) {
          std::printf(" smi=%u num=%u str=%u big=%u obj=%u other=%u",
                      s.classCounts[0], s.classCounts[1], s.classCounts[2],
                      s.classCounts[3], s.classCounts[4], s.classCounts[5]);
        } else if (s.kind == ts::FeedbackKind::Branch) {
          std::printf(" taken=%u notTaken=%u", s.takenCount,
                      s.notTakenCount);
        } else {
          std::printf(" calls=%u known=[", s.callCount);
          for (uint32_t k = 0; k < ts::kCallProfileRing; k++) {
            if (s.callees[k] >= 0) {
              std::printf("%sfn@%d x%u", k ? ", " : "", s.callees[k],
                          s.calleeCounts[k]);
            }
          }
          std::printf("] unknown=%u", s.unknownCallees);
        }
        std::printf("\n");
      }
    }
  }

  if (stats) {
    std::fprintf(stderr, "opcode execution counts:\n");
    const std::vector<uint64_t>& counts = isolate.opcodeCounts();
    for (uint32_t i = 0; i < counts.size(); i++) {
      if (counts[i] > 0) {
        std::fprintf(stderr, "  %-20s %llu\n",
                     ts::opcodeInfo(static_cast<ts::Opcode>(i)).name,
                     static_cast<unsigned long long>(counts[i]));
      }
    }
    std::fprintf(stderr, "heap allocations: %llu\n",
                 static_cast<unsigned long long>(
                     isolate.heap().allocationCount()));
  }

  if (!checkPath.empty()) {
    std::string expected = readFile(checkPath.c_str());
    if (produced != expected) {
      std::fprintf(stderr, "check FAILED: output differs from %s\n",
                   checkPath.c_str());
      std::fprintf(stderr, "--- expected ---\n%s--- actual ---\n%s",
                   expected.c_str(), produced.c_str());
      return 1;
    }
    std::printf("check OK\n");
  }
  return 0;
}
