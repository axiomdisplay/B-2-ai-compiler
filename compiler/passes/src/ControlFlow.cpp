// B-2 Passes - control-flow cleanup: branch normalization (key 19),
// control-flow simplification + region repair (key 20), and null-check
// folding (key 18). The repair machinery is shared with trivial block
// fusion (key 12) and the pipeline.
//
// WHY THIS FILE EXISTS:
// Killing control nodes in a sea of nodes is constrained by the tombstone
// law (see PassInternal.h): every tombstone's edges must stay verifier-
// legal forever, and no live node may ever see a dead input. The
// discipline that satisfies both:
//   - replace() handles the LIVE path: replaceNode rewires every user
//     (live and dead) onto the replacement, then junks the tombstone's
//     own edges - freeing the old operands for DCE.
//   - kill() only fires once no LIVE node references the target; dead
//     referencers are junk-rewired first.
//   - unreachableSweep kills the dead subgraph BOTTOM-UP (nodes with zero
//     live referencers die first: terminals, then their predecessors) so
//     the live-referencer precondition holds at every step. Flow
//     reachability treats constant-condition Ifs/Guards as decided (only
//     the taken side flows; a constant-false guard stops flow exactly
//     like the Deopt it becomes).
//   - rebuildRegions drops dead predecessors by REBUILDING regions and
//     their phis (Graph has no removeInput; public API only, Rule 31),
//     collapsing single-pred regions to the pass-through edge.

#include <cstdint>
#include <vector>

#include "PassInternal.h"

