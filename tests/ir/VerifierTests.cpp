// B-2 IR tests - verifier acceptance and rejection (Rules 40, 126).
// Positive: minimal sound graph, loop shape, call+guard+speculation, PEA
// materialization closure, vector shape. Negative: every malformed axis the
// charter lists (dangling ids, missing FrameState, discontinuous memory
// chains, bad metadata, cyclic materialization, vector/tagged violations).

#include "TestHarness.h"

#include <b2/ir/Graph.h>
#include <b2/ir/Node.h>
#include <b2/ir/Printer.h>
#include <b2/ir/Serialize.h>
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
  // cannot create cycles - callers must already exist) and attach LIVE
  // snapshot nodes to both descriptors so the walk exercises the cycle
  // itself (the MSG-20260901-002 liveness check would otherwise fire
  // first and mask the steps-bound diagnostic).
  const ir::FrameStateId f0 = g.replayFrameStateDesc(1, 0, 1, {});
  const ir::FrameStateId f1 = g.replayFrameStateDesc(1, 0, 0, {});
  (void)f0;
  (void)f1;
  const ir::NodeId fs0 = g.makeFrameState(1, 0, {a});
  const ir::NodeId fs1 = g.makeFrameState(1, 0, {a});
  g.nodeForReplay(fs0).payload = 0; // live node for the cyclic desc 0
  g.nodeForReplay(fs1).payload = 1; // live node for the cyclic desc 1
  CHECK(!ir::verify(g).ok);
}

B2_TEST(verifier_rejects_materialize_of_non_vobj) {
  ir::Graph g;
  const ir::NodeId notVobj = g.constantI(1);
  g.make(NodeKind::Materialize, {g.startNode(), g.startNode(), notVobj});
  CHECK(!ir::verify(g).ok);
}

// --- MSG-20260831-009: resultTypeOf rows match the spec (4.7/4.8) -------------------

B2_TEST(verifier_result_types_comparisons_and_widening_match_spec) {
  // The five previously mis-grouped rows: comparisons yield Int
  // ((a, b) -> int, Node.h / ir_spec 4.7) and I2L widens to Long
  // (ir_spec 4.8); they had been grouped with their operand families
  // and returned Long/Float/Float/Double/Double.
  ir::Graph g;
  const ir::NodeId i = g.constantI(1);
  const ir::NodeId l = g.constantL(2);
  const ir::NodeId f = g.constantF(3.0F);
  const ir::NodeId d = g.constantD(4.0);
  CHECK(ir::resultTypeOf(g, g.make(NodeKind::CmpL, {l, l})) ==
        ir::IRType::Int);
  CHECK(ir::resultTypeOf(g, g.make(NodeKind::CmpFl, {f, f})) ==
        ir::IRType::Int);
  CHECK(ir::resultTypeOf(g, g.make(NodeKind::CmpFg, {f, f})) ==
        ir::IRType::Int);
  CHECK(ir::resultTypeOf(g, g.make(NodeKind::CmpDl, {d, d})) ==
        ir::IRType::Int);
  CHECK(ir::resultTypeOf(g, g.make(NodeKind::CmpDg, {d, d})) ==
        ir::IRType::Int);
  CHECK(ir::resultTypeOf(g, g.make(NodeKind::I2L, {i})) == ir::IRType::Long);
}

B2_TEST(verifier_accepts_i2l_l2i_round_trip_and_cmp_int_slots) {
  // The exact repro shapes from MSG-20260831-009, previously rejected as
  // "operand has type double, expected long" (L2I over I2L) and
  // "operand has type long, expected int" (a comparison feeding an Int
  // slot): the round trip and every comparison family now flow through
  // their typed consumer slots.
  ir::Graph g;
  const ir::NodeId zero = g.constantI(0);
  const ir::NodeId one = g.constantI(1);
  const ir::NodeId l0 = g.constantL(0);
  const ir::NodeId l1 = g.constantL(1);
  const ir::NodeId f = g.constantF(1.5F);
  const ir::NodeId d = g.constantD(2.5);
  const ir::NodeId wide = g.make(NodeKind::I2L, {one});
  const ir::NodeId narrow = g.make(NodeKind::L2I, {wide});
  const ir::NodeId eq =
      g.make(NodeKind::EqI, {g.make(NodeKind::CmpL, {l0, l1}), zero});
  const ir::NodeId ne =
      g.make(NodeKind::NeI, {g.make(NodeKind::CmpFl, {f, f}), zero});
  const ir::NodeId lt =
      g.make(NodeKind::LtI, {g.make(NodeKind::CmpDg, {d, d}), zero});
  const ir::NodeId o0 = g.make(NodeKind::OrI, {eq, ne});
  const ir::NodeId o1 = g.make(NodeKind::OrI, {o0, lt});
  const ir::NodeId o2 = g.make(NodeKind::OrI, {o1, narrow});
  g.make(NodeKind::Return, {g.startNode(), o2});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message);
}

