// B-2 passes tests - SCCP (registry key 38; charter rows 37-39).
//
// Discipline: Rule 35 - one golden test per rewrite rule family, RBC-built
// graphs for the builder-realistic shapes and hand-built graphs for the
// lattice corners the builder cannot produce (self-marker loop phis,
// never-executable backedges, unresolved-but-used Top values). Every test
// runs the IR verifier (Rule 40) and pins: the phi-meet rule (same value
// from different nodes folds; different values refuse), the executable-
// edge rule (a decided If contributes only its taken arm), the loop
// optimism (self-marker and zero-trip shapes resolve; varying counters
// refuse), the completion rule (an unresolved-but-used Top collapses the
// meet to Bot instead of claiming), the shared-evaluator semantics (the
// same fold catalog constfold uses, through the lattice), idempotency
// (Rule 10: a second run rewrites nothing), determinism (Rule 124:
// double-build byte-identical prints), and the kill switch (Rule 132).

#include "TestHarness.h"

#include <cstdio>
#include <string>
#include <vector>

#include <b2/ir/Node.h>
#include <b2/ir/Printer.h>
#include <b2/ir/Verifier.h>
#include <b2/passes/GraphBuilder.h>
#include <b2/passes/Passes.h>
#include <b2/rbc/RbcBuilder.h>

using namespace b2;
using ir::NodeKind;
using PK = passes::PassKey;

namespace {

// The same build-and-verify discipline as PassTests.cpp.
void buildOk(const rbc::Method& m, ir::Graph& g) {
  g.~Graph();
  new (&g) ir::Graph();
  passes::CounterResolver res;
  passes::BuildResult br = passes::buildGraph(m, res, g, ir::MethodId{7});
  CHECK_MSG(br.ok, br.diags.empty() ? "build failed"
                                    : br.diags[0].message.c_str());
  const ir::VerifyResult vr = ir::verify(g);
  CHECK_MSG(vr.ok, vr.diags.empty() ? "IR verify failed"
                                    : vr.diags[0].message.c_str());
}

passes::PassResult passOk(ir::Graph& g, PK key) {
  passes::PassResult r = passes::runSinglePass(g, key);
  if (!r.ok) {
    CHECK_MSG(false, r.diags.empty()
                         ? "pass failed"
                         : r.diags[0].message.c_str());
  }
  return r;
}

[[nodiscard]] bool verifyOk(const ir::Graph& g) {
  return ir::verify(g).ok;
}

[[nodiscard]] std::size_t countLiveUserKind(const ir::Graph& g, NodeKind k) {
  std::size_t n = 0;
  for (ir::NodeId i = 0; i < g.nodeCount(); ++i) {
    if (g.node(i).isDead() || g.node(i).kind != k) {
      continue;
    }
    for (const ir::Use& u : g.usesOf(i)) {
      if (!g.node(u.user).isDead()) {
        ++n;
        break;
      }
    }
  }
  return n;
}

[[nodiscard]] std::size_t countKind(const ir::Graph& g, NodeKind k) {
  std::size_t n = 0;
  for (ir::NodeId i = 0; i < g.nodeCount(); ++i) {
    if (!g.node(i).isDead() && g.node(i).kind == k) {
      ++n;
    }
  }
  return n;
}

[[nodiscard]] bool retValueIs(const ir::Graph& g, NodeKind k,
                              std::int64_t bits) {
  for (ir::NodeId i = 0; i < g.nodeCount(); ++i) {
    if (g.node(i).isDead() || g.node(i).kind != ir::NodeKind::Return) {
      continue;
    }
    if (g.node(i).numInputs < 2) {
      continue;
    }
    const ir::NodeId v = g.input(i, 1);
    return !g.node(v).isDead() && g.node(v).kind == k &&
           g.node(v).constValue == bits;
  }
  return false;
}

// main(I)I: x = flag ? 5 : arm2; return x  - the merge phi over a
// PARAMETER-driven branch (both projections executable; the phi's value
// comes from the MEET, not from edge pruning).
void buildParamMerge(rbc::Method& m, std::int32_t armThen,
                     std::int32_t armElse) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label elseL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  b.emitRegSlot(rbc::Op::Iload, 0, 0); // the parameter
  b.emitRegBranch(rbc::Op::Ifne, 0, thenL);
  b.emitBranch(rbc::Op::Goto, elseL);
  b.bind(thenL);
  b.emitRegImm(rbc::Op::Iconst, 1, static_cast<std::uint32_t>(armThen));
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.emitBranch(rbc::Op::Goto, endL);
  b.bind(elseL);
  b.emitRegImm(rbc::Op::Iconst, 1, static_cast<std::uint32_t>(armElse));
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.bind(endL);
  b.emitRegSlot(rbc::Op::Iload, 2, 0);
  b.emitReg(rbc::Op::Ireturn, 2);
  CHECK(b.finish(m).ok);
}

