// B-2 Passes - CM-PEA: cross-method region-based partial escape analysis
// (special_passes.md section 1; Part XVIII PEA Rule; keys 65/66/67/69).
//
// WHY THIS FILE EXISTS:
// The charter's first-class PEA pass, SMT-free per the fixed engineering
// design: escape is graph reachability, not a solver problem. The engine
// runs over the post-inline sea-of-nodes graph (cross-method data flow is
// already merged there - the "CM" in CM-PEA), classifies every allocation
// on the height-6 escape lattice by one monotone pass over its live uses,
// and executes one of the four PEA Rule dispositions:
//
//   SCALARIZED   (NoEscape, key 67): loads forward to their field values,
//                virtual stores vanish, the allocation disappears.
//   MATERIALIZED (Arg/Field/Return escape, keys 66/69): the FIRST escape
//                use becomes a Materialize [ctrl, mem, vobj] wired into
//                both the ctrl and mem chains; every later use reads the
//                materialized reference. Partial escape: the loads that
//                happened before the escape point still forward.
//   REJECTED     (identity/exception/fs/phi observability, the cost
//                gates, or a kill switch): the allocation stays exactly
//                as it was. PEA Rule option 4.
//
// SOUNDNESS CORE (special_passes.md 1.4 - correctness by construction):
// every live use of the allocation lands in a fixed classification
// table; every unclassifiable use publishes to GlobalEscape (rejected).
// Load forwarding is a backward memory-chain walk to the nearest
// same-field store, executed for ALL readers BEFORE any writer is
// removed (the chain is then still complete, so every reader sees the
// store that defines its Java value). The straight-segment scope gate -
// all pinned uses reach the allocation's control point without crossing
// a Region/LoopBegin and with EQUAL branch-projection sets - is exactly
// the condition under which "chain order == program order" holds, so no
// merge can re-broadcast a different field state between a store and
// its readers. Materialization rewrites the escape use's ctrl AND mem
// inputs onto the Materialize node, so the object state is published to
// the memory chain before any observer can run. Nested virtual objects
// materialize inner-first (verifier: a Materialize must not store
// still-virtual fields).
//
// The v1 scope gates (documented growth paths in pass_contracts.md 12):
//   - straight-segment uses only: a Phi merge of field states (phi-of-
//     stores) rejects the allocation;
//   - array element access needs constant indices (dynamic indices
//     materialize);
//   - a live FrameState referencing the allocation lists a per-instant
//     vobj on the snapshot instead of rejecting (the fs-escape listing,
//     MSG-20260901-006's appendFrameStateVobj - see the fs-escape block);
//   - cross-inline-site summary APPLICATION (the EscapeSummary reuse)
//     arrives with the multi-caller growth path; v1 analyzes the merged
//     post-inline graph directly, which is the same information.
//
// Determinism (Rule 124): allocations are visited in id order, fields in
// FieldId order, escape uses by id; node creation happens in visit
// order. Idempotency: a second run finds no live New/NewArray/
// NewRefArray candidates and performs zero rewrites.

#include "PassInternal.h"

#include "b2/ir/Printer.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace b2::passes::detail {

// The ir::* vocabulary this file speaks (qualified once, at the top).
using ir::EffectKind;
using ir::FrameStateId;
using ir::InputRole;
using ir::IRType;
using ir::Node;
using ir::NodeFlag;
using ir::NodeInfo;
using ir::NodeKind;
using ir::NodeId;
using ir::Use;