namespace b2::passes::detail {

namespace {

// Named budgets (Rule 23): the sweeps are monotone (each iteration kills
// or rewrites at least one node or stabilizes), so the caps only guard
// against pathological graphs; overrun is telemetry, never UB.
constexpr std::uint32_t kMaxSweepIterations = 64;

[[nodiscard]] bool isRegionLike(ir::NodeKind k) {
  return k == ir::NodeKind::Region || k == ir::NodeKind::LoopBegin;
}

// Phis merging at region-like node `r` (live users whose slot-0 input is
// r - the [region, v0..vN-1] shape), in id order (Rule 124).
[[nodiscard]] std::vector<ir::NodeId> phisOf(const ir::Graph& g,
                                             ir::NodeId r) {
  std::vector<ir::NodeId> out;
  for (const ir::Use& u : g.usesOf(r)) {
    if (!g.node(u.user).isDead() && u.slot == 0 &&
        g.node(u.user).kind == ir::NodeKind::Phi) {
      out.push_back(u.user);
    }
  }
  for (std::size_t i = 1; i < out.size(); ++i) {
    for (std::size_t j = i; j > 0 && out[j - 1] > out[j]; --j) {
      ir::NodeId tmp = out[j - 1];
      out[j - 1] = out[j];
      out[j] = tmp;
    }
  }
  return out;
}

// A constant int condition, if the node has one (dead defs keep their
// constValue, so tombstone conditions read back fine).
[[nodiscard]] bool constCondOf(const ir::Graph& g, ir::NodeId cond,
                               std::int32_t& value) {
  const ir::Node& c = g.node(cond);
  if (c.isDead() || c.kind != ir::NodeKind::ConstantI) {
    return false;
  }
  value = static_cast<std::int32_t>(c.constValue);
  return true;
}

// The live projection of `iff` with the wanted kind. DEAD projections are
// tombstones (already folded or killed): an If whose projections are dead
// is not foldable again, and the junk If anchors carry exactly such
// tombstone users.
[[nodiscard]] ir::NodeId projectionOf(const ir::Graph& g, ir::NodeId iff,
                                      ir::NodeKind want) {
  for (const ir::Use& u : g.usesOf(iff)) {
    if (u.slot == 0 && !g.node(u.user).isDead() &&
        g.node(u.user).kind == want) {
      return u.user;
    }
  }
  return ir::kInvalidNodeId;
}

// Flow successors of control node `c`: the live users that consume c in a
// control-role slot, filtered by branch decisions (constant conditions
// decide the If; constant-false guards stop flow).
void flowSuccessors(const ir::Graph& g, ir::NodeId c,
                    std::vector<ir::NodeId>& out) {
  const ir::Node& node = g.node(c);
  if (isTerminator(node.kind)) {
    return; // flow stops
  }
  std::int32_t condVal = 0;
  if (node.kind == ir::NodeKind::If && node.numInputs == 2 &&
      constCondOf(g, g.input(c, 1), condVal)) {
    // Decided branch: only the taken projection flows.
    const ir::NodeId keep = condVal != 0
                                ? projectionOf(g, c, ir::NodeKind::IfTrue)
                                : projectionOf(g, c, ir::NodeKind::IfFalse);
    for (const ir::Use& u : g.usesOf(c)) {
      if (u.slot == 0 && u.user == keep && !g.node(u.user).isDead()) {
        out.push_back(u.user);
      }
    }
    return;
  }
  if (node.kind == ir::NodeKind::Guard && node.numInputs >= 2 &&
      constCondOf(g, g.input(c, 1), condVal) && condVal == 0) {
    return; // always-deopt guard: flow stops (it IS an unconditional deopt)
  }
  for (const ir::Use& u : g.usesOf(c)) {
    if (u.user >= g.nodeCount() || g.node(u.user).isDead()) {
      continue;
    }
    if (g.input(u.user, u.slot) != c) {
      continue; // stale entry
    }
    const ir::InputRole role = roleOfSlot(g, u.user, u.slot);
    if (role == ir::InputRole::Ctrl || role == ir::InputRole::Parent) {
      out.push_back(u.user);
    }
  }
}

} // namespace

void rebuildRegions(ir::Graph& g, PassTelemetry& t, Budget& b,
                    const Junk& jk, const std::vector<bool>* reach) {
  // A predecessor is EFFECTIVE when it is alive and (when reachability is
  // known) flow-reachable: an unreachable live pred is a decided-dead
  // branch edge - dropping it orphans the pred for the kill phase below.
  const auto effectivePred = [&](ir::NodeId p) {
    if (g.node(p).isDead()) {
      return false;
    }
    if (reach != nullptr && p < reach->size() && !(*reach)[p]) {
      return false; // flow-unreachable live pred: a decided-dead edge
    }
    return true;
  };

  for (std::uint32_t iter = 0; iter < kMaxSweepIterations; ++iter) {
    bool changed = false;
    for (ir::NodeId n = 1; n < g.nodeCount() && !b.exceeded; ++n) {
      const ir::Node& node = g.node(n);
      if (node.isDead() || !isRegionLike(node.kind)) {
        continue;
      }

      std::vector<std::uint16_t> live;
      bool anyLive = false;
      for (std::uint16_t s = 0; s < node.numInputs; ++s) {
        if (effectivePred(g.input(n, s))) {
          live.push_back(s);
          anyLive = true;
        }
      }
      if (!anyLive) {
        // Unreachable region (no live pred): kill it and its phis (kill
        // refuses while a live node references the target - the phis'
        // FrameState users keep them alive if they are live).
        for (const ir::NodeId p : phisOf(g, n)) {
          if (kill(g, p, t, b, jk)) {
            changed = true;
          }
        }
        if (kill(g, n, t, b, jk)) {
          changed = true;
        }
        continue;
      }
      if (live.size() == static_cast<std::size_t>(node.numInputs)) {
        continue; // healthy: nothing to repair
      }

      const std::vector<ir::NodeId> phis = phisOf(g, n);
      if (live.size() == 1) {
        // Collapse to the pass-through edge: the region IS its single
        // live pred, and every phi collapses to the surviving value.
        const ir::NodeId pred = g.input(n, live[0]);
        for (const ir::NodeId p : phis) {
          const ir::NodeId v =
              g.input(p, 1 + static_cast<std::uint16_t>(live[0]));
          if (!g.node(v).isDead() && v != p) {
            replace(g, p, v, t, b, jk);
            changed = true;
          } else {
            if (kill(g, p, t, b, jk)) {
              changed = true;
            }
          }
        }
        if (!g.node(pred).isDead() && pred != n) {
          replace(g, n, pred, t, b, jk);
        } else if (kill(g, n, t, b, jk)) {
          changed = true;
        }
        continue;
      }

      // Multi-pred rebuild: fresh region with only live preds, phis
      // rebuilt with values for the surviving pred slots (creation order
      // = old phi id order; new node ids append monotonically -
      // deterministic, Rule 124).
      std::vector<ir::NodeId> predIds;
      predIds.reserve(live.size());
      for (const std::uint16_t s : live) {
        predIds.push_back(g.input(n, s));
      }
      const ir::NodeKind kind = node.kind;
      const ir::NodeId fresh =
          g.make(kind, std::span<const ir::NodeId>(predIds));
      for (const ir::NodeId p : phis) {
        std::vector<ir::NodeId> values{fresh};
        for (const std::uint16_t s : live) {
          values.push_back(g.input(p, 1 + s));
        }
        const ir::NodeId freshPhi =
            g.make(ir::NodeKind::Phi,
                   std::span<const ir::NodeId>(values));
        replace(g, p, freshPhi, t, b, jk);
        changed = true;
      }
      replace(g, n, fresh, t, b, jk);
      changed = true;
    }
    if (!changed) {
      return;
    }
  }
}

void unreachableSweep(ir::Graph& g, PassTelemetry& t, Budget& b,
                      const Junk& jk) {
  for (std::uint32_t iter = 0; iter < kMaxSweepIterations; ++iter) {
    // Phase A: flow reachability from Start (branch decisions included).
    std::vector<bool> reach(g.nodeCount(), false);
    std::vector<ir::NodeId> work;
    if (g.startNode() < g.nodeCount()) {
      reach[g.startNode()] = true;
      work.push_back(g.startNode());
    }
    while (!work.empty()) {
      const ir::NodeId c = work.back();
      work.pop_back();
      std::vector<ir::NodeId> succ;
      flowSuccessors(g, c, succ);
      for (const ir::NodeId s : succ) {
        if (s < g.nodeCount() && !reach[s]) {
          reach[s] = true;
          work.push_back(s);
        }
      }
    }

    // Phase B: regions first - dropping unreachable/dead preds orphans
    // the projections and chains for the kill phase below.
    rebuildRegions(g, t, b, jk, &reach);

    // Phase C: kill control consumers whose LIVE referencers are gone:
    // unreachable nodes (bottom-up: the region repair above already
    // orphaned the decided-dead branch edges), always-deopting guards
    // (flow stops at them; the Deopt node records the continuation),
    // and userless branch structure (If/Switch/projections with no
    // referencers are dangling forks - pure structure, no effects).
    bool killed = false;
    for (ir::NodeId n = 0; n < g.nodeCount() && !b.exceeded; ++n) {
      const ir::Node& node = g.node(n);
      if (node.isDead() || !consumesControl(node.kind) ||
          isJunkSink(jk, n)) {
        continue; // junk sinks are the tombstone anchors: immortal
      }
      // Nodes created after this iteration's reach computation (junk
      // sinks, fresh rebuild nodes) are out of reach's bounds - they are
      // never kill candidates.
      const bool unreachable = n < reach.size() && !reach[n];
      std::int32_t guardCond = 0;
      const bool deadGuard =
          node.kind == ir::NodeKind::Guard && node.numInputs >= 2 &&
          constCondOf(g, g.input(n, 1), guardCond) && guardCond == 0;
      // Note: userless branch forks (a folded If whose projections died)
      // are deliberately KEPT: they are flow-reachable, effect-free, and
      // structurally indistinguishable from the junk anchors - killing
      // them would re-junk tombstones on every pipeline application and
      // break idempotency. They are invisible to live-user analyses and
      // await the IR team's sanctioned removal API (RFC MSG-008).
      if ((unreachable || deadGuard) && liveUseCount(g, n) == 0) {
        if (kill(g, n, t, b, jk)) {
          killed = true;
        }
      }
    }

    // Stable when nothing died and no region changed shape. Region
    // rebuilds can orphan new values (DCE's business) but cannot make
    // more control unreachable, so kill-progress is the loop condition.
    if (!killed) {
      return;
    }
  }
}

void runBranchNormalization(ir::Graph& g, PassTelemetry& t, Budget& b,
                            const Junk& jk) {
  for (ir::NodeId n = 0; n < g.nodeCount() && !b.exceeded; ++n) {
    const ir::Node& node = g.node(n);
    // Junk sinks are skipped: their constant conditions and tombstone
    // "projections" are bookkeeping, never foldable branches.
    if (node.isDead() || isJunkSink(jk, n)) {
      continue;
    }

    if (node.kind == ir::NodeKind::If && node.numInputs == 2) {
      std::int32_t condVal = 0;
      if (!constCondOf(g, g.input(n, 1), condVal)) {
        continue;
      }
      const bool taken = condVal != 0;
      const ir::NodeId trueProj = projectionOf(g, n, ir::NodeKind::IfTrue);
      const ir::NodeId falseProj = projectionOf(g, n, ir::NodeKind::IfFalse);
      if (trueProj == ir::kInvalidNodeId ||
          falseProj == ir::kInvalidNodeId) {
        continue; // malformed If: the verifier's business, not ours
      }
      // The taken projection IS the If's control input (control flows
      // around the branch); the If and the untaken side become
      // unreachable and the sweep below reclaims them bottom-up.
      const ir::NodeId keep = taken ? trueProj : falseProj;
      const ir::NodeId ctrl = g.input(n, 0);
      if (!g.node(ctrl).isDead() && ctrl != keep) {
        replace(g, keep, ctrl, t, b, jk);
      }
      continue;
    }

    if (node.kind == ir::NodeKind::Guard && node.numInputs >= 2) {
      std::int32_t condVal = 0;
      if (!constCondOf(g, g.input(n, 1), condVal)) {
        continue;
      }
      if (condVal != 0) {
        // Always-passing guard: control flows around it.
        replace(g, n, g.input(n, 0), t, b, jk);
      } else {
        // Always-failing guard: record the unconditional deopt (same
        // DeoptId, same FrameState - the deopt reconstructs the same T0
        // state) as a terminal consuming the guard's control. Idempotent:
        // a re-run (fixpoint rounds) finds the existing Deopt instead of
        // creating a duplicate. The guard itself becomes sweep business
        // (flow stops at it, so it and everything chained after it are
        // unreachable - the sweep kills bottom-up).
        const ir::NodeId ctrl = g.input(n, 0);
        const ir::NodeId fs = g.input(n, 2);
        bool exists = false;
        for (const ir::Use& u : g.usesOf(ctrl)) {
          const ir::Node& cand = g.node(u.user);
          if (!cand.isDead() && cand.kind == ir::NodeKind::Deopt &&
              cand.numInputs == 2 && g.input(u.user, 1) == fs &&
              cand.payload == node.payload2) {
            exists = true;
            break;
          }
        }
        if (!exists) {
          (void)g.make(ir::NodeKind::Deopt, {ctrl, fs}, node.payload2);
        }
      }
    }
  }
  // The If/untaken/guard removals require the bottom-up sweep to stay
  // verifier-clean (single-pass runs must leave legal graphs too).
  unreachableSweep(g, t, b, jk);
}

void runNullCheckFolding(ir::Graph& g, PassTelemetry& t, Budget& b,
                         const Junk& jk) {
  for (ir::NodeId n = 0; n < g.nodeCount() && !b.exceeded; ++n) {
    const ir::Node& node = g.node(n);
    if (node.isDead() || node.kind != ir::NodeKind::Guard ||
        node.numInputs < 3) {
      continue;
    }
    if (static_cast<ir::GuardKind>(node.payload) !=
        ir::GuardKind::NullCheck) {
      continue;
    }
    // The builder's shape: Guard(NullCheck, Not(IsNull(x))) (graph_builder
    // section 3). Fold when x is provably never null.
    const ir::NodeId cond = g.input(n, 1);
    const ir::Node& condNode = g.node(cond);
    if (condNode.isDead() || condNode.kind != ir::NodeKind::Not ||
        condNode.numInputs < 1) {
      continue;
    }
    const ir::NodeId isnull = g.input(cond, 0);
    const ir::Node& isnullNode = g.node(isnull);
    if (isnullNode.isDead() || isnullNode.kind != ir::NodeKind::IsNull ||
        isnullNode.numInputs < 1) {
      continue;
    }
    if (!isNeverNullNode(g, g.input(isnull, 0))) {
      continue;
    }
    // Pass-through: the guarded operation consumes the guard's control.
    // The FrameState stays alive with the guarded op when shared (builder
    // policy section 4); an unshared FS becomes DCE's business - the
    // tombstone junking frees it.
    replace(g, n, g.input(n, 0), t, b, jk);
  }
}

} // namespace b2::passes::detail
