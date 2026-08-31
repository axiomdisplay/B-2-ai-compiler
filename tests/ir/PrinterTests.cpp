// B-2 IR tests - deterministic printer + serialization round-trips
// (Rules 31, 124: byte-stable dumps, id-preserving replay).

#include "TestHarness.h"

#include <b2/ir/Graph.h>
#include <b2/ir/Node.h>
#include <b2/ir/Printer.h>
#include <b2/ir/Serialize.h>
#include <b2/ir/Verifier.h>

using namespace b2;
using b2::ir::NodeKind;

namespace {

// A graph exercising every metadata subsystem: control diamond, loop-less
// phi, call with FrameState, speculative guard with dependency, PEA vobj on
// the deopt list, a replacement with epoch. Graphs are non-movable (their
// pmr storage points at the member arena), so this builds in place.
void buildRichGraph(ir::Graph& g) {
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  const ir::NodeId cmp = g.make(NodeKind::CmpI, {a, b});
  const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), cmp});
  const ir::NodeId t = g.make(NodeKind::IfTrue, {iff});
  const ir::NodeId f = g.make(NodeKind::IfFalse, {iff});
  const ir::NodeId region = g.make(NodeKind::Region, {t, f});
  const ir::NodeId phi = g.make(NodeKind::Phi, {region, a, b});

  const ir::NodeId vo = g.makeVirtualObject(30, {a});
  const ir::NodeId fs = g.makeFrameState(1, 4, {phi}, ir::kInvalidFrameState,
                                         {vo});
  const ir::NodeId guard = g.make(
      NodeKind::Guard, {g.startNode(), cmp, fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 2);
  const ir::NodeId dep = g.addDependency({ir::Dependency::Kind::ClassHierarchy, 17});
  ir::SpecMeta sm;
  sm.kind = ir::SpecMeta::Kind::TypeMonomorphic;
  sm.source = ir::SpecMeta::Source::PGO;
  sm.confidence = 7500;
  sm.guard = guard;
  sm.deoptTarget = 4;
  sm.cost = 12;
  sm.dependency = dep;
  sm.rollback = ir::SpecMeta::Rollback::DeferredEffects;
  const ir::SpecMetaId sid = g.addSpecMeta(sm);
  g.attachSpecMeta(guard, sid);

  const ir::NodeId cfs = g.makeFrameState(1, 7, {a, b});
  const ir::NodeId call =
      g.make(NodeKind::CallVirtual, {g.startNode(), g.startNode(), phi, cfs},
             9, static_cast<std::uint32_t>(ir::IRType::Int));

  // A replacement with a later value.
  const ir::NodeId fresh = g.constantI(9);
  g.replaceNode(b, fresh);

  g.make(NodeKind::Return, {g.startNode(), call});
}

} // namespace

B2_TEST(printer_output_is_deterministic_across_identical_rebuilds) {
  ir::Graph g1;
  buildRichGraph(g1);
  ir::Graph g2;
  buildRichGraph(g2);
  const std::string p1 = ir::print(g1);
  const std::string p2 = ir::print(g2);
  CHECK(p1 == p2);
  CHECK(!p1.empty());
}

B2_TEST(printer_contains_expected_node_lines) {
  ir::Graph g;
  buildRichGraph(g);
  const std::string p = ir::print(g);
  CHECK(p.find("; B-2 IR v1 nodes=") == 0);
  CHECK(p.find("Start") != std::string::npos);
  CHECK(p.find("CallVirtual") != std::string::npos);
  CHECK(p.find("kind=nullcheck deopt=2") != std::string::npos);
  CHECK(p.find("spec=s0") != std::string::npos);
  CHECK(p.find("kind=typemono src=pgo conf=7500") != std::string::npos);
  CHECK(p.find("; dep d0 kind=classhierarchy target=17") != std::string::npos);
  CHECK(p.find("; replaced n") != std::string::npos);
  CHECK(p.find("[dead@") != std::string::npos);
  CHECK(p.find("vobjlist=[n") != std::string::npos);
}

B2_TEST(printer_prints_replacement_log_stably) {
  ir::Graph g;
  buildRichGraph(g);
  const std::string p1 = ir::print(g);
  // Print again - no mutation, byte-identical.
  const std::string p2 = ir::print(g);
  CHECK(p1 == p2);
}