// main(I)I: x = flag ? 5 : (10 - 5); return x - the SAME VALUE from two
// DIFFERENT expressions. The builder interns constants (two iconst 5s are
// one node and the trivial-phi cleanup collapses that), so the
// builder-reachable "equal-value meet" needs one arm non-literal: the phi
// merges [ConstantI(5), SubI(10, 5)] - different nodes, same value. Only
// the lattice meet can fold this shape.
void buildParamMergeExpr(rbc::Method& m) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label elseL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  b.emitRegSlot(rbc::Op::Iload, 0, 0); // the parameter
  b.emitRegBranch(rbc::Op::Ifne, 0, thenL);
  b.emitBranch(rbc::Op::Goto, elseL);
  b.bind(thenL);
  b.emitRegImm(rbc::Op::Iconst, 1, 5);
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.emitBranch(rbc::Op::Goto, endL);
  b.bind(elseL);
  b.emitRegImm(rbc::Op::Iconst, 1, 10);
  b.emitRegImm(rbc::Op::Iconst, 2, 5);
  b.emitRegRegReg(rbc::Op::Isub, 1, 1, 2);
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.bind(endL);
  b.emitRegSlot(rbc::Op::Iload, 3, 0);
  b.emitReg(rbc::Op::Ireturn, 3);
  CHECK(b.finish(m).ok);
}

} // namespace

// --- the phi meet rule ---------------------------------------------------------

B2_TEST(sccp_phi_meet_same_constant_folds) {
  // x = flag ? 5 : (10 - 5): the phi merges [ConstantI(5), SubI(10, 5)] -
  // different nodes, the same value. Trivial-phi needs identical nodes,
  // GVN has not run: only the meet folds it. The SubI AND both slot phis
  // resolve (the builder emits one phi per slot reader).
  rbc::Method m;
  buildParamMergeExpr(m);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::Phi) >= 2);
  CHECK(countKind(g, NodeKind::SubI) == 1);
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 3);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  CHECK(countLiveUserKind(g, NodeKind::SubI) == 0);
  CHECK(retValueIs(g, NodeKind::ConstantI, 5));
  CHECK(verifyOk(g));
}

B2_TEST(sccp_phi_meet_distinct_constants_refuse) {
  // meet(5, 9) = Bot: the phi is genuinely variable; SCCP must not claim.
  rbc::Method m;
  buildParamMerge(m, 5, 9);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 0);
  CHECK(countLiveUserKind(g, NodeKind::Phi) >= 1);
  CHECK(verifyOk(g));
}

