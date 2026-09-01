// B-2 IR - deterministic printer + shared result-type computation.
//
// WHY THIS FILE EXISTS:
// Golden tests need byte-stable dumps (Rule 124); resultTypeOf is the single
// authoritative "what value does this node produce" computation shared by the
// printer and the verifier's operand-type checks (Rule 33 enforcement).
// The format is normative (docs/ir_spec.md appendix A); it changes only with
// a format-version bump (Rule 31).

#include "b2/ir/Printer.h"

#include <cstdio>
#include <string>

#include "b2/ir/Serialize.h"

namespace b2::ir {

IRType resultTypeOf(const Graph& g, NodeId n) {
  const Node& nd = g.node(n);
  switch (nd.kind) {
  // Control-only and state nodes produce no value.
  case NodeKind::Start:
  case NodeKind::End:
  case NodeKind::Region:
  case NodeKind::If:
  case NodeKind::IfTrue:
  case NodeKind::IfFalse:
  case NodeKind::Switch:
  case NodeKind::SwitchCase:
  case NodeKind::SwitchDefault:
  case NodeKind::LoopBegin:
  case NodeKind::LoopEnd:
  case NodeKind::LoopExit:
  case NodeKind::Return:
  case NodeKind::Unwind:
  case NodeKind::Deopt:
  case NodeKind::StoreField:
  case NodeKind::StoreStatic:
  case NodeKind::StoreElem:
  case NodeKind::MemBar:
  case NodeKind::MonitorEnter:
  case NodeKind::MonitorExit:
  case NodeKind::ClassInit:
  case NodeKind::Guard:
  case NodeKind::RepTransitionGuard:
  case NodeKind::VectorStore:
  case NodeKind::FrameState:
    return IRType::Bottom;

  // Memory reads / object ops.
  case NodeKind::LoadField:
  case NodeKind::LoadStatic:
  case NodeKind::UnboxPrimitive:
    return static_cast<IRType>(nd.payload2);
  case NodeKind::LoadElem:
    return static_cast<IRType>(nd.payload);
  case NodeKind::ArrayLength:
  case NodeKind::InstanceOf:
    return IRType::Int;
  case NodeKind::New:
  case NodeKind::NewArray:
  case NodeKind::NewRefArray:
  case NodeKind::NewMultiArray:
  case NodeKind::ConstantSym:
  case NodeKind::CheckCast:
  case NodeKind::Materialize:
  case NodeKind::CompressedRefDecode:
  case NodeKind::BoxPrimitive:
  case NodeKind::VirtualObjectState:
    return IRType::Ref;
  case NodeKind::CompressedRefEncode:
    return IRType::Int;

  // Calls: payload2 = result type (Bottom = void).
  case NodeKind::CallStatic:
  case NodeKind::CallVirtual:
  case NodeKind::CallInterface:
  case NodeKind::CallDynamic:
  case NodeKind::CallExcept:
  case NodeKind::LoadException:
    return (nd.kind == NodeKind::CallStatic || nd.kind == NodeKind::CallVirtual ||
            nd.kind == NodeKind::CallInterface ||
            nd.kind == NodeKind::CallDynamic)
               ? static_cast<IRType>(nd.payload2)
               : IRType::Ref;

  // Constants / parameters.
  case NodeKind::ConstantI:
    return IRType::Int;
  case NodeKind::ConstantL:
    return IRType::Long;
  case NodeKind::ConstantF:
    return IRType::Float;
  case NodeKind::ConstantD:
    return IRType::Double;
  case NodeKind::ConstantNull:
    return IRType::Null;
  case NodeKind::Parameter:
    return static_cast<IRType>(nd.payload2);

  // Typed arithmetic.
  case NodeKind::AddI: case NodeKind::SubI: case NodeKind::MulI:
  case NodeKind::DivI: case NodeKind::RemI: case NodeKind::NegI:
  case NodeKind::ShlI: case NodeKind::ShrI: case NodeKind::UShrI:
  case NodeKind::AndI: case NodeKind::OrI: case NodeKind::XorI:
  case NodeKind::CmpI:
  case NodeKind::I2B: case NodeKind::I2C: case NodeKind::I2S:
  case NodeKind::Truncate:
  // v2 boolean tests (graph-builder branch/guard conditions).
  case NodeKind::Not: case NodeKind::IsNull: case NodeKind::RefEq:
  case NodeKind::EqI: case NodeKind::NeI:
  case NodeKind::LtI: case NodeKind::LeI:
  case NodeKind::GtI: case NodeKind::GeI:
  // MSG-20260831-009: comparisons yield Int (Node.h / ir_spec 4.7:
  // (a, b) -> int; they were previously grouped with their operand
  // families and returned Long/Float/Double, so a comparison result
  // could never feed an Int slot - branches, guards, boolean tests).
  case NodeKind::CmpL:
  case NodeKind::CmpFl: case NodeKind::CmpFg:
  case NodeKind::CmpDl: case NodeKind::CmpDg:
    return IRType::Int;
  case NodeKind::AddL: case NodeKind::SubL: case NodeKind::MulL:
  case NodeKind::DivL: case NodeKind::RemL: case NodeKind::NegL:
  case NodeKind::ShlL: case NodeKind::ShrL: case NodeKind::UShrL:
  case NodeKind::AndL: case NodeKind::OrL: case NodeKind::XorL:
  case NodeKind::SignExtend: case NodeKind::ZeroExtend:
    return IRType::Long;
  case NodeKind::AddF: case NodeKind::SubF: case NodeKind::MulF:
  case NodeKind::DivF: case NodeKind::RemF: case NodeKind::NegF:
    return IRType::Float;
  case NodeKind::AddD: case NodeKind::SubD: case NodeKind::MulD:
  case NodeKind::DivD: case NodeKind::RemD: case NodeKind::NegD:
    return IRType::Double;

  // Conversions. MSG-20260831-009: I2L widens to Long (Node.h /
  // ir_spec 4.8); it was previously grouped with the FP widenings and
  // returned Double, so L2I(I2L(x)) - the exact round-trip identity -
  // failed verification.
  case NodeKind::I2D: case NodeKind::L2D: case NodeKind::F2D:
    return IRType::Double;
  case NodeKind::I2F: case NodeKind::L2F: case NodeKind::D2F:
    return IRType::Float;
  case NodeKind::L2I: case NodeKind::F2I: case NodeKind::D2I:
    return IRType::Int;
  case NodeKind::I2L: case NodeKind::F2L: case NodeKind::D2L:
    return IRType::Long;
  case NodeKind::BitCast: {
    if (nd.numInputs >= 1) {
      const IRType t = resultTypeOf(g, g.input(n, 0));
      if (t != IRType::Bottom) {
        return t; // same-width reinterpretation preserves the type
      }
    }
    return IRType::Bottom;
  }

  // Merge: join of value inputs (inputs[0] is the region).
  case NodeKind::Phi: {
    IRType t = IRType::Bottom;
    bool seen = false;
    for (std::uint16_t s = 1; s < nd.numInputs; ++s) {
      const IRType v = resultTypeOf(g, g.input(n, s));
      t = seen ? join(t, v) : v;
      seen = true;
    }
    return t;
  }

  // Vector / SIMD.
  case NodeKind::VectorBroadcast:
  case NodeKind::VectorPack:
    return vectorOfLane(static_cast<IRType>(nd.payload));
  case NodeKind::VectorOp:
  case NodeKind::VectorMaskOp:
  case NodeKind::VectorInsert:
  case NodeKind::VectorLoad:
    return vectorOfLane(unpackVecLane(nd.payload2));
  case NodeKind::VectorExtract:
  case NodeKind::VectorReduce:
    return unpackVecLane(nd.payload2);

  // Tagged values.
  case NodeKind::TagValue:
    return IRType::Tagged;
  case NodeKind::UntagValue:
    switch (static_cast<ValueRep>(nd.payload2)) {
    case ValueRep::UnboxedInt32:
    case ValueRep::TaggedInt31:
      return IRType::Int;
    case ValueRep::UnboxedInt64:
      return IRType::Long;
    case ValueRep::UnboxedFloat32:
      return IRType::Float;
    case ValueRep::UnboxedFloat64:
      return IRType::Double;
    default:
      return IRType::Ref; // CompressedOop / NaNBoxedRef / BoxedObject
    }

  default:
    return IRType::Bottom;
  }
}

namespace {

void appendNodeIds(std::string& out, const Graph& g, NodeId n,
                   std::uint16_t from) {
  out += '[';
  const Node& nd = g.node(n);
  for (std::uint16_t s = from; s < nd.numInputs; ++s) {
    if (s != from) {
      out += ' ';
    }
    out += "n" + std::to_string(g.input(n, s));
  }
  out += ']';
}

const char* specKindName(SpecMeta::Kind k) {
  switch (k) {
  case SpecMeta::Kind::ClassHierarchyStable: return "classhierarchy";
  case SpecMeta::Kind::MethodFinal: return "methodfinal";
  case SpecMeta::Kind::TypeMonomorphic: return "typemono";
  case SpecMeta::Kind::TypeBimorphic: return "typebi";
  case SpecMeta::Kind::NullNeverSeen: return "nullnever";
  case SpecMeta::Kind::BoundsAlwaysValid: return "boundsvalid";
  case SpecMeta::Kind::ArgumentConstant: return "argconst";
  case SpecMeta::Kind::LoopTripCountProfile: return "tripcount";
  case SpecMeta::Kind::StaticProofCarried: return "staticproof";
  default: return "?";
  }
}

const char* specSourceName(SpecMeta::Source s) {
  switch (s) {
  case SpecMeta::Source::PGO: return "pgo";
  case SpecMeta::Source::Static: return "static";
  case SpecMeta::Source::Assumption: return "assumption";
  default: return "?";
  }
}

const char* rollbackName(SpecMeta::Rollback r) {
  switch (r) {
  case SpecMeta::Rollback::None: return "none";
  case SpecMeta::Rollback::DeferredEffects: return "deferred";
  case SpecMeta::Rollback::Compensated: return "compensated";
  default: return "?";
  }
}

const char* depKindName(Dependency::Kind k) {
  switch (k) {
  case Dependency::Kind::ClassHierarchy: return "classhierarchy";
  case Dependency::Kind::MethodBody: return "methodbody";
  case Dependency::Kind::FieldFinality: return "fieldfinal";
  case Dependency::Kind::ProfileCounter: return "profile";
  case Dependency::Kind::StaticProof: return "proof";
  default: return "?";
  }
}

void appendPayloadSuffix(std::string& out, const Graph& g, NodeId n) {
  const Node& nd = g.node(n);
  char buf[64];
  const std::size_t mark = out.size(); // rollback point for empty suffixes
  if (!out.empty() && out.back() != ' ') {
    out += ' '; // separate payload from inputs (v2: was glued)
  }
  switch (nd.kind) {
  case NodeKind::Parameter:
    std::snprintf(buf, sizeof(buf), "#%u : %s", nd.payload,
                  typeName(static_cast<IRType>(nd.payload2)));
    out += buf;
    break;
  case NodeKind::ConstantI:
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(
                      static_cast<std::int32_t>(nd.constValue)));
    out += buf;
    break;
  case NodeKind::ConstantL:
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(nd.constValue));
    out += buf;
    break;
  case NodeKind::ConstantF: {
    const std::uint32_t bits =
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(nd.constValue));
    std::snprintf(buf, sizeof(buf), "bits=0x%08x", bits);
    out += buf;
    break;
  }
  case NodeKind::ConstantD: {
    const std::uint64_t bits = static_cast<std::uint64_t>(nd.constValue);
    std::snprintf(buf, sizeof(buf), "bits=0x%016llx",
                  static_cast<unsigned long long>(bits));
    out += buf;
    break;
  }
  case NodeKind::ConstantSym:
    std::snprintf(buf, sizeof(buf), "s=%u", nd.payload);
    out += buf;
    break;
  case NodeKind::LoadField:
  case NodeKind::LoadStatic:
    std::snprintf(buf, sizeof(buf), "f=%u ret=%s", nd.payload,
                  typeName(static_cast<IRType>(nd.payload2)));
    out += buf;
    break;
  case NodeKind::StoreField:
  case NodeKind::StoreStatic:
    std::snprintf(buf, sizeof(buf), "f=%u", nd.payload);
    out += buf;
    break;
  case NodeKind::LoadElem:
  case NodeKind::StoreElem:
    std::snprintf(buf, sizeof(buf), "t=%s",
                  typeName(static_cast<IRType>(nd.payload)));
    out += buf;
    break;
  case NodeKind::MemBar:
    std::snprintf(buf, sizeof(buf), "bar=%s",
                  memBarName(static_cast<MemBarKind>(nd.payload)));
    out += buf;
    break;
  case NodeKind::New:
  case NodeKind::NewRefArray:
  case NodeKind::NewMultiArray:
  case NodeKind::ClassInit:
  case NodeKind::CheckCast:
  case NodeKind::InstanceOf:
    std::snprintf(buf, sizeof(buf), "t=%u", nd.payload);
    out += buf;
    break;
  case NodeKind::NewArray:
    std::snprintf(buf, sizeof(buf), "t=%s",
                  typeName(static_cast<IRType>(nd.payload)));
    out += buf;
    break;
  case NodeKind::CallStatic:
  case NodeKind::CallVirtual:
  case NodeKind::CallInterface:
  case NodeKind::CallDynamic:
    std::snprintf(buf, sizeof(buf), "m=%u ret=%s", nd.payload,
                  typeName(static_cast<IRType>(nd.payload2)));
    out += buf;
    break;
  case NodeKind::Guard:
    std::snprintf(buf, sizeof(buf), "kind=%s deopt=%u",
                  guardKindName(static_cast<GuardKind>(nd.payload)),
                  nd.payload2);
    out += buf;
    break;
  case NodeKind::Deopt:
    std::snprintf(buf, sizeof(buf), "deopt=%u", nd.payload);
    out += buf;
    break;
  case NodeKind::Switch:
    std::snprintf(buf, sizeof(buf), "sw=%u", nd.payload);
    out += buf;
    break;
  case NodeKind::SwitchCase:
    std::snprintf(buf, sizeof(buf), "case=%u", nd.payload);
    out += buf;
    break;
  case NodeKind::VectorBroadcast:
  case NodeKind::VectorPack:
    std::snprintf(buf, sizeof(buf), "t=%s lanes=%u",
                  typeName(static_cast<IRType>(nd.payload)),
                  nd.kind == NodeKind::VectorPack ? nd.numInputs
                                                  : unpackVecLanes(nd.payload2));
    out += buf;
    break;
  case NodeKind::VectorOp:
  case NodeKind::VectorMaskOp:
  case NodeKind::VectorReduce:
    std::snprintf(buf, sizeof(buf), "op=%s t=%s lanes=%u",
                  vectorOpName(static_cast<VectorOp>(nd.payload)),
                  typeName(unpackVecLane(nd.payload2)),
                  unpackVecLanes(nd.payload2));
    out += buf;
    break;
  case NodeKind::VectorLoad:
  case NodeKind::VectorStore:
    std::snprintf(buf, sizeof(buf), "t=%s lanes=%u",
                  typeName(unpackVecLane(nd.payload2)),
                  unpackVecLanes(nd.payload2));
    out += buf;
    break;
  case NodeKind::VirtualObjectState:
    std::snprintf(buf, sizeof(buf), "t=%u kind=%s", nd.payload,
                  nd.payload2 == 1 ? "array" : "instance");
    out += buf;
    break;
  case NodeKind::TagValue:
  case NodeKind::UntagValue:
  case NodeKind::RepTransitionGuard:
    std::snprintf(buf, sizeof(buf), "from=%s to=%s",
                  repName(static_cast<ValueRep>(nd.payload)),
                  repName(static_cast<ValueRep>(nd.payload2)));
    out += buf;
    break;
  case NodeKind::FrameState: {
    const FrameStateDesc& d = g.frameState(nd.payload);
    char head[48];
    std::snprintf(head, sizeof(head), "m=%u pc=%u caller=", d.method, d.pc);
    out += head;
    if (d.caller == kInvalidFrameState) {
      out += "none";
    } else {
      out += "fs" + std::to_string(d.caller);
    }
    out += " vobjs=";
    appendNodeIds(out, g, n, 0); // FrameState inputs ARE the locals
    // vobj list comes from the descriptor; printed after locals.
    out += " vobjlist=[";
    auto vobjs = g.frameStateVobjs(nd.payload);
    for (std::size_t i = 0; i < vobjs.size(); ++i) {
      if (i != 0) {
        out += ' ';
      }
      out += "n" + std::to_string(vobjs[i]);
    }
    out += ']';
    break;
  }
  default:
    break;
  }
  if (nd.specMeta != 0) {
    std::snprintf(buf, sizeof(buf), " spec=s%u", nd.specMeta - 1);
    out += buf;
  }
  if (out.size() == mark + 1 && out.back() == ' ') {
    out.pop_back(); // no payload: drop the separator
  }
}

} // namespace

