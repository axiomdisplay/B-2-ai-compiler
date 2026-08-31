// B-2 IR tests - verifier acceptance and rejection (Rules 40, 126).
// Positive: minimal sound graph, loop shape, call+guard+speculation, PEA
// materialization closure, vector shape. Negative: every malformed axis the
// charter lists (dangling ids, missing FrameState, discontinuous memory
// chains, bad metadata, cyclic materialization, vector/tagged violations).

#include "TestHarness.h"

#include <b2/ir/Graph.h>
#include <b2/ir/Node.h>
#include <b2/ir/Printer.h>
#include <b2/ir/Verifier.h>

using namespace b2;
using b2::ir::NodeKind;

namespace {

bool onlyDiagAbout(const ir::VerifyResult& r, ir::NodeId n) {
  return !r.ok && r.diags.size() == 1 && r.diags[0].node == n;
}

} // namespace

B2_TEST(verifier_accepts_minimal_sound_graph) {
  ir::Graph g;
  g.make(NodeKind::Return, {g.startNode()});
  CHECK(ir::verify(g).ok);
}

B2_TEST(verifier_accepts_loop_shaped_graph_with_backedge_phi) {
  ir::Graph g;
  const ir::NodeId zero = g.constantI(0);
  const ir::NodeId one = g.constantI(1);
  const ir::NodeId ten = g.constantI(10);
  const ir::NodeId loop = g.make(NodeKind::LoopBegin, {g.startNode()});
  const ir::NodeId phi = g.make(NodeKind::Phi, {loop, zero});
  const ir::NodeId inc = g.make(NodeKind::AddI, {phi, one});
  const ir::NodeId cmp = g.make(NodeKind::CmpI, {inc, ten});
  const ir::NodeId iff = g.make(NodeKind::If, {loop, cmp});
  const ir::NodeId exit = g.make(NodeKind::IfFalse, {iff});
  // Backedge value arrives later: appendInput grows the Phi and LoopBegin.
  g.appendInput(phi, inc);
  g.appendInput(loop, g.make(NodeKind::LoopEnd, {g.make(NodeKind::IfTrue, {iff})}));
  const ir::NodeId lfs = g.makeFrameState(1, 5, {phi, inc});
  const ir::NodeId lx = g.make(NodeKind::LoopExit, {exit, lfs});
  g.make(NodeKind::Return, {lx, phi});
  CHECK(ir::verify(g).ok);
}

B2_TEST(verifier_accepts_call_with_framestate_and_speculative_guard) {
  ir::Graph g;
  const ir::NodeId arg = g.constantI(5);
  const ir::NodeId cond = g.constantI(1);
  const ir::NodeId gfs = g.makeFrameState(1, 2, {cond});
  const ir::NodeId guard =
      g.make(NodeKind::Guard, {g.startNode(), cond, gfs},
             static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 2);

  const ir::NodeId dep = g.addDependency({ir::Dependency::Kind::MethodBody, 3});
  ir::SpecMeta sm;
  sm.kind = ir::SpecMeta::Kind::MethodFinal;
  sm.source = ir::SpecMeta::Source::PGO;
  sm.confidence = 9000;
  sm.guard = guard;
  sm.deoptTarget = 2;
  sm.cost = 8;
  sm.dependency = dep;
  sm.rollback = ir::SpecMeta::Rollback::None;
  const ir::SpecMetaId sid = g.addSpecMeta(sm);
  g.attachSpecMeta(guard, sid);

  const ir::NodeId cfs = g.makeFrameState(1, 3, {arg});
  const ir::NodeId call = g.make(
      NodeKind::CallVirtual, {g.startNode(), g.startNode(), arg, cfs}, 9,
      static_cast<std::uint32_t>(ir::IRType::Int));
  g.make(NodeKind::Return, {g.startNode(), call});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message);
}

B2_TEST(verifier_accepts_pea_materialization_with_closure) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  // Deopt-listed pair: outer references inner, both in the same list.
  const ir::NodeId inner = g.makeVirtualObject(30, {a});
  const ir::NodeId outer = g.makeVirtualObject(31, {b, inner});
  g.makeFrameState(1, 4, {a}, ir::kInvalidFrameState, {inner, outer});
  g.make(NodeKind::Return, {g.startNode()});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message);
}

