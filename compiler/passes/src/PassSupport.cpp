// B-2 Passes - shared support: the Junk tombstone-sink machinery, the
// legal kill/replace protocols, and the registry-derived predicates.
//
// WHY THIS FILE EXISTS:
// The IR verifier checks tombstone (Dead/Replaced) nodes exactly like
// live ones: every input must be alive, role/kind-conformant, and operand
// type-correct (checkNode has no dead skip; the memory-chain pass is the
// only one that skips). Since kills and replacements leave tombstones
// whose edges keep pointing at their old operands, a pass suite that
// wants to reclaim dead values needs every tombstone edge to end on an
// IMMORTAL, verifier-legal sink. This file is that protocol: Junk (the
// lazy sink cache), junkEdges (tombstone normalization), replace
// (replaceNode + junking), and kill (dead-referencer junking + own-edge
// junking + killNode, refusing while a live referencer remains).
//
// Soundness argument (the invariant the whole suite relies on): after
// every replace/kill, every node - live or dead - has only ALIVE inputs,
// so no live node ever observes a dead input and the verifier passes at
// any pass boundary. Junk sinks are flow-dead by construction (nothing
// live consumes them) and immortal in practice (tombstones reference
// them; DCE only removes nodes with zero total referencers).

#include "PassInternal.h"

#include <cstdint>
#include <vector>

#include "b2/ir/Printer.h"

