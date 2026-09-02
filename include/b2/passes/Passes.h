#pragma once
// B-2 Passes - the T2/T3 optimization pass suite v1: pass registry, pass
// contracts, kill switches, budgets, telemetry, and the early-cleanup +
// GVN + SCCP pipeline (charter appendix items 11-20, 35, and 38).
//
// WHY THIS FILE EXISTS:
// The passes team owns the optimizer on top of b2::ir (Amendment B.4: the
// full suite applies to T2/T3 only; T1 runs no passes). The builder
// (GraphBuilder.h) is the front door; this header is everything AFTER it:
// the registry with stable keys (appendix numbering is normative), the
// Rule 123 contract declarations, the Rule 132/144 kill-switch
// configuration, the Rule 23 rewrite budgets, the Rule 26 telemetry
// counters, and the pipeline driver that runs the group to a bounded
// fixpoint (Rule 10) with the IR verifier between passes (Rule 40).
//
// CHARTER PROMISE (docs/teams/passes-team.md): graphs are transformed ONLY
// through the IR public API (make/setInput/appendInput/replaceNode/
// killNode); compiler/ir/core/ is never touched. Every pass is
// deterministic (Rule 124: fixed id-order sweeps, replacement by the
// lowest id, creation in sweep order), idempotent and monotone in dead
// nodes (Rule 10), and independently disableable (Rule 132).
//
// The full per-pass contracts - including the exact rewrite catalogs that
// are the review artifact for every soundness rule - live in
// docs/pass_contracts.md (the normative reference for this header).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "b2/ir/Graph.h"

namespace b2::passes {

// Registry keys: the passes-team charter appendix numbering (stable and
// normative for pass registry keys). v1 delivers the early-cleanup group
// (11-20, minus 17: redundant-cast removal needs type proofs), GVN, SCCP,
// and the CM-PEA family (65/66/67/69 - one engine, four stage keys;
// docs/ pass_contracts.md section 12 is the normative contract). Keys not
// delivered yet resolve to the bad registry row (passInfo().key != key)
// and runSinglePass refuses them with a diagnostic. Coverage note: SCCP
// (38) is the engine for the charter's whole conditional/unconditional
// constant-propagation family - rows 37 CPROP and 39 conditional constant
// propagation are subsumed by it exactly like CSE (36) is covered by GVN.
enum class PassKey : std::uint16_t {
  DeadCodeElimination = 11,       // remove pure values with no live users
  TrivialBlockFusion = 12,        // single-pred regions + trivial phis
  Canonicalization = 13,          // commutative order + boolean kind forms
  ConstantFolding = 14,           // evaluate constant expressions exactly
  StrengthReduction = 15,         // multiply-by-power-of-two -> shift
  IdentityRemoval = 16,           // x+0, x*1, x^0, x-x, self-compares...
  RedundantCastRemoval = 17,      // NOT DELIVERED (needs type proofs)
  NullCheckFolding = 18,          // null guards on provably non-null values
  BranchNormalization = 19,       // constant If/Guard conditions
  ControlFlowSimplification = 20, // unreachable sweep + region repair
  GVN = 35,                       // structural global value numbering
  SparseConditionalConstantPropagation = 38, // lattice + executable edges
  EscapeAnalysis = 65,            // CM-PEA: escape classification lattice
  PartialEscapeAnalysis = 66,     // CM-PEA: materialization placement
  ScalarReplacement = 67,         // CM-PEA: virtual objects + forwarding
  MaterializationPlanning = 69,   // CM-PEA: vobj snapshots + cascades
};

// Fixpoint budget (Rule 23): the early-cleanup group reruns while rewrites
// happen, at most this many rounds; stopping while still changing reports
// converged=false in telemetry (Rule 26) - never an invalid graph.
inline constexpr std::uint32_t kMaxEarlyCleanupRounds = 3;

// Per-pass rewrite budgets (Rule 23; see docs/pass_contracts.md for the
// rationale). A budget overrun stops the pass at a structurally valid
// point and is reported through telemetry (Rule 26) - never UB, never a
// half-applied rewrite.
inline constexpr std::uint32_t kDefaultPassRewriteBudget = 200000;

// --- CM-PEA cost model and analysis budgets (Rule 45 + special_passes.md
// section 1.5: linear in IR size; every knob is a named constant, Law 23).
// The cost gates exist so materialization never costs more than the
// allocation it replaces (Rule 45: PEA materialization needs a cost model).

// Instance fields per virtual object above which the allocation is
// rejected: the materialization would copy more state than the
// allocation it replaces.
inline constexpr std::uint32_t kPeaMaxFields = 16;

// Array elements (constant indices only) per virtual array object.
inline constexpr std::uint32_t kPeaMaxArrayElems = 16;

// Allocations analyzed per graph per run (the analysis budget; overrun
// leaves the remaining allocations untouched - sound, reported).
inline constexpr std::uint32_t kPeaMaxAllocsPerGraph = 64;

// Nesting depth of the materialization cascade (vobj field holding
// another vobj materializes inner-first; deeper nests are rejected).
inline constexpr std::uint32_t kPeaMaxMaterializeDepth = 4;

// Memory/ctrl chain walk step cap per lookup (belt: chains are verified
// acyclic; this bounds pathological hand-built shapes).
inline constexpr std::uint32_t kPeaMaxChainSteps = 10000;

// Rule 123 contract row. `contract` is the one-line summary; the full
// requires/produces/invalidates/budget/determinism text is the doc.
struct PassInfo {
  PassKey key;
  const char* name;     // registry name (telemetry, b2graph, logs)
  const char* contract; // one-line requires/produces summary
  bool growsGraph;      // Rule 10: may ADD nodes (guarded fixpoint needed)
  std::uint32_t rewriteBudget;
};

// Registry lookup. Unknown/not-yet-delivered keys get the static bad row
// (name "bad<key>", key value rewritten to an impossible sentinel); call
//ers compare passInfo(k).key == k to test deliverability.
[[nodiscard]] const PassInfo& passInfo(PassKey key);

// The delivered registry, in appendix-key order (deterministic; Rule 124).
[[nodiscard]] std::span<const PassInfo> passRegistry();

// Index of `key` in passRegistry(), or kInvalidPassIndex.
inline constexpr std::size_t kInvalidPassIndex =
    static_cast<std::size_t>(-1);
[[nodiscard]] std::size_t passRegistryIndex(PassKey key);

// Run configuration: kill switches (Rules 132, 144) and verification mode.
// Plain value type, no global state (Rule 125); every knob is explicit and
// documented in docs/pass_contracts.md.
struct PassConfig {
  // All delivered passes enabled by default. setPassEnabled(key, false)
  // kills one pass for bisection / emergency response; disableAll() is the
  // global kill switch. Running a disabled pass is a no-op that succeeds.
  [[nodiscard]] bool isPassEnabled(PassKey key) const;
  void setPassEnabled(PassKey key, bool on);
  void disableAll();

