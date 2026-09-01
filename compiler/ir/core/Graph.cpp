// B-2 IR - Graph implementation: arena storage, edge pool, use-def chains,
// epoch-tagged replacement (Rules 7, 14, 15, 19).
//
// WHY THIS FILE EXISTS:
// The Graph is the T2/T3 core container (Amendment B.1). All mutation flows
// through three primitives - make / setInput+appendInput / replaceNode+kill -
// and every one of them maintains the def-use invariant: an edge (user, slot)
// exists in usesOf(def) iff user.input(slot) == def. The tests pin that
// invariant under every mutation. Node ids are creation order and stable for
// the graph's lifetime (tombstones stay; Rule 124's snapshot determinism).

#include "b2/ir/Graph.h"

#include <cstring>
#include <span>
#include <utility>

namespace b2::ir {

Graph::Graph() {
  // Node 0 is always Start (the control + initial-memory origin).
  start_ = makeRaw(NodeKind::Start, {}, 0, 0);
}

Graph::~Graph() = default;

const Node& Graph::node(NodeId n) const noexcept {
  if (n < nodes_.size()) {
    return nodes_[n];
  }
  static const Node bad{}; // deterministic OOB sink (never UB)
  return bad;
}

Node& Graph::nodeForReplay(NodeId n) noexcept {
  if (n < nodes_.size()) {
    return nodes_[n];
  }
  // Unreachable from the deserializer (it checks ids first); keep a safe
  // sink anyway so the replay path can never index out of bounds.
  static Node sink{};
  return sink;
}

NodeId Graph::makeRaw(NodeKind kind, std::span<const NodeId> inputs,
                      std::uint32_t payload, std::uint32_t payload2,
                      std::int64_t constValue) {
  const NodeId id = static_cast<NodeId>(nodes_.size());
  Node n;
  n.kind = kind;
  n.edgeOffset = static_cast<std::uint32_t>(edgePool_.size());
  n.numInputs = static_cast<std::uint16_t>(inputs.size());
  n.payload = payload;
  n.payload2 = payload2;
  n.constValue = constValue;
  for (NodeId in : inputs) {
    edgePool_.push_back(in);
  }
  nodes_.push_back(n);
  uses_.emplace_back(&arena_);
  for (std::uint16_t s = 0; s < n.numInputs; ++s) {
    if (inputs[s] < nodes_.size()) {
      uses_[inputs[s]].push_back(Use{id, s});
    }
    // Out-of-range input ids are stored verbatim; the verifier rejects the
    // graph (total behavior, same discipline as the RBC builder).
  }
  return id;
}

NodeId Graph::make(NodeKind kind, std::span<const NodeId> inputs,
                   std::uint32_t payload, std::uint32_t payload2) {
  return makeRaw(kind, inputs, payload, payload2);
}

NodeId Graph::make(NodeKind kind, std::initializer_list<NodeId> inputs,
                   std::uint32_t payload, std::uint32_t payload2) {
  return makeRaw(kind, std::span<const NodeId>(inputs.begin(), inputs.size()),
                 payload, payload2);
}

NodeId Graph::constantI(std::int32_t v) {
  return makeRaw(NodeKind::ConstantI, {}, 0, 0,
                 static_cast<std::int64_t>(v));
}

NodeId Graph::constantL(std::int64_t v) {
  return makeRaw(NodeKind::ConstantL, {}, 0, 0, v);
}

NodeId Graph::constantF(float v) {
  std::uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(v));
  std::memcpy(&bits, &v, sizeof(bits));
  return makeRaw(NodeKind::ConstantF, {}, 0, 0,
                 static_cast<std::int64_t>(bits));
}

NodeId Graph::constantD(double v) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(v));
  std::memcpy(&bits, &v, sizeof(bits));
  return makeRaw(NodeKind::ConstantD, {}, 0, 0,
                 static_cast<std::int64_t>(bits));
}

NodeId Graph::constantNull() {
  return makeRaw(NodeKind::ConstantNull, {}, 0, 0, 0);
}

NodeId Graph::constantSym(SymbolId sym) {
  return makeRaw(NodeKind::ConstantSym, {}, sym, 0, 0);
}

NodeId Graph::parameter(std::uint16_t index, IRType type) {
  return makeRaw(NodeKind::Parameter, {}, index,
                 static_cast<std::uint32_t>(type), 0);
}

FrameStateId Graph::addFrameStateDesc(MethodId method, std::uint32_t pc,
                                      FrameStateId caller,
                                      std::span<const VirtualObjectId> vobjs) {
  FrameStateDesc d;
  d.method = method;
  d.pc = pc;
  d.caller = caller;
  d.vobjOffset = static_cast<std::uint32_t>(fsVobjs_.size());
  d.vobjCount = static_cast<std::uint32_t>(vobjs.size());
  for (VirtualObjectId v : vobjs) {
    fsVobjs_.push_back(v);
  }
  fsDesc_.push_back(d);
  return static_cast<FrameStateId>(fsDesc_.size() - 1);
}