std::string print(const Graph& g) {
  std::string out;
  char buf[96];
  std::snprintf(buf, sizeof(buf),
                "; B-2 IR v%u nodes=%u live=%u epoch=%u\n", kIrFormatVersion,
                g.nodeCount(), g.liveNodeCount(), g.epoch());
  out += buf;

  for (NodeId id = 0; id < g.nodeCount(); ++id) {
    const Node& nd = g.node(id);
    std::snprintf(buf, sizeof(buf), "n%-3u %-22s", id, info(nd.kind).name);
    out += buf;
    for (std::uint16_t s = 0; s < nd.numInputs; ++s) {
      out += ' ';
      out += 'n';
      out += std::to_string(g.input(id, s));
    }
    appendPayloadSuffix(out, g, id);
    if (nd.flags.has(NodeFlag::Dead)) {
      std::snprintf(buf, sizeof(buf), " [dead@%u]", nd.epoch);
      out += buf;
    }
    out += '\n';
  }

  for (SpecMetaId i = 0; i < g.specMetaCount(); ++i) {
    const SpecMeta& sm = g.specMeta(i);
    const std::string guard =
        (sm.guard == kInvalidNodeId) ? std::string("none")
                                     : ("n" + std::to_string(sm.guard));
    const std::string dep =
        (sm.dependency == kInvalidDependency)
            ? std::string("none")
            : ("d" + std::to_string(sm.dependency));
    char sbuf[160];
    std::snprintf(sbuf, sizeof(sbuf),
                  "; spec s%u kind=%s src=%s conf=%u guard=%s deopt=%u "
                  "cost=%u dep=%s rollback=%s\n",
                  i, specKindName(sm.kind), specSourceName(sm.source),
                  sm.confidence, guard.c_str(), sm.deoptTarget, sm.cost,
                  dep.c_str(), rollbackName(sm.rollback));
    out += sbuf;
  }
  for (DependencyId i = 0; i < g.dependencyCount(); ++i) {
    const Dependency& d = g.dependency(i);
    std::snprintf(buf, sizeof(buf), "; dep d%u kind=%s target=%u\n", i,
                  depKindName(d.kind), d.target);
    out += buf;
  }
  for (const Replacement& r : g.replacements()) {
    std::snprintf(buf, sizeof(buf), "; replaced n%u -> n%u (epoch %u)\n",
                  r.oldNode, r.newNode, r.epoch);
    out += buf;
  }
  return out;
}

} // namespace b2::ir