  // Implementation detail: bit i of enabledBits mirrors passRegistry()
  // entry i (all set by default). Use the accessors, not the bits. The
  // registry passed 16 rows (SCCP made 15), so the width is 32 - the
  // remaining headroom covers the planned inline-registry and loop-pass
  // rows without another breaking change.
  std::uint32_t enabledBits = 0xFFFFFFFFu;

  // Rule 40: ir::verify runs after every pass while true. Tools, tests,
  // and debug builds keep it on; the T2 runtime driver may turn it off in
  // release once golden coverage is trusted (the verifier is TOTAL, so
  // this is a cost decision, not a correctness one).
  bool verifyAfterEachPass = true;

  // Fixpoint budget for runEarlyCleanup (Rule 10/23). Zero rounds is a
  // valid degenerate configuration (pipeline becomes a no-op).
  std::uint32_t maxCleanupRounds = kMaxEarlyCleanupRounds;
};

// Rule 26 telemetry: every pass and pipeline run reports what it did.
struct PassTelemetry {
  std::uint32_t rewrites = 0;  // replaceNode calls (rewiring users)
  std::uint32_t removals = 0;  // killNode calls
  std::uint32_t folds = 0;     // simplify-family rewrites (13-16, phi)
  std::uint32_t gvnDedups = 0; // GVN replacements
  std::uint32_t sccpConstants = 0; // SCCP value->constant replacements
  std::uint32_t rounds = 0;    // fixpoint rounds executed
  std::uint32_t budgetOverruns = 0;
  std::uint32_t peaScalarized = 0;   // CM-PEA: allocations fully replaced
  std::uint32_t peaMaterialized = 0; // CM-PEA: escape-point materializations
  std::uint32_t peaRejected = 0;     // CM-PEA: refused allocations
  bool converged = true;       // false: stopped at the round budget while
                               // the graph was still changing
};

struct PassDiag {
  PassKey pass;
  std::string message;
};

struct PassResult {
  bool ok = true;
  std::vector<PassDiag> diags; // ordered by emission, capped
  PassTelemetry telemetry;

