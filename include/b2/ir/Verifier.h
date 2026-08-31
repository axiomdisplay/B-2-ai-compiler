#pragma once
// B-2 IR - graph verifier (Rules 40, 126).
//
// WHY THIS FILE EXISTS:
// The verifier is the debug-build gate that runs after every pass (Rule 40)
// and the load gate for replayed graphs. It checks structure AND metadata:
// dangling ids, use-def consistency, effect-chain (memory) continuity,
// guard FrameState attachment, Rule 122 speculative metadata completeness,
// materialization-graph acyclicity, vector/tagged well-formedness. Like the
// RBC verifier it is TOTAL: arbitrary malformed graphs produce a bounded
// diagnostic list, never UB, never an infinite loop.

#include <cstdint>
#include <string>
#include <vector>

#include "b2/ir/Graph.h"

namespace b2::ir {

struct VerifyDiag {
  NodeId node = kInvalidNodeId; // the node the diagnostic is about
  std::string message;          // human-readable, self-contained
};

struct VerifyResult {
  bool ok = false;
  std::vector<VerifyDiag> diags; // ordered by emission, capped (default 100)

  [[nodiscard]] bool hasErrors() const noexcept { return !ok; }
};

// Verifies the whole graph. Checks, in order:
//   1. structural: every input id in range and alive (not Dead/Replaced),
//      no self-input, input count matches the registry signature
//      (fixed prefix + variadic + mandatory FrameState);
//   2. role conformance: Ctrl inputs come from control-producing kinds, Mem
//      inputs from memory-state kinds, FrameState inputs are FrameState
//      nodes, projections point at their parent kind;
//   3. operand types: arithmetic/comparison/conversion operands match the
//      kind's fixed types (Rule 33: mismatches are errors, never coercions);
//   4. memory continuity: every Mem input traces back to Start through
//      memory-state nodes, with no cycle (Rules 40, 121);
//   5. guards: FrameState attached (Rule 5); speculative guards carry
//      complete Rule 122 metadata including a valid dependency (Rule 42);
//   6. calls / LoopExit / Deopt: FrameState attached (deopt points
//      reachable, Rule 126);
//   7. FrameState caller chains are acyclic; vobj references valid;
//   8. PEA: materialization graphs acyclic (Rule 126);
//   9. vector well-formedness: lane counts power-of-two within
//      [kMinVectorLanes, kMaxVectorLanes], operand lane/type agreement,
//      mask lane agreement (Part XVIII SWLP);
//  10. tagged values: no Polymorphic rep on unguarded tag/untag nodes
//      (Part XVIII: unguarded transitions are verifier errors).
[[nodiscard]] VerifyResult verify(const Graph& g) noexcept;

} // namespace b2::ir
