// B-2 IR - versioned serialization + verified replay (Rules 31, 38, 124).
//
// WHY THIS FILE EXISTS:
// Rule 124 requires deterministic replay; Rule 38 replay artifacts need a
// stable, versioned on-disk form. The writer emits nodes in id order (ids
// preserved, tombstones included), side tables in id order, and the
// replacement log in log order - byte-deterministic for the same graph
// state. The reader is a total parser: every read is bounds-checked, every
// id/kind validated, and the rebuilt graph is verified before use; a graph
// that parses but does not verify is a load error, not a crash.

#include "b2/ir/Serialize.h"

#include <cstring>
#include <vector>

#include "b2/ir/Printer.h"
#include "b2/ir/Verifier.h"

namespace b2::ir {

namespace {

// Layout (all little-endian):
//   u32 magic, u32 version
//   u32 nodeCount
//   nodeCount nodes:
//     u16 kind, u16 flags, u32 payload, u32 payload2, u64 constValue,
//     u32 specMeta, u32 epoch, u16 numInputs, u16 reserved,
//     numInputs x u32 input ids
//   u32 frameStateCount
//   per FrameState: u32 method, u32 pc, u32 caller, u32 vobjCount,
//     vobjCount x u32 vobj ids
//   u32 specMetaCount
//   per SpecMeta: u32 kind, u32 source, u32 confidence, u32 guard,
//     u32 deoptTarget, u32 cost, u32 dependency, u32 rollback
//   u32 dependencyCount
//   per Dependency: u32 kind, u32 target
//   u32 replacementCount
//   per Replacement: u32 oldNode, u32 newNode, u32 epoch

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

void putU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

class Cursor {
public:
  Cursor(const std::uint8_t* data, std::size_t size)
      : data_(data), size_(size) {}