B2_TEST(verifier_accepts_pea_escape_materialization_of_flat_vobj) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  const ir::NodeId vo = g.makeVirtualObject(30, {a, b});
  const ir::NodeId mat =
      g.make(NodeKind::Materialize, {g.startNode(), g.startNode(), vo});
  g.make(NodeKind::Return, {g.startNode(), mat});
  CHECK(ir::verify(g).ok);
}

B2_TEST(verifier_rejects_materialize_of_still_virtual_field) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId inner = g.makeVirtualObject(30, {a});
  const ir::NodeId outer = g.makeVirtualObject(31, {inner});
  g.make(NodeKind::Materialize, {g.startNode(), g.startNode(), outer});
  g.make(NodeKind::Return, {g.startNode()});
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_accepts_vector_shape_with_mask) {
  ir::Graph g;
  const ir::NodeId s0 = g.constantI(1);
  const ir::NodeId s1 = g.constantI(2);
  const ir::NodeId s2 = g.constantI(3);
  const ir::NodeId s3 = g.constantI(4);
  const ir::NodeId v = g.make(NodeKind::VectorPack, {s0, s1, s2, s3},
                              static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId bc = g.make(
      NodeKind::VectorBroadcast, {s0},
      static_cast<std::uint32_t>(ir::IRType::Int),
      ir::packVecPayload(ir::IRType::Int, 4));
  // Masks are VectorI8 lanes of 0/1 (SWLP convention).
  const ir::NodeId mask = g.make(
      NodeKind::VectorBroadcast, {s0},
      static_cast<std::uint32_t>(ir::IRType::Int8),
      ir::packVecPayload(ir::IRType::Int8, 4));
  CHECK(ir::resultTypeOf(g, mask) == ir::IRType::VectorI8);
  CHECK(ir::resultTypeOf(g, v) == ir::IRType::VectorI32);
  const ir::NodeId op = g.make(
      NodeKind::VectorMaskOp, {mask, v, bc},
      static_cast<std::uint32_t>(ir::VectorOp::Add),
      ir::packVecPayload(ir::IRType::Int, 4));
  const ir::NodeId red = g.make(
      NodeKind::VectorReduce, {op},
      static_cast<std::uint32_t>(ir::VectorOp::Sum),
      ir::packVecPayload(ir::IRType::Int, 4));
  CHECK(ir::resultTypeOf(g, red) == ir::IRType::Int);
  g.make(NodeKind::Return, {g.startNode(), red});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message);
}

B2_TEST(verifier_rejects_dangling_input_id) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId add = g.make(NodeKind::AddI, {a, 999}); // 999 does not exist
  const ir::VerifyResult r = ir::verify(g);
  CHECK(onlyDiagAbout(r, add));
}

B2_TEST(verifier_rejects_self_reference) {
  ir::Graph g;
  const ir::NodeId add = g.make(NodeKind::AddI, {1, 1});
  g.setInput(add, 0, add);
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_input_of_dead_node) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  g.killNode(b);
  g.make(NodeKind::AddI, {a, b});
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_missing_framestate_on_guard) {
  ir::Graph g;
  const ir::NodeId cond = g.constantI(1);
  const ir::NodeId guard = g.make(NodeKind::Guard, {g.startNode(), cond},
                                  static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 0);
  (void)guard;
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_call_without_framestate) {
  ir::Graph g;
  const ir::NodeId arg = g.constantI(5);
  g.make(NodeKind::CallStatic, {g.startNode(), g.startNode(), arg}, 7,
         static_cast<std::uint32_t>(ir::IRType::Int));
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_speculative_node_without_specmeta) {
  ir::Graph g;
  const ir::NodeId cond = g.constantI(1);
  const ir::NodeId fs = g.makeFrameState(1, 0, {cond});
  const ir::NodeId guard =
      g.make(NodeKind::Guard, {g.startNode(), cond, fs},
             static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 0);
  g.nodeForReplay(guard).flags.set(ir::NodeFlag::Speculative); // no meta
  const ir::VerifyResult r = ir::verify(g);
  CHECK(!r.ok);
}

