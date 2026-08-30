#pragma once
// B-2 codegen - the Tier-1 execution engine: code cache + runtime helpers
// + deopt-to-T0 + exception dispatch into compiled frames.
//
// WHY THIS FILE EXISTS:
// A CompiledCode block is inert bytes; something must enter it, service
// its helper calls (allocation, fields, arrays, monitors, calls - the v0
// object model lives behind interp::Runtime, Rule 15 indices), and honor
// its deopt exits (rebuild a T0 Frame and Interpreter::resume - Amendment
// B.3). That "something" is this engine, and its seam is exactly what T2
// will reuse when it starts emitting its own code against the same
// activation-record and helper ABI (docs/codegen_contract.md SS8/SS9).
//
// LAW PINS (docs/laws.md):
// - Rule 4 / Amendment B.3: deopt direction is T1 -> T0. Every deopt path
//   here ends in Interpreter::resume with a fully reconstructed Frame.
// - Rule 96: T1 is a cache, not a correctness claim. compile/instantiate
//   refusal => the method runs on T0 via the interpreter.
// - Rule 74: exceptions are values (ObjRefs); no C++ exceptions cross the
//   helper ABI (Rule 6). Every failure is a status/return value.
// - Rule 47: invariant violations become java/lang/InternalError results,
//   never crashes.
// - Rule 119: tier behavior is observable - Tier1Stats exposes compiled
//   methods, deopts by reason, helper calls, differential hooks.
// - Rule 114: counters saturate; Rule 16: no strings on hot paths (names
//   live in diagnostics only).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "b2/baseline/Compiler.h"
#include "b2/baseline/Plan.h"
#include "b2/baseline/StencilSet.h"
#include "b2/codegen/Archive.h"
#include "b2/codegen/Instantiate.h"
#include "b2/interp/Interp.h"
#include "b2/interp/Runtime.h"
#include "b2/rbc/Rbc.h"

namespace b2::codegen {

// --- configuration + stats ---------------------------------------------------------

inline constexpr std::uint32_t kT1MaxCallDepthDefault = 512;

struct Tier1Config {
  // Method-heat threshold is NOT a thing in v1: the engine compiles on
  // demand for every method it is asked to run (the tiering heuristics
  // arrive with the profiler; ICDG Phase 1). The budget is the kill switch.
  baseline::CompileOptions plan_options{};

  // Frame depth cap for T1 helper recursion (SS9: native-stack exhaustion
  // becomes a Java-visible StackOverflowError, never a C++ crash).
  std::uint32_t maxCallDepth = kT1MaxCallDepthDefault;
};

struct Tier1Stats {
  std::uint32_t compile_attempts = 0;
  std::uint32_t compile_ok = 0;
  std::uint32_t plan_refusals = 0;       // compilePlan refused (stays on T0)
  std::uint32_t instantiation_refusals = 0; // instantiate refused (stays T0)
  std::uint32_t deopt_trap = 0;          // Trap deopts (incl. athrow)
  std::uint32_t deopt_call_exception = 0;
  std::uint32_t deopt_guard = 0;
  std::uint32_t t0_fallback_executions = 0; // callee ran interpreted
  std::uint64_t t1_entries = 0;         // activations entered (saturating)
  std::uint64_t helper_calls = 0;        // helper invocations (saturating)
  std::uint64_t code_bytes = 0;          // published code total (saturating)
};

// --- run result ---------------------------------------------------------------------

enum class Tier1Status : std::uint8_t {
  Returned,          // entry method returned; result holds the value
  Threw,             // uncaught exception escaped; exception set
  VerifyFailed,      // hard gate: rbc::verify failed (mirrors RunStatus)
  NoSuchMethod,      // entry method not found (mirrors T0's Threw form)
};

struct Tier1RunResult {
  Tier1Status status = Tier1Status::Returned;
  interp::Value result{};              // Returned
  interp::ObjRef exception{};          // Threw
  std::vector<rbc::VerifyDiag> verify_diags; // VerifyFailed
  Tier1Stats stats{};                  // snapshot at completion
};

// --- the engine -----------------------------------------------------------------------
//
// One engine per program execution, owning one Interpreter (shared Runtime:
// heap, statics, profiles - the SAME objects T0 would see). Not thread-safe
// (v0 world is single-threaded; the concurrent form is a future contract).
class Tier1 {
public:
  struct Impl; // pimpl, defined in the engine TU (helpers reach it too)

  Tier1(const rbc::Program& program, const Tier1Config& config = {});
  ~Tier1(); // out-of-line: Impl is incomplete here

  // Verifies the whole program (the same hard gate as T0), resolves the
  // entry method, compiles + instantiates + executes it on T1. Any refusal
  // along the way runs the method on the owned interpreter instead (Rule
  // 96). Never throws, never crashes: every failure is a Tier1RunResult.
  [[nodiscard]] Tier1RunResult run(std::string_view name,
                                   std::string_view descriptor,
                                   std::span<const interp::Value> args);

  // Execute ONE method (by index) on T1 with args (receiver first for
  // instance methods); used by the call helper for callees and by tests.
  // Returns the run status + result/exception for THIS invocation.
  [[nodiscard]] Tier1RunResult runMethod(std::uint32_t method_index,
                                         std::span<const interp::Value> args);

  // Was the last runMethod invocation actually compiled? (Differential
  // tests pin which tier executed; false = interpreted on T0.)
  [[nodiscard]] bool lastRunWasCompiled() const noexcept {
    return last_run_compiled_;
  }

  [[nodiscard]] const Tier1Stats& stats() const noexcept { return stats_; }
  [[nodiscard]] const interp::Interpreter& interp() const noexcept {
    return interp_;
  }
  [[nodiscard]] interp::Interpreter& interp() noexcept { return interp_; }
  [[nodiscard]] const baseline::StencilSet& set() const noexcept { return set_; }

  // Compiled-code lookup for tools/tests (offsets are into exec blocks).
  [[nodiscard]] const CompiledCode* codeFor(std::uint32_t method_index) const;

private:
  std::unique_ptr<Impl> impl_;
  // Keep the interpreter by value: its Runtime owns the heap/statics the
  // helpers and the fallback share (the constructor pins program_ lifetime).
  const rbc::Program& program_;
  Tier1Config cfg_;
  interp::Interpreter interp_;
  baseline::StencilSet set_;
  Tier1Stats stats_;
  bool last_run_compiled_ = false;

  Tier1(const Tier1&) = delete;
  Tier1& operator=(const Tier1&) = delete;
  Tier1(Tier1&&) = delete;
  Tier1& operator=(Tier1&&) = delete;
};

} // namespace b2::codegen
