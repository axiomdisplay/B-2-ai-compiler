#pragma once
// B-2 Passes - T2 inlining v2: direct-inline (v1) + guard-inline (the
// ICDG Phase 2 ingestion; charter items 24/29/30/31 + the profile-driven
// item 25 of docs/teams/passes-team.md; the ICDG contract is
// docs/icdg.md; the normative implementation contract is
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
// v2 adds GUARD-INLINE (icdg.md Phase 2 "T2 devirtualization" over the
// Phase 1 profile): a monomorphic CallVirtual/CallInterface site whose
// T0 dispatch profile names one receiver class is inlined behind a
// TypeProfile guard - the failure path deopts at the call pc and T0
// re-executes the invoke, so any receiver the guard rejects dispatches
// exactly as before. Complete Rule 122 SpecMeta + a ClassHierarchy
// dependency (Rule 42) ride on the guard. The classification policy
// lives HERE (the consumer), never in T0 (interp_contract.md SS8.1:
// "the team produces the data, not the policy").
//
// SCOPE:
// - Direct calls: the v1 static-proof path (no speculation), unchanged.
// - Virtual/interface: guard-inlined ONLY when a dispatch profile is
//   attached (InlineConfig::profile); without one the engine does not
//   even consider the site - v1 behavior, byte-identical (the pinned
//   zero-regression property). Bimorphic/polymorphic sites refuse (the
//   two-target guard is charter item 26, the documented follow-up).
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
#include <string_view>
#include <vector>

#include "b2/ir/Graph.h"
#include "b2/passes/GraphBuilder.h"
#include "b2/rbc/Rbc.h"

namespace b2::passes {

// --- ICDG Phase 2: the T0 dispatch-profile snapshot (icdg.md SS7/SS24) ------
//
// The raw per-site dispatch histogram exactly as T0 records it
// (docs/interp_contract.md SS8.1, v1.2.0), with one representation
// change: receiver CLASS IDS are resolved to internal NAMES, because the
// IR-side TypeId the guard needs lives in the CalleeSource's resolver
// space, and the NAME is the bridge between the two id spaces. The
// snapshot is a compilation INPUT (like the RBC text), not IR state, so
// strings are legal here (Rule 16 scopes to the graph).
//
// Site key: (caller MethodId, call pc) - recovered from the call node's
// trailing FrameState descriptor (FrameStateDesc.method/.pc, the builder
// pins the fs at the call pc). The engine classifies; T0 stays data-only.
//
// CONSTRUCTION: whoever owns a T0 run converts (b2graph --pgo, the
// tests); compiler/passes/tools/DispatchProfileSnapshot.h is the shared
// converter (it includes the interp headers, so only interp-linking
// integrators may use it - the passes LIBRARY stays interp-free).

// The invoke family of a profiled site. The 1:1 mirror of interp's
// DispatchSiteKind (interp_contract.md SS8.1); passes-local so this
// header does not include interp headers.
enum class ProfileSiteKind : std::uint8_t {
  Virtual,    // invokevirtual / invokevirtual_quick
  Interface,  // invokeinterface / invokeinterface_quick
  Special,    // invokespecial / invokespecial_quick
  Static,     // invokestatic / invokestatic_quick
};

// Sentinel target id: the site total counted but no RBC target was
// resolved (builtin execution; T0 stores no entry for these either).
inline constexpr std::uint32_t kProfileNoTarget = 0xFFFF'FFFFu;

// One receiver-class observation at one site (icdg.md SS4.3
// DispatchCandidate's raw-data form). `recvClass` empty = the
// static-resolution sentinel (special/static families key entries by
// target, not receiver). `count` zero = unused slot.
struct ProfileEntry {
  std::string recvClass;                 // internal name, "" = sentinel
  std::uint32_t target = kProfileNoTarget; // program method table index
  std::uint32_t count = 0;               // saturating observations
};

// The per-site profile (the raw T0 shape: first-seen entry order,
// frozen by the sticky megamorphic flag; deterministic by construction).
struct ProfileSite {
  ProfileSiteKind kind = ProfileSiteKind::Virtual;
  bool megamorphic = false; // sticky: > 3 distinct receiver classes
  std::uint32_t count = 0;  // saturating successful dispatches at the site
  ProfileEntry entries[3];
};

// The snapshot: [methodIdx][pc] -> site profile, the T0 storage shape.
struct DispatchProfile {
  std::vector<std::vector<ProfileSite>> sites;
};


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

// GuardInline thresholds (Rule 23; Rule 44: no profile data without
// confidence). `guardMinObservations` is the site total below which the
// profile says nothing; `guardMinRatioBP` is the minimum share (basis
// points, 0..10000) the monomorphic entry must hold of that total - a
// site mixing RBC dispatches with builtin executions (count-only) or
// with deopted classes falls below and refuses.
inline constexpr std::uint32_t kGuardInlineMinObservations = 3;
inline constexpr std::uint32_t kGuardInlineMinRatioBP = 9000;

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