B2_TEST(verifier_rejects_int_operands_for_long_comparison) {
  ir::Graph g;
  const ir::NodeId i = g.constantI(1);
  // The result-side fix must not relax the operand side: CmpL still
  // takes (Long, Long) (Rule 33 - mismatches are errors, never coercions).
  g.make(NodeKind::CmpL, {i, i});
  CHECK(!ir::verify(g).ok);
}

// --- MSG-20260901-004: memory producers inside loops --------------------------------
//
// The walk previously reported "memory chain cycle" for every loop whose
// body writes memory: the header memory Phi's BACKEDGE input chains
// through the body producers back to the header itself (a legal loop
// closure). An on-path Phi revisit is now skipped; a non-Phi revisit
// stays a cycle. Shapes: (a) putfield in a loop, (b) a call in a loop,
// (c) an allocation in a loop, (d) the loop-invariant self-input Phi
// (pin), (e) a real non-Phi cycle behind a loop Phi (still rejected).

namespace {

// The shared loop skeleton: iv counts 0..3; memory threads through
// mphi (entry = Start); each test appends its own body memory producer
// as the phi's backedge input.
void mkLoopSkeleton(ir::Graph& g, ir::NodeId& mphi, ir::NodeId& body,
                    ir::NodeId& exit, ir::NodeId& iv) {
  const ir::NodeId zero = g.constantI(0);
  const ir::NodeId one = g.constantI(1);
  const ir::NodeId three = g.constantI(3);
  const ir::NodeId loop = g.make(NodeKind::LoopBegin, {g.startNode()});
  iv = g.make(NodeKind::Phi, {loop, zero});
  mphi = g.make(NodeKind::Phi, {loop, g.startNode()});
  const ir::NodeId inc = g.make(NodeKind::AddI, {iv, one});
  const ir::NodeId cmp = g.make(NodeKind::CmpI, {inc, three});
  const ir::NodeId iff = g.make(NodeKind::If, {loop, cmp});
  body = g.make(NodeKind::IfTrue, {iff});
  exit = g.make(NodeKind::IfFalse, {iff});
  g.appendInput(iv, inc);
  g.appendInput(loop, g.make(NodeKind::LoopEnd, {body}));
}

} // namespace

B2_TEST(verifier_accepts_putfield_in_loop) {
  // (a) The MSG-20260901-004 repro: the FIRST body producer reads the
  // header phi; the phi's backedge input is the store. The post-loop
  // load's backward walk closes the backedge at the header phi.
  ir::Graph g;
  ir::NodeId mphi, body, exit, iv;
  mkLoopSkeleton(g, mphi, body, exit, iv);
  const ir::NodeId obj = g.constantNull();
  const ir::NodeId v = g.constantI(7);
  const ir::NodeId st =
      g.make(NodeKind::StoreField, {body, mphi, obj, v}, 1);
  g.appendInput(mphi, st);
  const ir::NodeId lfs = g.makeFrameState(1, 5, {iv});
  const ir::NodeId lx = g.make(NodeKind::LoopExit, {exit, lfs});
  const ir::NodeId ld = g.make(NodeKind::LoadField, {lx, mphi, obj}, 1,
                               static_cast<std::uint32_t>(ir::IRType::Int));
  g.make(NodeKind::Return, {lx, ld});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message);
}

