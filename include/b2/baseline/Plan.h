#pragma once
// B-2 baseline (T1) - the stencil plan: what "compiled" means before machine
// bytes exist.
//
// WHY THIS FILE EXISTS:
// The StencilPlan is T1's entire output (docs/stencils.md SS7). It is the
// copy-and-patch recipe: WHICH stencils to copy, in WHAT order, with WHICH
// patch values, plus the metadata every tier owes the runtime - pc map,
// stack maps, deopt points, exception edges (SS9). Building a plan is pure
// arithmetic over verified RBC and the StencilSet manifest: deterministic,
// linear, budget-bounded, golden-testable, and independent of any target.
// Instantiation (SS6: copy, patch, link, publish W^X) consumes the plan
// later; keeping the two stages separate is what lets T1 land and be tested
// before the codegen team delivers machine stencils.
//
// LAW PINS (docs/laws.md):
// - Rule 124 (deterministic replay): identical RBC + identical StencilSet +
//   identical options => byte-identical plan. The dump format below is the
//   golden-test proof.
// - Rule 96 / Amendment B.3: deopt target is T0. FrameState for a T1 point
//   is "current RBC pc + T0-compatible frame" (SS9); DeoptPoint carries the
//   ids the deopt stub needs, nothing speculative.
// - Amendment A: the plan contains NO analysis results, NO dataflow, NO
//   optimizations - only selection (which stencil) and relocation data
//   (patch values, pc map). Effect order in the plan IS RBC order.
// - Rule 15: all cross-references are indices (instance -> stencil id,
//   pc map -> instance, exception edge -> handler native offset).

#include <cstdint>
#include <string>
#include <vector>

#include "b2/baseline/Stencil.h"
#include "b2/rbc/Rbc.h"

namespace b2::baseline {

// --- patch values --------------------------------------------------------------
//
// One planned fill for one declared hole. `value` is the plan-computable
// payload where the source allows it (slot offsets, immediates, cp indices,
// branch TARGETS as rbc pcs). Runtime-resolved sources are carried as their
// semantic input (cp index / call pc / helper id) with pending_runtime=true;
// the instantiator resolves them against the live runtime at patch time.
struct PatchValue {
  std::uint8_t site = 0;        // index into StencilDesc::patches
  PatchKind kind = PatchKind::Imm32;
  PatchSource source = PatchSource::None;
  std::uint64_t value = 0;      // plan-computable payload, else semantic input
  bool pending_runtime = false; // true = instantiator resolves at patch time
};

// --- plan records ----------------------------------------------------------------

// One copied stencil in the output buffer (SS7). rbc_pc_start..end (exclusive)
// is the instruction range this instance covers (== 1 for opcode stencils,
// == fusion length for superinstructions). patch_values align 1:1 with the
// descriptor's declared holes, in declaration order.
struct StencilInstance {
  StencilId stencil;          // which stencil to copy
  std::uint32_t output_offset = 0; // byte offset in the staged output buffer
  std::uint32_t output_size = 0;   // descriptor nominal_size at plan time
  std::uint32_t rbc_pc_start = 0;
  std::uint32_t rbc_pc_end = 0;    // exclusive
  std::vector<PatchValue> patch_values;