B2_TEST(verifier_rejects_pgo_speculation_without_dependency) {
  ir::Graph g;
  const ir::NodeId cond = g.constantI(1);
  const ir::NodeId fs = g.makeFrameState(1, 0, {cond});
  const ir::NodeId guard =
      g.make(NodeKind::Guard, {g.startNode(), cond, fs},
             static_cast<std::uint32_t>(ir::GuardKind::TypeProfile), 1);
  ir::SpecMeta sm; // dependency left invalid, source PGO, guard set below
  sm.kind = ir::SpecMeta::Kind::TypeMonomorphic;
  sm.source = ir::SpecMeta::Source::PGO;
  sm.confidence = 8000;
  sm.guard = guard;
  sm.deoptTarget = 1;
  sm.cost = 3;
  sm.dependency = ir::kInvalidDependency; // Rule 42 violation
  const ir::SpecMetaId sid = g.addSpecMeta(sm);
  g.attachSpecMeta(guard, sid);
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_discontinuous_memory_chain) {
  ir::Graph g;
  const ir::NodeId obj = g.constantNull();
  // A load whose mem input is a LOAD (loads do not produce memory state).
  const ir::NodeId l1 = g.make(NodeKind::LoadField, {g.startNode(), g.startNode(), obj},
                               5, static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId l2 = g.make(NodeKind::LoadField, {g.startNode(), l1, obj}, 6,
                               static_cast<std::uint32_t>(ir::IRType::Int));
  (void)l2;
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_memory_chain_cycle) {
  ir::Graph g;
  const ir::NodeId obj = g.constantNull();
  const ir::NodeId v = g.constantI(7);
  // store1.mem = store2, store2.mem = store1.
  const ir::NodeId s1 =
      g.make(NodeKind::StoreField, {g.startNode(), g.startNode(), obj, v}, 1);
  const ir::NodeId s2 = g.make(NodeKind::StoreField, {g.startNode(), s1, obj, v}, 2);
  g.setInput(s1, 1, s2);
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_phi_region_arity_mismatch) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  const ir::NodeId cmp = g.make(NodeKind::CmpI, {a, b});
  const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), cmp});
  const ir::NodeId t = g.make(NodeKind::IfTrue, {iff});
  const ir::NodeId f = g.make(NodeKind::IfFalse, {iff});
  const ir::NodeId region = g.make(NodeKind::Region, {t, f});
  // Phi with only one value for a two-predecessor region.
  g.make(NodeKind::Phi, {region, a});
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_wrong_projection_parent) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  const ir::NodeId cmp = g.make(NodeKind::CmpI, {a, b});
  const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), cmp});
  (void)iff;
  // IfTrue directly on Start (not on the If).
  const ir::NodeId bad = g.make(NodeKind::IfTrue, {g.startNode()});
  (void)bad;
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_type_mismatched_arithmetic_operands) {
  ir::Graph g;
  const ir::NodeId i = g.constantI(1);
  const ir::NodeId l = g.constantL(2);
  g.make(NodeKind::AddI, {i, l}); // Long feeding an Int add: Rule 33.
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_control_node_as_data_operand) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  g.make(NodeKind::AddI, {a, g.startNode()}); // Bottom type operand
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_terminal_node_with_users) {
  ir::Graph g;
  const ir::NodeId ret = g.make(NodeKind::Return, {g.startNode()});
  const ir::NodeId a = g.constantI(1);
  g.make(NodeKind::AddI, {a, ret}); // using a Return as data
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_bad_vector_lane_count) {
  ir::Graph g;
  const ir::NodeId s = g.constantI(1);
  // 3 lanes: not a power of two.
  g.make(NodeKind::VectorPack, {s, s, s},
         static_cast<std::uint32_t>(ir::IRType::Int));
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_vector_operand_type_mismatch) {
  ir::Graph g;
  const ir::NodeId si = g.constantI(1);
  const ir::NodeId sl = g.constantL(2);
  // Lane type says Int but one input is Long.
  g.make(NodeKind::VectorPack, {si, sl},
         static_cast<std::uint32_t>(ir::IRType::Int));
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_fp_vector_bitwise_op) {
  ir::Graph g;
  const ir::NodeId f0 = g.constantF(1.0F);
  const ir::NodeId f1 = g.constantF(2.0F);
  const ir::NodeId va = g.make(NodeKind::VectorPack, {f0, f1},
                               static_cast<std::uint32_t>(ir::IRType::Float));
  const ir::NodeId vb = g.make(NodeKind::VectorPack, {f0, f1},
                               static_cast<std::uint32_t>(ir::IRType::Float));
  // And is not legal on float lanes (SWLP: Java FP semantics).
  g.make(NodeKind::VectorOp, {va, vb},
         static_cast<std::uint32_t>(ir::VectorOp::And),
         ir::packVecPayload(ir::IRType::Float, 2));
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_unguarded_polymorphic_rep_transition) {
  ir::Graph g;
  const ir::NodeId v = g.make(NodeKind::TagValue, {g.constantI(1)},
                              static_cast<std::uint32_t>(ir::ValueRep::UnboxedInt32),
                              static_cast<std::uint32_t>(ir::ValueRep::Polymorphic));
  (void)v;
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_accepts_guarded_polymorphic_rep_transition) {
  ir::Graph g;
  const ir::NodeId tagged = g.make(NodeKind::TagValue, {g.constantI(1)},
                                   static_cast<std::uint32_t>(ir::ValueRep::UnboxedInt32),
                                   static_cast<std::uint32_t>(ir::ValueRep::NaNBoxedRef));
  const ir::NodeId fs = g.makeFrameState(1, 0, {tagged});
  const ir::NodeId rg = g.make(
      NodeKind::RepTransitionGuard, {g.startNode(), tagged, fs},
      static_cast<std::uint32_t>(ir::ValueRep::NaNBoxedRef),
      static_cast<std::uint32_t>(ir::ValueRep::CompressedOop));
  const ir::NodeId dep = g.addDependency({ir::Dependency::Kind::ProfileCounter, 9});
  ir::SpecMeta sm;
  sm.kind = ir::SpecMeta::Kind::TypeMonomorphic;
  sm.source = ir::SpecMeta::Source::PGO;
  sm.confidence = 9900;
  sm.guard = rg;
  sm.deoptTarget = 6;
  sm.cost = 2;
  sm.dependency = dep;
  sm.rollback = ir::SpecMeta::Rollback::Compensated;
  const ir::SpecMetaId sid = g.addSpecMeta(sm);
  g.attachSpecMeta(rg, sid);
  g.make(NodeKind::Return, {g.startNode()});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message);
}