B2_TEST(sccp_arithmetic_cascade_through_the_meet) {
  // x = flag ? 5 : (10 - 5); return x + 7: the phi resolves to 5 THROUGH
  // the meet and the AddI follows to 12 in the same propagation.
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label elseL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegBranch(rbc::Op::Ifne, 0, thenL);
  b.emitBranch(rbc::Op::Goto, elseL);
  b.bind(thenL);
  b.emitRegImm(rbc::Op::Iconst, 1, 5);
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.emitBranch(rbc::Op::Goto, endL);
  b.bind(elseL);
  b.emitRegImm(rbc::Op::Iconst, 1, 10);
  b.emitRegImm(rbc::Op::Iconst, 2, 5);
  b.emitRegRegReg(rbc::Op::Isub, 1, 1, 2);
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.bind(endL);
  b.emitRegSlot(rbc::Op::Iload, 2, 0);
  b.emitRegImm(rbc::Op::Iconst, 3, 7);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 2, 3);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  // SubI + the two slot phis + the AddI.
  CHECK(r.telemetry.sccpConstants == 4);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  CHECK(countLiveUserKind(g, NodeKind::SubI) == 0);
  CHECK(countLiveUserKind(g, NodeKind::AddI) == 0);
  CHECK(retValueIs(g, NodeKind::ConstantI, 12));
  CHECK(verifyOk(g));
}

// --- the executable-edge rule (the "conditional" in SCCP) -----------------------

B2_TEST(sccp_decided_branch_prunes_the_dead_arm) {
  // if (1 == 1) x = 7 else x = 9: EqI(1,1) resolves through the LATTICE
  // (no constfold run first), the If takes only its true projection, and
  // the merge phi sees ONE executable edge: x = 7.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label elseL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 1);
  b.emitRegImm(rbc::Op::Iconst, 1, 1);
  b.emitRegRegBranch(rbc::Op::IfIcmpeq, 0, 1, thenL);
  b.emitBranch(rbc::Op::Goto, elseL);
  b.bind(thenL);
  b.emitRegImm(rbc::Op::Iconst, 2, 7);
  b.emitSlotReg(rbc::Op::Istore, 2, 0);
  b.emitBranch(rbc::Op::Goto, endL);
  b.bind(elseL);
  b.emitRegImm(rbc::Op::Iconst, 2, 9);
  b.emitSlotReg(rbc::Op::Istore, 2, 0);
  b.bind(endL);
  b.emitRegSlot(rbc::Op::Iload, 2, 0);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  // The EqI condition folds to 1 AND both slot phis prune to the then-arm.
  CHECK(r.telemetry.sccpConstants == 3);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  CHECK(retValueIs(g, NodeKind::ConstantI, 7));
  CHECK(verifyOk(g));
}

B2_TEST(sccp_param_condition_keeps_both_edges) {
  // A PARAMETER condition: both projections stay executable, the meet
  // sees both arms - distinct constants refuse (belt: the executable-edge
  // machinery is not fooled into pruning a live branch).
  rbc::Method m;
  buildParamMerge(m, 5, 9);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 0);
  CHECK(countKind(g, NodeKind::IfTrue) >= 1);
  CHECK(countKind(g, NodeKind::IfFalse) >= 1);
  CHECK(countLiveUserKind(g, NodeKind::Phi) >= 1);
  CHECK(verifyOk(g));
}

B2_TEST(sccp_pipeline_consumes_sccp_constants_same_round) {
  // The payoff for the pipeline position (sccp runs right after the
  // simplify sweep): the proven-constant condition folds via branchnorm,
  // the dead arm sweeps away via cfs - all in ONE round.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label elseL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 1);
  b.emitRegImm(rbc::Op::Iconst, 1, 1);
  b.emitRegRegBranch(rbc::Op::IfIcmpeq, 0, 1, thenL);
  b.emitBranch(rbc::Op::Goto, elseL);
  b.bind(thenL);
  b.emitRegImm(rbc::Op::Iconst, 2, 7);
  b.emitSlotReg(rbc::Op::Istore, 2, 0);
  b.emitBranch(rbc::Op::Goto, endL);
  b.bind(elseL);
  b.emitRegImm(rbc::Op::Iconst, 2, 9);
  b.emitSlotReg(rbc::Op::Istore, 2, 0);
  b.bind(endL);
  b.emitRegSlot(rbc::Op::Iload, 2, 0);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passes::runEarlyCleanup(g);
  CHECK(r.ok);
  CHECK(r.telemetry.sccpConstants >= 2);
  CHECK(r.telemetry.converged);
  CHECK(countLiveUserKind(g, NodeKind::If) == 0); // branchnorm ate it
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  CHECK(retValueIs(g, NodeKind::ConstantI, 7));
  CHECK(verifyOk(g));
}