namespace {

// --- small helpers ----------------------------------------------------------

// A pinned node's Ctrl-role input, or invalid when it has none.
[[nodiscard]] NodeId ctrlInputOf(const ir::Graph& g, NodeId n) {
  const std::uint16_t inputs = g.numInputs(n);
  for (std::uint16_t s = 0; s < inputs; ++s) {
    if (roleOfSlot(g, n, s) == InputRole::Ctrl) {
      return g.input(n, s);
    }
  }
  return ir::kInvalidNodeId;
}

// A pinned node's Mem-role input, or invalid when it has none.
[[nodiscard]] NodeId memInputOf(const ir::Graph& g, NodeId n) {
  const std::uint16_t inputs = g.numInputs(n);
  for (std::uint16_t s = 0; s < inputs; ++s) {
    if (roleOfSlot(g, n, s) == InputRole::Mem) {
      return g.input(n, s);
    }
  }
  return ir::kInvalidNodeId;
}

// The default value constant of an IRType (Java field default: zero /
// null). Returns invalid for non-defaultable types (caller rejects).
[[nodiscard]] NodeId defaultConstant(ir::Graph& g, IRType t) {
  switch (t) {
  case IRType::Int:
    return g.constantI(0);
  case IRType::Long:
    return g.constantL(0);
  case IRType::Float:
    return g.constantF(0.0f);
  case IRType::Double:
    return g.constantD(0.0);
  case IRType::Null:
  case IRType::Ref:
    return g.constantNull();
  default:
    return ir::kInvalidNodeId;
  }
}

// The ConstantI value of node n, or false when n is not an Int constant.
[[nodiscard]] bool intConstant(const ir::Graph& g, NodeId n,
                               std::int32_t& out) {
  if (n >= g.nodeCount() || g.node(n).kind != NodeKind::ConstantI) {
    return false;
  }
  out = static_cast<std::int32_t>(
      static_cast<std::uint32_t>(g.node(n).constValue & 0xFFFFFFFFu));
  return true;
}

[[nodiscard]] bool isAllocKind(NodeKind k) {
  return k == NodeKind::New || k == NodeKind::NewArray ||
         k == NodeKind::NewRefArray;
}

// The element IRType of an array allocation (NewArray carries it in
// payload; NewRefArray is a reference array).
[[nodiscard]] IRType elemTyOf(const ir::Graph& g, NodeId alloc,
                              NodeKind kind) {
  return kind == NodeKind::NewArray
             ? static_cast<IRType>(g.node(alloc).payload)
             : IRType::Ref;
}

[[nodiscard]] bool isMemStateProducer(NodeKind k) {
  // Produces the memory state for its successors (the chain advances
  // through these; loads pass their mem input through instead).
  switch (k) {
  case NodeKind::StoreField:
  case NodeKind::StoreStatic:
  case NodeKind::StoreElem:
  case NodeKind::CallStatic:
  case NodeKind::CallVirtual:
  case NodeKind::CallInterface:
  case NodeKind::CallDynamic:
  case NodeKind::ClassInit:
  case NodeKind::MemBar:
  case NodeKind::MonitorEnter:
  case NodeKind::MonitorExit:
  case NodeKind::Materialize:
  case NodeKind::VectorStore:
    return true;
  default:
    return false;
  }
}

// The memory state in effect just BEFORE control point `ctrl`: walks
// the ctrl chain to the first mem-state producer (itself), or a load
// (its mem input), and otherwise continues through pass-through nodes.
// Branch projections backtrack through the PARENT slot (their slot 0 is
// the Parent role, not Ctrl): a guard inside a branch observes the
// pre-branch state until the branch's own producers advance it.
[[nodiscard]] NodeId memStateBefore(const ir::Graph& g, NodeId ctrl) {
  NodeId cur = ctrl;
  for (std::uint32_t steps = 0; steps < kPeaMaxChainSteps; ++steps) {
    if (cur == ir::kInvalidNodeId || cur >= g.nodeCount()) {
      break;
    }
    const NodeKind k = g.node(cur).kind;
    if (cur == g.startNode() || isMemStateProducer(k)) {
      return cur;
    }
    if (k == NodeKind::LoadField || k == NodeKind::LoadStatic ||
        k == NodeKind::LoadElem || k == NodeKind::VectorLoad) {
      const NodeId m = memInputOf(g, cur);
      return m != ir::kInvalidNodeId ? m : g.startNode();
    }
    if (k == NodeKind::IfTrue || k == NodeKind::IfFalse ||
        k == NodeKind::SwitchCase || k == NodeKind::SwitchDefault) {
      cur = g.input(cur, 0); // the PARENT slot: the If/Switch itself
      continue;
    }
    cur = ctrlInputOf(g, cur);
  }
  return g.startNode();
}

// A node kind that can carry a deopt FrameState and reconstruct frames
// when it deopts: guards (deopt on failure), Deopt nodes, and every
// call family (the exceptional continuation deopts through the call's
// own FrameState - graph_builder classes 2.1/2.2/2.3).
[[nodiscard]] bool isDeoptPoint(NodeKind k) {
  switch (k) {
  case NodeKind::Guard:
  case NodeKind::Deopt:
  case NodeKind::CallStatic:
  case NodeKind::CallVirtual:
  case NodeKind::CallInterface:
  case NodeKind::CallDynamic:
    return true;
  default:
    return false;
  }
}

// The trailing FrameState input of a deopt point (the registry's
// hasFrameState slot), or invalid when it has none / is not a live
// FrameState node.
[[nodiscard]] NodeId fsInputOf(const ir::Graph& g, NodeId d) {
  const std::uint16_t n = g.numInputs(d);
  if (n == 0) {
    return ir::kInvalidNodeId;
  }
  const NodeId fs = g.input(d, n - 1);
  if (fs >= g.nodeCount() || g.node(fs).kind != NodeKind::FrameState ||
      g.node(fs).isDead()) {
    return ir::kInvalidNodeId;
  }
  return fs;
}

// The memory state observable at deopt point d: a call reads its own
// mem input (the state at the call); guards and Deopts sit in front of
// their protected op, so the state at their ctrl predecessor is the
// state they observe.
[[nodiscard]] NodeId deoptInstantMem(const ir::Graph& g, NodeId d) {
  const NodeKind k = g.node(d).kind;
  if (k == NodeKind::CallStatic || k == NodeKind::CallVirtual ||
      k == NodeKind::CallInterface || k == NodeKind::CallDynamic) {
    const NodeId m = memInputOf(g, d);
    return (m != ir::kInvalidNodeId && m < g.nodeCount() &&
            !g.node(m).isDead())
               ? m
               : g.startNode();
  }
  const NodeId c = ctrlInputOf(g, d);
  return (c != ir::kInvalidNodeId && c < g.nodeCount() &&
          !g.node(c).isDead())
             ? memStateBefore(g, c)
             : g.startNode();
}

// --- the use classification table (the whole soundness surface) -------------

enum class UseClass : std::uint8_t {
  Reader,           // LoadField(obj) / LoadElem(array, const idx)
  Writer,           // StoreField(obj) / StoreElem(array, const idx)
  LengthReader,     // ArrayLength(array)
  IsNullUser,       // IsNull(alloc) - folds to 0 under scalarization
  ArgEscape,        // Call argument
  ReturnEscape,     // Return value
  FieldEscape,      // stored into a foreign object/static/element
  DynamicIndex,     // LoadElem/StoreElem(array, non-const idx)
  IdentityReject,   // RefEq / MonitorEnter / MonitorExit
  ExceptionReject,  // Unwind(exception)
  FsEscape,         // live FrameState locals reference
  PhiReject,        // Phi value input
  CastReject,       // CheckCast(obj)
  InstanceOfReject, // InstanceOf(obj)
  VobjHeld,         // VirtualObjectState field edge (local escape)
  ChainLink,        // the allocation AS a ctrl-chain predecessor (the
                    // builder advances curCtrl_ through New; the splice
                    // rewires these users onto the predecessor)
  UnknownReject,    // anything else - conservatively observable
};

struct ClassifiedUse {
  NodeId user = ir::kInvalidNodeId;
  std::uint16_t slot = 0;
  UseClass cls = UseClass::UnknownReject;
};

// Classifies one live use (user, slot) of the allocation. The table is
// total: every (kind, slot) pair has an entry, and the default is
// GlobalEscape (special_passes.md 1.4: observability predicates are
// syntactic; when in doubt, publish).
[[nodiscard]] UseClass classifyUse(const ir::Graph& g, NodeId user,
                                   std::uint16_t slot) {
  const Node& nd = g.node(user);
  // The allocation as a ctrl/mem-chain PREDECESSOR (the builder advances
  // the control chain through New/NewArray/NewRefArray): harmless. The
  // splice (scalarize) or the pre-replace chain splice (materialize)
  // rewires these users onto the allocation's control predecessor.
  const InputRole role = roleOfSlot(g, user, slot);
  if (role == InputRole::Ctrl || role == InputRole::Mem) {
    return UseClass::ChainLink;
  }
  switch (nd.kind) {
  case NodeKind::LoadField:
    return slot == 2 ? UseClass::Reader : UseClass::UnknownReject;
  case NodeKind::StoreField:
    return slot == 2 ? UseClass::Writer : UseClass::FieldEscape;
  case NodeKind::StoreStatic:
    return slot == 2 ? UseClass::FieldEscape : UseClass::UnknownReject;
  case NodeKind::LoadElem:
    return slot == 2 ? UseClass::Reader : UseClass::UnknownReject;
  case NodeKind::StoreElem:
    if (slot == 2) {
      std::int32_t idx = 0;
      return intConstant(g, g.input(user, 3), idx) ? UseClass::Writer
                                                   : UseClass::DynamicIndex;
    }
    return slot == 4 ? UseClass::FieldEscape : UseClass::UnknownReject;
  case NodeKind::ArrayLength:
    return slot == 0 ? UseClass::LengthReader : UseClass::UnknownReject;
  case NodeKind::IsNull:
    return slot == 0 ? UseClass::IsNullUser : UseClass::UnknownReject;
  case NodeKind::CallStatic:
  case NodeKind::CallVirtual:
  case NodeKind::CallInterface:
  case NodeKind::CallDynamic:
    // The variadic Data slots are the arguments; the trailing
    // FrameState slot is a snapshot edge (alloc cannot legally be one,
    // but hand-built graphs get the conservative answer).
    return roleOfSlot(g, user, slot) == InputRole::Data ? UseClass::ArgEscape
                                                        : UseClass::UnknownReject;
  case NodeKind::Return:
    return slot == 1 ? UseClass::ReturnEscape : UseClass::UnknownReject;
  case NodeKind::Unwind:
    return slot == 1 ? UseClass::ExceptionReject : UseClass::UnknownReject;
  case NodeKind::RefEq:
    return UseClass::IdentityReject;
  case NodeKind::MonitorEnter:
  case NodeKind::MonitorExit:
    return slot == 2 ? UseClass::IdentityReject : UseClass::UnknownReject;
  case NodeKind::CheckCast:
    return slot == 1 ? UseClass::CastReject : UseClass::UnknownReject;
  case NodeKind::InstanceOf:
    return slot == 0 ? UseClass::InstanceOfReject
                     : UseClass::UnknownReject;
  case NodeKind::Phi:
    return UseClass::PhiReject;
  case NodeKind::FrameState:
    return UseClass::FsEscape;
  case NodeKind::VirtualObjectState:
    return UseClass::VobjHeld;
  default:
    return UseClass::UnknownReject;
  }
}

// True when the use class is PINNED (has a ctrl input) and participates
// in a chain-order-dependent rewrite - these are exactly the classes the
// straight-segment gate must check. Floating readers (ArrayLength,
// IsNull) are position-independent: their rewrites (length forwarding /
// never-null fold) cannot observe chain order, so they skip the gate.
// FsEscape follows the materialization point chosen by the other escape
// classes; rejected classes leave the graph alone.
[[nodiscard]] bool useRewrites(UseClass c) {
  switch (c) {
  case UseClass::Reader:
  case UseClass::Writer:
  case UseClass::ArgEscape:
  case UseClass::ReturnEscape:
  case UseClass::FieldEscape:
  case UseClass::DynamicIndex:
    return true;
  default:
    return false;
  }
}

// --- one allocation's analysis record ----------------------------------------

struct AllocRecord {
  NodeId alloc = ir::kInvalidNodeId;
  NodeKind kind = NodeKind::New;
  bool isArray = false;
  EscapeState state = EscapeState::NoEscape;
  const char* reason = "";
  NodeId escapeUse = ir::kInvalidNodeId; // first materializing use
  std::vector<ClassifiedUse> uses;       // in id order (Rule 124)
  std::uint32_t readers = 0;
  std::uint32_t writers = 0;
};

// Straight-segment gate: walks user's ctrl chain back toward the
// allocation's control point, collecting branch projections; bails on
// Region/LoopBegin (a merge can re-broadcast field state) or when the
// chain leaves the segment.
struct PathCheck {
  bool ok = true;
  const char* reason = "";
  std::vector<NodeId> projections; // If*/Switch projections, path order
};

[[nodiscard]] PathCheck pathToAllocCtrl(const ir::Graph& g, NodeId user,
                                        NodeId allocCtrl) {
  PathCheck pc;
  NodeId cur = ctrlInputOf(g, user);
  for (std::uint32_t steps = 0; steps < kPeaMaxChainSteps; ++steps) {
    if (cur == ir::kInvalidNodeId || cur >= g.nodeCount()) {
      pc.ok = false;
      pc.reason = "chain-unreachable";
      return pc;
    }
    if (cur == allocCtrl) {
      return pc; // reached the allocation's segment start
    }
    if (cur == g.startNode()) {
      pc.ok = false; // hit the entry before the allocation's ctrl point
      pc.reason = "chain-unreachable";
      return pc;
    }
    const NodeKind k = g.node(cur).kind;
    if (k == NodeKind::Region || k == NodeKind::LoopBegin) {
      pc.ok = false;
      pc.reason = "merge-crossed";
      return pc;
    }
    if (k == NodeKind::IfTrue || k == NodeKind::IfFalse ||
        k == NodeKind::SwitchCase || k == NodeKind::SwitchDefault) {
      pc.projections.push_back(cur);
      cur = g.input(cur, 0); // the PARENT slot: the If/Switch itself
      continue;
    }
    cur = ctrlInputOf(g, cur);
  }
  pc.ok = false;
  pc.reason = "chain-steps";
  return pc;
}

// Backward memory-chain lookup: the value of (alloc, fieldId) visible to
// a read whose mem chain starts at `mem`. `value` invalid when no
// same-field store precedes (caller uses the type default); `merged`
// true when the walk hits a Phi/Region (caller rejects the allocation).
struct FieldLookup {
  NodeId value = ir::kInvalidNodeId;
  bool merged = false;
};

[[nodiscard]] FieldLookup chainLookup(const ir::Graph& g, NodeId mem,
                                      NodeId alloc, bool array,
                                      std::uint32_t fieldId,
                                      std::int32_t index) {
  FieldLookup fl;
  NodeId cur = mem;
  for (std::uint32_t steps = 0; steps < kPeaMaxChainSteps; ++steps) {
    if (cur == ir::kInvalidNodeId || cur >= g.nodeCount()) {
      break;
    }
    const Node& nd = g.node(cur);
    if (nd.kind == NodeKind::Phi || nd.kind == NodeKind::Region ||
        nd.kind == NodeKind::LoopBegin) {
      fl.merged = true; // a merge re-broadcasts field state
      return fl;
    }
    if (!array && nd.kind == NodeKind::StoreField &&
        nd.payload == fieldId && g.input(cur, 2) == alloc) {
      fl.value = g.input(cur, 3);
      return fl;
    }
    if (array && nd.kind == NodeKind::StoreElem && g.input(cur, 2) == alloc) {
      std::int32_t idx = 0;
      if (intConstant(g, g.input(cur, 3), idx) && idx == index) {
        fl.value = g.input(cur, 4);
        return fl;
      }
      // Same array, other slot: keep walking.
    }
    cur = memInputOf(g, cur);
  }
  return fl; // no store found: the type default
}

} // namespace

