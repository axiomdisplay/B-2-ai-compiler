#pragma once
// B-2 Passes - the T2/T3 optimization pass suite v1: pass registry, pass
// contracts, kill switches, budgets, telemetry, and the early-cleanup + GVN
// pipeline (charter appendix items 11-20 and 35).
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
// (11-20, minus 17: redundant-cast removal needs type proofs) and GVN.
// Keys not delivered yet resolve to the bad registry row (passInfo().key
// != key) and runSinglePass refuses them with a diagnostic.
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
  // entry i (all set by default). Use the accessors, not the bits.
  std::uint16_t enabledBits = 0xFFFF;

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
  std::uint32_t rounds = 0;    // fixpoint rounds executed
  std::uint32_t budgetOverruns = 0;
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

} // namespace b2::passes