// --- the loop optimism -----------------------------------------------------------

B2_TEST(sccp_loop_phi_self_marker_resolves) {
  // [lb, 5, self]: the classic loop-invariant marker. The optimistic
  // iteration (the self input contributes the phi's OWN current value,
  // Top at first) resolves it to 5 without trivial-phi's identical-node
  // requirement.
  ir::Graph g;
  const ir::NodeId c5 = g.constantI(5);
  const ir::NodeId lb =
      g.make(NodeKind::LoopBegin, {g.startNode(), g.startNode()});
  const ir::NodeId le = g.make(NodeKind::LoopEnd, {lb});
  g.setInput(lb, 1, le);
  const ir::NodeId phi = g.make(NodeKind::Phi, {lb, c5});
  g.appendInput(phi, phi); // the self marker
  const ir::NodeId ret = g.make(NodeKind::Return, {lb, phi});
  CHECK(verifyOk(g));
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 1);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  CHECK(!g.node(g.input(ret, 1)).isDead());
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::ConstantI);
  CHECK(g.node(g.input(ret, 1)).constValue == 5);
  CHECK(verifyOk(g));
}

B2_TEST(sccp_loop_phi_invariant_through_arithmetic) {
  // [lb, 5, AddI(phi, 0)]: the backedge value DEPENDS on the phi, so no
  // node-identity rule can fire - only the optimistic fixpoint can (phi
  // = meet(5, phi+0) stabilizes at 5; the AddI follows to 5).
  ir::Graph g;
  const ir::NodeId c5 = g.constantI(5);
  const ir::NodeId c0 = g.constantI(0);
  const ir::NodeId lb =
      g.make(NodeKind::LoopBegin, {g.startNode(), g.startNode()});
  const ir::NodeId le = g.make(NodeKind::LoopEnd, {lb});
  g.setInput(lb, 1, le);
  const ir::NodeId phi = g.make(NodeKind::Phi, {lb, c5});
  const ir::NodeId add = g.make(NodeKind::AddI, {phi, c0});
  g.appendInput(phi, add); // backedge: i = i + 0
  const ir::NodeId ret = g.make(NodeKind::Return, {lb, phi});
  CHECK(verifyOk(g));
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 2); // the phi AND the AddI (interned)
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  CHECK(countLiveUserKind(g, NodeKind::AddI) == 0);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::ConstantI);
  CHECK(g.node(g.input(ret, 1)).constValue == 5);
  CHECK(verifyOk(g));
}

B2_TEST(sccp_loop_phi_varying_counter_refuses) {
  // [lb, 0, AddI(phi, 1)]: the counter that actually counts. The
  // optimism is REFUTED by the backedge value (meet(0, 1) = Bot) - the
  // refusal is the soundness story as much as the folds are.
  ir::Graph g;
  const ir::NodeId c0 = g.constantI(0);
  const ir::NodeId c1 = g.constantI(1);
  const ir::NodeId lb =
      g.make(NodeKind::LoopBegin, {g.startNode(), g.startNode()});
  const ir::NodeId le = g.make(NodeKind::LoopEnd, {lb});
  g.setInput(lb, 1, le);
  const ir::NodeId phi = g.make(NodeKind::Phi, {lb, c0});
  const ir::NodeId add = g.make(NodeKind::AddI, {phi, c1});
  g.appendInput(phi, add);
  g.make(NodeKind::Return, {lb, phi});
  CHECK(verifyOk(g));
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 0);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 1);
  CHECK(countLiveUserKind(g, NodeKind::AddI) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(sccp_zero_trip_loop_resolves_to_init) {
  // The backedge anchor hangs off a never-executable projection (the
  // loop condition is literally false): the backedge never fires, so the
  // loop phi is its ENTRY value alone. Hand-built: the builder cannot
  // produce a zero-trip loop without folding the branch first.
  ir::Graph g;
  const ir::NodeId c5 = g.constantI(5);
  const ir::NodeId c9 = g.constantI(9);
  const ir::NodeId c0 = g.constantI(0);
  const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), c0});
  const ir::NodeId never = g.make(NodeKind::IfTrue, {iff}); // cond==0: no flow
  g.make(NodeKind::IfFalse, {iff});
  const ir::NodeId le = g.make(NodeKind::LoopEnd, {never});
  const ir::NodeId lb =
      g.make(NodeKind::LoopBegin, {g.startNode(), le});
  const ir::NodeId phi = g.make(NodeKind::Phi, {lb, c5, c9});
  const ir::NodeId ret = g.make(NodeKind::Return, {lb, phi});
  CHECK(verifyOk(g));
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 1);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::ConstantI);
  CHECK(g.node(g.input(ret, 1)).constValue == 5);
  CHECK(verifyOk(g));
}

