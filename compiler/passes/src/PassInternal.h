#pragma once
// B-2 Passes - internal shared machinery for the pass suite.
//
// WHY THIS FILE EXISTS:
// The pass sources (Simplify.cpp, ControlFlow.cpp, DCE.cpp, GVN.cpp,
// Passes.cpp) share: the budget guard (Rules 10/23/26), the tombstone-law
// junk machinery, registry-derived predicates (Rule 130: kind facts come
// from NodeInfo), and the pass entry points the dispatcher calls.
// Internal by design: tests and tools use include/b2/passes/Passes.h only.
//
// THE TOMBSTONE LAW (why Junk exists): the IR verifier checks EVERY node -
// including Dead/Replaced tombstones - for input liveness, role kind, and
// operand type (ir::Verifier checkNode). A node's tombstone therefore
// keeps its input edges forever, and those edges must stay verifier-legal
// forever. Passes comply by rewiring tombstone edges onto immortal "junk"
// nodes (Start for Ctrl/Mem slots, typed zero-constants for Data slots,
// an empty FrameState for FS slots, kind-matched If/Switch/Call sinks for
// projection Parent slots, and arity-matched Regions for Phi region
// slots). This is what makes kills legal at all AND frees orphaned pure
// values for DCE (a def referenced only by a tombstone would otherwise be
// unremovable). The junk nodes are flow-dead by construction (nothing
// live consumes them) and survive via tombstone references; a sanctioned
// removal mechanism is requested from the IR team (see messages/).

#include <cstdint>
#include <span>
#include <vector>

#include "b2/ir/Graph.h"
#include "b2/ir/Node.h"
#include "b2/ir/Types.h"
#include "b2/passes/GraphBuilder.h"
#include "b2/passes/Passes.h"

namespace b2::passes::detail {

// --- Budget guard (Rules 10, 23, 26) --------------------------------------
struct Budget {
  std::uint32_t limit = 0;
  std::uint32_t used = 0;
  bool exceeded = false;

  // Returns false (and flags) when the budget is exhausted.
  [[nodiscard]] bool charge() {
    if (exceeded || used >= limit) {
      exceeded = true;
      return false;
    }
    ++used;
    return true;
  }
};

// --- Junk nodes (the tombstone-law sinks) ----------------------------------

// Lazy per-run cache of the immortal sink nodes (see the file header).
// Deterministic (Rule 124): nodes are created on first need in a fixed
// order, ids append monotonically. One Junk per pipeline/pass run.
struct Junk {
  ir::Graph& g;
  mutable ir::NodeId ifSink = ir::kInvalidNodeId;
  mutable ir::NodeId switchSink = ir::kInvalidNodeId;
  mutable ir::NodeId callSink = ir::kInvalidNodeId;
  mutable ir::NodeId fsSink = ir::kInvalidNodeId;
  mutable ir::NodeId constI = ir::kInvalidNodeId;
  mutable ir::NodeId constL = ir::kInvalidNodeId;
  mutable ir::NodeId constF = ir::kInvalidNodeId;
  mutable ir::NodeId constD = ir::kInvalidNodeId;
  mutable ir::NodeId constNull = ir::kInvalidNodeId;
  // Phi region sinks, keyed by predecessor count (>= 2).
  mutable std::vector<ir::NodeId> regionSinks;

  explicit Junk(ir::Graph& graph) : g(graph) {}

  // Ctrl/Mem slots sink to Start (it produces control AND memory state
  // and is not a terminal, so it may hold users).
  [[nodiscard]] ir::NodeId ctrlSink() const noexcept { return g.startNode(); }
  [[nodiscard]] ir::NodeId memSink() const noexcept { return g.startNode(); }

