// B-2 Passes - dead code elimination (registry key 11, suite item 11).
//
// WHY THIS FILE EXISTS:
// The smallest safe claim in the optimizer: a node with no live users
// whose kind is in the documented DCE-safe set (docs/pass_contracts.md
// section 4) is unobservable, so removing it changes nothing. The safe
// set is deliberately CONSERVATIVE: loads (volatile-read classification
// is opaque at the IR level), stores, calls, allocations, casts, guards,
// control, and the trapping integer div/rem family are never removed by
// v1; unused FrameState nodes are (deopt metadata without a deopt point).
// Removing one node can orphan its inputs, so the sweep iterates - the
// cascade is monotone, bounded by the node count, and budget-guarded.

#include <cstdint>

#include "PassInternal.h"

namespace b2::passes::detail {

namespace {
// Named sweep cap (Rule 23): each iteration removes at least one node or
// the sweep stops, so the cap only guards pathological graphs.
constexpr std::uint32_t kMaxDceSweeps = 64;
} // namespace

void runDCE(ir::Graph& g, PassTelemetry& t, Budget& b, const Junk& jk) {
  for (std::uint32_t sweep = 0; sweep < kMaxDceSweeps; ++sweep) {
    bool changed = false;
    for (ir::NodeId n = 0; n < g.nodeCount() && !b.exceeded; ++n) {
      const ir::Node& node = g.node(n);
      if (node.isDead() || !isDceSafe(node.kind)) {
        continue;
      }
      if (n == g.startNode()) {
        continue; // Start is the control/memory origin, always live
      }
      // Tombstone law: a node referenced by ANY node (live or dead) is
      // unremovable; kills free their tombstone-referenced operands only
      // via the junk sinks, so the cascade runs across sweeps.
      if (useCount(g, n) == 0) {
        if (kill(g, n, t, b, jk)) {
          changed = true;
        }
      }
    }
    if (!changed) {
      return;
    }
  }
}

} // namespace b2::passes::detail