  // ICDG Phase 2: the T0 dispatch profile snapshot. Null (default) =
  // direct calls only - CallVirtual/CallInterface sites are not even
  // considered, and the run is byte-identical to v1 (graphs, telemetry,
  // decision log). Non-null: virtual/interface sites are considered,
  // classified against the profile, and monomorphic sites guard-inline.
  // The snapshot must be keyed by THIS program's method table (the site
  // key is (table index, pc)); a mismatched snapshot only ever refuses
  // (the agreement check), never mis-inlines.
  const DispatchProfile* profile = nullptr;

  std::uint32_t guardMinObservations = kGuardInlineMinObservations;
  std::uint32_t guardMinRatioBP = kGuardInlineMinRatioBP;
};

// --- decisions (icdg.md 19: every decision is explainable) -------------------

enum class InlineAction : std::uint8_t {
  DirectInline = 0, // the call site is replaced by the callee body
  GuardInline,     // replaced by guard + callee body (speculative, Rule 3)
  KeepIndirect,    // the call stays (every refusal is this)
};

// One decision record. Reason strings are static (no per-decision
// allocation; the log is a compilation artifact, never IR state).
// siteCount/recvCount carry the profile numbers behind a GuardInline
// decision (the site total and the monomorphic entry's count) so the
// log explains the confidence, icdg.md 19.
struct InlineDecision {
  ir::NodeId call = ir::kInvalidNodeId;
  ir::MethodId target = 0;
  std::uint32_t depth = 0;
  InlineAction action = InlineAction::KeepIndirect;
  std::uint32_t calleeInsns = 0;
  std::uint32_t calleeSlots = 0;
  std::uint32_t calleeNodes = 0; // 0 = not measured (pre-trial refusal)
  std::uint32_t siteCount = 0;   // profile site total (GuardInline only)
  std::uint32_t recvCount = 0;   // profiled entry count (GuardInline only)
  const char* reason = "";
};

// --- telemetry (Rule 26) and the result --------------------------------------

struct InlineTelemetry {
  std::uint32_t sitesConsidered = 0;
  std::uint32_t sitesInlined = 0;    // direct + guard (the budget counts both)
  std::uint32_t sitesGuardInlined = 0; // the speculative subset
  std::uint32_t sitesRefused = 0;
  std::uint32_t guardsEmitted = 0;   // TypeProfile guards installed
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

// Runs the direct- + guard-inline engine over `g`. PRECONDITION: g is
// a verified graph (builder output or post-pipeline). Sites are
// considered in node-id order; a successful inline is followed by its
// body's own call sites (depth + 1, recursion-controlled), then the next
// root site. The IR verifier runs after every site; any failure stops
// the run fail-closed with diagnostics (the graph is left in the
// post-last-good-site state). Deterministic (Rule 124): same (graph,
// source, config, profile) => same decisions and the same resulting
// graph bytes. With cfg.profile == null, virtual/interface sites are
// not considered (v1 behavior, byte-identical).
[[nodiscard]] InlineResult runInlining(ir::Graph& g, InlineCalleeSource& src,
                                       const InlineConfig& cfg = {});

} // namespace b2::passes