// WHY: fsVobjs_ is a flat vector; each FrameStateDesc owns a [offset, +count)
// slice. Appending to an existing desc's slice means a mid-vector insert, which
// shifts every later element by one — so every later desc's vobjOffset must
// shift by one too, or its slice would point at the wrong ids. fs's own offset
// is unchanged (the new element lands at the END of its slice); only its count
// grows. Determinism (Rule 124): the insert position is a pure function of fs's
// current (offset, count); no reordering; desc ids are stable.
void Graph::appendFrameStateVobj(FrameStateId fs, VirtualObjectId vobj) {
  if (fs >= fsDesc_.size()) {
    return; // out-of-range: no-op (verifier rejects bogus desc ids)
  }
  FrameStateDesc& d = fsDesc_[fs];
  const std::uint32_t insertPos = d.vobjOffset + d.vobjCount;
  fsVobjs_.insert(fsVobjs_.begin() + insertPos, vobj);
  ++d.vobjCount;
  // Shift every desc whose slice starts at or after the insert point. fs
  // itself is skipped: its offset is unchanged, only its count grew.
  for (FrameStateId i = 0; i < fsDesc_.size(); ++i) {
    if (i == fs) {
      continue;
    }
    if (fsDesc_[i].vobjOffset >= insertPos) {
      ++fsDesc_[i].vobjOffset;
    }
  }
}

NodeId Graph::makeFrameState(MethodId method, std::uint32_t pc,
                             std::span<const NodeId> locals,
                             FrameStateId caller,
                             std::span<const VirtualObjectId> vobjs) {
  const FrameStateId desc =
      addFrameStateDesc(method, pc, caller, vobjs);
  return makeRaw(NodeKind::FrameState, locals,
                 static_cast<std::uint32_t>(desc), 0, 0);
}

NodeId Graph::makeFrameState(MethodId method, std::uint32_t pc,
                             std::initializer_list<NodeId> locals,
                             FrameStateId caller,
                             std::initializer_list<VirtualObjectId> vobjs) {
  return makeFrameState(method, pc,
                        std::span<const NodeId>(locals.begin(), locals.size()),
                        caller,
                        std::span<const VirtualObjectId>(vobjs.begin(),
                                                         vobjs.size()));
}

NodeId Graph::makeVirtualObject(TypeId classId,
                                std::span<const NodeId> fieldValues) {
  return makeRaw(NodeKind::VirtualObjectState, fieldValues, classId,
                 /*isArray=*/0, 0);
}

NodeId Graph::makeVirtualObject(TypeId classId,
                                std::initializer_list<NodeId> fieldValues) {
  return makeRaw(NodeKind::VirtualObjectState,
                 std::span<const NodeId>(fieldValues.begin(),
                                         fieldValues.size()),
                 classId, 0, 0);
}

NodeId Graph::makeArrayVirtualObject(IRType elemType, NodeId length,
                                     std::span<const NodeId> elements) {
  // Array layout: length first, then elements.
  std::pmr::vector<NodeId> all(&arena_);
  all.push_back(length);
  for (NodeId e : elements) {
    all.push_back(e);
  }
  return makeRaw(NodeKind::VirtualObjectState,
                 std::span<const NodeId>(all.data(), all.size()),
                 static_cast<std::uint32_t>(elemType), /*isArray=*/1, 0);
}

NodeId Graph::makeArrayVirtualObject(IRType elemType, NodeId length,
                                     std::initializer_list<NodeId> elements) {
  return makeArrayVirtualObject(elemType, length,
                                std::span<const NodeId>(elements.begin(),
                                                        elements.size()));
}

SpecMetaId Graph::addSpecMeta(const SpecMeta& sm) {
  specMetas_.push_back(sm);
  return static_cast<SpecMetaId>(specMetas_.size() - 1);
}

DependencyId Graph::addDependency(const Dependency& dep) {
  deps_.push_back(dep);
  return static_cast<DependencyId>(deps_.size() - 1);
}

void Graph::attachSpecMeta(NodeId n, SpecMetaId id) {
  if (n >= nodes_.size()) {
    return; // trusted-path discipline: verifier catches the dangling case
  }
  nodes_[n].specMeta = id + 1; // 0 = none, so store id+1
  nodes_[n].flags.set(NodeFlag::Speculative);
}

std::uint16_t Graph::numInputs(NodeId n) const {
  return node(n).numInputs;
}

NodeId Graph::input(NodeId n, std::uint16_t slot) const {
  const Node& nd = node(n);
  if (slot < nd.numInputs) {
    return edgePool_[nd.edgeOffset + slot];
  }
  return kInvalidNodeId;
}

void Graph::removeFromUses(NodeId def, NodeId user, std::uint16_t slot) {
  if (def >= uses_.size()) {
    return;
  }
  (void)uses_[def].remove(Use{user, slot});
}