B2_TEST(verifier_accepts_call_in_loop) {
  // (b) A call is a memory producer: call.mem = the header phi, the
  // phi's backedge input = the call. The most common real-world inline
  // candidate shape (the canonical dispatch-profile loop program).
  ir::Graph g;
  ir::NodeId mphi, body, exit, iv;
  mkLoopSkeleton(g, mphi, body, exit, iv);
  const ir::NodeId obj = g.constantNull();
  const ir::NodeId cfs = g.makeFrameState(1, 2, {iv});
  const ir::NodeId call = g.make(
      NodeKind::CallStatic, {body, mphi, iv, cfs}, 7,
      static_cast<std::uint32_t>(ir::IRType::Int));
  g.appendInput(mphi, call);
  const ir::NodeId lfs = g.makeFrameState(1, 5, {iv});
  const ir::NodeId lx = g.make(NodeKind::LoopExit, {exit, lfs});
  const ir::NodeId ld = g.make(NodeKind::LoadField, {lx, mphi, obj}, 1,
                               static_cast<std::uint32_t>(ir::IRType::Int));
  g.make(NodeKind::Return, {lx, ld});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message);
}

B2_TEST(verifier_accepts_allocation_in_loop) {
  // (c) new + putfield on the fresh object: New produces memory state
  // but READS none, so the store's Mem input chains through the
  // allocation (not the header phi) and the walk terminates at the New;
  // the phi's backedge input is the store. Pins the allocation shape.
  ir::Graph g;
  ir::NodeId mphi, body, exit, iv;
  mkLoopSkeleton(g, mphi, body, exit, iv);
  const ir::NodeId obj = g.constantNull();
  const ir::NodeId v = g.constantI(7);
  const ir::NodeId alloc = g.make(NodeKind::New, {body}, 3);
  const ir::NodeId st =
      g.make(NodeKind::StoreField, {body, alloc, alloc, v}, 1);
  g.appendInput(mphi, st);
  const ir::NodeId lfs = g.makeFrameState(1, 5, {iv});
  const ir::NodeId lx = g.make(NodeKind::LoopExit, {exit, lfs});
  const ir::NodeId ld = g.make(NodeKind::LoadField, {lx, mphi, obj}, 1,
                               static_cast<std::uint32_t>(ir::IRType::Int));
  g.make(NodeKind::Return, {lx, ld});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message);
}

B2_TEST(verifier_accepts_loop_invariant_memory_self_input_phi) {
  // (d) The loop-invariant memory marker: the phi's backedge value is
  // the phi ITSELF (the body produced no memory the header has not
  // seen). Pins the pre-existing self-input skip.
  ir::Graph g;
  ir::NodeId mphi, body, exit, iv;
  mkLoopSkeleton(g, mphi, body, exit, iv);
  g.appendInput(mphi, mphi);
  const ir::NodeId obj = g.constantNull();
  const ir::NodeId lfs = g.makeFrameState(1, 5, {iv});
  const ir::NodeId lx = g.make(NodeKind::LoopExit, {exit, lfs});
  const ir::NodeId ld = g.make(NodeKind::LoadField, {lx, mphi, obj}, 1,
                               static_cast<std::uint32_t>(ir::IRType::Int));
  g.make(NodeKind::Return, {lx, ld});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message);
}

B2_TEST(verifier_rejects_non_phi_cycle_behind_loop_phi) {
  // (e) A genuine two-store cycle reachable through the phi's backedge:
  // the walk skips ONLY the phi closure itself; a non-Phi on-path
  // revisit behind it is still a cycle error.
  ir::Graph g;
  ir::NodeId mphi, body, exit, iv;
  mkLoopSkeleton(g, mphi, body, exit, iv);
  const ir::NodeId obj = g.constantNull();
  const ir::NodeId v = g.constantI(7);
  const ir::NodeId s1 =
      g.make(NodeKind::StoreField, {body, mphi, obj, v}, 1);
  const ir::NodeId s2 =
      g.make(NodeKind::StoreField, {body, s1, obj, v}, 2);
  g.setInput(s1, 1, s2); // s1.mem = s2 while s2.mem = s1: a real cycle
  g.appendInput(mphi, s2);
  const ir::NodeId lfs = g.makeFrameState(1, 5, {iv});
  const ir::NodeId lx = g.make(NodeKind::LoopExit, {exit, lfs});
  const ir::NodeId ld = g.make(NodeKind::LoadField, {lx, mphi, obj}, 1,
                               static_cast<std::uint32_t>(ir::IRType::Int));
  g.make(NodeKind::Return, {lx, ld});
  CHECK(!ir::verify(g).ok);
}

// --- MSG-20260901-002: chain targets must be live ----------------------------------

