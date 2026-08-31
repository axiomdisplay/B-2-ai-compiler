#pragma once
// B-2 Passes - the RBC-to-IR graph builder (T2 pipeline entry, suite items
// 1-6 of docs/teams/passes-team.md).
//
// WHY THIS FILE EXISTS:
// T2/T3 consume sea-of-nodes IR (Amendment B.1); this builder is the total
// function from verified RBC to that IR. It is the FIRST consumer of
// b2::ir and therefore owns the lowering contract: how every RBC opcode
// family maps to IR nodes, where FrameStates are seeded (Rules 5, 126),
// how guards gate fixed nodes, how loops/merges become Phis, and how the
// memory state chains (docs/graph_builder.md is the normative reference).
//
// SCOPE (v1):
// - All un-quickened and quickened RBC opcodes EXCEPT: invokedynamic,
//   multianewarray, guard_class, and ldc of MethodType/MethodHandle (each
//   refuses with a diagnostic - the same set T0 refuses or defers).
// - Reducible control flow only; an irreducible edge rejects the method
//   (javac-shaped bytecode is always reducible).
// - Exception POLICY v1: every exceptional continuation DEOPTS (classes
//   2.1/2.2/2.3 of docs/deopt_backend.md). Uncaught exceptions unwind via
//   Unwind; caught exceptions and re-executable traps deopt to T0, which
//   re-executes and dispatches through THE EXCEPTION ALGORITHM. In-graph
//   handler compilation (CallExcept -> handler Region -> LoadException,
//   with instanceof dispatch chains) is the documented follow-up.
// - Synchronized methods refuse (FrameState does not yet carry the held
//   monitor record deopt must reproduce; interp_contract.md section 1).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "b2/ir/Graph.h"
#include "b2/ir/Types.h"
#include "b2/rbc/Rbc.h"

namespace b2::passes {

// Cp-string -> opaque IR id resolution (Rule 16: the IR carries no strings).
// The REAL frontend inter owns the mapping (class tables, field/method
// tables, symbol tables); tests and tools use CounterResolver. Ids only
// need to be stable for the compilation unit: the IR stores them verbatim.
class SymbolResolver {
 public:
  virtual ~SymbolResolver() = default;

  // Non-const by design: resolution may inter/memoize as it goes (the
  // CounterResolver does). Deterministic resolvers keep the build
  // deterministic (Rule 124).
  [[nodiscard]] virtual ir::TypeId classId(std::string_view internalName) = 0;
  [[nodiscard]] virtual ir::FieldId fieldId(std::string_view cls,
                                            std::string_view name,
                                            std::string_view descriptor) = 0;
  [[nodiscard]] virtual ir::MethodId methodId(std::string_view cls,
                                              std::string_view name,
                                              std::string_view descriptor) = 0;
  [[nodiscard]] virtual ir::SymbolId symbolId(std::string_view payload) = 0;
  [[nodiscard]] virtual std::uint32_t
  switchTableId(const std::vector<std::int32_t>& payload) = 0;
};

// Deterministic default resolver: first-encounter counter interning. Two
// builds of the same method produce identical ids (Rule 124).
class CounterResolver final : public SymbolResolver {
 public:
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
  [[nodiscard]] std::uint32_t intern(std::string_view key);

  std::vector<std::string> keys_; // 1:1 with assigned ids
};

struct BuildDiag {
  std::uint32_t pc = 0;    // instruction index the diagnostic is about
  std::string message;     // human-readable, self-contained
};

struct BuildResult {
  bool ok = false;
  std::vector<BuildDiag> diags; // ordered by emission, capped (default 100)

  [[nodiscard]] bool hasErrors() const noexcept { return !ok; }
};

// Builds `m` into `g`. PRECONDITION: rbc::verify(m) passed (the tier-input
// law: RBC is verified before any tier consumes it; the T2 driver verifies
// at class-load time). The builder is TOTAL anyway: unverified or hostile
// input yields bounded diagnostics, never UB - every register/slot/cp/pc
// index is re-checked defensively.
//
// `methodId` lands in every FrameStateDesc (deopt reconstruction needs the
// owning method). `g` must be freshly constructed (node 0 = Start only);
// building into a used graph is a caller bug and rejects.
//
// DETERMINISM (Rule 124): the same (method, resolver ids) produces the same
// node ids, printed dump, and serialized bytes. Deopt ids are allocated in
// materialization order starting at 1.
[[nodiscard]] BuildResult buildGraph(const rbc::Method& m,
                                     SymbolResolver& res, ir::Graph& g,
                                     ir::MethodId methodId);

} // namespace b2::passes