// --- the chain splice ---------------------------------------------------------
//
// Removes a pinned node X from the ctrl/mem chains: X's Ctrl-role users
// rewire onto X's ctrl predecessor, Mem-role users onto its mem
// predecessor, Data users onto `dataReplacement` (loads only; stores
// have no value users). X's own edges are junked (the tombstone law)
// and X is killed. One budget charge per splice.
namespace {

void spliceOut(ir::Graph& g, NodeId x, NodeId dataReplacement,
               PassTelemetry& t, Budget& b, const Junk& jk) {
  if (x >= g.nodeCount() || g.node(x).isDead() || !b.charge()) {
    return;
  }
  NodeId ctrlPred = ir::kInvalidNodeId;
  NodeId memPred = ir::kInvalidNodeId;
  for (std::uint16_t s = 0; s < g.numInputs(x); ++s) {
    const InputRole r = roleOfSlot(g, x, s);
    if (r == InputRole::Ctrl && ctrlPred == ir::kInvalidNodeId) {
      ctrlPred = g.input(x, s);
    } else if (r == InputRole::Mem && memPred == ir::kInvalidNodeId) {
      memPred = g.input(x, s);
    }
  }
  if (ctrlPred == ir::kInvalidNodeId || ctrlPred >= g.nodeCount()) {
    ctrlPred = g.startNode();
  }
  if (memPred == ir::kInvalidNodeId || memPred >= g.nodeCount()) {
    memPred = g.startNode();
  }
  // Snapshot: setInput mutates the use list being iterated.
  std::vector<Use> snap(g.usesOf(x).begin(), g.usesOf(x).end());
  for (const Use& u : snap) {
    if (u.user >= g.nodeCount() || u.slot >= g.numInputs(u.user)) {
      continue;
    }
    if (g.input(u.user, u.slot) != x) {
      continue; // slot rewired since the entry was made
    }
    const InputRole r = roleOfSlot(g, u.user, u.slot);
    NodeId target = ir::kInvalidNodeId;
    if (r == InputRole::Ctrl) {
      target = ctrlPred;
    } else if (r == InputRole::Mem) {
      target = memPred;
    } else if (r == InputRole::Data && dataReplacement != ir::kInvalidNodeId) {
      target = dataReplacement;
    }
    if (target != ir::kInvalidNodeId) {
      g.setInput(u.user, u.slot, target);
    }
  }
  junkEdges(g, x, jk);
  g.killNode(x);
  ++t.removals;
}

// Splices the ALLOCATION itself out of the ctrl chain: its Ctrl-role
// users (the nodes the builder chained after New) rewire onto the
// allocation's ctrl predecessor. Data-role users are left for
// replaceNode (materialization) or the kill (scalarization). Called
// before replace/kill so no live user is ever wired onto a dead node.
void spliceAllocFromChain(ir::Graph& g, NodeId alloc) {
  NodeId ctrlPred = ctrlInputOf(g, alloc);
  if (ctrlPred == ir::kInvalidNodeId || ctrlPred >= g.nodeCount()) {
    ctrlPred = g.startNode();
  }
  std::vector<Use> snap(g.usesOf(alloc).begin(), g.usesOf(alloc).end());
  for (const Use& u : snap) {
    if (u.user >= g.nodeCount() || u.slot >= g.numInputs(u.user)) {
      continue;
    }
    if (g.input(u.user, u.slot) != alloc) {
      continue;
    }
    const InputRole r = roleOfSlot(g, u.user, u.slot);
    if (r == InputRole::Ctrl || r == InputRole::Mem) {
      g.setInput(u.user, u.slot, ctrlPred);
    }
  }
}

} // namespace

// --- materialization helpers (keys 66/69) -------------------------------------

namespace {

// Nested-vobj cascade NOTE: v1 never needs a cascade. A stored inner
// allocation is a foreign-store FieldEscape for the INNER allocation,
// and the inner allocation is visited first (the store's value edge
// depends on it - id order), so by the time the outer snapshot is built
// the field value is already a Materialize reference (or a rejected raw
// New). The verifier's still-virtual-field rule therefore holds by
// construction; the cascade machinery arrives with the phi-of-stores
// growth path.

// Wires the escape point `e` to run AFTER the materialization on both
// the ctrl and the mem chain, and rebases every remaining live user of
// the allocation onto the materialized reference.
void rewireEscapePoint(ir::Graph& g, NodeId e, NodeId m) {
  for (std::uint16_t s = 0; s < g.numInputs(e); ++s) {
    const InputRole r = roleOfSlot(g, e, s);
    if (r == InputRole::Ctrl) {
      g.setInput(e, s, m);
    } else if (r == InputRole::Mem) {
      g.setInput(e, s, m);
    }
  }
}

} // namespace