// --- the value-fact and null lattices ---------------------------------------------

B2_TEST(sccp_null_phi_meets_to_null_and_isnull_folds) {
  // meet(null, null) = null (a Ref-typed phi), and IsNull(null) = 1
  // follows through the lattice in the same run.
  ir::Graph g;
  const ir::NodeId n1 = g.constantNull();
  const ir::NodeId n2 = g.constantNull();
  const ir::NodeId reg = g.make(NodeKind::Region, {g.startNode(), g.startNode()});
  const ir::NodeId phi = g.make(NodeKind::Phi, {reg, n1, n2});
  const ir::NodeId isn = g.make(NodeKind::IsNull, {phi});
  const ir::NodeId ret = g.make(NodeKind::Return, {reg, isn});
  CHECK(verifyOk(g));
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 2); // the phi AND the IsNull
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  CHECK(countLiveUserKind(g, NodeKind::IsNull) == 0);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::ConstantI);
  CHECK(g.node(g.input(ret, 1)).constValue == 1);
  CHECK(verifyOk(g));
}

B2_TEST(sccp_isnull_never_null_fact_folds) {
  // IsNull(New): the allocation is provably non-null (the NeverNull
  // dataflow fact, read at NODE level even though the allocation's own
  // lattice value is Bot) - the branch on it becomes decidably false.
  ir::Graph g;
  const ir::NodeId alloc =
      g.make(NodeKind::New, {g.startNode()}, 3); // TypeId 3
  const ir::NodeId isn = g.make(NodeKind::IsNull, {alloc});
  const ir::NodeId ret = g.make(NodeKind::Return, {g.startNode(), isn});
  CHECK(verifyOk(g));
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 1);
  CHECK(countLiveUserKind(g, NodeKind::IsNull) == 0);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::ConstantI);
  CHECK(g.node(g.input(ret, 1)).constValue == 0);
  CHECK(verifyOk(g));
}

B2_TEST(sccp_guard_condition_resolves_but_control_untouched) {
  // Guard(cond = EqI(3, 3)): the CONDITION folds to 1 through the
  // lattice, the guard itself is untouched (control rewrites are
  // branchnorm's business - SCCP never half-applies a control decision),
  // and flow continues past it exactly as before.
  ir::Graph g;
  const ir::NodeId c3a = g.constantI(3);
  const ir::NodeId c3b = g.constantI(3);
  const ir::NodeId c7 = g.constantI(7);
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{7}, 5,
                                          std::span<const ir::NodeId>{});
  const ir::NodeId eq = g.make(NodeKind::EqI, {c3a, c3b});
  const ir::NodeId guard =
      g.make(NodeKind::Guard, {g.startNode(), eq, fs},
             static_cast<std::uint32_t>(ir::GuardKind::BoundsCheck), 0);
  const ir::NodeId ret = g.make(NodeKind::Return, {guard, c7});
  CHECK(verifyOk(g));
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 1); // the EqI only
  CHECK(!g.node(guard).isDead());
  CHECK(g.node(guard).kind == NodeKind::Guard);
  CHECK(countLiveUserKind(g, NodeKind::EqI) == 0);
  CHECK(g.input(guard, 1) != eq); // rewired onto the constant
  CHECK(g.node(g.input(guard, 1)).kind == NodeKind::ConstantI);
  CHECK(verifyOk(g));
  (void)ret;
}

