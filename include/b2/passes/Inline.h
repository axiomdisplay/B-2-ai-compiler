#pragma once
// B-2 Passes - T2 inlining v1: the ICDG direct-inline engine (charter
// items 24/29/30/31 of docs/teams/passes-team.md; the ICDG contract is
// docs/icdg.md; the normative v1 implementation contract is
// docs/inlining.md).
//
// WHY THIS FILE EXISTS:
// Call boundaries block every downstream T2 optimization (icdg.md 2).
// This is the first real inliner: CallStatic sites (invokestatic /
// invokespecial - direct dispatch, one resolvable body) are re-built
// INTO the caller's sea-of-nodes graph (Graal-style: the RBC->IR builder
// runs over the callee with the call site's control/memory/arguments as
// the entry state), the callee's FrameStates chain to the call-site
// snapshot (Rule 75 - deopt reconstructs the inlined frame stack), and
// every decision is recorded with a structured explanation (icdg.md 19).
//
// SCOPE (v1, icdg.md Phase 2 "T2 simple inlining" on the static-proof
// path):
// - Direct calls only. Virtual/interface devirtualization needs the
//   dispatch profile (icdg.md Phase 1) and CHA - both arrive with the
//   PGO-driven passes (Rule 1: tier filters then, not now).
// - No speculation: the target is a compile-time-resolvable single body
//   (StaticProof source, icdg.md 6), so no guards and no SpecMeta are
//   attached. GuardInline (TypeProfile guards) is the Phase 2+ growth.
// - The engine is a do-not-inline engine first: every refusal is a
//   KeepIndirect with a reason (icdg.md 12).
//
// LAWS: Rule 23 (named-constant budgets), Rule 26 (telemetry), Rules
// 132/144 (the `enabled` kill switch), Rule 124 (deterministic: sites in
// id order, bodies in builder order, decisions logged in processing
// order), Rule 40 (ir::verify after every site while configured - the
// caller-enforced discipline; this driver verifies after every site),
// Rule 35 (tests in tests/passes/InlineTests.cpp).

#include <cstdint>
#include <string>
#include <vector>

#include "b2/ir/Graph.h"
#include "b2/passes/GraphBuilder.h"
#include "b2/rbc/Rbc.h"

namespace b2::passes {

// --- callee resolution (the id-space seam) ----------------------------------
//
// The IR stores opaque MethodIds (Rule 16). Call node payloads carry ids
// from the SymbolResolver space the graph was built with (un-quickened
// MethodRef calls) - the RESOLVER owns that space, so the source is the
// single authority for payload -> body. Quickened call payloads are the
// runtime's program-index space (rbc_spec SS6.2, the T0 v0 pin) - a
// DIFFERENT space; ProgramCalleeSource refuses ambiguous ids (below).

// One resolution: the callee body (null = unresolvable: external method,
// ambiguous id, or not a program method - the site stays indirect) plus
// the MethodId the callee's FrameStates must carry (the caller graph's
// FrameState id space; the v0 pin is the program method-table index).
struct InlineCallee {
  const rbc::Method* method = nullptr;
  ir::MethodId frameMethodId = 0;

  [[nodiscard]] bool resolved() const noexcept { return method != nullptr; }
};

class InlineCalleeSource {
 public:
  virtual ~InlineCalleeSource() = default;

  // Resolve one direct-call target (the Call node's method payload).
  // Deterministic resolvers keep the inline run deterministic (Rule 124).
  [[nodiscard]] virtual InlineCallee resolve(ir::MethodId callTarget) = 0;