B2_TEST(printer_constants_print_lossless_bits) {
  ir::Graph g;
  g.constantF(-0.0F);
  g.constantD(0.5);
  const std::string p = ir::print(g);
  CHECK(p.find("bits=0x80000000") != std::string::npos);
  CHECK(p.find("bits=0x3fe0000000000000") != std::string::npos);
}

B2_TEST(serialize_roundtrip_preserves_ids_and_print) {
  ir::Graph g;
  buildRichGraph(g);
  CHECK(ir::verify(g).ok);
  const std::string before = ir::print(g);
  const std::vector<std::uint8_t> bytes = ir::serialize(g);

  ir::Graph h;
  const ir::DeserializeResult r = ir::deserializeInto(bytes, h);
  CHECK_MSG(r.ok, r.error.message.c_str());
  CHECK(h.nodeCount() == g.nodeCount());
  CHECK(ir::verify(h).ok);
  const std::string after = ir::print(h);
  CHECK_MSG(before == after, "print differs after round-trip");
  // Byte-deterministic re-serialization (Rule 124).
  CHECK(ir::serialize(h) == bytes);
}

B2_TEST(serialize_roundtrip_of_empty_graph) {
  ir::Graph g;
  const std::vector<std::uint8_t> bytes = ir::serialize(g);
  ir::Graph h;
  const ir::DeserializeResult r = ir::deserializeInto(bytes, h);
  CHECK_MSG(r.ok, r.error.message.c_str());
  CHECK(h.nodeCount() == 1);
  CHECK(ir::print(h) == ir::print(g));
}

B2_TEST(serialize_rejects_wrong_magic) {
  ir::Graph g;
  std::vector<std::uint8_t> bytes = ir::serialize(g);
  bytes[0] = 0x00;
  ir::Graph h;
  const ir::DeserializeResult r = ir::deserializeInto(bytes, h);
  CHECK(!r.ok);
  CHECK(r.error.message.find("magic") != std::string::npos);
}

B2_TEST(serialize_rejects_stale_version) {
  ir::Graph g;
  std::vector<std::uint8_t> bytes = ir::serialize(g);
  // Version lives at offset 4..8.
  bytes[4] = static_cast<std::uint8_t>(ir::kIrFormatVersion + 1);
  ir::Graph h;
  const ir::DeserializeResult r = ir::deserializeInto(bytes, h);
  CHECK(!r.ok);
  CHECK(r.error.message.find("version") != std::string::npos);
}

B2_TEST(serialize_rejects_truncated_payloads) {
  ir::Graph g;
  buildRichGraph(g);
  const std::vector<std::uint8_t> bytes = ir::serialize(g);
  for (std::size_t cut : {std::size_t{0}, std::size_t{3}, std::size_t{11},
                          std::size_t{12}, std::size_t{31},
                          bytes.size() - 1}) {
    ir::Graph h;
    const ir::DeserializeResult r =
        ir::deserializeInto(bytes.data(), cut, h);
    CHECK_MSG(!r.ok, "truncation at " + std::to_string(cut) +
                         " must be rejected");
  }
}

B2_TEST(serialize_rejects_bad_node_kind) {
  ir::Graph g;
  buildRichGraph(g);
  std::vector<std::uint8_t> bytes = ir::serialize(g);
  // Node 0's kind sits at offset 12 (after magic/version/nodeCount).
  bytes[12] = 0xFF;
  bytes[13] = 0xFF;
  ir::Graph h;
  const ir::DeserializeResult r = ir::deserializeInto(bytes, h);
  CHECK(!r.ok);
  CHECK(r.error.message.find("kind out of range") != std::string::npos);
}

B2_TEST(serialize_rejects_node0_not_start) {
  ir::Graph g;
  buildRichGraph(g);
  std::vector<std::uint8_t> bytes = ir::serialize(g);
  bytes[12] = 0x01; // End
  bytes[13] = 0x00;
  ir::Graph h;
  const ir::DeserializeResult r = ir::deserializeInto(bytes, h);
  CHECK(!r.ok);
  CHECK(r.error.message.find("Start") != std::string::npos);
}