// --- the completion rule (the soundness bolt) --------------------------------------

B2_TEST(sccp_unresolved_used_value_collapses_the_meet) {
  // The hand-buildable shape the completion rule exists for: a phi whose
  // EXECUTABLE edge carries a value that never resolves (x is a phi over
  // a never-executable region; v1 = x + 3 stays Top). Optimistically the
  // meet would claim 5 - the completion forces the unresolved-but-USED
  // Tops to Bot, and the meet collapses to Bot. No claim is made.
  ir::Graph g;
  const ir::NodeId c1a = g.constantI(1);
  const ir::NodeId c1b = g.constantI(1);
  const ir::NodeId c3 = g.constantI(3);
  const ir::NodeId c5 = g.constantI(5);
  const ir::NodeId c9 = g.constantI(9);
  // Two always-true Ifs: their IfFalse projections never flow.
  const ir::NodeId iff1 = g.make(NodeKind::If, {g.startNode(), c1a});
  const ir::NodeId iffF1 = g.make(NodeKind::IfFalse, {iff1});
  g.make(NodeKind::IfTrue, {iff1});
  const ir::NodeId iff2 = g.make(NodeKind::If, {g.startNode(), c1b});
  const ir::NodeId iffF2 = g.make(NodeKind::IfFalse, {iff2});
  g.make(NodeKind::IfTrue, {iff2});
  // x: phi over a region whose predecessors never flow -> never resolves.
  const ir::NodeId deadReg = g.make(NodeKind::Region, {iffF1, iffF2});
  const ir::NodeId x = g.make(NodeKind::Phi, {deadReg, c9, c9});
  const ir::NodeId v1 = g.make(NodeKind::AddI, {x, c3});
  // p: phi over a LIVE region; the v1 edge IS executable.
  const ir::NodeId liveReg =
      g.make(NodeKind::Region, {g.startNode(), g.startNode()});
  const ir::NodeId p = g.make(NodeKind::Phi, {liveReg, c5, v1});
  g.make(NodeKind::Return, {liveReg, p});
  CHECK(verifyOk(g));
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 0); // the meet must NOT claim 5
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 2); // x and p both stay
  CHECK(verifyOk(g));
}

B2_TEST(sccp_param_operand_is_never_constant) {
  // A phi merging a parameter with a constant: the parameter's lattice
  // value is Bot from the seed, the meet refuses. (T3 static mode has no
  // argument-constant speculation; PGO's ArgumentConstant is a growth
  // path, not v1.)
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label elseL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegBranch(rbc::Op::Ifne, 0, thenL);
  b.emitBranch(rbc::Op::Goto, elseL);
  b.bind(thenL);
  b.emitRegSlot(rbc::Op::Iload, 1, 0); // the parameter itself
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.emitBranch(rbc::Op::Goto, endL);
  b.bind(elseL);
  b.emitRegImm(rbc::Op::Iconst, 1, 5);
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.bind(endL);
  b.emitRegSlot(rbc::Op::Iload, 2, 0);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 0);
  CHECK(countLiveUserKind(g, NodeKind::Phi) >= 1);
  CHECK(verifyOk(g));
}

// --- the machine discipline (idempotency, determinism, kill switch) ----------------

B2_TEST(sccp_idempotent_second_run_rewrites_nothing) {
  rbc::Method m;
  buildParamMergeExpr(m);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r1 = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r1.telemetry.sccpConstants >= 1);
  const std::string print1 = ir::print(g);
  const passes::PassResult r2 = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r2.telemetry.sccpConstants == 0);
  CHECK(r2.telemetry.rewrites == 0);
  CHECK(ir::print(g) == print1);
  CHECK(verifyOk(g));
}

B2_TEST(sccp_deterministic_double_build_byte_identical) {
  rbc::Method m;
  buildParamMergeExpr(m);
  ir::Graph g1;
  buildOk(m, g1);
  passOk(g1, PK::SparseConditionalConstantPropagation);
  ir::Graph g2;
  buildOk(m, g2);
  passOk(g2, PK::SparseConditionalConstantPropagation);
  CHECK(ir::print(g1) == ir::print(g2));
}