  // The resolver that owns the caller graph's id space (the inline body
  // build interns the callee's constant pool through it - the same space,
  // so nested calls resolve through the same source).
  [[nodiscard]] virtual SymbolResolver& resolver() = 0;
};

// Program-backed source for the v0 one-class world. Doubles as the
// SymbolResolver the caller graph should be built with: the METHOD id
// space is UNIFIED by construction - a program method's id is its table
// index (matching the tool FrameState convention and the quickened
// payload pin, rbc_spec SS6.2), external methods intern above the table -
// so call payloads from un-quickened MethodRefs and quickened payloads
// resolve identically, with no id-space ambiguity.
//
// CONTRACT: the graph must have been built with THIS object as its
// resolver (the ids are only meaningful together). The driver's per-site
// consistency checks (argument window, return type) additionally refuse
// mis-spaced resolutions that happen to be shape-inconsistent.
class ProgramCalleeSource final : public InlineCalleeSource,
                                  public SymbolResolver {
 public:
  explicit ProgramCalleeSource(const rbc::Program& program);

  // InlineCalleeSource.
  [[nodiscard]] InlineCallee resolve(ir::MethodId callTarget) override;
  [[nodiscard]] SymbolResolver& resolver() override { return *this; }

  // SymbolResolver (the unified method-id space; the non-method spaces
  // delegate to an internal first-encounter interner - they never
  // interact with call payloads).
  [[nodiscard]] ir::TypeId classId(std::string_view internalName) override;
  [[nodiscard]] ir::FieldId fieldId(std::string_view cls,
                                    std::string_view name,
                                    std::string_view descriptor) override;
  [[nodiscard]] ir::MethodId methodId(std::string_view cls,
                                      std::string_view name,
                                      std::string_view descriptor) override;
  [[nodiscard]] ir::SymbolId symbolId(std::string_view payload) override;
  [[nodiscard]] std::uint32_t
  switchTableId(const std::vector<std::int32_t>& payload) override;

 private:
  const rbc::Program& program_;
  std::vector<std::string> extKeys_; // external method interning
  std::vector<std::string> otherKeys_; // class/field/symbol/table interning
};

// --- configuration (Rule 23 named constants; Rules 132/144 kill switch) -----

inline constexpr std::uint32_t kMaxInlineCalleeInsns = 35;
inline constexpr std::uint32_t kMaxInlineCalleeSlots = 24;  // frame width
inline constexpr std::uint32_t kMaxInlineCalleeNodes = 300; // trial graph size
inline constexpr std::uint32_t kMaxInlineDepth = 3;         // nesting depth
inline constexpr std::uint32_t kMaxInlineSitesPerGraph = 64;
inline constexpr std::uint32_t kMaxInlineNodesPerGraph = 4096;

struct InlineConfig {
  // Rules 132/144: the kill switch. Disabled = a successful no-op (zero
  // decisions, zero telemetry, byte-identical graph).
  bool enabled = true;

  std::uint32_t maxCalleeInsns = kMaxInlineCalleeInsns;
  std::uint32_t maxCalleeSlots = kMaxInlineCalleeSlots;
  std::uint32_t maxCalleeNodes = kMaxInlineCalleeNodes;
  std::uint32_t maxDepth = kMaxInlineDepth;
  std::uint32_t maxSitesPerGraph = kMaxInlineSitesPerGraph;
  std::uint32_t maxNodesPerGraph = kMaxInlineNodesPerGraph;
};

// --- decisions (icdg.md 19: every decision is explainable) -------------------

enum class InlineAction : std::uint8_t {
  DirectInline = 0, // the call site is replaced by the callee body
  KeepIndirect,     // the call stays (every v1 refusal is this)
};

// One decision record. Reason strings are static (no per-decision
// allocation; the log is a compilation artifact, never IR state).
struct InlineDecision {
  ir::NodeId call = ir::kInvalidNodeId;
  ir::MethodId target = 0;
  std::uint32_t depth = 0;
  InlineAction action = InlineAction::KeepIndirect;
  std::uint32_t calleeInsns = 0;
  std::uint32_t calleeSlots = 0;
  std::uint32_t calleeNodes = 0; // 0 = not measured (pre-trial refusal)
  const char* reason = "";
};

// --- telemetry (Rule 26) and the result --------------------------------------

struct InlineTelemetry {
  std::uint32_t sitesConsidered = 0;
  std::uint32_t sitesInlined = 0;
  std::uint32_t sitesRefused = 0;
  std::uint32_t nodesAdded = 0;    // nodes appended by inlined bodies
  std::uint32_t deoptsEmitted = 0; // deopt points created inside bodies
  std::uint32_t exitMerges = 0;    // Region+Phi exit merges created
  std::uint32_t removals = 0;      // call + projection + continuation kills
  std::uint32_t maxDepthReached = 0;
  std::uint32_t budgetStops = 0;   // sites skipped by graph-level budgets
  bool converged = true;           // false: sites remained at a budget stop
};

struct InlineDiag {
  ir::NodeId node = ir::kInvalidNodeId; // the call (invalid = graph-level)
  std::string message;
};

struct InlineResult {
  bool ok = true; // false: fail-closed (inline build or verify failure)
  std::vector<InlineDiag> diags;
  InlineTelemetry telemetry;
  std::vector<InlineDecision> decisions; // in processing order (Rule 124)

  [[nodiscard]] bool hasErrors() const noexcept { return !ok; }
};

// Runs the direct-inline engine over `g`. PRECONDITION: g is a verified
// graph (builder output or post-pipeline). Sites are considered in node-id
// order; a successful inline is followed by its body's own call sites
// (depth + 1, recursion-controlled), then the next root site. The IR
// verifier runs after every site; any failure stops the run fail-closed
// with diagnostics (the graph is left in the post-last-good-site state).
// Deterministic (Rule 124): same (graph, source, config) => same decisions
// and the same resulting graph bytes.
[[nodiscard]] InlineResult runInlining(ir::Graph& g, InlineCalleeSource& src,
                                       const InlineConfig& cfg = {});

} // namespace b2::passes