// --- the engine ----------------------------------------------------------------

void runPEA(ir::Graph& g, std::uint32_t stageMask, PassTelemetry& t,
            Budget& b, const Junk& jk,
            std::vector<PeaDecision>* decisions) {
  if ((stageMask & kPeaAnalyze) == 0) {
    return; // key 65 off: the whole engine is a no-op
  }
  const bool canMaterialize =
      (stageMask & kPeaPartial) != 0 && (stageMask & kPeaPlanning) != 0;
  const bool canScalarize = (stageMask & kPeaScalar) != 0;
  // Analysis-only (key 65 in isolation): classify and decide, execute
  // nothing. The decisions carry the theoretical disposition; the
  // telemetry counters stay zero (they count what actually happened).
  const bool analysisOnly = stageMask == kPeaAnalyze;

  // 1. Collect candidates in id order (Rule 124), budgeted.
  std::vector<NodeId> allocs;
  bool allocBudgetHit = false;
  for (NodeId n = 0; n < g.nodeCount(); ++n) {
    if (g.node(n).isDead() || !isAllocKind(g.node(n).kind)) {
      continue;
    }
    if (allocs.size() >= kPeaMaxAllocsPerGraph) {
      allocBudgetHit = true;
      break;
    }
    allocs.push_back(n);
  }

  for (NodeId alloc : allocs) {
    if (alloc >= g.nodeCount() || g.node(alloc).isDead()) {
      continue; // an earlier cascade consumed it as a vobj-held value
    }
    AllocRecord rec;
    rec.alloc = alloc;
    rec.kind = g.node(alloc).kind;
    rec.isArray = rec.kind != NodeKind::New;

    PeaDecision dec;
    dec.alloc = alloc;
    dec.kind = rec.kind;

    // 2. Classify the live uses in id order. Zero uses is a legal
    // record: a userless allocation scalarizes to nothing.
    for (const Use& u : g.usesOf(alloc)) {
      if (u.user >= g.nodeCount() || g.node(u.user).isDead()) {
        continue; // tombstone-law history is not a live use
      }
      ClassifiedUse cu;
      cu.user = u.user;
      cu.slot = u.slot;
      cu.cls = classifyUse(g, u.user, u.slot);
      rec.uses.push_back(cu);
    }
    std::sort(rec.uses.begin(), rec.uses.end(),
              [](const ClassifiedUse& a, const ClassifiedUse& rhs) {
                return a.user < rhs.user;
              });

    // 3. Lattice join (monotone: max over the use grades).
    EscapeState st = EscapeState::NoEscape;
    const char* reason = "";
    NodeId escapeUse = ir::kInvalidNodeId;
    bool hasFsUse = false;
    for (const ClassifiedUse& u : rec.uses) {
      switch (u.cls) {
      case UseClass::Reader:
      case UseClass::LengthReader:
      case UseClass::IsNullUser:
        ++rec.readers;
        break; // no escape contribution
      case UseClass::Writer:
        ++rec.writers;
        break;
      case UseClass::ChainLink:
        break; // the allocation as a chain predecessor: spliced away
      case UseClass::VobjHeld:
        if (st < EscapeState::LocalEscape) {
          st = EscapeState::LocalEscape;
          reason = "virtual-object-held";
        }
        break;
      case UseClass::ArgEscape:
        if (st < EscapeState::ArgEscape) {
          st = EscapeState::ArgEscape;
          reason = "call-arg";
        }
        break;
      case UseClass::DynamicIndex:
        if (st < EscapeState::ArgEscape) {
          st = EscapeState::ArgEscape;
          reason = "dynamic-index";
        }
        break;
      case UseClass::FieldEscape:
        if (st < EscapeState::FieldEscape) {
          st = EscapeState::FieldEscape;
          reason = "foreign-store";
        }
        break;
      case UseClass::ReturnEscape:
        if (st < EscapeState::ReturnEscape) {
          st = EscapeState::ReturnEscape;
          reason = "return-escape";
        }
        break;
      case UseClass::IdentityReject:
        st = EscapeState::GlobalEscape;
        reason = "identity-observable";
        break;
      case UseClass::ExceptionReject:
        st = EscapeState::GlobalEscape;
        reason = "exception-observable";
        break;
      case UseClass::FsEscape:
        // A deopt-snapshot reference FOLLOWS the other uses: on the
        // materialization path replaceNode(alloc, Materialize) rewires
        // the FrameState locals edge onto the real reference (deopt
        // reconstruction then sees a live object). fs-only references
        // (no other escape) keep the allocation virtual and LIST a
        // per-instant vobj on the snapshot instead (the fs-escape
        // block; MSG-20260901-006's appendFrameStateVobj).
        hasFsUse = true;
        break;
      case UseClass::PhiReject:
        st = EscapeState::GlobalEscape;
        reason = "phi-merge";
        break;
      case UseClass::CastReject:
        st = EscapeState::GlobalEscape;
        reason = "cast-observable";
        break;
      case UseClass::InstanceOfReject:
        st = EscapeState::GlobalEscape;
        reason = "instanceof-observable";
        break;
      case UseClass::UnknownReject:
        st = EscapeState::GlobalEscape;
        reason = "unknown-use";
        break;
      }
      if (escapeUse == ir::kInvalidNodeId &&
          (u.cls == UseClass::ArgEscape ||
           u.cls == UseClass::ReturnEscape ||
           u.cls == UseClass::FieldEscape ||
           u.cls == UseClass::DynamicIndex)) {
        escapeUse = u.user;
      }
    }
    if (hasFsUse && st == EscapeState::NoEscape) {
      // The escape-relevant uses are readers/writers plus deopt
      // snapshots: the fs-escape listing (the execution block) keeps
      // the allocation virtual and lists a per-instant vobj on every
      // referencing snapshot. Multi-instant refusals land in the plan
      // below; the reason carries the disposition for the log.
      reason = "fs-escape";
    }
    rec.state = st;
    rec.reason = reason;
    rec.escapeUse = escapeUse;

    // 4. Cost gates (Rule 45): field/index inventory per disposition.
    std::vector<std::uint32_t> fieldIds;    // instance, sorted unique
    std::vector<std::int32_t> elemIndices;  // array, sorted unique
    std::vector<std::uint32_t> readerTypes; // payload2 of LoadField rows
    for (const ClassifiedUse& u : rec.uses) {
      if (u.cls != UseClass::Reader && u.cls != UseClass::Writer) {
        continue;
      }
      const Node& un = g.node(u.user);
      if (un.kind == NodeKind::LoadField ||
          un.kind == NodeKind::StoreField) {
        if (std::find(fieldIds.begin(), fieldIds.end(), un.payload) ==
            fieldIds.end()) {
          fieldIds.push_back(un.payload);
        }
        if (un.kind == NodeKind::LoadField) {
          readerTypes.push_back(un.payload2);
        }
      } else if (un.kind == NodeKind::LoadElem ||
                 un.kind == NodeKind::StoreElem) {
        std::int32_t idx = 0;
        if (intConstant(g, g.input(u.user, 3), idx)) {
          if (std::find(elemIndices.begin(), elemIndices.end(), idx) ==
              elemIndices.end()) {
            elemIndices.push_back(idx);
          }
        }
      }
    }
    std::sort(fieldIds.begin(), fieldIds.end());
    std::sort(elemIndices.begin(), elemIndices.end());
    const std::uint32_t slotCount =
        rec.isArray ? static_cast<std::uint32_t>(elemIndices.size())
                    : static_cast<std::uint32_t>(fieldIds.size());
    if (rec.isArray && slotCount > kPeaMaxArrayElems) {
      rec.state = EscapeState::GlobalEscape;
      rec.reason = "too-many-elems";
    } else if (!rec.isArray && slotCount > kPeaMaxFields) {
      rec.state = EscapeState::GlobalEscape;
      rec.reason = "too-many-fields";
    }

    // 5. Straight-segment gate over the REWRITING uses only (equal
    // branch-projection sets; no Region/LoopBegin on any path; the
    // allocation's ctrl input must exist).
    const NodeId allocCtrl = ctrlInputOf(g, alloc);
    if (rec.state != EscapeState::GlobalEscape &&
        !rec.uses.empty()) {
      if (allocCtrl == ir::kInvalidNodeId || allocCtrl >= g.nodeCount()) {
        rec.state = EscapeState::GlobalEscape;
        rec.reason = "chain-unreachable";
      } else {
        std::vector<NodeId> refProjections;
        bool projectionsSet = false;
        for (const ClassifiedUse& u : rec.uses) {
          if (!useRewrites(u.cls)) {
            continue; // rejected/floating classes leave the graph alone
          }
          const PathCheck pc = pathToAllocCtrl(g, u.user, allocCtrl);
          if (!pc.ok) {
            rec.state = EscapeState::GlobalEscape;
            rec.reason = pc.reason;
            break;
          }
          if (!projectionsSet) {
            refProjections = pc.projections;
            projectionsSet = true;
          } else if (refProjections != pc.projections) {
            rec.state = EscapeState::GlobalEscape;
            rec.reason = "merge-crossed";
            break;
          }
        }
      }
    }

    // 6. Kill-switch downgrades (a disabled stage rejects rather than
    // half-applies - PEA Rule option 4). Analysis-only mode skips the
    // downgrades AND the execution: the decision records what WOULD
    // happen, the graph and the counters stay untouched.
    if (analysisOnly) {
      dec.action = rec.state == EscapeState::NoEscape
                       ? "scalarized"
                       : (rec.state == EscapeState::GlobalEscape ||
                              rec.state == EscapeState::LocalEscape
                          ? "rejected"
                          : "materialized");
      dec.reason = rec.reason;
      dec.state = rec.state;
      dec.fields = slotCount;
      if (decisions != nullptr) {
        decisions->push_back(dec);
      }
      continue;
    }
    if (rec.state == EscapeState::NoEscape && !canScalarize) {
      rec.state = EscapeState::GlobalEscape;
      rec.reason = "scalarize-disabled";
    } else if (rec.state != EscapeState::NoEscape &&
               rec.state != EscapeState::GlobalEscape &&
               rec.state != EscapeState::LocalEscape && !canMaterialize) {
      rec.state = EscapeState::GlobalEscape;
      rec.reason = "materialize-disabled";
    }

    // The per-disposition field type map: a field's default constant
    // type comes from its LoadField rows (payload2); writer-only fields
    // never need a default (they always have a store value, or they are
    // not in the pre-escape snapshot at all).
    auto fieldDefaultType = [&](std::uint32_t fid) {
      for (const ClassifiedUse& u : rec.uses) {
        if (u.cls == UseClass::Reader && u.user < g.nodeCount() &&
            g.node(u.user).kind == NodeKind::LoadField &&
            g.node(u.user).payload == fid) {
          return static_cast<IRType>(g.node(u.user).payload2);
        }
      }
      return IRType::Bottom;
    };

    // Shared reader forwarding. `onlyBefore` is the escape-point id
    // (materialization forwards only the pre-escape reads) or invalid
    // (scalarization forwards every read). All readers forward BEFORE
    // any writer is spliced - the chain is then still complete, so each
    // reader resolves to the store that defines its Java value.
    auto forwardReaders = [&](NodeId onlyBefore, PeaDecision& d,
                              bool& ok) {
      for (const ClassifiedUse& u : rec.uses) {
        if (!ok || u.cls != UseClass::Reader) {
          continue;
        }
        if (onlyBefore != ir::kInvalidNodeId && u.user >= onlyBefore) {
          break; // id order == chain order in one straight segment
        }
        const Node& un = g.node(u.user);
        NodeId value = ir::kInvalidNodeId;
        // The load's declared result type: LoadField carries it in
        // payload2, LoadElem in payload (the registry is asymmetric).
        const IRType loadTy =
            un.kind == NodeKind::LoadField
                ? static_cast<IRType>(un.payload2)
                : static_cast<IRType>(un.payload);
        if (un.kind == NodeKind::LoadField) {
          const FieldLookup fl = chainLookup(
              g, memInputOf(g, u.user), alloc, false, un.payload, 0);
          if (fl.merged) {
            ok = false;
            break;
          }
          value = fl.value != ir::kInvalidNodeId
                      ? fl.value
                      : defaultConstant(g, loadTy);
        } else { // LoadElem with a constant index (the gate guaranteed it)
          std::int32_t idx = 0;
          (void)intConstant(g, g.input(u.user, 3), idx);
          const FieldLookup fl =
              chainLookup(g, memInputOf(g, u.user), alloc, true, 0, idx);
          if (fl.merged) {
            ok = false;
            break;
          }
          value = fl.value != ir::kInvalidNodeId
                      ? fl.value
                      : defaultConstant(g, loadTy);
        }
        if (value == ir::kInvalidNodeId || value >= g.nodeCount() ||
            g.node(value).isDead()) {
          ok = false;
          break;
        }
        // Operand-type defense: the forwarded value must carry the
        // load's declared result type (hand-built graphs only).
        if (ir::resultTypeOf(g, value) != loadTy) {
          ok = false;
          break;
        }
        spliceOut(g, u.user, value, t, b, jk);
        ++d.loads;
      }
    };

    // Shared writer splice + floating folds (same onlyBefore rule).
    auto spliceWritersAndFold = [&](NodeId onlyBefore, PeaDecision& d) {
      for (const ClassifiedUse& u : rec.uses) {
        if (u.cls == UseClass::Writer) {
          if (onlyBefore != ir::kInvalidNodeId && u.user >= onlyBefore) {
            break; // post-escape stores write the materialized object
          }
          spliceOut(g, u.user, ir::kInvalidNodeId, t, b, jk);
          ++d.stores;
        }
      }
      for (const ClassifiedUse& u : rec.uses) {
        if (u.cls == UseClass::LengthReader &&
            (onlyBefore == ir::kInvalidNodeId || u.user < onlyBefore)) {
          if (rec.isArray) {
            // NewArray/NewRefArray carry the length as input 1 (input 0
            // is the ctrl); the ArrayLength node's own input IS the
            // array, not the length.
            const NodeId len = g.input(alloc, 1);
            if (len < g.nodeCount() && !g.node(len).isDead() &&
                b.charge()) {
              replace(g, u.user, len, t, b, jk);
            }
          }
        } else if (u.cls == UseClass::IsNullUser &&
                   (onlyBefore == ir::kInvalidNodeId ||
                    u.user < onlyBefore)) {
          if (b.charge()) {
            replace(g, u.user, g.constantI(0), t, b, jk);
          }
        }
      }
    };

    // --- the fs-escape listing (MSG-20260901-006 growth path) ---------------
    //
    // NoEscape + FrameState references: the allocation still scalarizes
    // (readers forward, writers splice), but the deoptimizer must be
    // able to materialize the object at every deopt point that
    // reconstructs a frame holding it. Every referencing fs resolves
    // ONE observation instant; a VirtualObjectState carries the field
    // state visible there (a backward chain lookup - NOT id order:
    // inlined-body stores carry higher ids than the call-site fss that
    // precede them in program order). The fs's slot edges rebase onto
    // the vobj and the desc lists it (the deopt-materialization
    // channel).
    //
    // IDENTITY: every fs of this allocation that lies inside ONE
    // deopt point's caller chain is reconstructed at the same instant
    // - the same Java object in (possibly) several frames. They must
    // share ONE vobj (union-find over the fss co-occurring in a live
    // chain), and every deopt point serving the cluster must observe
    // the SAME field state (the agreement check) - one vobj cannot
    // carry two states. Deopt-UNREACHABLE userless fss (the chain
    // below them was folded away) share one final-state vobj: dead
    // metadata, never reconstructed, any deterministic sound state.
    struct FsVobjVals {
      std::vector<NodeId> fields; // instance: parallel to the included
                                  // (sorted) field ids
      std::vector<NodeId> elems;  // array: dense [0..maxIncluded]
      NodeId len = ir::kInvalidNodeId;
    };
    struct FsPlan {
      NodeId fs = ir::kInvalidNodeId;
      std::size_t cluster = static_cast<std::size_t>(-1);
      std::vector<std::uint16_t> slots;
    };
    // The read-field / read-element sets (the inclusion rule: a field
    // resolved at the instant, or read somewhere, is listed).
    std::vector<std::uint32_t> readFields;
    std::vector<std::int32_t> readElems;
    for (const ClassifiedUse& u : rec.uses) {
      if (u.cls != UseClass::Reader) {
        continue;
      }
      const Node& un = g.node(u.user);
      if (un.kind == NodeKind::LoadField ||
          un.kind == NodeKind::StoreField) {
        if (std::find(readFields.begin(), readFields.end(), un.payload) ==
            readFields.end()) {
          readFields.push_back(un.payload);
        }
      } else {
        std::int32_t idx = 0;
        if (intConstant(g, g.input(u.user, 3), idx) &&
            std::find(readElems.begin(), readElems.end(), idx) ==
                readElems.end()) {
          readElems.push_back(idx);
        }
      }
    }
    // The per-plan default-constant cache: constantI/constantL/etc. do
    // not intern, so every fresh resolution of "the field default"
    // would create a NEW node - the agreement comparison (node
    // identity) would false-positive, and the graph would carry one
    // zero constant per vobj field. One node per IRType per plan.
    std::vector<NodeId> defCache;
    auto defOf = [&](IRType ty) -> NodeId {
      const auto idx = static_cast<std::uint32_t>(ty);
      if (defCache.size() <= idx) {
        defCache.resize(idx + 1, ir::kInvalidNodeId);
      }
      if (defCache[idx] == ir::kInvalidNodeId) {
        defCache[idx] = defaultConstant(g, ty);
      }
      return defCache[idx];
    };
    // Resolves the vobj field values visible at `mem` (pure). Returns
    // false on a merged chain (snapshot-merge) or an unresolvable
    // value; the caller refuses the allocation then.
    auto resolveFsVals = [&](NodeId mem, FsVobjVals& out) -> bool {
      if (!rec.isArray) {
        for (std::uint32_t fid : fieldIds) {
          const FieldLookup fl =
              chainLookup(g, mem, alloc, false, fid, 0);
          if (fl.merged) {
            return false;
          }
          NodeId v = ir::kInvalidNodeId;
          if (fl.value != ir::kInvalidNodeId) {
            v = fl.value;
          } else if (std::find(readFields.begin(), readFields.end(),
                               fid) != readFields.end()) {
            const IRType ty = fieldDefaultType(fid);
            v = ty == IRType::Bottom ? ir::kInvalidNodeId : defOf(ty);
          }
          if (v == ir::kInvalidNodeId || v >= g.nodeCount() ||
              g.node(v).isDead()) {
            return false;
          }
          out.fields.push_back(v);
        }
        return true;
      }
      const NodeId len = g.input(alloc, 1);
      if (len >= g.nodeCount() || g.node(len).isDead()) {
        return false;
      }
      out.len = len;
      std::int32_t maxIdx = -1;
      for (std::int32_t idx : elemIndices) {
        const FieldLookup fl =
            chainLookup(g, mem, alloc, true, 0, idx);
        if (fl.merged) {
          return false;
        }
        if (fl.value != ir::kInvalidNodeId ||
            std::find(readElems.begin(), readElems.end(), idx) !=
                readElems.end()) {
          maxIdx = std::max(maxIdx, idx);
        }
      }
      out.elems.resize(static_cast<std::size_t>(maxIdx) + 1,
                       ir::kInvalidNodeId);
      for (std::int32_t idx : elemIndices) {
        if (static_cast<std::size_t>(idx) >= out.elems.size()) {
          continue; // not included at this instant
        }
        const FieldLookup fl =
            chainLookup(g, mem, alloc, true, 0, idx);
        NodeId v = ir::kInvalidNodeId;
        if (fl.value != ir::kInvalidNodeId) {
          v = fl.value;
        } else if (std::find(readElems.begin(), readElems.end(), idx) !=
                   readElems.end()) {
          v = defOf(elemTyOf(g, alloc, rec.kind));
        }
        if (v == ir::kInvalidNodeId || v >= g.nodeCount() ||
            g.node(v).isDead()) {
          return false;
        }
        out.elems[static_cast<std::size_t>(idx)] = v;
      }
      // Unlisted element slots materialize to their type default.
      const NodeId dflt = defOf(elemTyOf(g, alloc, rec.kind));
      if (dflt == ir::kInvalidNodeId) {
        return false;
      }
      for (std::size_t i = 0; i < out.elems.size(); ++i) {
        if (out.elems[i] == ir::kInvalidNodeId) {
          out.elems[i] = dflt;
        }
      }
      return true;
    };
    // The observation-instant plan (pure). Returns the refusal reason
    // or nullptr. The plan phase leaves the graph untouched; a refusal
    // means the allocation stays real (PEA Rule option 4).
    std::vector<FsPlan> fsPlans;
    struct FsCluster {
      NodeId key = ir::kInvalidNodeId; // the min member fs id (Rule 124)
      NodeId instantMem = ir::kInvalidNodeId;
      bool deadMeta = false;
    };
    std::vector<FsCluster> fsClusters;
    std::vector<FsVobjVals> fsVals; // parallel to fsClusters
    auto planFsEscape = [&]() -> const char* {
      // 1. The referencing fss in id order (Rule 124), grouped with
      //    their slots.
      for (const ClassifiedUse& u : rec.uses) {
        if (u.cls != UseClass::FsEscape || u.user >= g.nodeCount() ||
            g.node(u.user).isDead()) {
          continue;
        }
        FsPlan p;
        p.fs = u.user;
        auto it = std::find_if(fsPlans.begin(), fsPlans.end(),
                               [&](const FsPlan& q) { return q.fs == p.fs; });
        if (it != fsPlans.end()) {
          it->slots.push_back(u.slot);
          continue;
        }
        p.slots.push_back(u.slot);
        fsPlans.push_back(p);
      }
      if (fsPlans.empty()) {
        return nullptr; // unreachable: hasFsUse guards the call
      }
      const std::size_t n = fsPlans.size();
      // 2. The live deopt points (the chain heads that can reconstruct
      //    frames holding this allocation).
      struct DeoptPoint {
        NodeId node;
        NodeId head;
      };
      std::vector<DeoptPoint> dpoints;
      for (NodeId d = 0; d < g.nodeCount(); ++d) {
        if (g.node(d).isDead() || !isDeoptPoint(g.node(d).kind)) {
          continue;
        }
        const NodeId h = fsInputOf(g, d);
        if (h != ir::kInvalidNodeId) {
          dpoints.push_back(DeoptPoint{d, h});
        }
      }
      // 3. Per-fs: the live deopt consumers (a foreign consumer kind
      //    refuses - the fs is not a deopt snapshot then).
      std::vector<std::vector<NodeId>> own(n);
      for (std::size_t i = 0; i < n; ++i) {
        for (const Use& u : g.usesOf(fsPlans[i].fs)) {
          if (u.user >= g.nodeCount() || g.node(u.user).isDead()) {
            continue;
          }
          if (isDeoptPoint(g.node(u.user).kind)) {
            own[i].push_back(u.user);
          } else {
            return "fs-unknown-consumer";
          }
        }
        std::sort(own[i].begin(), own[i].end());
      }
      // 4. The identity union: fss of this allocation co-occurring in
      //    one live caller chain share one vobj. sources[i] collects
      //    the deopt points whose chain REACHES fs i (as an ancestor);
      //    a chain's head is reached through its own consumers.
      std::vector<std::uint32_t> parent(n);
      for (std::size_t i = 0; i < n; ++i) {
        parent[i] = static_cast<std::uint32_t>(i);
      }
      auto find = [&](std::uint32_t x) {
        while (parent[x] != x) {
          parent[x] = parent[parent[x]];
          x = parent[x];
        }
        return x;
      };
      auto unite = [&](std::uint32_t x, std::uint32_t y) {
        const std::uint32_t rx = find(x);
        const std::uint32_t ry = find(y);
        if (rx == ry) {
          return;
        }
        if (rx < ry) {
          parent[ry] = rx; // union by min id: deterministic roots
        } else {
          parent[rx] = ry;
        }
      };
      std::vector<std::vector<NodeId>> sources(n);
      for (const DeoptPoint& dp : dpoints) {
        // The members of THIS allocation referenced inside dp's chain.
        std::vector<std::uint32_t> hits;
        const FrameStateId headDesc = g.node(dp.head).payload;
        for (std::size_t i = 0; i < n; ++i) {
          const FrameStateId d = g.node(fsPlans[i].fs).payload;
          if (d >= g.frameStateCount()) {
            continue;
          }
          if (d == headDesc) {
            hits.push_back(static_cast<std::uint32_t>(i)); // position 0
            continue;
          }
          FrameStateId cur = headDesc;
          std::uint32_t steps = 0;
          while (cur != ir::kInvalidFrameState &&
                 cur < g.frameStateCount() &&
                 steps++ <= g.frameStateCount()) {
            cur = g.frameState(cur).caller;
            if (cur == d) {
              hits.push_back(static_cast<std::uint32_t>(i));
              sources[i].push_back(dp.node);
              break;
            }
          }
        }
        for (std::size_t k = 1; k < hits.size(); ++k) {
          unite(hits[0], hits[k]);
        }
      }
      // 5. One instant per cluster: every serving deopt point must
      //    observe the same field state.
      for (std::size_t i = 0; i < n; ++i) {
        for (NodeId d : own[i]) {
          sources[i].push_back(d);
        }
        std::sort(sources[i].begin(), sources[i].end());
        sources[i].erase(std::unique(sources[i].begin(), sources[i].end()),
                         sources[i].end());
      }
      // The final-state anchor for dead metadata: the last writer.
      NodeId lastWriter = ir::kInvalidNodeId;
      for (const ClassifiedUse& u : rec.uses) {
        if (u.cls == UseClass::Writer && u.user > lastWriter &&
            u.user < g.nodeCount() && !g.node(u.user).isDead()) {
          lastWriter = u.user;
        }
      }
      // 6. Assemble the clusters: one per root (min fs id as the key),
      //    all deopt-unreachable roots merged into ONE dead-meta
      //    cluster (the shared final-state vobj). Real clusters first
      //    (key ascending), the dead-meta cluster last.
      std::vector<std::vector<std::size_t>> members(n);
      for (std::size_t i = 0; i < n; ++i) {
        members[find(static_cast<std::uint32_t>(i))].push_back(i);
      }
      for (std::size_t root = 0; root < n; ++root) {
        if (members[root].empty()) {
          continue;
        }
        bool dead = true;
        for (std::size_t i : members[root]) {
          if (!sources[i].empty()) {
            dead = false;
            break;
          }
        }
        FsCluster c;
        c.deadMeta = dead;
        c.key = fsPlans[members[root][0]].fs; // id order: the min
        c.instantMem = dead ? lastWriter : ir::kInvalidNodeId;
        if (!dead) {
          const std::vector<NodeId>& src = sources[members[root][0]];
          c.instantMem = deoptInstantMem(g, src[0]);
        }
        // Attach every member plan.
        for (std::size_t i : members[root]) {
          fsPlans[i].cluster = fsClusters.size();
        }
        fsClusters.push_back(c);
      }
      // Merge the dead-meta clusters into one (all share the vobj).
      std::size_t deadIdx = static_cast<std::size_t>(-1);
      for (std::size_t ci = 0; ci < fsClusters.size(); ++ci) {
        if (!fsClusters[ci].deadMeta) {
          continue;
        }
        if (deadIdx == static_cast<std::size_t>(-1)) {
          deadIdx = ci;
          continue;
        }
        for (FsPlan& p : fsPlans) {
          if (p.cluster == ci) {
            p.cluster = deadIdx;
          }
        }
      }
      if (deadIdx != static_cast<std::size_t>(-1)) {
        // Drop the merged dead-meta duplicates.
        std::vector<FsCluster> keep;
        for (std::size_t ci = 0; ci < fsClusters.size(); ++ci) {
          if (fsClusters[ci].deadMeta && ci != deadIdx) {
            continue;
          }
          keep.push_back(fsClusters[ci]);
        }
        // Reindex the plans (offsets shift by the dropped count).
        std::vector<std::size_t> map(fsClusters.size(), 0);
        std::size_t w = 0;
        for (std::size_t ci = 0; ci < fsClusters.size(); ++ci) {
          if (fsClusters[ci].deadMeta && ci != deadIdx) {
            map[ci] = deadIdx < ci ? deadIdx : deadIdx - 1;
            continue;
          }
          map[ci] = w++;
        }
        for (FsPlan& p : fsPlans) {
          p.cluster = map[p.cluster];
        }
        fsClusters = std::move(keep);
      }
      // 7. Resolve the agreed field state per cluster (and enforce the
      //    agreement: one vobj, one state).
      for (std::size_t ci = 0; ci < fsClusters.size(); ++ci) {
        FsCluster& c = fsClusters[ci];
        FsVobjVals vals;
        if (!resolveFsVals(c.instantMem, vals)) {
          return "snapshot-merge";
        }
        if (!c.deadMeta) {
          for (std::size_t i = 0; i < n; ++i) {
            if (fsPlans[i].cluster != ci || sources[i].empty()) {
              continue;
            }
            for (std::size_t k = c.key == fsPlans[i].fs ? 1 : 0;
                 k < sources[i].size(); ++k) {
              FsVobjVals other;
              if (!resolveFsVals(deoptInstantMem(g, sources[i][k]),
                                 other)) {
                return "snapshot-merge";
              }
              if (other.fields != vals.fields ||
                  other.elems != vals.elems || other.len != vals.len) {
                return "fs-multi-deopt";
              }
            }
          }
        }
        fsVals.push_back(std::move(vals));
      }
      return nullptr;
    };
    // Executes a successful plan: create the vobjs (cluster order),
    // rebase the slot edges, list the vobjs on the descs. No failure
    // paths (the plan validated everything; the defensive
    // kill-refusal below is the partial-bail belt).
    auto applyFsEscape = [&]() {
      std::vector<NodeId> vobjs; // parallel to fsClusters
      for (std::size_t i = 0; i < fsClusters.size(); ++i) {
        const FsVobjVals& v = fsVals[i];
        NodeId vo = ir::kInvalidNodeId;
        if (!rec.isArray) {
          vo = g.makeVirtualObject(
              ir::TypeId{g.node(alloc).payload},
              std::span<const NodeId>(v.fields.data(), v.fields.size()));
        } else {
          vo = g.makeArrayVirtualObject(
              elemTyOf(g, alloc, rec.kind), v.len,
              std::span<const NodeId>(v.elems.data(), v.elems.size()));
        }
        vobjs.push_back(vo);
      }
      for (const FsPlan& p : fsPlans) {
        if (p.cluster >= vobjs.size()) {
          continue; // unreachable: the plan assigned every fs
        }
        const NodeId vo = vobjs[p.cluster];
        for (std::uint16_t slot : p.slots) {
          g.setInput(p.fs, slot, vo);
        }
        const FrameStateId desc = g.node(p.fs).payload;
        if (desc < g.frameStateCount()) {
          g.appendFrameStateVobj(desc, vo);
        }
      }
      ++t.rewrites;
    };

    if (rec.state == EscapeState::NoEscape) {
      // --- scalar replacement (key 67) --------------------------------------
      bool ok = true;
      const char* fsRefusal = nullptr;
      if (hasFsUse) {
        fsRefusal = planFsEscape();
        if (fsRefusal == nullptr) {
          applyFsEscape();
        }
      }
      if (fsRefusal == nullptr) {
        forwardReaders(ir::kInvalidNodeId, dec, ok);
        spliceWritersAndFold(ir::kInvalidNodeId, dec);
        if (ok) {
          spliceAllocFromChain(g, alloc);
        }
        if (ok && kill(g, alloc, t, b, jk)) {
          dec.action = "scalarized";
          dec.reason = hasFsUse ? "fs-escape" : "no-escape";
          ++t.peaScalarized;
        } else {
          // Defensive bail (kill refused or a lookup failed): every landed
          // rewrite is individually sound
          // (a forwarded load / removed virtual store / folded IsNull /
          // listed vobj never publishes the object); the allocation
          // stays real for the remaining uses.
          dec.action = "rejected";
          dec.reason = "partial-bail";
          ++t.peaRejected;
        }
      } else {
        // The plan refused: the graph is untouched (the plan phase is
        // pure), the allocation stays real (PEA Rule option 4).
        dec.action = "rejected";
        dec.reason = fsRefusal;
        ++t.peaRejected;
      }
    } else if (rec.state == EscapeState::ArgEscape ||
               rec.state == EscapeState::FieldEscape ||
               rec.state == EscapeState::ReturnEscape) {
      // --- partial escape: materialize at the FIRST escape use (66/69) ---
      const NodeId e = rec.escapeUse;
      if (e == ir::kInvalidNodeId || e >= g.nodeCount() ||
          g.node(e).isDead()) {
        dec.action = "rejected";
        dec.reason = "escape-use-lost";
        ++t.peaRejected;
      } else {
        // The pre-splice chains anchor the snapshot lookups.
        const NodeId eMem0 = memInputOf(g, e);

        // (a) forward the pre-escape reads (chain still complete),
        // (b) snapshot the object state at the escape point,
        // (c) THEN splice the pre-escape writers,
        // (d) re-read the escape point's (rewired) chain predecessors
        //     and insert the Materialize before it on both chains.
        bool ok = true;
        forwardReaders(e, dec, ok);

        NodeId vobj = ir::kInvalidNodeId;
        if (ok) {
          const NodeId snapMem = (eMem0 != ir::kInvalidNodeId &&
                                  eMem0 < g.nodeCount())
                                     ? eMem0
                                     : g.startNode();
          if (!rec.isArray) {
            // Snapshot fields: every READ field plus every WRITTEN field
            // whose store precedes the escape point. Fields written only
            // after the escape stay absent (the fresh object's zero
            // default, then the real store writes it).
            std::vector<std::uint32_t> snapFields;
            for (std::uint32_t fid : fieldIds) {
              bool read = false;
              bool writtenPre = false;
              for (const ClassifiedUse& u : rec.uses) {
                const NodeKind uk = g.node(u.user).kind;
                if ((uk != NodeKind::LoadField &&
                     uk != NodeKind::StoreField) ||
                    g.node(u.user).payload != fid) {
                  continue;
                }
                if (u.cls == UseClass::Reader) {
                  read = true;
                } else if (u.cls == UseClass::Writer && u.user < e) {
                  writtenPre = true;
                }
              }
              if (read || writtenPre) {
                snapFields.push_back(fid);
              }
            }
            std::vector<NodeId> fields;
            for (std::uint32_t fid : snapFields) {
              const FieldLookup fl =
                  chainLookup(g, snapMem, alloc, false, fid, 0);
              NodeId v = ir::kInvalidNodeId;
              if (fl.merged) {
                ok = false;
                break;
              }
              if (fl.value != ir::kInvalidNodeId) {
                v = fl.value;
              } else {
                const IRType ty = fieldDefaultType(fid);
                v = ty == IRType::Bottom ? ir::kInvalidNodeId
                                         : defaultConstant(g, ty);
              }
              if (v == ir::kInvalidNodeId || v >= g.nodeCount() ||
                  g.node(v).isDead()) {
                ok = false;
                break;
              }
              fields.push_back(v);
            }
            if (ok) {
              vobj = g.makeVirtualObject(
                  ir::TypeId{g.node(alloc).payload},
                  std::span<const NodeId>(fields));
            }
          } else {
            std::vector<std::int32_t> snapElems;
            for (std::int32_t idx : elemIndices) {
              bool read = false;
              bool writtenPre = false;
              for (const ClassifiedUse& u : rec.uses) {
                const NodeKind uk = g.node(u.user).kind;
                if (uk != NodeKind::LoadElem && uk != NodeKind::StoreElem) {
                  continue;
                }
                std::int32_t uIdx = 0;
                if (!intConstant(g, g.input(u.user, 3), uIdx) ||
                    uIdx != idx) {
                  continue;
                }
                if (u.cls == UseClass::Reader) {
                  read = true;
                } else if (u.cls == UseClass::Writer && u.user < e) {
                  writtenPre = true;
                }
              }
              if (read || writtenPre) {
                snapElems.push_back(idx);
              }
            }
            NodeId len = g.input(alloc, 1);
            if (len >= g.nodeCount() || g.node(len).isDead()) {
              ok = false;
            }
            std::int32_t maxIdx = -1;
            for (std::int32_t idx : snapElems) {
              maxIdx = std::max(maxIdx, idx);
            }
            std::vector<NodeId> elems;
            if (ok && maxIdx >= 0) {
              elems.resize(static_cast<std::size_t>(maxIdx) + 1,
                           ir::kInvalidNodeId);
            }
            const IRType elemTy =
                rec.kind == NodeKind::NewArray
                    ? static_cast<IRType>(g.node(alloc).payload)
                    : IRType::Ref;
            if (ok) {
              for (std::int32_t idx : snapElems) {
                const FieldLookup fl =
                    chainLookup(g, snapMem, alloc, true, 0, idx);
                NodeId v = ir::kInvalidNodeId;
                if (fl.merged) {
                  ok = false;
                  break;
                }
                if (fl.value != ir::kInvalidNodeId) {
                  v = fl.value;
                } else {
                  const NodeId dflt = defaultConstant(g, elemTy);
                  v = dflt;
                }
                if (v == ir::kInvalidNodeId || v >= g.nodeCount() ||
                    g.node(v).isDead()) {
                  ok = false;
                  break;
                }
                elems[static_cast<std::size_t>(idx)] = v;
              }
            }
            if (ok) {
              // Unlisted element slots materialize to their type default
              // (the fresh array's zero fill).
              for (std::size_t i = 0; i < elems.size(); ++i) {
                if (elems[i] == ir::kInvalidNodeId) {
                  elems[i] = defaultConstant(g, elemTy);
                }
              }
              vobj = g.makeArrayVirtualObject(
                  elemTy, len, std::span<const NodeId>(elems));
            }
          }
        }

        if (ok) {
          spliceWritersAndFold(e, dec);
        }

        if (ok && vobj != ir::kInvalidNodeId) {
          // (d) the escape point's post-splice chain predecessors.
          NodeId eCtrl = ctrlInputOf(g, e);
          if (eCtrl == ir::kInvalidNodeId || eCtrl >= g.nodeCount()) {
            eCtrl = g.startNode();
          }
          NodeId eMem = memInputOf(g, e);
          if (eMem == ir::kInvalidNodeId || eMem >= g.nodeCount() ||
              g.node(eMem).isDead()) {
            eMem = memStateBefore(g, eCtrl);
          }
          const NodeId m = g.make(NodeKind::Materialize, {eCtrl, eMem,
                                                           vobj});
          ++t.rewrites;
          rewireEscapePoint(g, e, m);
          spliceAllocFromChain(g, alloc);
          replace(g, alloc, m, t, b, jk);
          dec.action = "materialized";
          dec.reason = rec.reason;
          dec.materializeAt = m;
          ++t.peaMaterialized;
        } else {
          dec.action = "rejected";
          dec.reason = ok ? "snapshot-merge" : "partial-bail";
          ++t.peaRejected;
        }
      }
    } else {
      // --- rejected (PEA Rule option 4): the graph is untouched ----------
      dec.action = "rejected";
      dec.reason = rec.reason;
      ++t.peaRejected;
    }

    dec.state = rec.state;
    dec.fields = slotCount;
    if (decisions != nullptr) {
      decisions->push_back(dec);
    }
    if (!b.charge()) {
      break; // budget spent: stop at a valid, verified point
    }
  }

  if (allocBudgetHit) {
    ++t.budgetOverruns; // the remaining allocations are untouched
  }
}

} // namespace b2::passes::detail