namespace b2::passes::detail {

// The ir::* vocabulary this file speaks (qualified once, at the top).
using ir::EffectKind;
using ir::InputRole;
using ir::IRType;
using ir::Node;
using ir::NodeClass;
using ir::NodeFlag;
using ir::NodeInfo;
using ir::NodeKind;
using ir::Use;

namespace {

// A live FrameState sink with no slots (descriptor 0/0, no caller, no
// virtual objects): verifier-legal by construction (checkFrameStates
// skips dead nodes, and a live empty FrameState satisfies every check it
// does run).
[[nodiscard]] ir::NodeId makeFsSink(ir::Graph& g) {
  return g.makeFrameState(ir::MethodId{0}, 0, std::span<const ir::NodeId>{});
}

} // namespace

ir::NodeId Junk::parentSink(ir::NodeKind projectionKind) const {
  switch (projectionKind) {
  case NodeKind::IfTrue:
  case NodeKind::IfFalse: {
    if (ifSink == ir::kInvalidNodeId || g.node(ifSink).isDead()) {
      ifSink = g.make(NodeKind::If, {g.startNode(), g.constantI(0)});
    }
    return ifSink;
  }
  case NodeKind::SwitchCase:
  case NodeKind::SwitchDefault: {
    if (switchSink == ir::kInvalidNodeId || g.node(switchSink).isDead()) {
      switchSink =
          g.make(NodeKind::Switch, {g.startNode(), g.constantI(0)});
    }
    return switchSink;
  }
  case NodeKind::CallExcept: {
    if (callSink == ir::kInvalidNodeId || g.node(callSink).isDead()) {
      const ir::NodeId fs = makeFsSink(g);
      callSink = g.make(NodeKind::CallStatic, {g.startNode(), g.startNode()},
                        0, static_cast<std::uint32_t>(IRType::Bottom));
      g.appendInput(callSink, fs); // mandatory trailing FrameState
    }
    return callSink;
  }
  default:
    return ctrlSink(); // not a projection: Start is the generic control sink
  }
}

ir::NodeId Junk::fsSinkGet() const {
  if (fsSink == ir::kInvalidNodeId || g.node(fsSink).isDead()) {
    fsSink = makeFsSink(g);
  }
  return fsSink;
}

ir::NodeId Junk::dataSink(ir::IRType type) const {
  // Dead ids (e.g. a sink replaced before its protection existed) are
  // recreated on demand; the GVN/kill guards keep them alive from then
  // on.
  switch (type) {
  case IRType::Int:
    if (constI == ir::kInvalidNodeId || g.node(constI).isDead()) {
      constI = g.constantI(0);
    }
    return constI;
  case IRType::Long:
    if (constL == ir::kInvalidNodeId || g.node(constL).isDead()) {
      constL = g.constantL(0);
    }
    return constL;
  case IRType::Float:
    if (constF == ir::kInvalidNodeId || g.node(constF).isDead()) {
      constF = g.constantF(0.0f);
    }
    return constF;
  case IRType::Double:
    if (constD == ir::kInvalidNodeId || g.node(constD).isDead()) {
      constD = g.constantD(0.0);
    }
    return constD;
  case IRType::Null:
  case IRType::Ref:
    if (constNull == ir::kInvalidNodeId || g.node(constNull).isDead()) {
      constNull = g.constantNull();
    }
    return constNull;
  case IRType::Bottom:
    // Bottom-typed Data slots are memory-phi value edges (stores as
    // memory states): Start is the memory-state origin, produces Bottom,
    // and joins legally in phi value position - the one sink that keeps
    // both the operand-type join and the memory-chain walk verifier-
    // clean. (Typed slots never see Bottom operands in verified input.)
    return ctrlSink();
  default:
    return ir::kInvalidNodeId; // Tagged / vectors: no junk sink
  }
}

ir::NodeId Junk::regionSink(std::uint16_t preds) const {
  if (preds < 2) {
    preds = 2; // no 1-predecessor Region can verify; callers pad phis
  }
  while (regionSinks.size() < static_cast<std::size_t>(preds - 1)) {
    const std::uint16_t n = static_cast<std::uint16_t>(regionSinks.size() + 2);
    std::vector<ir::NodeId> p(n, g.startNode());
    regionSinks.push_back(
        g.make(NodeKind::Region, std::span<const ir::NodeId>(p)));
  }
  return regionSinks[preds - 2];
}

bool isJunkSink(const Junk& jk, ir::NodeId n) {
  if (n == jk.ifSink || n == jk.switchSink || n == jk.callSink ||
      n == jk.fsSink || n == jk.constI || n == jk.constL || n == jk.constF ||
      n == jk.constD || n == jk.constNull) {
    return true;
  }
  for (const ir::NodeId r : jk.regionSinks) {
    if (r == n) {
      return true;
    }
  }
  return false;
}

ir::NodeId junkForSlot(const ir::Graph& g, ir::NodeId n, std::uint16_t slot,
                       const Junk& jk) {
  const Node& node = g.node(n);
  if (slot >= node.numInputs) {
    return ir::kInvalidNodeId;
  }
  // Phi region slot: must be a Region/LoopBegin KIND with matching arity.
  if (node.kind == NodeKind::Phi && slot == 0) {
    const std::uint16_t values = static_cast<std::uint16_t>(
        node.numInputs > 0 ? node.numInputs - 1 : 0);
    return jk.regionSink(values < 2 ? 2 : values);
  }
  const InputRole role = roleOfSlot(g, n, slot);
  switch (role) {
  case InputRole::Ctrl:
    return jk.ctrlSink();
  case InputRole::Parent:
    return jk.parentSink(node.kind);
  case InputRole::Mem:
    return jk.memSink();
  case InputRole::FrameState:
    return jk.fsSinkGet();
  case InputRole::Data:
  case InputRole::None:
    break;
  }
  // Data slot: a junk constant of the CURRENT edge's value type (computed
  // from the def's kind/payload, which survive death).
  return jk.dataSink(resultTypeOf(g, g.input(n, slot)));
}

void junkEdges(ir::Graph& g, ir::NodeId n, const Junk& jk) {
  // Phi with a single value input: no 1-predecessor Region can verify, so
  // pad to two value slots first (appendInput preserves use lists; this
  // only runs on a node that is already a tombstone or is about to become
  // one, so the padding rewrites history, not semantics).
  if (g.node(n).kind == NodeKind::Phi && g.node(n).numInputs == 2) {
    const IRType t = resultTypeOf(g, g.input(n, 1));
    const ir::NodeId pad = jk.dataSink(t == IRType::Bottom ? IRType::Int : t);
    if (pad != ir::kInvalidNodeId) {
      g.appendInput(n, pad);
    }
  }
  for (std::uint16_t s = 0; s < g.node(n).numInputs; ++s) {
    const ir::NodeId cur = g.input(n, s);
    if (cur == g.startNode() || cur == n) {
      continue; // already sunk (or a legal self marker)
    }
    const ir::NodeId sink = junkForSlot(g, n, s, jk);
    if (sink == ir::kInvalidNodeId || sink == cur) {
      continue; // no legal junk: keep the edge (DCE-protected, sound)
    }
    g.setInput(n, s, sink);
  }
}

void replace(ir::Graph& g, ir::NodeId oldNode, ir::NodeId withNode,
             PassTelemetry& t, Budget& b, const Junk& jk) {
  if (!b.charge()) {
    return; // budget stop happens before the mutation: graph stays valid
  }
  if (g.replaceNode(oldNode, withNode) != 0) {
    ++t.rewrites;
    junkEdges(g, oldNode, jk); // tombstone law: sinks free the old inputs
  }
}

bool kill(ir::Graph& g, ir::NodeId n, PassTelemetry& t, Budget& b,
          const Junk& jk) {
  if (n >= g.nodeCount() || g.node(n).isDead()) {
    return true; // already gone (idempotent)
  }
  if (!b.charge()) {
    return false; // budget stop: caller retries/defers
  }
  // Refuse while any LIVE node still references n ("removed all uses
  // first"). DEAD referencers get their edge junked (history rewiring is
  // structurally free and semantically irrelevant).
  for (const Use& u : g.usesOf(n)) {
    if (u.user < g.nodeCount() && !g.node(u.user).isDead()) {
      return false; // live referencer: the caller must repair or defer
    }
  }
  // Snapshot: setInput mutates the use list we are iterating.
  std::vector<Use> snapshot(g.usesOf(n).begin(), g.usesOf(n).end());
  for (const Use& u : snapshot) {
    if (u.user >= g.nodeCount() || u.slot >= g.node(u.user).numInputs) {
      continue; // stale entry
    }
    if (g.input(u.user, u.slot) != n) {
      continue; // slot was rewired since the entry was made
    }
    const ir::NodeId sink = junkForSlot(g, u.user, u.slot, jk);
    if (sink != ir::kInvalidNodeId) {
      g.setInput(u.user, u.slot, sink);
    }
  }
  junkEdges(g, n, jk);
  g.killNode(n);
  ++t.removals;
  return true;
}

ir::InputRole roleOfSlot(const ir::Graph& g, ir::NodeId n,
                         std::uint16_t slot) {
  const Node& node = g.node(n);
  const NodeInfo& row = info(node.kind);
  if (slot >= node.numInputs) {
    return InputRole::None;
  }
  if (row.hasFrameState &&
      slot + 1 == static_cast<std::uint16_t>(node.numInputs)) {
    return InputRole::FrameState;
  }
  if (slot < row.numFixed) {
    return slot < 6 ? row.roles[slot] : InputRole::None;
  }
  if (row.variadic) {
    return row.variadicRole;
  }
  return InputRole::None;
}

bool consumesControl(ir::NodeKind k) {
  const NodeInfo& row = info(k);
  for (std::uint8_t i = 0; i < row.numFixed && i < 6; ++i) {
    if (row.roles[i] == InputRole::Ctrl || row.roles[i] == InputRole::Parent) {
      return true;
    }
  }
  return row.variadic && (row.variadicRole == InputRole::Ctrl);
}

bool isTerminator(ir::NodeKind k) {
  switch (k) {
  case NodeKind::Return:
  case NodeKind::Unwind:
  case NodeKind::Deopt:
  case NodeKind::End:
    return true;
  default:
    return false;
  }
}

bool isNeverNullNode(const ir::Graph& g, ir::NodeId n) {
  const Node& node = g.node(n);
  if (node.isDead()) {
    return false;
  }
  if (node.flags.has(NodeFlag::NeverNull)) {
    return true;
  }
  switch (node.kind) {
  case NodeKind::New:
  case NodeKind::NewArray:
  case NodeKind::NewRefArray:
  case NodeKind::NewMultiArray:
  case NodeKind::Materialize:
  case NodeKind::ConstantSym:
    return true;
  default:
    return false;
  }
}

std::uint32_t useCount(const ir::Graph& g, ir::NodeId n) {
  // Use lists are exact (edge (user,slot) exists iff input(slot)==def),
  // including tombstone users: killNode does not scrub, and setInput keeps
  // the invariant. "No referencers at all" is the only legal DCE kill.
  return static_cast<std::uint32_t>(g.usesOf(n).size());
}

std::uint32_t liveUseCount(const ir::Graph& g, ir::NodeId n) {
  std::uint32_t live = 0;
  for (const Use& u : g.usesOf(n)) {
    if (!g.node(u.user).isDead()) {
      ++live;
    }
  }
  return live;
}

bool isPureValueKind(ir::NodeKind k) {
  const NodeInfo& row = info(k);
  return row.cls == NodeClass::Value && row.effect == EffectKind::Pure;
}

bool isDceSafe(ir::NodeKind k) {
  // FrameState: deopt metadata; unused means no deopt point references it.
  if (k == NodeKind::FrameState) {
    return true;
  }
  // Guard-contract readers: the builder null-guards receivers in control
  // (docs/graph_builder.md section 3), so the nodes themselves cannot trap
  // and a userless instance is unobservable.
  if (k == NodeKind::ArrayLength || k == NodeKind::InstanceOf) {
    return true;
  }
  if (!isPureValueKind(k)) {
    return false;
  }
  // Trapping integer div/rem: an unguarded userless instance may be the
  // only trap witness in a hand-built graph - removed only after a
  // guard-adjacency proof exists (documented deferral).
  if (k == NodeKind::DivI || k == NodeKind::RemI || k == NodeKind::DivL ||
      k == NodeKind::RemL) {
    return false;
  }
  // PEA analysis state: FrameState descriptors reference virtual objects
  // from a SIDE TABLE, not use edges - killing one would dangle the
  // descriptor. PEA owns their lifetime.
  if (k == NodeKind::VirtualObjectState) {
    return false;
  }
  return true;
}

bool isGvnEligible(ir::NodeKind k) {
  if (k == NodeKind::Phi || k == NodeKind::ArrayLength ||
      k == NodeKind::InstanceOf || k == NodeKind::LoadField ||
      k == NodeKind::LoadStatic || k == NodeKind::LoadElem) {
    return true;
  }
  if (!isPureValueKind(k)) {
    return false;
  }
  // State markers: semantically fine to dedup, but they are analysis
  // state, not values; replacement churn buys nothing.
  if (k == NodeKind::Undef || k == NodeKind::VirtualObjectState) {
    return false;
  }
  return true;
}

} // namespace b2::passes::detail