B2_TEST(serialize_rejects_graph_that_fails_verification) {
  // Serialize a SOUND graph, then corrupt a post-parse invariant the loader
  // cannot see but the verifier can: swap a Guard's FrameState input for a
  // constant by editing the serialized edge.
  ir::Graph g;
  const ir::NodeId cond = g.constantI(1);
  const ir::NodeId fs = g.makeFrameState(1, 0, {cond});
  g.make(NodeKind::Guard, {g.startNode(), cond, fs},
         static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 0);
  g.make(NodeKind::Return, {g.startNode()});
  std::vector<std::uint8_t> bytes = ir::serialize(g);

  // Node layout: node0 Start(0 inputs), node1 ConstantI, node2 FrameState,
  // node3 Guard(3 inputs), node4 Return(1 input).
  // Walk to the Guard's input array and point slot 2 at node 1 (a constant).
  std::size_t off = 12;
  for (std::uint32_t i = 0; i < 5; ++i) {
    off += 2 + 2 + 4 + 4 + 8 + 4 + 4 + 2 + 2; // fixed node record fields
    std::uint32_t nIn = 0;
    for (unsigned k = 0; k < 4; ++k) {
      nIn |= static_cast<std::uint32_t>(bytes[off - 4 + k]) << (8 * k);
    }
    if (i == 3) { // Guard's inputs start here
      bytes[off + 2 * 4] = 1; // input slot 2 -> node 1 (ConstantI)
      bytes[off + 2 * 4 + 1] = 0;
      bytes[off + 2 * 4 + 2] = 0;
      bytes[off + 2 * 4 + 3] = 0;
      break;
    }
    off += static_cast<std::size_t>(nIn) * 4;
  }

  ir::Graph h;
  const ir::DeserializeResult r = ir::deserializeInto(bytes, h);
  CHECK(!r.ok);
  CHECK(r.error.message.find("verification") != std::string::npos);
}

B2_TEST(ir_node_registry_covers_every_kind_with_unique_names) {
  CHECK(ir::registeredNodeKinds() == ir::nodeKindCount());
  for (std::uint16_t i = 0; i < ir::nodeKindCount(); ++i) {
    const char* ni = ir::info(static_cast<NodeKind>(i)).name;
    CHECK(ni != nullptr);
    CHECK(std::string(ni) != "bad<kind>");
    for (std::uint16_t j = static_cast<std::uint16_t>(i + 1);
         j < ir::nodeKindCount(); ++j) {
      const char* nj = ir::info(static_cast<NodeKind>(j)).name;
      CHECK_MSG(std::string(ni) != std::string(nj),
                std::string("duplicate node name: ") + ni);
    }
  }
}