B2_TEST(verifier_rejects_caller_chain_to_dead_snapshot) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId cond = g.constantI(1);
  // A caller snapshot KILLED while a live callee FrameState still chains
  // to its descriptor: the desc survives as side-table data, but no live
  // node carries the caller-frame slot values anymore - the deoptimizer
  // would read a destroyed snapshot (Rule 75). The single diagnostic
  // lands on the live callee whose chain no longer resolves.
  const ir::NodeId callerFs = g.makeFrameState(1, 8, {a});
  const ir::NodeId calleeFs =
      g.makeFrameState(2, 3, {a}, g.node(callerFs).payload);
  g.make(NodeKind::Guard, {g.startNode(), cond, calleeFs},
         static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 0);
  g.killNode(callerFs);
  const ir::VerifyResult r = ir::verify(g);
  CHECK(onlyDiagAbout(r, calleeFs));
}

// --- MSG-20260901-007: memory-chain DFS step belt ----------------------------------
//
// checkMemoryChains walks the memory chain with an explicit DFS stack: each
// visited node costs TWO while-iterations (one push-visit, one pop-visit), so
// a well-formed acyclic chain of L producers costs ~2L steps. The old belt
// was `nodeCount + 1`, which falsely tripped whenever `2L > nodeCount + 1`
// (any chain longer than half the graph). The belt is now `2 * nodeCount + 2`.
//
// This test builds the minimal repro from the bug report: a fresh allocation
// followed by 17 chained StoreFields and a Load that consumes the last
// store's memory state (22 nodes; longest walk = 18 producers ~ 35 steps).
// Under the old belt (23) the walk from the Load and the last several stores
// each reported "memory chain walk exceeded graph size (cycle?)" on this
// verifier-CLEAN acyclic graph. With the fix, it verifies clean.

B2_TEST(verifier_accepts_long_straight_memory_chain) {
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId alloc = g.make(NodeKind::New, {start}, 3);
  const ir::NodeId v = g.constantI(7);
  // 17 chained stores: S1.mem = alloc, S2.mem = S1, ..., S17.mem = S16.
  ir::NodeId prev = alloc;
  for (int i = 0; i < 17; ++i) {
    prev = g.make(NodeKind::StoreField, {start, prev, alloc, v},
                  static_cast<std::uint32_t>(i + 1));
  }
  // The Load consumes S17's memory state (keeps the chain live) and walks
  // back through all 17 stores + the New = 18 memory producers.
  const ir::NodeId ld = g.make(NodeKind::LoadField, {start, prev, alloc}, 1,
                               static_cast<std::uint32_t>(ir::IRType::Int));
  g.make(NodeKind::Return, {start, ld});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message);
}

// --- MSG-20260901-006: appendFrameStateVobj post-hoc vobj listing ------------------
//
// makeFrameState accepts a vobj list at creation time only; a pass that runs
// AFTER the builder (every optimization pass) can never use it. The new
// appendFrameStateVobj API splices a vobj into an existing descriptor's slice,
// grows its count, and shifts later descriptors' offsets. These tests pin the
// API contract: slice order, multi-desc offset repair, closure enforcement,
// bounds safety, and serialize round-trip.

B2_TEST(appendFrameStateVobj_appends_to_slice_and_verifies_clean) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId inner = g.makeVirtualObject(30, {a});
  // Create a FrameState with NO vobjs, then append one post-hoc.
  const ir::NodeId fsNode = g.makeFrameState(1, 4, {a});
  const ir::FrameStateId fsDesc = g.node(fsNode).payload;
  CHECK(g.frameStateVobjs(fsDesc).size() == 0);
  g.appendFrameStateVobj(fsDesc, inner);
  const auto vobjs = g.frameStateVobjs(fsDesc);
  CHECK(vobjs.size() == 1);
  CHECK(vobjs[0] == inner);
  g.make(NodeKind::Return, {g.startNode()});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message);
}