  [[nodiscard]] bool hasErrors() const noexcept { return !ok; }
};

// The early-cleanup + GVN pipeline (suite items 11-20, 35; one list for
// T2 Profile Mode and T3 Static Mode per Rule 1 - tier-specific filters
// arrive with the speculation passes, not by forking this list).
//
// PRECONDITION: g is a verified graph (builder output or equivalent).
// Bounded fixpoint; verifier between passes when configured; a
// verification failure stops the pipeline, reports diagnostics, and
// leaves the graph in the post-last-good-pass state (fail closed).
[[nodiscard]] PassResult runEarlyCleanup(ir::Graph& g,
                                          const PassConfig& cfg = {});

// One registry pass in isolation (tests, bisection, replay). Unknown keys
// refuse with a diagnostic; disabled keys are successful no-ops.
[[nodiscard]] PassResult runSinglePass(ir::Graph& g, PassKey key,
                                       const PassConfig& cfg = {});

// --- CM-PEA (docs/special_passes.md section 1; Part XVIII PEA Rule) ---------

// The height-6 escape lattice (special_passes.md 1.1, verbatim semantics;
// the numeric order IS the lattice order, so join == max). v1 computes it
// by one monotone classification pass over the allocation's live uses -
// conservative in the exact direction the lattice demands (any unknown
// use publishes to Top). The intermediate grades feed the decision log;
// v1 action selection treats every non-Bottom grade as materializable and
// every Top-observable grade as rejected (the growth path applies
// per-grade summaries across inline sites).
enum class EscapeState : std::uint8_t {
  NoEscape = 0,   // Bottom: never observable outside the virtual lifetime
  LocalEscape = 1, // escapes to the caller's local scope only
  ArgEscape = 2,  // escapes only through a callee argument
  FieldEscape = 3, // escapes only through a receiver/other-object field
  ReturnEscape = 4, // escapes only through the return value
  GlobalEscape = 5,  // Top: heap/GC/native/reflection/identity observable
};

[[nodiscard]] constexpr const char* escapeStateName(EscapeState s) noexcept {
  switch (s) {
  case EscapeState::NoEscape: return "no-escape";
  case EscapeState::LocalEscape: return "local-escape";
  case EscapeState::ArgEscape: return "arg-escape";
  case EscapeState::FieldEscape: return "field-escape";
  case EscapeState::ReturnEscape: return "return-escape";
  case EscapeState::GlobalEscape: return "global-escape";
  default: return "?";
  }
}

// One CM-PEA decision (the explainable surface: PEA Rule option 1-4 and
// the refusal reason are part of the record, like InlineDecision).
struct PeaDecision {
  ir::NodeId alloc = ir::kInvalidNodeId; // the New/NewArray/NewRefArray
  ir::NodeKind kind = ir::NodeKind::New;
  EscapeState state = EscapeState::GlobalEscape;
  const char* action = "rejected";  // scalarized | materialized | rejected
  const char* reason = "";           // the refusal-catalog key
  ir::NodeId materializeAt = ir::kInvalidNodeId; // the escape-point node
  std::uint32_t fields = 0;    // instance fields / array slots involved
  std::uint32_t loads = 0;     // loads forwarded to field values
  std::uint32_t stores = 0;    // virtual stores removed
};

struct PeaResult : PassResult {
  std::vector<PeaDecision> decisions; // in allocation-id order (Rule 124)
};

// Runs the CM-PEA engine over one graph (the four registry stages obey
// the PassConfig kill switches: EscapeAnalysis off disables everything,
// ScalarReplacement off downgrades no-escape allocations to rejected,
// PartialEscapeAnalysis/MaterializationPlanning off downgrade material-
// ization to rejected). PRECONDITION: verifier-clean graph. Returns the
// per-allocation decisions plus pass-style telemetry; verification after
// the run is the caller's (the pipeline does it per pass).
[[nodiscard]] PeaResult runPartialEscapeAnalysis(ir::Graph& g,
                                                  const PassConfig& cfg = {});

} // namespace b2::passes