  // Exception coverage: indices into StencilPlan::exception_edges of handler
  // ranges covering [rbc_pc_start, rbc_pc_end) (empty = no coverage). Every
  // CanTrap instance must be covered by every handler range that covers its
  // rbc range - the deopt stub uses this to redispatch exactly like T0's
  // exception algorithm (innermost-first, table order).
  std::vector<std::uint16_t> exc_covers;
};

// Bidirectional native <-> RBC pc map (SS9 item 1). One entry per instance,
// sorted by native_offset (== plan order). Deopt translation:
// native pc -> containing entry -> rbc_pc = entry.rbc_pc + (skipped steps of
// a fusion, recovered from the instance's step layout when needed; v0 fusions
// fuse to a single observable boundary at the START of the fused range, so
// rbc_pc is exact at fusion starts).
struct PcMapEntry {
  std::uint32_t native_offset = 0; // instance start in the output buffer
  std::uint32_t rbc_pc = 0;        // rbc_pc_start of the instance
  std::uint32_t instance = 0;      // index into StencilPlan::instances
};

// A safepoint: where compiled code polls and where the GC may walk the frame.
// With the T0-compatible frame (SS8) the GC map is the frame itself - every
// slot is a 16-byte tagged Value (interp_contract v1.0.0 SS2), so the point
// records WHERE, not WHAT: the frame layout (numRegs/numLocals in the plan)
// plus slot tags answer the "which slots are refs" question. This is the
// documented simplification that keeps T1 stack maps trivial (SS8.2).
struct StackMapPoint {
  std::uint32_t native_offset = 0;
  std::uint32_t rbc_pc = 0;
  enum class Kind : std::uint8_t { Poll, Call, Trap } kind = Kind::Poll;
  std::uint32_t deopt_point = 0xFFFF'FFFF; // owning DeoptPoint, if any
};

// Why a deopt can occur here. Trap = a Java exception raised by the
// instruction (redispatch via exception edges); Guard = a T1 assumption
// failed (guards carry rbc DeoptIds); CallException = the callee threw and
// T1 redispatches the caller frame; Uncommon = cold path abandoned to T0.
enum class DeoptReason : std::uint8_t {
  Trap, Guard, CallException, Uncommon,
};

// One deopt point (SS9 item 6). `deopt_id` is plan-unique: for Guard/Trap
// it equals the RBC instruction's own DeoptId (guards/deopt_trap imm) where
// the instruction carries one, else the plan allocates fresh ids >=
// kPlanAllocatedDeoptIdBase so RBC-declared and plan-allocated ids never
// collide. The deopt stub's contract: translate native pc -> rbc pc via the
// pc map, rebuild the T0 frame from the T0-compatible T1 frame, call
// Interpreter::resume (interp_contract SS3). pending_exception_possible
// marks points re-entering THE EXCEPTION ALGORITHM (resume with
// pendingException set).
struct DeoptPoint {
  std::uint32_t deopt_id = 0;
  std::uint32_t native_offset = 0;
  std::uint32_t rbc_pc = 0;
  DeoptReason reason = DeoptReason::Trap;
  bool pending_exception_possible = false;
  std::uint32_t stack_map = 0xFFFF'FFFF; // owning StackMapPoint, if any
};

inline constexpr std::uint32_t kPlanAllocatedDeoptIdBase = 0x8000'0000u;

// Exception edge, translated to native terms (SS9 item 4): instructions in
// rbc [start, end) throwing an exception assignable to catch_type dispatch
// to handler_pc; native_handler is that handler's native offset (resolved
// through the pc map; kNoNativeHandler marks a handler rbc pc that no
// instance starts at - a plan-invariant violation the builder must refuse).
// catch_type is the cp index of the Class, or -1 for catch-all, exactly as
// in rbc::ExceptionHandler.
struct ExceptionEdge {
  std::uint32_t start = 0;         // rbc pc, inclusive
  std::uint32_t end = 0;           // rbc pc, exclusive
  std::uint32_t handler_pc = 0;    // rbc pc of the handler entry
  std::int32_t catch_type = -1;    // cp index or -1 (catch-all)
  std::uint32_t native_handler = 0; // output-buffer offset of handler entry
};

inline constexpr std::uint32_t kNoNativeHandler = 0xFFFF'FFFFu;

// --- the plan (docs/stencils.md SS7) ---------------------------------------------

struct StencilPlan {
  // Method identity for replay/telemetry (Rule 124). method_index indexes
  // the compiled Program; names are copied for standalone diagnostics.
  std::uint32_t method_index = 0;
  std::string method_name;
  std::string method_descriptor;
  bool method_static = false;

  // T0-compatible frame layout this plan's code assumes (SS8): registers
  // r0..numRegs-1 then locals l0..numLocals-1, each a 16-byte tagged slot,
  // locals first (params first) - exactly interp_contract SS1. The GC and
  // the deopt runtime rely on this; changing it is a cross-team RFC.
  std::uint32_t num_regs = 0;
  std::uint32_t num_locals = 0;

  // Set-version the plan was built against (Stencil Rule 4: instantiation
  // must refuse a version mismatch instead of mispatching holes).
  StencilSetVersion set_version{};

  std::vector<StencilInstance> instances;   // in output order
  std::vector<PcMapEntry> pc_map;           // sorted by native_offset
  std::vector<StackMapPoint> stack_maps;    // sorted by native_offset
  std::vector<DeoptPoint> deopt_points;     // sorted by deopt_id then offset
  std::vector<ExceptionEdge> exception_edges; // rbc handler table, translated

  std::uint32_t entry_native_offset = 0;    // first instruction's instance
  std::uint32_t code_size = 0;              // sum of instance output sizes

  // Budget/telemetry accounting (Rules 10/45 discipline without the T2 cost
  // models): what the builder consumed, for kill-switch decisions and stats.
  std::uint32_t fusion_count = 0;    // superinstruction instances emitted
  std::uint32_t patch_count = 0;     // total patch values
  std::uint32_t safepoint_count = 0; // stack maps emitted
};

// --- plan invariants (checked by verifyPlan; the builder must uphold) -----------
//
// 1. Coverage: every rbc pc in [0, code.size()) is inside exactly one
//    instance's [rbc_pc_start, rbc_pc_end); instances tile the method with
//    no gaps and no overlaps, in rbc order.
// 2. Layout: instance i's output_offset + output_size == instance i+1's
//    output_offset; the first starts at 0; code_size is the end of the last.
// 3. PcMap: one entry per instance, native_offset == instance output_offset,
//    sorted ascending, rbc_pc == rbc_pc_start.
// 4. Deopt ids: unique; RBC-declared ids (< kPlanAllocatedDeoptIdBase) match
//    the imm of the guard/deopt_trap instruction at that rbc pc.
// 5. Exception edges: native_handler of every edge is the output_offset of
//    the instance starting at handler_pc; every CanTrap instance lists every
//    edge covering its rbc range in exc_covers.
// 6. Every patch value's site index is < the descriptor's patch_count and
//    the value's source matches the descriptor hole's source.
// 7. Determinism: same RBC + same set + same options => same plan bytes.

struct PlanCheckResult {
  bool ok = false;
  std::string error; // first violated invariant, human-readable
};

// Full invariant audit of a built plan against its RBC and StencilSet.
// Used by tests, the b2plan tool's --check, and CI. Never modifies inputs.
[[nodiscard]] PlanCheckResult verifyPlan(const StencilPlan& plan,
                                         const rbc::Method& method,
                                         const class StencilSet& set);

} // namespace b2::baseline