  [[nodiscard]] bool u16(std::uint16_t& v) noexcept {
    if (pos_ + 2 > size_) {
      return false;
    }
    v = static_cast<std::uint16_t>(data_[pos_]) |
        (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return true;
  }
  [[nodiscard]] bool u32(std::uint32_t& v) noexcept {
    if (pos_ + 4 > size_) {
      return false;
    }
    v = 0;
    for (unsigned i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(data_[pos_ + i]) << (8 * i);
    }
    pos_ += 4;
    return true;
  }
  [[nodiscard]] bool u64(std::uint64_t& v) noexcept {
    if (pos_ + 8 > size_) {
      return false;
    }
    v = 0;
    for (unsigned i = 0; i < 8; ++i) {
      v |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i);
    }
    pos_ += 8;
    return true;
  }
  [[nodiscard]] std::size_t pos() const noexcept { return pos_; }

private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

} // namespace

std::vector<std::uint8_t> serialize(const Graph& g) {
  std::vector<std::uint8_t> out;
  out.reserve(64 + g.nodeCount() * 40);
  putU32(out, kIrMagic);
  putU32(out, kIrFormatVersion);
  putU32(out, g.nodeCount());

  for (NodeId n = 0; n < g.nodeCount(); ++n) {
    const Node& nd = g.node(n);
    putU16(out, static_cast<std::uint16_t>(nd.kind));
    putU16(out, nd.flags.raw());
    putU32(out, nd.payload);
    putU32(out, nd.payload2);
    putU64(out, static_cast<std::uint64_t>(nd.constValue));
    putU32(out, nd.specMeta);
    putU32(out, nd.epoch);
    putU16(out, nd.numInputs);
    putU16(out, nd.reserved);
    for (std::uint16_t s = 0; s < nd.numInputs; ++s) {
      putU32(out, g.input(n, s));
    }
  }

  putU32(out, g.frameStateCount());
  for (FrameStateId i = 0; i < g.frameStateCount(); ++i) {
    const FrameStateDesc& d = g.frameState(i);
    putU32(out, d.method);
    putU32(out, d.pc);
    putU32(out, d.caller);
    const auto vobjs = g.frameStateVobjs(i);
    putU32(out, static_cast<std::uint32_t>(vobjs.size()));
    for (VirtualObjectId v : vobjs) {
      putU32(out, v);
    }
  }

  putU32(out, g.specMetaCount());
  for (SpecMetaId i = 0; i < g.specMetaCount(); ++i) {
    const SpecMeta& sm = g.specMeta(i);
    putU32(out, static_cast<std::uint32_t>(sm.kind));
    putU32(out, static_cast<std::uint32_t>(sm.source));
    putU32(out, sm.confidence);
    putU32(out, sm.guard);
    putU32(out, sm.deoptTarget);
    putU32(out, sm.cost);
    putU32(out, sm.dependency);
    putU32(out, static_cast<std::uint32_t>(sm.rollback));
  }

  putU32(out, g.dependencyCount());
  for (DependencyId i = 0; i < g.dependencyCount(); ++i) {
    const Dependency& d = g.dependency(i);
    putU32(out, static_cast<std::uint32_t>(d.kind));
    putU32(out, d.target);
  }

  putU32(out, static_cast<std::uint32_t>(g.replacements().size()));
  for (const Replacement& r : g.replacements()) {
    putU32(out, r.oldNode);
    putU32(out, r.newNode);
    putU32(out, r.epoch);
  }
  return out;
}

DeserializeResult deserializeInto(const std::uint8_t* data,
                                 std::size_t size, Graph& g) noexcept {
  DeserializeResult res;
  auto fail = [&res](const char* msg) {
    res.ok = false;
    res.error.message = msg;
  };

  Cursor c(data, size);
  std::uint32_t magic = 0, version = 0;
  if (!c.u32(magic) || !c.u32(version)) {
    fail("truncated header");
    return res;
  }
  if (magic != kIrMagic) {
    fail("bad magic (not a B-2 IR artifact)");
    return res;
  }
  if (version != kIrFormatVersion) {
    if (version >= kIrMinReadVersion && version < kIrFormatVersion) {
      // v1 artifact: kind values are unchanged (v2 only appended), and no
      // v1 node can use a builder kind - proceed; replay rejects anything
      // unexpected.
    } else {
      fail("unsupported IR format version (Rule 31: stale artifact)");
      return res;
    }
  }

  std::uint32_t nodeCount = 0;
  if (!c.u32(nodeCount)) {
    fail("truncated node count");
    return res;
  }

  // Pass 1: create nodes with empty inputs (ids = creation order).
  // Node 0 is the ctor-created Start; replay patches it in place.
  std::vector<std::vector<std::uint32_t>> pendingInputs(nodeCount);
  for (std::uint32_t i = 0; i < nodeCount; ++i) {
    std::uint16_t kind = 0, flags = 0, numInputs = 0, reserved = 0;
    std::uint32_t payload = 0, payload2 = 0, specMeta = 0, epoch = 0;
    std::uint64_t constValue = 0;
    if (!c.u16(kind) || !c.u16(flags) || !c.u32(payload) || !c.u32(payload2) ||
        !c.u64(constValue) || !c.u32(specMeta) || !c.u32(epoch) ||
        !c.u16(numInputs) || !c.u16(reserved)) {
      fail("truncated node record");
      return res;
    }
    if (kind >= static_cast<std::uint16_t>(NodeKind::_Count)) {
      fail("node kind out of range");
      return res;
    }
    if (reserved != 0) {
      fail("node record reserved field must be 0 (corrupt artifact)");
      return res;
    }
    pendingInputs[i].resize(numInputs);
    for (std::uint16_t s = 0; s < numInputs; ++s) {
      if (!c.u32(pendingInputs[i][s])) {
        fail("truncated node inputs");
        return res;
      }
    }
    NodeId id = kInvalidNodeId;
    if (i == 0) {
      if (static_cast<NodeKind>(kind) != NodeKind::Start) {
        fail("node 0 must be Start (corrupt artifact)");
        return res;
      }
      id = 0;
    } else {
      id = g.make(static_cast<NodeKind>(kind), std::span<const NodeId>{},
                  payload, payload2);
      if (id != i) {
        fail("node id mismatch during replay");
        return res;
      }
    }
    Node& nd = g.nodeForReplay(id);
    nd.flags = NodeFlags(flags);
    nd.constValue = static_cast<std::int64_t>(constValue);
    nd.specMeta = specMeta;
    nd.epoch = epoch;
    nd.reserved = 0;
  }

  // Pass 2: replay edges (appendInput maintains use lists; forward refs OK
  // because every node already exists).
  for (std::uint32_t i = 0; i < nodeCount; ++i) {
    for (std::uint32_t in : pendingInputs[i]) {
      g.appendInput(i, in);
    }
  }

  // FrameState descriptors.
  std::uint32_t fsCount = 0;
  if (!c.u32(fsCount)) {
    fail("truncated FrameState table");
    return res;
  }
  for (std::uint32_t i = 0; i < fsCount; ++i) {
    std::uint32_t method = 0, pc = 0, caller = 0, vobjCount = 0;
    if (!c.u32(method) || !c.u32(pc) || !c.u32(caller) || !c.u32(vobjCount)) {
      fail("truncated FrameState descriptor");
      return res;
    }
    std::vector<VirtualObjectId> vobjs(vobjCount);
    for (std::uint32_t k = 0; k < vobjCount; ++k) {
      if (!c.u32(vobjs[k])) {
        fail("truncated FrameState vobj list");
        return res;
      }
    }
    const FrameStateId got = g.replayFrameStateDesc(
        method, pc, caller, std::span<const VirtualObjectId>(vobjs));
    if (got != i) {
      fail("FrameState descriptor id mismatch during replay");
      return res;
    }
  }

  // SpecMeta table.
  std::uint32_t specCount = 0;
  if (!c.u32(specCount)) {
    fail("truncated SpecMeta table");
    return res;
  }
  for (std::uint32_t i = 0; i < specCount; ++i) {
    std::uint32_t k = 0, src = 0, conf = 0, guard = 0, deopt = 0, cost = 0,
                  dep = 0, rb = 0;
    if (!c.u32(k) || !c.u32(src) || !c.u32(conf) || !c.u32(guard) ||
        !c.u32(deopt) || !c.u32(cost) || !c.u32(dep) || !c.u32(rb)) {
      fail("truncated SpecMeta record");
      return res;
    }
    SpecMeta sm;
    sm.kind = static_cast<SpecMeta::Kind>(k);
    sm.source = static_cast<SpecMeta::Source>(src);
    sm.confidence = conf;
    sm.guard = guard;
    sm.deoptTarget = deopt;
    sm.cost = cost;
    sm.dependency = dep;
    sm.rollback = static_cast<SpecMeta::Rollback>(rb);
    if (k > static_cast<std::uint32_t>(SpecMeta::Kind::StaticProofCarried) ||
        src > static_cast<std::uint32_t>(SpecMeta::Source::Assumption) ||
        rb > static_cast<std::uint32_t>(SpecMeta::Rollback::Compensated)) {
      fail("SpecMeta enum out of range");
      return res;
    }
    if (g.addSpecMeta(sm) != i) {
      fail("SpecMeta id mismatch during replay");
      return res;
    }
  }

  // Dependency table.
  std::uint32_t depCount = 0;
  if (!c.u32(depCount)) {
    fail("truncated Dependency table");
    return res;
  }
  for (std::uint32_t i = 0; i < depCount; ++i) {
    std::uint32_t k = 0, target = 0;
    if (!c.u32(k) || !c.u32(target)) {
      fail("truncated Dependency record");
      return res;
    }
    if (k > static_cast<std::uint32_t>(Dependency::Kind::StaticProof)) {
      fail("Dependency enum out of range");
      return res;
    }
    Dependency d;
    d.kind = static_cast<Dependency::Kind>(k);
    d.target = target;
    if (g.addDependency(d) != i) {
      fail("Dependency id mismatch during replay");
      return res;
    }
  }

  // Replacement log (re-tags WITHOUT rewiring: serialized state is final).
  std::uint32_t replCount = 0;
  if (!c.u32(replCount)) {
    fail("truncated replacement log");
    return res;
  }
  for (std::uint32_t i = 0; i < replCount; ++i) {
    std::uint32_t oldNode = 0, newNode = 0, epoch = 0;
    if (!c.u32(oldNode) || !c.u32(newNode) || !c.u32(epoch)) {
      fail("truncated replacement entry");
      return res;
    }
    if (oldNode >= nodeCount || newNode >= nodeCount) {
      fail("replacement entry node id out of range");
      return res;
    }
    g.replayReplacement(oldNode, newNode, epoch);
  }

  // Load gate: the rebuilt graph must verify (Rule 40).
  const VerifyResult vr = verify(g);
  if (!vr.ok) {
    fail("replayed graph failed verification");
    return res;
  }

  res.ok = true;
  return res;
}

} // namespace b2::ir
