#pragma once
// B-2 baseline (T1) - the compiler API: verified RBC in, StencilPlan out.
//
// WHY THIS FILE EXISTS:
// This is T1's entire public surface. One entry point (compilePlan), one
// budget, one kill switch, one deterministic dump. Everything else the tier
// will grow (instantiation, IC patching, code cache) consumes the plan; it
// does not change this seam. The API deliberately mirrors the interpreter's
// verify-first discipline: the builder TRUSTS that the method verified and
// re-checks only what selection itself needs (operand ranges it patches).
//
// LAW PINS (docs/laws.md):
// - Amendment A / B.1: NO IR, no graph, no dataflow. compilePlan is a single
//   linear scan over code[] with a peephole window for superinstructions.
//   Complexity is O(N x C) with C = candidates per opcode (single digits).
// - Amendment A: "always safe to abandon or fall back from". Any budget
//   overrun, any op without an Available stencil, any invariant the builder
//   cannot uphold => Refused with a reason. NEVER a partial plan.
// - Rule 10/45 discipline (T1 form): the budget is the kill switch; there is
//   no cost model because there is no optimization to cost.
// - Rule 114/124: telemetry counters are plain saturating integers; identical
//   inputs produce identical plans and identical dumps.
// - Rule 96: when T1 refuses or deopts, T0 runs the method. The plan is a
//   cache, not a correctness claim.

#include <cstdint>
#include <string>
#include <string_view>

#include "b2/baseline/Plan.h"
#include "b2/baseline/StencilSet.h"
#include "b2/rbc/Rbc.h"
#include "b2/rbc/Verifier.h"

namespace b2::baseline {

// --- budget (the kill switch) ---------------------------------------------------

// Every counter is a hard ceiling. Hitting ANY ceiling refuses the plan (the
// method simply stays on T0 - Amendment A's "always safe to abandon").
// Defaults are named constants (Rule 23) sized for v0 methods; they are
// budget knobs, not tuning knobs - T1 does not trade quality for budget
// because T1 has no quality axis, only coverage.
struct PlanBudget {
  std::uint32_t max_instances = 65536;    // one per instruction worst case
  std::uint32_t max_output_bytes = 1u << 20; // 1 MiB staged code per method
  std::uint32_t max_patches = 262144;     // total patch values
  std::uint32_t max_deopt_points = 65536; // guards + traps + call sites
  std::uint32_t max_fusions = 16384;      // superinstructions emitted
};

// --- options ---------------------------------------------------------------------

struct CompileOptions {
  // Superinstruction fusion on/off (default on; off is the diagnostic mode
  // that pins opcode-stencil-only plans for golden tests and A/B diffs).
  bool superinstructions = true;

  // Require an explicit rbc::Op::SafepointPoll instruction at every loop
  // backedge target the plan emits (T1 does not synthesize polls - the
  // RBC stream owns them; this option only CHECKS they exist and refuses
  // if missing, keeping "no analysis" honest: the check is per-branch-target
  // membership in the set of SafepointPoll instructions, precomputed in one
  // linear pass, not a loop analysis).
  bool require_backedge_polls = true;

  PlanBudget budget{};
};

// --- result -----------------------------------------------------------------------

// Refusal reasons are part of the contract (tests pin each one):
//   UnverifiableMethod    - rbc::verify failed (T1 only compiles verified RBC)
//   NoStencilForOp        - an op's candidates are all NeedsRuntimeFeature
//   MissingBackedgePoll   - require_backedge_polls and a branch target lacks one
//   BadOperandRange       - a value to patch exceeds its hole width (e.g.
//                           slot > 4095 in a 12-bit SlotOffset hole)
//   BudgetExceeded        - a PlanBudget ceiling was hit
//   InternalInvariant     - the builder's own invariant check tripped
//                           (bug; refuse loudly rather than emit a bad plan)
enum class RefuseReason : std::uint8_t {
  Ok = 0,
  UnverifiableMethod,
  NoStencilForOp,
  MissingBackedgePoll,
  BadOperandRange,
  BudgetExceeded,
  InternalInvariant,
};

[[nodiscard]] constexpr std::string_view refuseReasonName(RefuseReason r) noexcept {
  switch (r) {
    case RefuseReason::Ok: return "ok";
    case RefuseReason::UnverifiableMethod: return "unverifiable-method";
    case RefuseReason::NoStencilForOp: return "no-stencil-for-op";
    case RefuseReason::MissingBackedgePoll: return "missing-backedge-poll";
    case RefuseReason::BadOperandRange: return "bad-operand-range";
    case RefuseReason::BudgetExceeded: return "budget-exceeded";
    case RefuseReason::InternalInvariant: return "internal-invariant";
  }
  return "?";
}

struct PlanResult {
  bool ok = false;
  RefuseReason reason = RefuseReason::Ok;
  std::string detail;     // refusal diagnostics (op, pc, limit) - never empty on refusal
  StencilPlan plan;       // valid only when ok
};

// --- the compiler ------------------------------------------------------------------
//
// NORMATIVE ALGORITHM (v1 of this contract; changes are an RFC + version bump):
//
//   1. rbc::verify(method) must pass (the same hard gate as T0/b2run).
//   2. One linear scan over code[]:
//        a. candidates(op[pc]) in set order (longest Available first);
//        b. try to match each superinstruction pattern at pc (local
//           producer-consumer links only - the peephole window);
//        c. emit the winning StencilInstance: output_offset = running sum of
//           nominal sizes; patch values from the RBC operands per hole source;
//           exc_covers from the method's handler table;
//        d. advance pc by the covered instruction count.
//      Refuse (NoStencilForOp / BadOperandRange) on the first violation.
//   3. Second linear pass over emitted instances: pc_map, stack_maps
//      (every IsSafepoint or CanCall instance), deopt_points (every HasDeopt
//      or CanTrap instance; Guard keeps the rbc DeoptId; call sites may set
//      pending_exception_possible), exception edges translated via the pc map.
//   4. require_backedge_polls check (if on): every branch target that is the
//      destination of a backward edge must be a SafepointPoll instruction.
//   5. verifyPlan() on the result; any failure => InternalInvariant refusal.
//
// The dump (dumpPlan) is line-oriented, stable, and complete: header (method
// identity, frame layout, set version, code_size), then one line per
// instance (offset, stencil name, rbc range, patch values with kinds/sources
// and pending markers), then pc map, stack maps, deopt points, exception
// edges, then the trailer counters. Golden tests pin it byte-for-byte
// (Rule 124); the format may only grow by APPENDING sections (versioned).

[[nodiscard]] PlanResult compilePlan(const rbc::Program& program,
                                     std::uint32_t method_index,
                                     const StencilSet& set,
                                     const CompileOptions& options = {});

// Deterministic plan dump (golden-test format; see above). Two calls with
// equal plans produce equal strings, independent of machine or allocator.
[[nodiscard]] std::string dumpPlan(const StencilPlan& plan, const StencilSet& set);

// Convenience: compile by method name+descriptor instead of index.
[[nodiscard]] PlanResult compilePlanFor(const rbc::Program& program,
                                        std::string_view name,
                                        std::string_view descriptor,
                                        const StencilSet& set,
                                        const CompileOptions& options = {});

} // namespace b2::baseline