  [[nodiscard]] ir::NodeId parentSink(ir::NodeKind projectionKind) const;
  [[nodiscard]] ir::NodeId fsSinkGet() const;
  [[nodiscard]] ir::NodeId dataSink(ir::IRType type) const;
  // Region with `preds` (>= 2) Start predecessors, cached per count.
  [[nodiscard]] ir::NodeId regionSink(std::uint16_t preds) const;
};

// Is `n` one of this run's junk sinks? The sweep never kills them (they
// are the tombstone-law anchors; each is referenced by tombstones and is
// flow-dead by construction).
[[nodiscard]] bool isJunkSink(const Junk& jk, ir::NodeId n);

// Junk target for node `n`'s input slot `s`, or kInvalidNodeId when the
// slot's semantics have no junk (Tagged/vector Data, exotic kinds). The
// caller leaves such edges untouched (their defs then stay tombstone-
// referenced, i.e. DCE-protected: sound, just less clean).
[[nodiscard]] ir::NodeId junkForSlot(const ir::Graph& g, ir::NodeId n,
                                     std::uint16_t slot, const Junk& jk);

// Rewires every input edge of (tombstone) `n` onto junk sinks. Slots
// without a legal junk target keep their edge. Phi region slots get the
// arity-matched region sink (appending a junk value first when the phi
// has a single value, since no 1-predecessor Region can verify).
void junkEdges(ir::Graph& g, ir::NodeId n, const Junk& jk);

// Uniform accounting so telemetry cannot drift from reality.
// replace: replaceNode + tombstone-edge junking (frees the old inputs for
// DCE; every user - live or tombstone - was already rewired by
// replaceNode itself).
void replace(ir::Graph& g, ir::NodeId oldNode, ir::NodeId withNode,
             PassTelemetry& t, Budget& b, const Junk& jk);

// The legal kill: junk-rewires every DEAD referencer of `n`, junks `n`'s
// own edges, then killNode. REFUSES (returns false, no change) while any
// LIVE node still references `n` - the caller must repair or defer; this
// is the "removed all uses first" discipline the Graph API documents.
[[nodiscard]] bool kill(ir::Graph& g, ir::NodeId n, PassTelemetry& t,
                        Budget& b, const Junk& jk);

// --- registry-derived predicates ------------------------------------------

// Role of node `n`'s input `slot` (fixed prefix from the registry; the
// mandatory trailing FrameState slot when the row demands one; variadic
// slots get variadicRole).
[[nodiscard]] ir::InputRole roleOfSlot(const ir::Graph& g, ir::NodeId n,
                                       std::uint16_t slot);

// True if the kind consumes control anywhere in its signature (a Ctrl
// fixed/variadic role, or the Parent role of projections).
[[nodiscard]] bool consumesControl(ir::NodeKind k);

// Control flow terminators: flow does not continue past them (the
// reachability sweep stops here; everything chained after one is
// unreachable).
[[nodiscard]] bool isTerminator(ir::NodeKind k);

// Provably non-null VALUES (dataflow facts usable without a proof
// machinery): fresh allocations, materializations, interned symbol
// constants, and any node carrying the NeverNull flag.
[[nodiscard]] bool isNeverNullNode(const ir::Graph& g, ir::NodeId n);

// Total use-list entries of `n` (live AND tombstone referencers - the
// use-def invariant is exact for every node). "No referencers at all" is
// the DCE legality test under the tombstone law.
[[nodiscard]] std::uint32_t useCount(const ir::Graph& g, ir::NodeId n);
// Live (non-dead) referencers only.
[[nodiscard]] std::uint32_t liveUseCount(const ir::Graph& g, ir::NodeId n);

// NC::Value && EK::Pure per the registry (the floating pure values).
[[nodiscard]] bool isPureValueKind(ir::NodeKind k);

// The documented v1 DCE-safe set: pure values minus the trapping integer
// div/rem family, minus PEA analysis state (side-table references are not
// edges), plus the guard-gated-by-contract readers (ArrayLength,
// InstanceOf) and unused FrameState nodes. Stores, loads, allocations,
// calls, casts, guards, and control are NEVER DCE-removed (see
// docs/pass_contracts.md section 4 for the soundness table).
[[nodiscard]] bool isDceSafe(ir::NodeKind k);

// The documented v1 GVN set: pure values (state markers excluded, but
// constants included), Phi, and the readers whose value is a total
// function of their inputs (ArrayLength, InstanceOf, Load*). Loads chain
// control, so builder output rarely produces equal keys - they are
// eligible for hand-built/transformed graphs. Calls/stores/allocations/
// guards/control are never value-numbered.
[[nodiscard]] bool isGvnEligible(ir::NodeKind k);

// --- control-flow repair (used by keys 12/19/20 and the pipeline) ---------

// Flow-reachability sweep: computes the nodes reachable from Start
// (constant-condition Ifs/Guards propagate flow only through their live
// outcome; terminators stop flow), kills unreachable control consumers
// bottom-up (each kill fires only once no LIVE node references it - the
// protocol keeps every live input alive by construction), then repairs
// regions. Iterated to a bounded fixpoint.
void unreachableSweep(ir::Graph& g, PassTelemetry& t, Budget& b,
                      const Junk& jk);

// Rebuilds Region/LoopBegin nodes with dead predecessors: drops dead
// preds (fresh node + slot-for-pred phi trims), collapses single-pred
// regions to the pass-through edge (phis collapse to the surviving
// value), kills unreachable regions and their phis. All tombstones are
// junked by replace/kill, so DCE can reclaim the freed values.
void rebuildRegions(ir::Graph& g, PassTelemetry& t, Budget& b,
                    const Junk& jk,
                    const std::vector<bool>* reach = nullptr);

// --- inline body building (the inliner's builder seam) --------------------
//
// The inliner re-runs the RBC->IR builder over the callee INSIDE the
// caller's graph (Graal-style): the entry state is the call site's control/
// memory/arguments (no Start, no Parameters), every callee FrameState
// chains to the call-site FrameState (Rule 75), return instructions
// become normal exits (no Return terminals; the driver merges them), and
// the exceptional escapes route through the caller's call-site policy
// (docs/inlining.md section 4 is the normative contract).

// Wiring for building one callee body into the caller graph.
struct InlineSiteWiring {
  ir::NodeId entryCtrl = ir::kInvalidNodeId; // the call's control predecessor
  ir::NodeId entryMem = ir::kInvalidNodeId;  // the call's memory predecessor
  std::span<const ir::NodeId> args;          // caller arg defs (receiver first)
  ir::FrameStateId fsCaller = ir::kInvalidFrameState; // call-site fs desc id
  bool siteCovered = false;   // caller's handler table covers the call pc
  std::uint32_t deoptIdSeed = 0; // first deopt id this body may allocate
};

// One normal return exit of an inlined body (the state that flows back to
// the call site on that path). `value` is invalid for void returns.
struct InlineExit {
  ir::NodeId ctrl = ir::kInvalidNodeId;
  ir::NodeId mem = ir::kInvalidNodeId;
  ir::NodeId value = ir::kInvalidNodeId;
};

struct InlineBodyResult {
  bool ok = false;
  std::uint32_t nodesAdded = 0;   // nodes appended to the caller graph
  std::uint32_t deoptsEmitted = 0;
  std::vector<InlineExit> exits;  // normal exits, in materialization order
  std::vector<BuildDiag> diags;   // builder refusals, bounded (pc + message)
};

// Builds `m` into `g` in inline mode per `w`. The graph is NOT required to
// be fresh (the caller's nodes are already there); nodes append. Callers
// must have validated the callee builds at all (trial build) and that the
// argument window matches - this entry point is the trusted seam, exactly
// like buildGraph is for standalone builds.
[[nodiscard]] InlineBodyResult
buildInlineBody(const rbc::Method& m, SymbolResolver& res, ir::Graph& g,
                ir::MethodId frameMethodId, const InlineSiteWiring& w);

// --- pass entry points -----------------------------------------------------

// Simplify classes (keys 12-16 share one sweep engine; the mask selects
// which rewrite classes may fire - each registry pass is a mask of one).
enum RewriteClass : std::uint32_t {
  kFold = 1u << 0,        // key 14: evaluate constant expressions
  kIdentity = 1u << 1,    // key 16: identity/self patterns
  kStrength = 1u << 2,    // key 15: mul-by-pow2 -> shift
  kCanonical = 1u << 3,   // key 13: commutative order + Not-of-test
  kTrivialPhi = 1u << 4,  // key 12: trivial phi collapse
};
void runSimplify(ir::Graph& g, std::uint32_t classMask, PassTelemetry& t,
                 Budget& b, const Junk& jk);
void runBranchNormalization(ir::Graph& g, PassTelemetry& t, Budget& b,
                            const Junk& jk);
void runNullCheckFolding(ir::Graph& g, PassTelemetry& t, Budget& b,
                         const Junk& jk);
void runDCE(ir::Graph& g, PassTelemetry& t, Budget& b, const Junk& jk);
void runGVN(ir::Graph& g, PassTelemetry& t, Budget& b, const Junk& jk);

// --- CM-PEA engine (Escape.cpp; keys 65/66/67/69 share it) ------------------
//
// One engine, four registry stages (like the simplify class mask): the
// mask bits are the four PassKeys so each stage is independently
// kill-switchable. `decisions` collects the explainable per-allocation
// record when non-null (the public runPartialEscapeAnalysis passes one;
// the pipeline runs with nullptr and reads the telemetry counters).
// PRECONDITION: verifier-clean graph (the null guards on the allocation's
// uses must already be folded - the pipeline runs PEA after nullfold/DCE).
enum PeaStage : std::uint32_t {
  kPeaAnalyze = 1u << 0,   // key 65: classification + decision
  kPeaPartial = 1u << 1,   // key 66: escape-point materialization
  kPeaScalar = 1u << 2,    // key 67: virtual objects + load forwarding
  kPeaPlanning = 1u << 3,  // key 69: vobj snapshots + nested cascades
  kPeaAll = kPeaAnalyze | kPeaPartial | kPeaScalar | kPeaPlanning,
};
void runPEA(ir::Graph& g, std::uint32_t stageMask, PassTelemetry& t,
            Budget& b, const Junk& jk,
            std::vector<PeaDecision>* decisions);

} // namespace b2::passes::detail