B2_TEST(ir_node_registry_rows_match_the_spec_document_counts) {
  // docs/ir_spec.md appendix B pins these category counts; keep them in
  // lockstep when kinds are added (Rule 130 documentation lint).
  std::uint16_t control = 0, memory = 0, calls = 0, typeOps = 0, guards = 0,
                constants = 0, arith = 0, cmp = 0, conv = 0, merges = 0,
                vector = 0, pea = 0, tagged = 0, state = 0;
  for (std::uint16_t i = 0; i < ir::nodeKindCount(); ++i) {
    switch (static_cast<NodeKind>(i)) {
    case NodeKind::Start: case NodeKind::End: case NodeKind::Region:
    case NodeKind::If: case NodeKind::IfTrue: case NodeKind::IfFalse:
    case NodeKind::Switch: case NodeKind::SwitchCase:
    case NodeKind::SwitchDefault: case NodeKind::LoopBegin:
    case NodeKind::LoopEnd: case NodeKind::LoopExit: case NodeKind::Return:
    case NodeKind::Unwind: case NodeKind::Deopt:
      ++control; break;
    case NodeKind::LoadField: case NodeKind::StoreField:
    case NodeKind::LoadStatic: case NodeKind::StoreStatic:
    case NodeKind::LoadElem: case NodeKind::StoreElem:
    case NodeKind::ArrayLength: case NodeKind::MemBar:
    case NodeKind::MonitorEnter: case NodeKind::MonitorExit:
    case NodeKind::New: case NodeKind::NewArray: case NodeKind::NewRefArray:
    case NodeKind::NewMultiArray: case NodeKind::ClassInit:
      ++memory; break;
    case NodeKind::CallStatic: case NodeKind::CallVirtual:
    case NodeKind::CallInterface: case NodeKind::CallDynamic:
    case NodeKind::CallExcept: case NodeKind::LoadException:
      ++calls; break;
    case NodeKind::CheckCast: case NodeKind::InstanceOf:
      ++typeOps; break;
    case NodeKind::Guard:
      ++guards; break;
    case NodeKind::ConstantI: case NodeKind::ConstantL:
    case NodeKind::ConstantF: case NodeKind::ConstantD:
    case NodeKind::ConstantNull: case NodeKind::ConstantSym:
    case NodeKind::Parameter:
      ++constants; break;
    case NodeKind::AddI: case NodeKind::SubI: case NodeKind::MulI:
    case NodeKind::DivI: case NodeKind::RemI: case NodeKind::NegI:
    case NodeKind::ShlI: case NodeKind::ShrI: case NodeKind::UShrI:
    case NodeKind::AndI: case NodeKind::OrI: case NodeKind::XorI:
    case NodeKind::AddL: case NodeKind::SubL: case NodeKind::MulL:
    case NodeKind::DivL: case NodeKind::RemL: case NodeKind::NegL:
    case NodeKind::ShlL: case NodeKind::ShrL: case NodeKind::UShrL:
    case NodeKind::AndL: case NodeKind::OrL: case NodeKind::XorL:
    case NodeKind::AddF: case NodeKind::SubF: case NodeKind::MulF:
    case NodeKind::DivF: case NodeKind::RemF: case NodeKind::NegF:
    case NodeKind::AddD: case NodeKind::SubD: case NodeKind::MulD:
    case NodeKind::DivD: case NodeKind::RemD: case NodeKind::NegD:
      ++arith; break;
    case NodeKind::CmpI: case NodeKind::CmpL: case NodeKind::CmpFl:
    case NodeKind::CmpFg: case NodeKind::CmpDl: case NodeKind::CmpDg:
      ++cmp; break;
    case NodeKind::I2L: case NodeKind::I2F: case NodeKind::I2D:
    case NodeKind::L2I: case NodeKind::L2F: case NodeKind::L2D:
    case NodeKind::F2I: case NodeKind::F2L: case NodeKind::F2D:
    case NodeKind::D2I: case NodeKind::D2L: case NodeKind::D2F:
    case NodeKind::I2B: case NodeKind::I2C: case NodeKind::I2S:
    case NodeKind::SignExtend: case NodeKind::ZeroExtend:
    case NodeKind::Truncate: case NodeKind::BitCast:
    case NodeKind::CompressedRefEncode: case NodeKind::CompressedRefDecode:
    case NodeKind::BoxPrimitive: case NodeKind::UnboxPrimitive:
      ++conv; break;
    case NodeKind::Phi:
      ++merges; break;
    case NodeKind::VectorBroadcast: case NodeKind::VectorPack:
    case NodeKind::VectorExtract: case NodeKind::VectorInsert:
    case NodeKind::VectorOp: case NodeKind::VectorMaskOp:
    case NodeKind::VectorLoad: case NodeKind::VectorStore:
    case NodeKind::VectorReduce:
      ++vector; break;
    case NodeKind::VirtualObjectState: case NodeKind::Materialize:
      ++pea; break;
    case NodeKind::TagValue: case NodeKind::UntagValue:
    case NodeKind::RepTransitionGuard:
      ++tagged; break;
    case NodeKind::FrameState:
      ++state; break;
    default:
      CHECK_MSG(false, "uncategorized NodeKind in the spec-count test");
    }
  }
  CHECK(control == 15);
  CHECK(memory == 15);
  CHECK(calls == 6);
  CHECK(typeOps == 2);
  CHECK(guards == 1);
  CHECK(constants == 7);
  CHECK(arith == 36);
  CHECK(cmp == 6);
  CHECK(conv == 23);
  CHECK(merges == 1);
  CHECK(vector == 9);
  CHECK(pea == 2);
  CHECK(tagged == 3);
  CHECK(state == 1);
}