B2_TEST(sccp_kill_switch_is_a_successful_noop) {
  rbc::Method m;
  buildParamMergeExpr(m);
  ir::Graph g;
  buildOk(m, g);
  const std::string before = ir::print(g);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::SparseConditionalConstantPropagation, false);
  const passes::PassResult r = passes::runSinglePass(g, PK::SparseConditionalConstantPropagation, cfg);
  CHECK(r.ok);
  CHECK(r.telemetry.rewrites == 0);
  CHECK(r.telemetry.sccpConstants == 0);
  CHECK(ir::print(g) == before);
  CHECK(verifyOk(g));
}

B2_TEST(sccp_pipeline_reports_the_counter) {
  // The pipeline runs SCCP after the simplify sweep; the telemetry
  // surface (b2graph prints it) must see the replacements.
  rbc::Method m;
  buildParamMergeExpr(m);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passes::runEarlyCleanup(g);
  CHECK(r.ok);
  CHECK(r.telemetry.sccpConstants >= 1);
  CHECK(r.telemetry.converged);
  CHECK(verifyOk(g));
}

B2_TEST(sccp_memory_phi_never_claimed) {
  // A Bottom-typed (memory-state) phi over Start and a memory producer:
  // no flavor types as Bottom, so no constant replacement can ever fire
  // on memory merges - pinned here so a future lattice change cannot
  // silently start rewriting memory state.
  ir::Graph g;
  const ir::NodeId reg =
      g.make(NodeKind::Region, {g.startNode(), g.startNode()});
  const ir::NodeId memPhi =
      g.make(NodeKind::Phi, {reg, g.startNode(), g.startNode()});
  const ir::NodeId ret = g.make(NodeKind::Return, {reg, g.startNode()});
  (void)ret;
  (void)memPhi;
  CHECK(verifyOk(g));
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants == 0);
  CHECK(verifyOk(g));
}

B2_TEST(sccp_framestate_snapshots_auto_update) {
  // Rule 14 payoff: a call's FrameState captured the merge phi; after
  // SCCP replaces it with the constant, the snapshot edge points at the
  // constant - deopt reconstruction sees the exact value.
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label elseL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegBranch(rbc::Op::Ifne, 0, thenL);
  b.emitBranch(rbc::Op::Goto, elseL);
  b.bind(thenL);
  b.emitRegImm(rbc::Op::Iconst, 1, 5);
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.emitBranch(rbc::Op::Goto, endL);
  b.bind(elseL);
  b.emitRegImm(rbc::Op::Iconst, 1, 10);
  b.emitRegImm(rbc::Op::Iconst, 2, 5);
  b.emitRegRegReg(rbc::Op::Isub, 1, 1, 2);
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.bind(endL);
  b.emitRegSlot(rbc::Op::Iload, 2, 0);
  const std::uint32_t mc = b.constMethodRef("M", "callee", "()I");
  b.emitCall(rbc::Op::Invokestatic, 3, 0, 0, mc); // FS captures slot 0
  b.emitReg(rbc::Op::Ireturn, 3);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::SparseConditionalConstantPropagation);
  CHECK(r.telemetry.sccpConstants >= 1);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  // Some live FrameState now has the constant as a locals edge.
  bool fsSeesConstant = false;
  for (ir::NodeId i = 0; i < g.nodeCount(); ++i) {
    if (g.node(i).isDead() || g.node(i).kind != NodeKind::FrameState) {
      continue;
    }
    for (std::uint16_t s = 0; s < g.node(i).numInputs; ++s) {
      const ir::NodeId v = g.input(i, s);
      if (!g.node(v).isDead() && g.node(v).kind == NodeKind::ConstantI &&
          g.node(v).constValue == 5) {
        fsSeesConstant = true;
      }
    }
  }
  CHECK(fsSeesConstant);
  CHECK(verifyOk(g));
}