B2_TEST(verifier_rejects_cyclic_materialization_graph) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  const ir::NodeId v1 = g.makeVirtualObject(30, {a});
  const ir::NodeId v2 = g.makeVirtualObject(31, {b, v1});
  // Make v1 reference v2: cycle.
  g.appendInput(v1, v2);
  const ir::NodeId fs = g.makeFrameState(1, 4, {a}, ir::kInvalidFrameState,
                                         {v1, v2});
  (void)fs;
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_materialization_closure_violation) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  const ir::NodeId inner = g.makeVirtualObject(30, {a});
  const ir::NodeId outer = g.makeVirtualObject(31, {b, inner});
  // FS materializes only the outer; the inner never gets materialized here.
  g.makeFrameState(1, 4, {a}, ir::kInvalidFrameState, {outer});
  g.make(NodeKind::Return, {g.startNode()});
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_framestate_caller_cycle) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  // Build a cyclic caller chain through the replay API (the builder API
  // cannot create cycles - callers must already exist).
  const ir::FrameStateId f0 = g.replayFrameStateDesc(1, 0, 1, {});
  const ir::FrameStateId f1 = g.replayFrameStateDesc(1, 0, 0, {});
  (void)f0;
  (void)f1;
  const ir::NodeId fs = g.makeFrameState(1, 0, {a});
  g.nodeForReplay(fs).payload = 0; // point at the cyclic descriptor
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_materialize_of_non_vobj) {
  ir::Graph g;
  const ir::NodeId notVobj = g.constantI(1);
  g.make(NodeKind::Materialize, {g.startNode(), g.startNode(), notVobj});
  CHECK(!ir::verify(g).ok);
}