void Graph::setInput(NodeId n, std::uint16_t slot, NodeId v) {
  if (n >= nodes_.size()) {
    return;
  }
  Node& nd = nodes_[n];
  if (slot >= nd.numInputs) {
    return;
  }
  const NodeId old = edgePool_[nd.edgeOffset + slot];
  if (old == v) {
    return;
  }
  edgePool_[nd.edgeOffset + slot] = v;
  removeFromUses(old, n, slot);
  if (v < nodes_.size()) {
    uses_[v].push_back(Use{n, slot});
  }
}

void Graph::appendInput(NodeId n, NodeId v) {
  if (n >= nodes_.size()) {
    return;
  }
  Node& nd = nodes_[n];
  // Relocate the node's edge slice to the pool tail (arena discipline: the
  // old slice is abandoned, not freed).
  const std::uint32_t newOffset =
      static_cast<std::uint32_t>(edgePool_.size());
  for (std::uint16_t s = 0; s < nd.numInputs; ++s) {
    edgePool_.push_back(edgePool_[nd.edgeOffset + s]);
  }
  edgePool_.push_back(v);
  nd.edgeOffset = newOffset;
  ++nd.numInputs;
  if (v < nodes_.size()) {
    uses_[v].push_back(Use{n, static_cast<std::uint16_t>(nd.numInputs - 1)});
  }
}

const SmallVector<Use, 3>& Graph::usesOf(NodeId n) const {
  if (n < uses_.size()) {
    return uses_[n];
  }
  static const SmallVector<Use, 3> empty;
  return empty;
}

std::uint32_t Graph::replaceNode(NodeId oldNode, NodeId withNode) {
  if (oldNode >= nodes_.size() || withNode >= nodes_.size()) {
    return 0;
  }
  if (nodes_[withNode].isDead()) {
    return 0; // refuse to rewire onto a tombstone
  }
  if (oldNode == withNode) {
    return 0;
  }
  // Snapshot the use list: rewiring mutates it.
  SmallVector<Use, 3> users = uses_[oldNode];
  for (const Use& u : users) {
    if (u.user < nodes_.size() && u.slot < nodes_[u.user].numInputs) {
      if (edgePool_[nodes_[u.user].edgeOffset + u.slot] == oldNode) {
        setInput(u.user, u.slot, withNode);
      }
    }
  }
  ++epoch_;
  nodes_[oldNode].flags.set(NodeFlag::Replaced);
  nodes_[oldNode].flags.set(NodeFlag::Dead);
  nodes_[oldNode].epoch = epoch_;
  Replacement r;
  r.oldNode = oldNode;
  r.newNode = withNode;
  r.epoch = epoch_;
  replacements_.push_back(r);
  return epoch_;
}

void Graph::killNode(NodeId n) {
  if (n >= nodes_.size()) {
    return;
  }
  ++epoch_;
  nodes_[n].flags.set(NodeFlag::Dead);
  nodes_[n].epoch = epoch_;
}

std::uint32_t Graph::liveNodeCount() const noexcept {
  std::uint32_t live = 0;
  for (const Node& n : nodes_) {
    if (!n.isDead()) {
      ++live;
    }
  }
  return live;
}

const FrameStateDesc& Graph::frameState(FrameStateId id) const noexcept {
  if (id < fsDesc_.size()) {
    return fsDesc_[id];
  }
  static const FrameStateDesc bad{};
  return bad;
}

const SpecMeta& Graph::specMeta(SpecMetaId id) const noexcept {
  if (id < specMetas_.size()) {
    return specMetas_[id];
  }
  static const SpecMeta bad{};
  return bad;
}

const Dependency& Graph::dependency(DependencyId id) const noexcept {
  if (id < deps_.size()) {
    return deps_[id];
  }
  static const Dependency bad{};
  return bad;
}

std::span<const VirtualObjectId>
Graph::frameStateVobjs(FrameStateId id) const noexcept {
  if (id < fsDesc_.size()) {
    const FrameStateDesc& d = fsDesc_[id];
    if (d.vobjOffset + d.vobjCount <= fsVobjs_.size()) {
      return std::span<const VirtualObjectId>(
          fsVobjs_.data() + d.vobjOffset, d.vobjCount);
    }
  }
  return {};
}

FrameStateId Graph::replayFrameStateDesc(MethodId method, std::uint32_t pc,
                                         FrameStateId caller,
                                         std::span<const VirtualObjectId> vobjs) {
  return addFrameStateDesc(method, pc, caller, vobjs);
}

void Graph::replayReplacement(NodeId oldNode, NodeId newNode,
                              std::uint32_t ep) {
  if (oldNode >= nodes_.size()) {
    return; // corrupt input; verifier will flag the dangling state
  }
  nodes_[oldNode].flags.set(NodeFlag::Replaced);
  nodes_[oldNode].flags.set(NodeFlag::Dead);
  nodes_[oldNode].epoch = ep;
  Replacement r;
  r.oldNode = oldNode;
  r.newNode = newNode;
  r.epoch = ep;
  replacements_.push_back(r);
  if (ep > epoch_) {
    epoch_ = ep;
  }
}

} // namespace b2::ir