B2_TEST(appendFrameStateVobj_shifts_later_descs_offsets) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  const ir::NodeId v0 = g.makeVirtualObject(30, {a});
  const ir::NodeId v1 = g.makeVirtualObject(31, {b});
  const ir::NodeId v2 = g.makeVirtualObject(32, {a});
  const ir::NodeId v3 = g.makeVirtualObject(33, {b});
  // fsA: 2 vobjs [v0, v1]; fsB: 1 vobj [v2].
  const ir::NodeId fsA = g.makeFrameState(1, 4, {a}, ir::kInvalidFrameState,
                                         {v0, v1});
  const ir::NodeId fsB = g.makeFrameState(2, 8, {b}, ir::kInvalidFrameState,
                                         {v2});
  const ir::FrameStateId descA = g.node(fsA).payload;
  const ir::FrameStateId descB = g.node(fsB).payload;
  CHECK(g.frameStateVobjs(descA).size() == 2);
  CHECK(g.frameStateVobjs(descB).size() == 1);
  CHECK(g.frameStateVobjs(descB)[0] == v2);
  // Append v3 to fsA: insertPos = descA.vobjOffset + 2 (end of A's slice).
  g.appendFrameStateVobj(descA, v3);
  // A's slice grew to 3: [v0, v1, v3] in order.
  const auto aVobjs = g.frameStateVobjs(descA);
  CHECK(aVobjs.size() == 3);
  CHECK(aVobjs[0] == v0);
  CHECK(aVobjs[1] == v1);
  CHECK(aVobjs[2] == v3);
  // B's slice content is unchanged ([v2]) — its offset shifted by 1 but
  // the accessor resolves through the descriptor, so the slice is correct.
  const auto bVobjs = g.frameStateVobjs(descB);
  CHECK(bVobjs.size() == 1);
  CHECK(bVobjs[0] == v2);
  g.make(NodeKind::Return, {g.startNode()});
  CHECK(ir::verify(g).ok);
}

B2_TEST(appendFrameStateVobj_closure_violation_still_rejected) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  const ir::NodeId inner = g.makeVirtualObject(30, {a});
  const ir::NodeId outer =
      g.makeVirtualObject(31, {b, inner}); // outer references inner
  // Create a FrameState with NO vobjs, then append ONLY outer. The closure
  // check (ir_spec 7) must reject: inner is referenced by a field of the
  // listed outer but is not listed itself.
  const ir::NodeId fsNode = g.makeFrameState(1, 4, {a});
  const ir::FrameStateId fsDesc = g.node(fsNode).payload;
  g.appendFrameStateVobj(fsDesc, outer);
  g.make(NodeKind::Return, {g.startNode()});
  CHECK(!ir::verify(g).ok);
}

B2_TEST(appendFrameStateVobj_out_of_range_is_noop) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId v = g.makeVirtualObject(30, {a});
  const ir::NodeId fsNode = g.makeFrameState(1, 4, {a});
  const ir::FrameStateId fsDesc = g.node(fsNode).payload;
  const std::uint32_t fsCountBefore = g.frameStateCount();
  CHECK(g.frameStateVobjs(fsDesc).size() == 0);
  // Bogus desc ids: no-op (no crash, no mutation).
  g.appendFrameStateVobj(999, v);
  g.appendFrameStateVobj(ir::kInvalidFrameState, v);
  CHECK(g.frameStateCount() == fsCountBefore);
  CHECK(g.frameStateVobjs(fsDesc).size() == 0);
  g.make(NodeKind::Return, {g.startNode()});
  CHECK(ir::verify(g).ok);
}

B2_TEST(appendFrameStateVobj_round_trips_through_serialize) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  const ir::NodeId v0 = g.makeVirtualObject(30, {a});
  const ir::NodeId v1 = g.makeVirtualObject(31, {b});
  // Create fs with [v0], append v1 post-hoc -> [v0, v1].
  const ir::NodeId fsNode =
      g.makeFrameState(1, 4, {a}, ir::kInvalidFrameState, {v0});
  const ir::FrameStateId fsDesc = g.node(fsNode).payload;
  g.appendFrameStateVobj(fsDesc, v1);
  g.make(NodeKind::Return, {g.startNode()});
  CHECK(ir::verify(g).ok);
  const std::vector<std::uint8_t> bytes = ir::serialize(g);
  ir::Graph h;
  const ir::DeserializeResult r = ir::deserializeInto(bytes, h);
  CHECK_MSG(r.ok, r.error.message.c_str());
  CHECK(ir::verify(h).ok);
  CHECK(ir::print(h) == ir::print(g));
  CHECK(ir::serialize(h) == bytes);
  // The replayed graph's FrameState has the appended vobj in order.
  const auto hVobjs = h.frameStateVobjs(fsDesc);
  CHECK(hVobjs.size() == 2);
  CHECK(hVobjs[0] == v0);
  CHECK(hVobjs[1] == v1);
}
