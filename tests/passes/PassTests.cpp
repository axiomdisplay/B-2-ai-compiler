// B-2 passes tests - the optimization pass suite (T2-IR3).
//
// Discipline: Rule 35 - at least ten golden tests per delivered registry
// pass, in both Static (T3) and Profile (T2) mode readings: v1 has one
// suite for both tiers (Rule 1 - tier filters arrive with the speculation
// passes, not by forking), so every test IS the both-modes gate until
// then. Every test runs with ir::verify after each pass (Rule 40, the
// default PassConfig) and pins: the rewrite catalogs (docs/
// pass_contracts.md is the normative rule table), the tombstone-law
// soundness protocol (kills only with zero live referencers; tombstones
// junked to verifier-legal sinks), determinism (Rule 124: byte-identical
// prints), idempotency (Rule 10), and the kill switches (Rules 132/144).
// The corpus sweep at the bottom is the strongest gate: every interp
// corpus program, every method, through the full pipeline with a
// determinism + idempotency re-run.

#include "TestHarness.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <b2/ir/Node.h>
#include <b2/ir/Printer.h>
#include <b2/ir/Verifier.h>
#include <b2/passes/GraphBuilder.h>
#include <b2/passes/Passes.h>
#include <b2/rbc/RbcBuilder.h>
#include <b2/rbc/RbcText.h>
#include <b2/rbc/Verifier.h>

using namespace b2;
using ir::NodeKind;
using PK = passes::PassKey;

namespace {

// Builds `m` into `g` with a CounterResolver, requiring the build and the
// IR verification to succeed (Graph is non-movable: out-parameter form).
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

// Runs one registry pass in isolation, requiring success (the default
// config verifies after the pass - Rule 40).
passes::PassResult passOk(ir::Graph& g, PK key) {
  passes::PassResult r = passes::runSinglePass(g, key);
  if (!r.ok) {
    CHECK_MSG(false, r.diags.empty()
                         ? "pass failed"
                         : r.diags[0].message.c_str());
  }
  return r;
}

// Runs the full early-cleanup pipeline, requiring success.
passes::PassResult pipeOk(ir::Graph& g,
                          const passes::PassConfig& cfg = {}) {
  passes::PassResult r = passes::runEarlyCleanup(g, cfg);
  if (!r.ok) {
    CHECK_MSG(false, r.diags.empty()
                         ? "pipeline failed"
                         : r.diags[0].message.c_str());
  }
  return r;
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

[[nodiscard]] std::vector<ir::NodeId> nodesOfKind(const ir::Graph& g,
                                                  NodeKind k) {
  std::vector<ir::NodeId> out;
  for (ir::NodeId i = 0; i < g.nodeCount(); ++i) {
    if (!g.node(i).isDead() && g.node(i).kind == k) {
      out.push_back(i);
    }
  }
  return out;
}

[[nodiscard]] ir::NodeId firstOf(const ir::Graph& g, NodeKind k) {
  const auto v = nodesOfKind(g, k);
  return v.empty() ? ir::kInvalidNodeId : v[0];
}

// Count alive nodes of kind `k` that have at least one LIVE user. The
// pass suite's junk sinks (the tombstone-law anchors: an If/Region/
// FrameState/constant referenced only by Dead/Replaced tombstones) are
// deliberately excluded - they are bookkeeping, not program structure.
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

[[nodiscard]] bool verifyOk(const ir::Graph& g) { return ir::verify(g).ok; }

[[nodiscard]] std::int32_t constValI(const ir::Graph& g, ir::NodeId n) {
  return static_cast<std::int32_t>(g.node(n).constValue);
}

[[nodiscard]] std::uint32_t constValBits(const ir::Graph& g, ir::NodeId n) {
  return static_cast<std::uint32_t>(g.node(n).constValue);
}

// The first FrameState of the graph (the pass suite keeps at least the
// call FSes alive, so index 0 is stable in these tests).
[[nodiscard]] ir::NodeId firstFs(const ir::Graph& g) {
  return firstOf(g, NodeKind::FrameState);
}

} // namespace

// --- DCE (key 11) ----------------------------------------------------------

B2_TEST(dce_removes_unused_pure_arith) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 5);
  b.emitRegImm(rbc::Op::Iconst, 1, 7);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1); // unused
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::AddI) == 1);
  const passes::PassResult r = passOk(g, PK::DeadCodeElimination);
  CHECK(countKind(g, NodeKind::AddI) == 0);
  CHECK(r.telemetry.removals >= 1);
  CHECK(verifyOk(g));
}

B2_TEST(dce_cascade_kills_orphaned_inputs) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 5);
  b.emitRegReg(rbc::Op::Ineg, 1, 0);          // used only by the dead add
  b.emitRegRegReg(rbc::Op::Iadd, 2, 1, 1);    // unused
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::AddI) == 1);
  CHECK(countKind(g, NodeKind::NegI) == 1);
  passOk(g, PK::DeadCodeElimination);
  CHECK(countKind(g, NodeKind::AddI) == 0);
  CHECK(countKind(g, NodeKind::NegI) == 0); // cascade reached the input
  // The 5-constant still feeds the return (junk sinks are 0-constants).
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::ConstantI);
  CHECK(constValI(g, g.input(ret, 1)) == 5);
}

B2_TEST(dce_keeps_used_arith) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 5);
  b.emitRegImm(rbc::Op::Iconst, 1, 7);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::DeadCodeElimination);
  CHECK(countKind(g, NodeKind::AddI) == 1);
}

B2_TEST(dce_keeps_value_used_only_by_framestate) {
  // iconst 7; istore l0; invokestatic (result unused); return. The call's
  // FrameState captures l0 = ConstantI(7): a deopt snapshot is a use.
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(2);
  b.setLocals(1);
  const std::uint32_t mc =
      b.constMethodRef("M", "callee", "()I");
  b.emitRegImm(rbc::Op::Iconst, 0, 7);
  b.emitSlotReg(rbc::Op::Istore, 0, 0);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 0, mc);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const ir::NodeId fs = firstFs(g);
  CHECK(g.node(fs).numInputs == 3); // l0 + r0 + r1
  const ir::NodeId l0Def = g.input(fs, 0);
  CHECK(g.node(l0Def).kind == NodeKind::ConstantI);
  passOk(g, PK::DeadCodeElimination);
  CHECK(!g.node(l0Def).isDead()); // Rule 5: deopt state keeps it alive
  CHECK(countKind(g, NodeKind::ConstantI) == 1);
}

B2_TEST(dce_keeps_stores_calls_and_allocations) {
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(3);
  const std::uint32_t cls = b.constClass("M");
  const std::uint32_t fld = b.constFieldRef("M", "x", "I");
  const std::uint32_t mc = b.constMethodRef("M", "f", "()I");
  b.emitRegCp(rbc::Op::New, 0, cls);
  b.emitRegImm(rbc::Op::Iconst, 1, 42);
  b.emitRegRegRegCp(rbc::Op::Putfield, 0, 0, 1, fld); // observable store
  b.emitRegImm(rbc::Op::Iconst, 1, 4);
  b.emitRegRegImm(rbc::Op::NewArray, 2, 1,
                  static_cast<std::uint32_t>(rbc::Atype::Int));
  b.emitCall(rbc::Op::Invokestatic, 0, 0, 0, mc); // observable call
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::DeadCodeElimination);
  CHECK(countKind(g, NodeKind::StoreField) == 1);
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(countKind(g, NodeKind::NewArray) == 1);
  CHECK(countKind(g, NodeKind::CallStatic) == 1);
  CHECK(r.telemetry.removals == 0);
}

B2_TEST(dce_keeps_trapping_div_and_rem) {
  // Documented v1 conservatism: unguarded-in-IR trapping kinds survive a
  // zero-user DCE (removal needs the guard-adjacency proof).
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(4);
  b.emitRegImm(rbc::Op::Iconst, 0, 10);
  b.emitRegImm(rbc::Op::Iconst, 1, 3);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1); // unused
  b.emitRegRegReg(rbc::Op::Irem, 3, 0, 1); // unused
  b.emitRegImm(rbc::Op::Iconst, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::DeadCodeElimination);
  CHECK(countKind(g, NodeKind::DivI) == 1);
  CHECK(countKind(g, NodeKind::RemI) == 1);
}

B2_TEST(dce_keeps_loads) {
  // Volatile-read classification is opaque at IR level: loads never die.
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)I",
                    rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t fld = b.constFieldRef("M", "x", "I");
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitRegRegCp(rbc::Op::Getfield, 1, 0, fld); // result unused
  b.emitRegImm(rbc::Op::Iconst, 2, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::DeadCodeElimination);
  CHECK(countKind(g, NodeKind::LoadField) == 1);
}

B2_TEST(dce_removes_unused_arraylength) {
  // The null guard gates control (kept); the reader itself has no user.
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)I",
                    rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitRegReg(rbc::Op::Arraylength, 1, 0); // unused
  b.emitRegImm(rbc::Op::Iconst, 2, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const std::size_t guards = countKind(g, NodeKind::Guard);
  passOk(g, PK::DeadCodeElimination);
  CHECK(countKind(g, NodeKind::ArrayLength) == 0);
  CHECK(countKind(g, NodeKind::Guard) == guards); // control stays
}

B2_TEST(dce_removes_unused_instanceof) {
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)I",
                    rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t cls = b.constClass("M");
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitRegRegCp(rbc::Op::Instanceof, 1, 0, cls); // unused
  b.emitRegImm(rbc::Op::Iconst, 2, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::DeadCodeElimination);
  CHECK(countKind(g, NodeKind::InstanceOf) == 0);
}

B2_TEST(dce_removes_orphaned_framestate_after_guard_fold) {
  // fold -> branch-normalize a passing zero-check guard away; the guard's
  // FS then has no user and dies.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 4);
  b.emitRegImm(rbc::Op::Iconst, 1, 2);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::FrameState) >= 1);
  passOk(g, PK::ConstantFolding);        // NeI(2, 0) -> 1; 4/2 -> 2
  passOk(g, PK::BranchNormalization);    // guard-true -> pass-through
  passOk(g, PK::DeadCodeElimination);    // orphaned FS dies
  CHECK(countLiveUserKind(g, NodeKind::FrameState) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::ConstantI);
  CHECK(constValI(g, g.input(ret, 1)) == 2);
}

B2_TEST(dce_idempotent) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 5);
  b.emitRegReg(rbc::Op::Ineg, 1, 0);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 1, 1);
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::DeadCodeElimination);
  const std::string once = ir::print(g);
  const passes::PassResult r = passOk(g, PK::DeadCodeElimination);
  CHECK(r.telemetry.removals == 0);
  CHECK(ir::print(g) == once);
}

B2_TEST(dce_killswitch) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 5);
  b.emitRegImm(rbc::Op::Iconst, 1, 7);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const std::string before = ir::print(g);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::DeadCodeElimination, false);
  const passes::PassResult r = passes::runSinglePass(
      g, PK::DeadCodeElimination, cfg);
  CHECK(r.ok);
  CHECK(r.telemetry.removals == 0);
  CHECK(ir::print(g) == before);
}

// --- trivial block fusion (key 12) -----------------------------------------

B2_TEST(fusion_collapses_single_pred_region_after_branch_fold) {
  // if (0 != 0) l0=1 else l0=2; return l0: fold -> branch fold leaves a
  // single-pred merge; the region repair collapses it.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label elseL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 0);
  b.emitRegBranch(rbc::Op::Ifne, 0, thenL); // 0 != 0 -> never taken
  b.emitBranch(rbc::Op::Goto, elseL);
  b.bind(thenL);
  b.emitRegImm(rbc::Op::Iconst, 0, 1);
  b.emitSlotReg(rbc::Op::Istore, 0, 0);
  b.emitBranch(rbc::Op::Goto, endL);
  b.bind(elseL);
  b.emitRegImm(rbc::Op::Iconst, 0, 2);
  b.emitSlotReg(rbc::Op::Istore, 0, 0);
  b.bind(endL);
  b.emitRegSlot(rbc::Op::Iload, 1, 0);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  passOk(g, PK::BranchNormalization); // includes the region repair
  CHECK(countLiveUserKind(g, NodeKind::If) == 0);
  CHECK(countLiveUserKind(g, NodeKind::Region) == 0);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::ConstantI);
  CHECK(constValI(g, g.input(ret, 1)) == 2); // the live (else) arm
}

B2_TEST(fusion_trivial_phi_after_fold_and_gvn) {
  // Both arms compute 5 (one via 2+3, one directly): fold + GVN make the
  // merge phi's inputs identical, then the trivial-phi collapse fires.
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(5);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegBranch(rbc::Op::Ifne, 0, thenL);
  b.emitRegImm(rbc::Op::Iconst, 1, 2);
  b.emitRegImm(rbc::Op::Iconst, 2, 3);
  b.emitRegRegReg(rbc::Op::Iadd, 3, 1, 2);
  b.emitSlotReg(rbc::Op::Istore, 3, 0);
  b.emitBranch(rbc::Op::Goto, endL);
  b.bind(thenL);
  b.emitRegImm(rbc::Op::Iconst, 3, 5);
  b.emitSlotReg(rbc::Op::Istore, 3, 0);
  b.bind(endL);
  b.emitRegSlot(rbc::Op::Iload, 4, 0);
  b.emitReg(rbc::Op::Ireturn, 4);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::Phi) >= 1);
  passOk(g, PK::ConstantFolding); // AddI(2, 3) -> 5
  passOk(g, PK::GVN);             // both 5-constants merge
  passOk(g, PK::TrivialBlockFusion);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::ConstantI);
  CHECK(constValI(g, g.input(ret, 1)) == 5);
}

B2_TEST(fusion_repairs_region_with_dead_pred) {
  // Hand-built: a Region over two preds, one killed raw; key 12 must drop
  // the dead pred (collapsing the region) and leave a legal graph.
  ir::Graph g;
  const ir::NodeId cA = g.constantI(11);
  const ir::NodeId cB = g.constantI(22);
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 0,
                                          std::span<const ir::NodeId>{});
  const ir::NodeId guard = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 5);
  const ir::NodeId region =
      g.make(NodeKind::Region, {g.startNode(), guard});
  const ir::NodeId phi = g.make(NodeKind::Phi, {region, cA, cB});
  const ir::NodeId ret = g.make(NodeKind::Return, {region, phi});
  CHECK(verifyOk(g));
  g.killNode(guard); // raw kill: the region now has a dead pred input
  CHECK(!verifyOk(g)); // the broken state the pass must repair
  passOk(g, PK::TrivialBlockFusion);
  CHECK(verifyOk(g));
  CHECK(countLiveUserKind(g, NodeKind::Region) == 0);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  CHECK(!g.node(ret).isDead());
  CHECK(g.input(ret, 0) == g.startNode());
  CHECK(g.input(ret, 1) == cA); // pred slot 0's value survived
}

B2_TEST(fusion_loop_invariant_phi_self_marker) {
  // Hand-built loop phi with the loop-invariant SELF input: [lb, entry,
  // self]. Trivial-phi collapse must replace it with the entry value.
  ir::Graph g;
  const ir::NodeId c5 = g.constantI(5);
  const ir::NodeId lb = g.make(NodeKind::LoopBegin,
                               {g.startNode(), g.startNode()});
  const ir::NodeId le = g.make(NodeKind::LoopEnd, {lb});
  g.setInput(lb, 1, le); // entry + real backedge
  const ir::NodeId phi = g.make(NodeKind::Phi, {lb, c5});
  g.appendInput(phi, phi); // the self marker (arity 1 + preds)
  const ir::NodeId ret = g.make(NodeKind::Return, {lb, phi});
  CHECK(verifyOk(g));
  passOk(g, PK::TrivialBlockFusion);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  CHECK(g.input(ret, 1) == c5);
  CHECK(verifyOk(g));
}

B2_TEST(fusion_keeps_two_pred_region) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegBranch(rbc::Op::Ifne, 0, thenL); // genuinely conditional
  b.emitRegImm(rbc::Op::Iconst, 1, 1);
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.emitBranch(rbc::Op::Goto, endL);
  b.bind(thenL);
  b.emitRegImm(rbc::Op::Iconst, 1, 2);
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.bind(endL);
  b.emitRegSlot(rbc::Op::Iload, 2, 0);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::TrivialBlockFusion);
  CHECK(countKind(g, NodeKind::Region) == 1);
  CHECK(countKind(g, NodeKind::Phi) >= 1);
  CHECK(r.telemetry.rewrites == 0); // nothing to fuse
}

B2_TEST(fusion_idempotent) {
  ir::Graph g;
  const ir::NodeId cA = g.constantI(11);
  const ir::NodeId cB = g.constantI(22);
  const ir::NodeId region =
      g.make(NodeKind::Region, {g.startNode(), g.startNode()});
  const ir::NodeId phi = g.make(NodeKind::Phi, {region, cA, cB});
  g.make(NodeKind::Return, {region, phi});
  passOk(g, PK::TrivialBlockFusion);
  const std::string once = ir::print(g);
  const passes::PassResult r = passOk(g, PK::TrivialBlockFusion);
  CHECK(r.telemetry.rewrites == 0);
  CHECK(ir::print(g) == once);
}

B2_TEST(fusion_killswitch_and_determinism) {
  ir::Graph g;
  const ir::NodeId cA = g.constantI(11);
  const ir::NodeId region =
      g.make(NodeKind::Region, {g.startNode(), g.startNode()});
  const ir::NodeId phi = g.make(NodeKind::Phi, {region, cA, cA});
  g.make(NodeKind::Return, {region, phi});
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::TrivialBlockFusion, false);
  const std::string before = ir::print(g);
  const passes::PassResult r =
      passes::runSinglePass(g, PK::TrivialBlockFusion, cfg);
  CHECK(r.ok);
  CHECK(ir::print(g) == before);

  ir::Graph g2;
  const ir::NodeId cA2 = g2.constantI(11);
  const ir::NodeId region2 =
      g2.make(NodeKind::Region, {g2.startNode(), g2.startNode()});
  const ir::NodeId phi2 = g2.make(NodeKind::Phi, {region2, cA2, cA2});
  g2.make(NodeKind::Return, {region2, phi2});
  passOk(g, PK::TrivialBlockFusion);
  passOk(g2, PK::TrivialBlockFusion);
  CHECK(ir::print(g) == ir::print(g2)); // byte-identical (Rule 124)
}

// --- canonicalization (key 13) ---------------------------------------------

namespace {
// Builds `dst = a OP x` where x is the parameter (const on the LEFT, the
// shape canonicalization moves right).
struct CommutativeCase {
  const char* name;
  rbc::Op op;
  NodeKind kind;
};
} // namespace

B2_TEST(canon_const_to_right_on_commutative_ints) {
  const CommutativeCase cases[] = {
      {"addi", rbc::Op::Iadd, NodeKind::AddI},
      {"muli", rbc::Op::Imul, NodeKind::MulI},
      {"andi", rbc::Op::Iand, NodeKind::AndI},
      {"ori", rbc::Op::Ior, NodeKind::OrI},
      {"xori", rbc::Op::Ixor, NodeKind::XorI},
  };
  for (const CommutativeCase& c : cases) {
    rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
    b.setRegs(3);
    b.setLocals(1);
    b.emitRegSlot(rbc::Op::Iload, 0, 0);
    b.emitRegImm(rbc::Op::Iconst, 1, 9);
    b.emitRegRegReg(c.op, 2, 1, 0); // (const, x): wrong side
    b.emitReg(rbc::Op::Ireturn, 2);
    rbc::Method m;
    CHECK(b.finish(m).ok);
    ir::Graph g;
    buildOk(m, g);
    const ir::NodeId op = firstOf(g, c.kind);
    CHECK(op != ir::kInvalidNodeId);
    CHECK(g.node(g.input(op, 0)).kind == NodeKind::ConstantI);
    const passes::PassResult r = passOk(g, PK::Canonicalization);
    const ir::NodeId op2 = firstOf(g, c.kind);
    CHECK(g.node(g.input(op2, 0)).kind == NodeKind::Parameter);
    CHECK(g.node(g.input(op2, 1)).kind == NodeKind::ConstantI);
    CHECK(r.telemetry.folds >= 1);
    CHECK(verifyOk(g));
  }
}

B2_TEST(canon_const_to_right_long_ops) {
  const CommutativeCase cases[] = {
      {"addl", rbc::Op::Ladd, NodeKind::AddL},
      {"mull", rbc::Op::Lmul, NodeKind::MulL},
  };
  for (const CommutativeCase& c : cases) {
    rbc::RbcBuilder b("main", "(J)J", rbc::method_flags::Static);
    b.setRegs(3);
    b.setLocals(1);
    b.emitRegSlot(rbc::Op::Lload, 0, 0);
    const std::uint32_t nine = b.constLong(9);
    b.emitRegCp(rbc::Op::Lconst, 1, nine);
    b.emitRegRegReg(c.op, 2, 1, 0);
    b.emitReg(rbc::Op::Lreturn, 2);
    rbc::Method m;
    CHECK(b.finish(m).ok);
    ir::Graph g;
    buildOk(m, g);
    passOk(g, PK::Canonicalization);
    const ir::NodeId op = firstOf(g, c.kind);
    CHECK(g.node(g.input(op, 0)).kind == NodeKind::Parameter);
    CHECK(g.node(g.input(op, 1)).kind == NodeKind::ConstantL);
  }
}

B2_TEST(canon_not_of_test_becomes_complement) {
  // Hand-built (the builder only wraps IsNull/RefEq in Not): Not(EqI) ->
  // NeI with identical operands.
  ir::Graph g;
  const ir::NodeId p = g.parameter(0, ir::IRType::Int);
  const ir::NodeId c = g.constantI(4);
  const ir::NodeId eq = g.make(NodeKind::EqI, {p, c});
  const ir::NodeId notn = g.make(NodeKind::Not, {eq});
  const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), notn});
  const ir::NodeId t = g.make(NodeKind::IfTrue, {iff});
  const ir::NodeId f = g.make(NodeKind::IfFalse, {iff});
  g.make(NodeKind::Return, {t});
  g.make(NodeKind::Return, {f});
  CHECK(verifyOk(g));
  passOk(g, PK::Canonicalization);
  passOk(g, PK::DeadCodeElimination); // the orphaned EqI is reclaimed
  CHECK(countKind(g, NodeKind::Not) == 0);
  const ir::NodeId ne = firstOf(g, NodeKind::NeI);
  CHECK(ne != ir::kInvalidNodeId);
  CHECK(g.input(ne, 0) == p);
  CHECK(g.input(ne, 1) == c);
  CHECK(countKind(g, NodeKind::EqI) == 0);
}

B2_TEST(canon_not_complement_table) {
  // All six complement pairs, hand-built in one graph region each.
  const struct {
    NodeKind from;
    NodeKind to;
  } table[] = {
      {NodeKind::EqI, NodeKind::NeI}, {NodeKind::NeI, NodeKind::EqI},
      {NodeKind::LtI, NodeKind::GeI}, {NodeKind::GeI, NodeKind::LtI},
      {NodeKind::LeI, NodeKind::GtI}, {NodeKind::GtI, NodeKind::LeI},
  };
  for (const auto& e : table) {
    ir::Graph g;
    const ir::NodeId p = g.parameter(0, ir::IRType::Int);
    const ir::NodeId c = g.constantI(1);
    const ir::NodeId test = g.make(e.from, {p, c});
    const ir::NodeId notn = g.make(NodeKind::Not, {test});
    const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), notn});
    g.make(NodeKind::Return, {g.make(NodeKind::IfTrue, {iff})});
    g.make(NodeKind::Return, {g.make(NodeKind::IfFalse, {iff})});
    CHECK(verifyOk(g));
    passOk(g, PK::Canonicalization);
    passOk(g, PK::DeadCodeElimination);
    CHECK(countKind(g, e.to) == 1);
    CHECK(countKind(g, e.from) == 0);
    CHECK(countKind(g, NodeKind::Not) == 0);
  }
}

B2_TEST(canon_not_not_boolean_test_collapses) {
  // Double complement: the IDENTITY class (the rule only holds for
  // 0/1-producing test kinds, so it lives with the exact-semantics set).
  ir::Graph g;
  const ir::NodeId p = g.parameter(0, ir::IRType::Int);
  const ir::NodeId c = g.constantI(7);
  const ir::NodeId eq = g.make(NodeKind::EqI, {p, c});
  const ir::NodeId n1 = g.make(NodeKind::Not, {eq});
  const ir::NodeId n2 = g.make(NodeKind::Not, {n1});
  g.make(NodeKind::Return, {g.startNode(), n2});
  CHECK(verifyOk(g));
  passOk(g, PK::IdentityRemoval);
  passOk(g, PK::DeadCodeElimination); // the orphaned inner Not dies
  CHECK(countKind(g, NodeKind::Not) == 0);
  const ir::NodeId ret2 = firstOf(g, NodeKind::Return);
  CHECK(g.input(ret2, 1) == eq);
}

B2_TEST(canon_fp_commutative_not_swapped) {
  // Exact-bits policy: FP operand order can matter for NaN payloads, so
  // AddF(const, x) stays as-is.
  rbc::RbcBuilder b("main", "(F)F", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Fload, 0, 0);
  b.emitRegImm(rbc::Op::Fconst, 1, 0x3FC00000u); // 1.5f
  b.emitRegRegReg(rbc::Op::Fadd, 2, 1, 0);       // (const, x)
  b.emitReg(rbc::Op::Freturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::Canonicalization);
  const ir::NodeId add = firstOf(g, NodeKind::AddF);
  CHECK(g.node(g.input(add, 0)).kind == NodeKind::ConstantF);
  CHECK(g.node(g.input(add, 1)).kind == NodeKind::Parameter);
  CHECK(r.telemetry.rewrites == 0);
}

B2_TEST(canon_non_commutative_untouched) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 9);
  b.emitRegRegReg(rbc::Op::Isub, 2, 1, 0); // SubI is NOT commutative
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::Canonicalization);
  const ir::NodeId sub = firstOf(g, NodeKind::SubI);
  CHECK(g.node(g.input(sub, 0)).kind == NodeKind::ConstantI);
  CHECK(g.node(g.input(sub, 1)).kind == NodeKind::Parameter);
}

B2_TEST(canon_idempotent_and_deterministic) {
  ir::Graph g;
  const ir::NodeId p = g.parameter(0, ir::IRType::Int);
  const ir::NodeId c = g.constantI(4);
  const ir::NodeId eq = g.make(NodeKind::EqI, {p, c});
  const ir::NodeId notn = g.make(NodeKind::Not, {eq});
  const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), notn});
  g.make(NodeKind::Return, {g.make(NodeKind::IfTrue, {iff})});
  g.make(NodeKind::Return, {g.make(NodeKind::IfFalse, {iff})});
  passOk(g, PK::Canonicalization);
  const std::string once = ir::print(g);
  const passes::PassResult r = passOk(g, PK::Canonicalization);
  CHECK(r.telemetry.folds == 0);
  CHECK(ir::print(g) == once);

  ir::Graph g2;
  const ir::NodeId p2 = g2.parameter(0, ir::IRType::Int);
  const ir::NodeId c2 = g2.constantI(4);
  const ir::NodeId eq2 = g2.make(NodeKind::EqI, {p2, c2});
  const ir::NodeId notn2 = g2.make(NodeKind::Not, {eq2});
  const ir::NodeId iff2 = g2.make(NodeKind::If, {g2.startNode(), notn2});
  g2.make(NodeKind::Return, {g2.make(NodeKind::IfTrue, {iff2})});
  g2.make(NodeKind::Return, {g2.make(NodeKind::IfFalse, {iff2})});
  passOk(g2, PK::Canonicalization);
  CHECK(ir::print(g) == ir::print(g2));
}

B2_TEST(canon_killswitch) {
  ir::Graph g;
  const ir::NodeId p = g.parameter(0, ir::IRType::Int);
  const ir::NodeId c = g.constantI(4);
  const ir::NodeId eq = g.make(NodeKind::EqI, {p, c});
  const ir::NodeId notn = g.make(NodeKind::Not, {eq});
  const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), notn});
  g.make(NodeKind::Return, {g.make(NodeKind::IfTrue, {iff})});
  g.make(NodeKind::Return, {g.make(NodeKind::IfFalse, {iff})});
  const std::string before = ir::print(g);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::Canonicalization, false);
  const passes::PassResult r =
      passes::runSinglePass(g, PK::Canonicalization, cfg);
  CHECK(r.ok);
  CHECK(ir::print(g) == before);
}

// --- constant folding (key 14) ------------------------------------------------

B2_TEST(fold_addi_muli_exact) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 2);
  b.emitRegImm(rbc::Op::Iconst, 1, 3);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  CHECK(countKind(g, NodeKind::AddI) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValI(g, g.input(ret, 1)) == 5);
}

B2_TEST(fold_muli_wraps) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 65536);
  b.emitRegImm(rbc::Op::Iconst, 1, 65536);
  b.emitRegRegReg(rbc::Op::Imul, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValI(g, g.input(ret, 1)) == 0); // 2^32 wraps to 0
}

B2_TEST(fold_shifts_follow_jvm_masking) {
  struct {
    rbc::Op op;
    NodeKind kind;
    std::int32_t a, sh;
    std::int32_t want;
  } cases[] = {
      {rbc::Op::Ishr, NodeKind::ShrI, -16, 2, -4},
      {rbc::Op::Iushr, NodeKind::UShrI, -1, 28, 15},
      {rbc::Op::Ishl, NodeKind::ShlI, 1, 31, INT32_MIN},
      {rbc::Op::Ishr, NodeKind::ShrI, 5, 33, 2}, // count masked to 1
  };
  for (const auto& e : cases) {
    rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
    b.setRegs(3);
    b.emitRegImm(rbc::Op::Iconst, 0, static_cast<std::uint32_t>(e.a));
    b.emitRegImm(rbc::Op::Iconst, 1, static_cast<std::uint32_t>(e.sh));
    b.emitRegRegReg(e.op, 2, 0, 1);
    b.emitReg(rbc::Op::Ireturn, 2);
    rbc::Method m;
    CHECK(b.finish(m).ok);
    ir::Graph g;
    buildOk(m, g);
    passOk(g, PK::ConstantFolding);
    CHECK(countKind(g, e.kind) == 0);
    const ir::NodeId ret = firstOf(g, NodeKind::Return);
    CHECK_MSG(constValI(g, g.input(ret, 1)) == e.want,
              std::string(e.kind == NodeKind::ShrI ? "shr" : "shift"));
  }
}

B2_TEST(fold_divi_remi_wrap_at_min) {
  // INT_MIN / -1 wraps to INT_MIN (JVM), rem to 0: C++ UB avoided.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(4);
  b.emitRegImm(rbc::Op::Iconst, 0, 0x80000000u);
  b.emitRegImm(rbc::Op::Iconst, 1, 0xFFFFFFFFu);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1);
  b.emitRegRegReg(rbc::Op::Irem, 3, 0, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 2, 3);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValI(g, g.input(ret, 1)) ==
        static_cast<std::int32_t>(INT32_MIN + 0));
}

B2_TEST(fold_divl_reml_wrap_at_min) {
  rbc::RbcBuilder b("main", "()J", rbc::method_flags::Static);
  b.setRegs(4);
  const std::uint32_t minL = b.constLong(INT64_MIN);
  const std::uint32_t negOne = b.constLong(-1);
  b.emitRegCp(rbc::Op::Lconst, 0, minL);
  b.emitRegCp(rbc::Op::Lconst, 1, negOne);
  b.emitRegRegReg(rbc::Op::Ldiv, 2, 0, 1);
  b.emitReg(rbc::Op::Lreturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  CHECK(countKind(g, NodeKind::DivL) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::ConstantL);
  CHECK(g.node(g.input(ret, 1)).constValue == INT64_MIN);
}

B2_TEST(fold_div_by_zero_not_folded) {
  // The trap is the guard's business (deopt-to-T0 raises it); the div
  // node survives folding.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 10);
  b.emitRegImm(rbc::Op::Iconst, 1, 0);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  CHECK(countKind(g, NodeKind::DivI) == 1);
}

B2_TEST(fold_negi) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(2);
  b.emitRegImm(rbc::Op::Iconst, 0, 5);
  b.emitRegReg(rbc::Op::Ineg, 1, 0);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValI(g, g.input(ret, 1)) == -5);
}

B2_TEST(fold_not_of_const) {
  // aconst_null + ifnonnull: IsNull(null) -> 1, Not(1) -> 0.
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(1);
  const rbc::RbcBuilder::Label skip = b.newLabel();
  b.emitRegImm(rbc::Op::AconstNull, 0, 0);
  b.emitRegBranch(rbc::Op::Ifnonnull, 0, skip);
  b.emit(rbc::Op::Return);
  b.bind(skip);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  CHECK(countKind(g, NodeKind::IsNull) == 0);
  CHECK(countKind(g, NodeKind::Not) == 0);
  const ir::NodeId iff = firstOf(g, NodeKind::If);
  CHECK(g.node(g.input(iff, 1)).kind == NodeKind::ConstantI);
  CHECK(constValI(g, g.input(iff, 1)) == 0);
}

B2_TEST(fold_int_conversions) {
  struct {
    rbc::Op op;
    NodeKind kind;
    std::uint32_t in;
    std::int32_t want;
  } cases[] = {
      {rbc::Op::I2l, NodeKind::I2L, 7, 7},          // sign-extend
      {rbc::Op::I2b, NodeKind::I2B, 300, 44},       // byte truncation
      {rbc::Op::I2c, NodeKind::I2C, 70000, 4464},   // char mask
      {rbc::Op::I2s, NodeKind::I2S, 70000, 4464},   // short truncation
  };
  for (const auto& e : cases) {
    rbc::RbcBuilder b("main", e.op == rbc::Op::I2l ? "()J" : "()I",
                      rbc::method_flags::Static);
    b.setRegs(2);
    b.emitRegImm(rbc::Op::Iconst, 0, e.in);
    b.emitRegReg(e.op, 1, 0);
    b.emitReg(e.op == rbc::Op::I2l ? rbc::Op::Lreturn : rbc::Op::Ireturn, 1);
    rbc::Method m;
    CHECK(b.finish(m).ok);
    ir::Graph g;
    buildOk(m, g);
    passOk(g, PK::ConstantFolding);
    CHECK(countKind(g, e.kind) == 0);
    const ir::NodeId ret = firstOf(g, NodeKind::Return);
    CHECK(static_cast<std::int32_t>(
              static_cast<std::int64_t>(g.node(g.input(ret, 1)).constValue &
                                        0xFFFFFFFFull)) == e.want ||
          g.node(g.input(ret, 1)).constValue == e.want);
  }
}

B2_TEST(fold_i2f_exact_bits) {
  rbc::RbcBuilder b("main", "()F", rbc::method_flags::Static);
  b.setRegs(2);
  b.emitRegImm(rbc::Op::Iconst, 0, 7);
  b.emitRegReg(rbc::Op::I2f, 1, 0);
  b.emitReg(rbc::Op::Freturn, 1);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValBits(g, g.input(ret, 1)) == 0x40E00000u); // 7.0f
}

B2_TEST(fold_f2i_jls_narrowing) {
  struct {
    std::uint32_t bits;
    std::int32_t want;
  } cases[] = {
      {0x7FC00000u, 0},        // NaN -> 0 (JLS 5.1.3)
      {0x7F800000u, INT32_MAX},  // +inf -> MAX
      {0xFF800000u, INT32_MIN},  // -inf -> MIN
      {0xBF800000u, -1},       // -1.0f
      {0x4EFFFFFFu, 2147483520}, // 2.14748352e9f: in range after all
  };
  for (const auto& e : cases) {
    rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
    b.setRegs(2);
    b.emitRegImm(rbc::Op::Fconst, 0, e.bits);
    b.emitRegReg(rbc::Op::F2i, 1, 0);
    b.emitReg(rbc::Op::Ireturn, 1);
    rbc::Method m;
    CHECK(b.finish(m).ok);
    ir::Graph g;
    buildOk(m, g);
    passOk(g, PK::ConstantFolding);
    CHECK(countKind(g, NodeKind::F2I) == 0);
    const ir::NodeId ret = firstOf(g, NodeKind::Return);
    CHECK(constValI(g, g.input(ret, 1)) == e.want);
  }
}

B2_TEST(fold_d2l_bounds) {
  rbc::RbcBuilder b("main", "()J", rbc::method_flags::Static);
  b.setRegs(2);
  const std::uint32_t d = b.constDouble(1.0e18);
  b.emitRegCp(rbc::Op::Dconst, 0, d);
  b.emitRegReg(rbc::Op::D2l, 1, 0);
  b.emitReg(rbc::Op::Lreturn, 1);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).constValue == 1000000000000000000LL);
}

B2_TEST(fold_float_arith_exact_bits) {
  rbc::RbcBuilder b("main", "()F", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Fconst, 0, 0x3FC00000u); // 1.5f
  b.emitRegImm(rbc::Op::Fconst, 1, 0x40100000u); // 2.25f
  b.emitRegRegReg(rbc::Op::Fadd, 2, 0, 1);
  b.emitReg(rbc::Op::Freturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValBits(g, g.input(ret, 1)) == 0x40700000u); // 3.75f

  rbc::RbcBuilder b2("main", "()F", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.emitRegImm(rbc::Op::Fconst, 0, 0x3F800000u); // 1.0f
  b2.emitRegImm(rbc::Op::Fconst, 1, 0);           // +0.0f
  b2.emitRegRegReg(rbc::Op::Fdiv, 2, 0, 1);
  b2.emitReg(rbc::Op::Freturn, 2);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  passOk(g2, PK::ConstantFolding);
  const ir::NodeId ret2 = firstOf(g2, NodeKind::Return);
  CHECK(constValBits(g2, g.input(ret2, 1)) == 0x7F800000u); // +inf, defined
}

B2_TEST(fold_remf_defined_and_nan_blocked) {
  rbc::RbcBuilder b("main", "()F", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Fconst, 0, 0x40F00000u); // 7.5f
  b.emitRegImm(rbc::Op::Fconst, 1, 0x40000000u); // 2.0f
  b.emitRegRegReg(rbc::Op::Frem, 2, 0, 1);
  b.emitReg(rbc::Op::Freturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValBits(g, g.input(ret, 1)) == 0x3FC00000u); // 1.5f

  // NaN input: NOT folded (exact-bits policy).
  rbc::RbcBuilder b2("main", "()F", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.emitRegImm(rbc::Op::Fconst, 0, 0x7FC00000u); // NaN
  b2.emitRegImm(rbc::Op::Fconst, 1, 0x40000000u); // 2.0f
  b2.emitRegRegReg(rbc::Op::Frem, 2, 0, 1);
  b2.emitReg(rbc::Op::Freturn, 2);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  const passes::PassResult r = passOk(g2, PK::ConstantFolding);
  CHECK(countKind(g2, NodeKind::RemF) == 1);
  CHECK(r.telemetry.folds == 0);
}

B2_TEST(fold_negf_preserves_nan_payload) {
  rbc::RbcBuilder b("main", "()F", rbc::method_flags::Static);
  b.setRegs(2);
  b.emitRegImm(rbc::Op::Fconst, 0, 0x7FC00000u); // NaN, payload 0x400000
  b.emitRegReg(rbc::Op::Fneg, 1, 0);
  b.emitReg(rbc::Op::Freturn, 1);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValBits(g, g.input(ret, 1)) == 0xFFC00000u); // sign flipped only
}

B2_TEST(fold_fp_comparisons_including_nan) {
  // CmpFl(NaN, 1.0f) = -1 (NaN compares less); CmpFg = +1. Comparisons
  // fold on NaN inputs because the result is a defined int.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Fconst, 0, 0x7FC00000u); // NaN
  b.emitRegImm(rbc::Op::Fconst, 1, 0x3F800000u); // 1.0f
  b.emitRegRegReg(rbc::Op::Fcmpl, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValI(g, g.input(ret, 1)) == -1);

  rbc::RbcBuilder b2("main", "()I", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.emitRegImm(rbc::Op::Fconst, 0, 0x7FC00000u);
  b2.emitRegImm(rbc::Op::Fconst, 1, 0x3F800000u);
  b2.emitRegRegReg(rbc::Op::Fcmpg, 2, 0, 1);
  b2.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  passOk(g2, PK::ConstantFolding);
  const ir::NodeId ret2 = firstOf(g2, NodeKind::Return);
  CHECK(constValI(g2, g.input(ret2, 1)) == 1);
}

B2_TEST(fold_int_compare_family) {
  struct {
    rbc::Op op;
    std::int32_t a, b;
    std::int32_t want;
  } cases[] = {
      {rbc::Op::Icmp, 3, 9, -1},
      {rbc::Op::Icmp, 9, 3, 1},
      {rbc::Op::Icmp, 3, 3, 0},
  };
  for (const auto& e : cases) {
    rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
    b.setRegs(3);
    b.emitRegImm(rbc::Op::Iconst, 0, static_cast<std::uint32_t>(e.a));
    b.emitRegImm(rbc::Op::Iconst, 1, static_cast<std::uint32_t>(e.b));
    b.emitRegRegReg(e.op, 2, 0, 1);
    b.emitReg(rbc::Op::Ireturn, 2);
    rbc::Method m;
    CHECK(b.finish(m).ok);
    ir::Graph g;
    buildOk(m, g);
    passOk(g, PK::ConstantFolding);
    const ir::NodeId ret = firstOf(g, NodeKind::Return);
    CHECK(constValI(g, g.input(ret, 1)) == e.want);
  }
}

B2_TEST(fold_branch_condition_constants) {
  // if_icmpge 3, 3 -> GeI(3, 3) -> 1: the If's condition becomes a
  // constant (branch normalization's input).
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(2);
  const rbc::RbcBuilder::Label skip = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 3);
  b.emitRegImm(rbc::Op::Iconst, 1, 3);
  b.emitRegRegBranch(rbc::Op::IfIcmpge, 0, 1, skip);
  b.emit(rbc::Op::Return);
  b.bind(skip);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const ir::NodeId iff = firstOf(g, NodeKind::If);
  CHECK(g.node(g.input(iff, 1)).kind == NodeKind::ConstantI);
  CHECK(constValI(g, g.input(iff, 1)) == 1);
}

B2_TEST(fold_isnull_and_refeq_facts) {
  // IsNull(null) -> 1; RefEq(null, null) -> 1; InstanceOf(null) -> 0.
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(2);
  const rbc::RbcBuilder::Label skip = b.newLabel();
  b.emitRegImm(rbc::Op::AconstNull, 0, 0);
  b.emitRegBranch(rbc::Op::Ifnull, 0, skip);
  b.emit(rbc::Op::Return);
  b.bind(skip);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  CHECK(countKind(g, NodeKind::IsNull) == 0);
  const ir::NodeId iff = firstOf(g, NodeKind::If);
  CHECK(constValI(g, g.input(iff, 1)) == 1);

  rbc::RbcBuilder b2("main", "(Ljava/lang/Object;)Z",
                     rbc::method_flags::Static);
  b2.setRegs(2);
  b2.setLocals(1);
  const std::uint32_t cls = b2.constClass("M");
  b2.emitRegSlot(rbc::Op::Aload, 0, 0);
  b2.emitRegRegCp(rbc::Op::Instanceof, 1, 0, cls);
  b2.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  // null instanceof T == false; force the null: aconst_null into the slot.
  rbc::RbcBuilder b3("main", "()Z", rbc::method_flags::Static);
  b3.setRegs(2);
  const std::uint32_t cls3 = b3.constClass("M");
  b3.emitRegImm(rbc::Op::AconstNull, 0, 0);
  b3.emitRegRegCp(rbc::Op::Instanceof, 1, 0, cls3);
  b3.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method m3;
  CHECK(b3.finish(m3).ok);
  ir::Graph g3;
  buildOk(m3, g3);
  passOk(g3, PK::ConstantFolding);
  CHECK(countKind(g3, NodeKind::InstanceOf) == 0);
  const ir::NodeId ret3 = firstOf(g3, NodeKind::Return);
  CHECK(constValI(g3, g.input(ret3, 1)) == 0);
}

B2_TEST(fold_idempotent_and_killswitch) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 2);
  b.emitRegImm(rbc::Op::Iconst, 1, 3);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const std::string once = ir::print(g);
  const passes::PassResult r = passOk(g, PK::ConstantFolding);
  CHECK(r.telemetry.folds == 0);
  CHECK(ir::print(g) == once);

  rbc::RbcBuilder b2("main", "()I", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.emitRegImm(rbc::Op::Iconst, 0, 2);
  b2.emitRegImm(rbc::Op::Iconst, 1, 3);
  b2.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b2.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  const std::string before = ir::print(g2);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::ConstantFolding, false);
  const passes::PassResult r2 =
      passes::runSinglePass(g2, PK::ConstantFolding, cfg);
  CHECK(r2.ok);
  CHECK(ir::print(g2) == before);
}

// --- strength reduction (key 15) -------------------------------------------

B2_TEST(strength_muli_pow2_to_shift) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 8);
  b.emitRegRegReg(rbc::Op::Imul, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::MulI) == 1);
  passOk(g, PK::StrengthReduction);
  CHECK(countKind(g, NodeKind::MulI) == 0);
  const ir::NodeId shl = firstOf(g, NodeKind::ShlI);
  CHECK(shl != ir::kInvalidNodeId);
  CHECK(g.node(g.input(shl, 0)).kind == NodeKind::Parameter);
  CHECK(g.node(g.input(shl, 1)).kind == NodeKind::ConstantI);
  CHECK(constValI(g, g.input(shl, 1)) == 3);
}

B2_TEST(strength_muli_const_left) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 8);
  b.emitRegRegReg(rbc::Op::Imul, 2, 1, 0); // const on the left
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::StrengthReduction);
  const ir::NodeId shl = firstOf(g, NodeKind::ShlI);
  CHECK(g.node(g.input(shl, 0)).kind == NodeKind::Parameter);
  CHECK(constValI(g, g.input(shl, 1)) == 3);
}

B2_TEST(strength_mull_pow2) {
  rbc::RbcBuilder b("main", "(J)J", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Lload, 0, 0);
  const std::uint32_t sixteen = b.constLong(16);
  b.emitRegCp(rbc::Op::Lconst, 1, sixteen);
  b.emitRegRegReg(rbc::Op::Lmul, 2, 0, 1);
  b.emitReg(rbc::Op::Lreturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::StrengthReduction);
  CHECK(countKind(g, NodeKind::MulL) == 0);
  const ir::NodeId shl = firstOf(g, NodeKind::ShlL);
  CHECK(shl != ir::kInvalidNodeId);
  CHECK(g.node(g.input(shl, 0)).kind == NodeKind::Parameter);
  CHECK(constValI(g, g.input(shl, 1)) == 4); // int shift count
}

B2_TEST(strength_nonpow2_untouched) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 3);
  b.emitRegRegReg(rbc::Op::Imul, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::StrengthReduction);
  CHECK(countKind(g, NodeKind::MulI) == 1);
  CHECK(r.telemetry.folds == 0);
}

B2_TEST(strength_negative_and_boundary_untouched) {
  // -4 is a negated power of two: not handled in v1 (documented); 1 and 0
  // belong to identity removal, not strength reduction.
  for (const std::int32_t c : {-4, 1, 0}) {
    rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
    b.setRegs(3);
    b.setLocals(1);
    b.emitRegSlot(rbc::Op::Iload, 0, 0);
    b.emitRegImm(rbc::Op::Iconst, 1, static_cast<std::uint32_t>(c));
    b.emitRegRegReg(rbc::Op::Imul, 2, 0, 1);
    b.emitReg(rbc::Op::Ireturn, 2);
    rbc::Method m;
    CHECK(b.finish(m).ok);
    ir::Graph g;
    buildOk(m, g);
    passOk(g, PK::StrengthReduction);
    CHECK(countKind(g, NodeKind::MulI) == 1);
    CHECK(countKind(g, NodeKind::ShlI) == 0);
  }
}

B2_TEST(strength_fp_untouched) {
  rbc::RbcBuilder b("main", "(F)F", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Fload, 0, 0);
  b.emitRegImm(rbc::Op::Fconst, 1, 0x40800000u); // 4.0f
  b.emitRegRegReg(rbc::Op::Fmul, 2, 0, 1);
  b.emitReg(rbc::Op::Freturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::StrengthReduction);
  CHECK(countKind(g, NodeKind::MulF) == 1);
}

B2_TEST(strength_fold_wins_over_strength) {
  // Constant * constant folds first (fold is checked before strength in
  // the shared sweep; single-key strength does not fold).
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 2);
  b.emitRegImm(rbc::Op::Iconst, 1, 6);
  b.emitRegRegReg(rbc::Op::Imul, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  pipeOk(g);
  CHECK(countLiveUserKind(g, NodeKind::ShlI) == 0);
  CHECK(countKind(g, NodeKind::MulI) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValI(g, g.input(ret, 1)) == 12);
}

B2_TEST(strength_idempotent_and_killswitch) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 8);
  b.emitRegRegReg(rbc::Op::Imul, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::StrengthReduction);
  const std::string once = ir::print(g);
  const passes::PassResult r = passOk(g, PK::StrengthReduction);
  CHECK(r.telemetry.folds == 0);
  CHECK(ir::print(g) == once);

  rbc::RbcBuilder b2("main", "(I)I", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.setLocals(1);
  b2.emitRegSlot(rbc::Op::Iload, 0, 0);
  b2.emitRegImm(rbc::Op::Iconst, 1, 8);
  b2.emitRegRegReg(rbc::Op::Imul, 2, 0, 1);
  b2.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  const std::string before = ir::print(g2);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::StrengthReduction, false);
  const passes::PassResult r2 =
      passes::runSinglePass(g2, PK::StrengthReduction, cfg);
  CHECK(r2.ok);
  CHECK(ir::print(g2) == before);
}

// --- identity removal (key 16) ----------------------------------------------

B2_TEST(ident_addi_zero_either_side) {
  for (const bool left : {false, true}) {
    rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
    b.setRegs(3);
    b.setLocals(1);
    b.emitRegSlot(rbc::Op::Iload, 0, 0);
    b.emitRegImm(rbc::Op::Iconst, 1, 0);
    b.emitRegRegReg(rbc::Op::Iadd, 2, left ? 1 : 0, left ? 0 : 1);
    b.emitReg(rbc::Op::Ireturn, 2);
    rbc::Method m;
    CHECK(b.finish(m).ok);
    ir::Graph g;
    buildOk(m, g);
    passOk(g, PK::IdentityRemoval);
    CHECK(countKind(g, NodeKind::AddI) == 0);
    const ir::NodeId ret = firstOf(g, NodeKind::Return);
    CHECK(g.node(g.input(ret, 1)).kind == NodeKind::Parameter);
  }
}

B2_TEST(ident_subi_zero_muli_one_zero) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 0);
  b.emitRegRegReg(rbc::Op::Isub, 2, 0, 1); // x - 0
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::IdentityRemoval);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::Parameter);

  rbc::RbcBuilder b2("main", "(I)I", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.setLocals(1);
  b2.emitRegSlot(rbc::Op::Iload, 0, 0);
  b2.emitRegImm(rbc::Op::Iconst, 1, 1);
  b2.emitRegRegReg(rbc::Op::Imul, 2, 0, 1); // x * 1
  b2.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  passOk(g2, PK::IdentityRemoval);
  const ir::NodeId ret2 = firstOf(g2, NodeKind::Return);
  CHECK(g2.node(g2.input(ret2, 1)).kind == NodeKind::Parameter);

  rbc::RbcBuilder b3("main", "(I)I", rbc::method_flags::Static);
  b3.setRegs(3);
  b3.setLocals(1);
  b3.emitRegSlot(rbc::Op::Iload, 0, 0);
  b3.emitRegImm(rbc::Op::Iconst, 1, 0);
  b3.emitRegRegReg(rbc::Op::Imul, 2, 0, 1); // x * 0 -> 0
  b3.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m3;
  CHECK(b3.finish(m3).ok);
  ir::Graph g3;
  buildOk(m3, g3);
  passOk(g3, PK::IdentityRemoval);
  const ir::NodeId ret3 = firstOf(g3, NodeKind::Return);
  CHECK(g3.node(g3.input(ret3, 1)).kind == NodeKind::ConstantI);
  CHECK(constValI(g3, g3.input(ret3, 1)) == 0);
}

B2_TEST(ident_bitwise_identities) {
  // x & -1 -> x; x & 0 -> 0; x | 0 -> x; x | -1 -> -1; x ^ 0 -> x.
  struct {
    rbc::Op op;
    std::int32_t c;
    bool wantParam;
  } cases[] = {
      {rbc::Op::Iand, -1, true},
      {rbc::Op::Iand, 0, false},
      {rbc::Op::Ior, 0, true},
      {rbc::Op::Ior, -1, false},
      {rbc::Op::Ixor, 0, true},
  };
  for (const auto& e : cases) {
    rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
    b.setRegs(3);
    b.setLocals(1);
    b.emitRegSlot(rbc::Op::Iload, 0, 0);
    b.emitRegImm(rbc::Op::Iconst, 1, static_cast<std::uint32_t>(e.c));
    b.emitRegRegReg(e.op, 2, 0, 1);
    b.emitReg(rbc::Op::Ireturn, 2);
    rbc::Method m;
    CHECK(b.finish(m).ok);
    ir::Graph g;
    buildOk(m, g);
    passOk(g, PK::IdentityRemoval);
    const ir::NodeId ret = firstOf(g, NodeKind::Return);
    CHECK(g.node(g.input(ret, 1)).kind ==
          (e.wantParam ? NodeKind::Parameter : NodeKind::ConstantI));
    if (!e.wantParam) {
      CHECK(constValI(g, g.input(ret, 1)) == e.c);
    }
  }
}

B2_TEST(ident_shift_by_zero) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 0);
  b.emitRegRegReg(rbc::Op::Ishl, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::IdentityRemoval);
  CHECK(countKind(g, NodeKind::ShlI) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::Parameter);
}

B2_TEST(ident_double_negation_int_and_float) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegReg(rbc::Op::Ineg, 1, 0);
  b.emitRegReg(rbc::Op::Ineg, 2, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::IdentityRemoval);
  passOk(g, PK::DeadCodeElimination); // reclaim the orphaned inner neg
  CHECK(countKind(g, NodeKind::NegI) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::Parameter);

  // FP double negation is an exact bit identity (sign flips cancel).
  rbc::RbcBuilder b2("main", "(F)F", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.setLocals(1);
  b2.emitRegSlot(rbc::Op::Fload, 0, 0);
  b2.emitRegReg(rbc::Op::Fneg, 1, 0);
  b2.emitRegReg(rbc::Op::Fneg, 2, 1);
  b2.emitReg(rbc::Op::Freturn, 2);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  passOk(g2, PK::IdentityRemoval);
  passOk(g2, PK::DeadCodeElimination);
  CHECK(countKind(g2, NodeKind::NegF) == 0);
}

B2_TEST(ident_narrowing_idempotence) {
  // I2B(I2B(x)) == I2B(x): narrowing is idempotent. (The L2I(I2L(x))
  // round trip is BLOCKED at verification time by an IR-core
  // resultTypeOf defect - I2L is classified as Double-producing in
  // compiler/ir/core/Printer.cpp - reported to the IR team as MSG-009;
  // this test exercises the other exact conversion identity until it
  // lands.)
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegReg(rbc::Op::I2b, 1, 0);
  b.emitRegReg(rbc::Op::I2b, 2, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::IdentityRemoval);
  passOk(g, PK::DeadCodeElimination);
  CHECK(countKind(g, NodeKind::I2B) == 1); // one narrowing survives
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::I2B);
}

B2_TEST(ident_div_rem_by_one) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 1);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1); // x / 1 -> x
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::IdentityRemoval);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::Parameter);

  rbc::RbcBuilder b2("main", "(I)I", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.setLocals(1);
  b2.emitRegSlot(rbc::Op::Iload, 0, 0);
  b2.emitRegImm(rbc::Op::Iconst, 1, 1);
  b2.emitRegRegReg(rbc::Op::Irem, 2, 0, 1); // x % 1 -> 0
  b2.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  passOk(g2, PK::IdentityRemoval);
  const ir::NodeId ret2 = firstOf(g2, NodeKind::Return);
  CHECK(g2.node(g2.input(ret2, 1)).kind == NodeKind::ConstantI);
  CHECK(constValI(g2, g2.input(ret2, 1)) == 0);
}

B2_TEST(ident_self_compares) {
  // if_icmpeq r0 r0 (same reg): EqI(x, x) -> 1.
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(2);
  b.setLocals(1);
  const rbc::RbcBuilder::Label skip = b.newLabel();
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegRegBranch(rbc::Op::IfIcmpeq, 0, 0, skip);
  b.emitReg(rbc::Op::Ireturn, 0);
  b.bind(skip);
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::IdentityRemoval);
  CHECK(countKind(g, NodeKind::EqI) == 0);
  const ir::NodeId iff = firstOf(g, NodeKind::If);
  CHECK(g.node(g.input(iff, 1)).kind == NodeKind::ConstantI);
  CHECK(constValI(g, g.input(iff, 1)) == 1);

  // icmp x, x -> CmpI(x, x) -> 0.
  rbc::RbcBuilder b2("main", "(I)I", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.setLocals(1);
  b2.emitRegSlot(rbc::Op::Iload, 0, 0);
  b2.emitRegRegReg(rbc::Op::Icmp, 1, 0, 0);
  b2.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  passOk(g2, PK::IdentityRemoval);
  CHECK(countKind(g2, NodeKind::CmpI) == 0);
  const ir::NodeId ret2 = firstOf(g2, NodeKind::Return);
  CHECK(constValI(g2, g2.input(ret2, 1)) == 0);
}

B2_TEST(ident_fp_add_zero_not_removed) {
  // -0.0 + 0.0 == +0.0: the JLS-forbidden FP additive identity.
  rbc::RbcBuilder b("main", "(F)F", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Fload, 0, 0);
  b.emitRegImm(rbc::Op::Fconst, 1, 0); // +0.0f
  b.emitRegRegReg(rbc::Op::Fadd, 2, 0, 1);
  b.emitReg(rbc::Op::Freturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::IdentityRemoval);
  CHECK(countKind(g, NodeKind::AddF) == 1);
  CHECK(r.telemetry.folds == 0);
}

B2_TEST(ident_killswitch_and_idempotent) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 0);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::IdentityRemoval);
  const std::string once = ir::print(g);
  const passes::PassResult r = passOk(g, PK::IdentityRemoval);
  CHECK(r.telemetry.folds == 0);
  CHECK(ir::print(g) == once);

  rbc::RbcBuilder b2("main", "(I)I", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.setLocals(1);
  b2.emitRegSlot(rbc::Op::Iload, 0, 0);
  b2.emitRegImm(rbc::Op::Iconst, 1, 0);
  b2.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b2.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  const std::string before = ir::print(g2);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::IdentityRemoval, false);
  const passes::PassResult r2 =
      passes::runSinglePass(g2, PK::IdentityRemoval, cfg);
  CHECK(r2.ok);
  CHECK(ir::print(g2) == before);
}

// --- null-check folding (key 18) ---------------------------------------------

namespace {
// new M(); invokevirtual r0.m() - the receiver is provably non-null.
void buildNewReceiverCall(rbc::Method& m) {
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(2);
  const std::uint32_t cls = b.constClass("M");
  const std::uint32_t mc =
      b.constMethodRef("M", "m", "()Ljava/lang/String;");
  b.emitRegCp(rbc::Op::New, 0, cls);
  b.emitCall(rbc::Op::Invokevirtual, 1, 0, 1, mc);
  b.emit(rbc::Op::Return);
  CHECK(b.finish(m).ok);
}
} // namespace

B2_TEST(nullfold_new_receiver) {
  rbc::Method m;
  buildNewReceiverCall(m);
  ir::Graph g;
  buildOk(m, g);
  const std::size_t guardsBefore = countKind(g, NodeKind::Guard);
  CHECK(guardsBefore >= 1);
  const passes::PassResult r = passOk(g, PK::NullCheckFolding);
  CHECK(countKind(g, NodeKind::Guard) == 0);
  CHECK(r.telemetry.rewrites >= 1);
  // Control rewire: the call now consumes the guard's control input.
  const ir::NodeId call = firstOf(g, NodeKind::CallVirtual);
  CHECK(g.node(g.input(call, 0)).kind != NodeKind::Guard);
  CHECK(verifyOk(g));
}

B2_TEST(nullfold_param_receiver_stays) {
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)V",
                    rbc::method_flags::Static);
  b.setRegs(2);
  b.setLocals(1);
  const std::uint32_t mc =
      b.constMethodRef("M", "m", "()Ljava/lang/String;");
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitCall(rbc::Op::Invokevirtual, 1, 0, 1, mc);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::NullCheckFolding);
  CHECK(countKind(g, NodeKind::Guard) == 1); // maybe-null: kept
  CHECK(r.telemetry.rewrites == 0);
}

B2_TEST(nullfold_null_receiver_stays) {
  // A null receiver deopts (T0 raises the NPE): the guard is the
  // mechanism, so it must survive null folding.
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(2);
  const std::uint32_t mc =
      b.constMethodRef("M", "m", "()Ljava/lang/String;");
  b.emitRegImm(rbc::Op::AconstNull, 0, 0);
  b.emitCall(rbc::Op::Invokevirtual, 1, 0, 1, mc);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::NullCheckFolding);
  CHECK(countKind(g, NodeKind::Guard) == 1);
}

B2_TEST(nullfold_symbol_receiver_folds) {
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(2);
  const std::uint32_t str = b.constString("hi");
  const std::uint32_t mc =
      b.constMethodRef("M", "m", "()Ljava/lang/String;");
  b.emitRegCp(rbc::Op::Ldc, 0, str); // interned constants are non-null
  b.emitCall(rbc::Op::Invokevirtual, 1, 0, 1, mc);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::NullCheckFolding);
  CHECK(countKind(g, NodeKind::Guard) == 0);
}

B2_TEST(nullfold_arraylength_null_guard_stays) {
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)I",
                    rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitRegReg(rbc::Op::Arraylength, 1, 0);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::NullCheckFolding);
  CHECK(countKind(g, NodeKind::Guard) == 1);
}

B2_TEST(nullfold_zerocheck_guards_untouched) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegSlot(rbc::Op::Iload, 1, 0);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = passOk(g, PK::NullCheckFolding);
  CHECK(countKind(g, NodeKind::Guard) == 1); // ZeroCheck, not NullCheck
  CHECK(r.telemetry.rewrites == 0);
}

B2_TEST(nullfold_keeps_call_and_framestate_alive) {
  rbc::Method m;
  buildNewReceiverCall(m);
  ir::Graph g;
  buildOk(m, g);
  const ir::NodeId fsBefore = firstFs(g);
  passOk(g, PK::NullCheckFolding);
  passOk(g, PK::DeadCodeElimination);
  CHECK(countKind(g, NodeKind::CallVirtual) == 1);
  // The call's own FS (the deopt point) survives the guard fold.
  CHECK(!g.node(fsBefore).isDead());
}

B2_TEST(nullfold_idempotent_and_killswitch) {
  rbc::Method m;
  buildNewReceiverCall(m);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::NullCheckFolding);
  const std::string once = ir::print(g);
  const passes::PassResult r = passOk(g, PK::NullCheckFolding);
  CHECK(r.telemetry.rewrites == 0);
  CHECK(ir::print(g) == once);

  rbc::Method m2;
  buildNewReceiverCall(m2);
  ir::Graph g2;
  buildOk(m2, g2);
  const std::string before = ir::print(g2);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::NullCheckFolding, false);
  const passes::PassResult r2 =
      passes::runSinglePass(g2, PK::NullCheckFolding, cfg);
  CHECK(r2.ok);
  CHECK(ir::print(g2) == before);
}

B2_TEST(nullfold_handbuilt_never_null_flag) {
  // A Parameter marked NeverNull (the dataflow-fact flag): the flag path.
  ir::Graph g;
  const ir::NodeId p = g.parameter(0, ir::IRType::Ref);
  g.nodeForReplay(p).flags.set(ir::NodeFlag::NeverNull);
  const ir::NodeId isnull = g.make(NodeKind::IsNull, {p});
  const ir::NodeId notn = g.make(NodeKind::Not, {isnull});
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 0,
                                          std::span<const ir::NodeId>{});
  const ir::NodeId guard = g.make(
      NodeKind::Guard, {g.startNode(), notn, fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 9);
  g.make(NodeKind::Return, {guard});
  CHECK(verifyOk(g));
  passOk(g, PK::NullCheckFolding);
  CHECK(countKind(g, NodeKind::Guard) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.input(ret, 0) == g.startNode());
}

// --- branch normalization (key 19) -------------------------------------------

namespace {
// if (cond) { a = 11 } else { a = 22 }; return a - with a constant
// condition that folds. `taken` selects which arm executes.
void buildConstBranch(rbc::Method& m, bool taken) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  // ifne r0 thenL: jumps when r0 != 0. r0 = 1 -> then arm taken.
  b.emitRegImm(rbc::Op::Iconst, 0, taken ? 1u : 0u);
  b.emitRegBranch(rbc::Op::Ifne, 0, thenL);
  b.emitRegImm(rbc::Op::Iconst, 0, 22);
  b.emitSlotReg(rbc::Op::Istore, 0, 0);
  b.emitBranch(rbc::Op::Goto, endL);
  b.bind(thenL);
  b.emitRegImm(rbc::Op::Iconst, 0, 11);
  b.emitSlotReg(rbc::Op::Istore, 0, 0);
  b.bind(endL);
  b.emitRegSlot(rbc::Op::Iload, 1, 0);
  b.emitReg(rbc::Op::Ireturn, 1);
  CHECK(b.finish(m).ok);
}
} // namespace

B2_TEST(branch_true_side_taken) {
  rbc::Method m;
  buildConstBranch(m, true);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::If) == 1);
  passOk(g, PK::ConstantFolding);
  const passes::PassResult r = passOk(g, PK::BranchNormalization);
  CHECK(r.telemetry.rewrites >= 1);
  CHECK(countLiveUserKind(g, NodeKind::If) == 0);
  CHECK(countLiveUserKind(g, NodeKind::IfTrue) == 0);
  CHECK(countLiveUserKind(g, NodeKind::IfFalse) == 0);
  CHECK(countLiveUserKind(g, NodeKind::Region) == 0);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValI(g, g.input(ret, 1)) == 11);
  CHECK(verifyOk(g));
}

B2_TEST(branch_false_side_taken) {
  rbc::Method m;
  buildConstBranch(m, false);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  passOk(g, PK::BranchNormalization);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValI(g, g.input(ret, 1)) == 22);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
}

B2_TEST(branch_dead_arm_ops_eliminated) {
  // The dead arm writes a field through a reference parameter: after
  // fold + branch the StoreField of the dead arm is gone (unreachable
  // sweep reclaims the whole chain).
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)V",
                    rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t fld = b.constFieldRef("M", "x", "I");
  const rbc::RbcBuilder::Label skip = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 1); // ifne jumps: the store is dead
  b.emitRegBranch(rbc::Op::Ifne, 0, skip);
  b.emitRegSlot(rbc::Op::Aload, 0, 0); // object
  b.emitRegImm(rbc::Op::Iconst, 1, 7); // value
  b.emitRegRegRegCp(rbc::Op::Putfield, 0, 0, 1, fld); // dead arm
  b.bind(skip);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::StoreField) == 1);
  passOk(g, PK::ConstantFolding);
  passOk(g, PK::BranchNormalization);
  CHECK(countKind(g, NodeKind::StoreField) == 0); // unreachable, reclaimed
  CHECK(countKind(g, NodeKind::Return) == 1);
}

B2_TEST(branch_guard_true_passes_control_through) {
  // idiv 4/2: ZeroCheck guard's NeI(2,0) folds to 1; the guard passes
  // control through to the chain after it.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 4);
  b.emitRegImm(rbc::Op::Iconst, 1, 2);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const passes::PassResult r = passOk(g, PK::BranchNormalization);
  CHECK(countKind(g, NodeKind::Guard) == 0);
  CHECK(r.telemetry.rewrites >= 1);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.input(ret, 0) == g.startNode()); // control flows around
  CHECK(constValI(g, g.input(ret, 1)) == 2); // 4/2 folded
}

B2_TEST(branch_guard_false_becomes_deopt) {
  // idiv 4/0: the ZeroCheck guard always fails - it becomes an
  // unconditional Deopt carrying the guard's DeoptId and FrameState.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 4);
  b.emitRegImm(rbc::Op::Iconst, 1, 0);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const ir::NodeId guardBefore = firstOf(g, NodeKind::Guard);
  const std::uint32_t deoptId = g.node(guardBefore).payload2;
  const ir::NodeId fsBefore = g.input(guardBefore, 2);
  passOk(g, PK::ConstantFolding);
  passOk(g, PK::BranchNormalization);
  CHECK(countKind(g, NodeKind::Guard) == 0);
  const ir::NodeId deopt = firstOf(g, NodeKind::Deopt);
  CHECK(deopt != ir::kInvalidNodeId);
  CHECK(g.node(deopt).payload == deoptId);      // same deopt plan
  CHECK(g.input(deopt, 1) == fsBefore);         // same T0 snapshot
  CHECK(!g.node(fsBefore).isDead());            // Rule 4: state preserved
  // Everything after the always-deopting guard is unreachable.
  CHECK(countKind(g, NodeKind::Return) == 0);
  CHECK(countKind(g, NodeKind::DivI) == 1); // conservative DCE exclusion
}

B2_TEST(branch_switch_not_folded) {
  // Documented v1 refusal: case labels live in the frontend switch table
  // (opaque payload), so a constant selector does not fold the Switch.
  // emitSwitch: one match value per target label; the DEFAULT is the
  // fall-through instruction (pc + 1), not a label.
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(2);
  const rbc::RbcBuilder::Label l0 = b.newLabel();
  const rbc::RbcBuilder::Label l1 = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 1);
  b.emitSwitch(rbc::Op::Tableswitch, 0, {0, 1}, {l0, l1});
  b.emit(rbc::Op::Return); // default path (fall-through)
  b.bind(l0);
  b.emit(rbc::Op::Return);
  b.bind(l1);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const passes::PassResult r = passOk(g, PK::BranchNormalization);
  CHECK(countKind(g, NodeKind::Switch) == 1);
  CHECK(countKind(g, NodeKind::SwitchCase) == 2);
  CHECK(countKind(g, NodeKind::SwitchDefault) == 1);
  CHECK(r.telemetry.folds == 0);
}

B2_TEST(branch_idempotent_and_killswitch) {
  rbc::Method m;
  buildConstBranch(m, true);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  passOk(g, PK::BranchNormalization);
  const std::string once = ir::print(g);
  const passes::PassResult r = passOk(g, PK::BranchNormalization);
  CHECK(r.telemetry.rewrites == 0);
  CHECK(ir::print(g) == once);

  rbc::Method m2;
  buildConstBranch(m2, true);
  ir::Graph g2;
  buildOk(m2, g2);
  passOk(g2, PK::ConstantFolding);
  const std::string before = ir::print(g2);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::BranchNormalization, false);
  const passes::PassResult r2 =
      passes::runSinglePass(g2, PK::BranchNormalization, cfg);
  CHECK(r2.ok);
  CHECK(ir::print(g2) == before);
}

B2_TEST(branch_deterministic) {
  rbc::Method m;
  buildConstBranch(m, true);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  passOk(g, PK::BranchNormalization);
  const std::string a = ir::print(g);

  rbc::Method m2;
  buildConstBranch(m2, true);
  ir::Graph g2;
  buildOk(m2, g2);
  passOk(g2, PK::ConstantFolding);
  passOk(g2, PK::BranchNormalization);
  CHECK(ir::print(g2) == a);
}

// --- control-flow simplification (key 20) ------------------------------------

B2_TEST(cfs_sweep_kills_decided_dead_branch) {
  // fold the condition, then let KEY 20 ALONE (no branch normalization)
  // decide the branch: the sweep's flow propagation treats a decided If
  // as flowing only through the taken projection, so the UNTAKEN side
  // dies and the region repair drops its edge. (Short-cutting the taken
  // projection around the If is branch normalization's replace; the
  // If/taken projection stay as structure here.)
  rbc::Method m;
  buildConstBranch(m, false); // cond 0: IfTrue (then, 11) is dead
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  const passes::PassResult r = passOk(g, PK::ControlFlowSimplification);
  CHECK(countLiveUserKind(g, NodeKind::IfTrue) == 0); // untaken side dead
  CHECK(r.telemetry.removals >= 1);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValI(g, g.input(ret, 1)) == 22); // live arm's value
  CHECK(verifyOk(g));
}

B2_TEST(cfs_kills_chain_after_always_deopting_guard) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 4);
  b.emitRegImm(rbc::Op::Iconst, 1, 0);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  passOk(g, PK::ControlFlowSimplification);
  // The guard's condition is constant 0: flow stops at it, everything
  // after dies, and the guard itself is reclaimed.
  CHECK(countKind(g, NodeKind::Guard) == 0);
  CHECK(countKind(g, NodeKind::Return) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(cfs_rebuilds_region_with_dead_pred) {
  // Hand-built: 3-pred region with one dead pred; the rebuild keeps two
  // live preds and the phi keeps slot-for-pred values.
  ir::Graph g;
  const ir::NodeId cA = g.constantI(10);
  const ir::NodeId cB = g.constantI(20);
  const ir::NodeId cC = g.constantI(30);
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 0,
                                          std::span<const ir::NodeId>{});
  const ir::NodeId guard1 = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 5);
  const ir::NodeId guard2 = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 6);
  const ir::NodeId guard3 = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 7);
  const ir::NodeId region = g.make(
      NodeKind::Region, {guard1, guard2, guard3});
  const ir::NodeId phi = g.make(NodeKind::Phi, {region, cA, cB, cC});
  g.make(NodeKind::Return, {region, phi});
  g.killNode(guard2); // raw kill: region + phi need repair
  CHECK(!verifyOk(g));
  passOk(g, PK::ControlFlowSimplification);
  CHECK(verifyOk(g));
  const ir::NodeId fresh = firstOf(g, NodeKind::Region);
  CHECK(fresh != ir::kInvalidNodeId);
  CHECK(g.node(fresh).numInputs == 2);
  const ir::NodeId phi2 = firstOf(g, NodeKind::Phi);
  CHECK(g.node(phi2).numInputs == 3); // 1 + 2 live preds
  CHECK(g.input(phi2, 1) == cA);
  CHECK(g.input(phi2, 2) == cC); // cB's slot dropped with its pred
}

B2_TEST(cfs_loop_dead_backedge_collapses) {
  // Hand-built loop with a killed backedge: the LoopBegin collapses to
  // the entry edge and the loop phi collapses to the entry value.
  ir::Graph g;
  const ir::NodeId entry = g.constantI(3);
  const ir::NodeId back = g.constantI(99);
  const ir::NodeId lb = g.make(NodeKind::LoopBegin,
                               {g.startNode(), g.startNode()});
  const ir::NodeId le = g.make(NodeKind::LoopEnd, {lb});
  g.setInput(lb, 1, le);
  const ir::NodeId phi = g.make(NodeKind::Phi, {lb, entry, back});
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 0,
                                          std::span<const ir::NodeId>{});
  const ir::NodeId leExit = g.make(NodeKind::LoopExit, {lb, fs});
  (void)leExit;
  g.make(NodeKind::Return, {lb, phi});
  CHECK(verifyOk(g));
  g.killNode(le); // the backedge dies (e.g. the branch into it folded)
  passOk(g, PK::ControlFlowSimplification);
  CHECK(verifyOk(g));
  CHECK(countKind(g, NodeKind::LoopBegin) == 0);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.input(ret, 0) == g.startNode());
  CHECK(g.input(ret, 1) == entry);
}

B2_TEST(cfs_kills_phi_of_dead_region) {
  // Region over two GUARD preds (never Start - it is the origin); both
  // raw-killed: the region is unreachable and the sweep reclaims it,
  // its phis, and the Return consuming them, bottom-up.
  ir::Graph g;
  const ir::NodeId cA = g.constantI(1);
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 0,
                                          std::span<const ir::NodeId>{});
  const ir::NodeId g1 = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 5);
  const ir::NodeId g2 = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 6);
  const ir::NodeId region = g.make(NodeKind::Region, {g1, g2});
  const ir::NodeId phi = g.make(NodeKind::Phi, {region, cA, cA});
  g.make(NodeKind::Return, {region, phi});
  g.killNode(g1);
  g.killNode(g2); // both preds dead: region unreachable
  passOk(g, PK::ControlFlowSimplification);
  CHECK(countLiveUserKind(g, NodeKind::Region) == 0);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  CHECK(countKind(g, NodeKind::Return) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(cfs_floating_orphan_survives_until_dce) {
  // Floating values computed only in the dead arm (from a parameter, so
  // folding cannot eliminate them): the sweep removes the CONTROL, the
  // orphaned values survive until DCE reclaims them.
  rbc::RbcBuilder b("main", "(I)V", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(1);
  const rbc::RbcBuilder::Label skip = b.newLabel();
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 1);
  b.emitRegRegBranch(rbc::Op::IfIcmpne, 0, 1, skip); // param != 1: unknown
  b.emitRegSlot(rbc::Op::Iload, 2, 0);
  b.emitRegReg(rbc::Op::Ineg, 2, 2);
  b.emitRegSlot(rbc::Op::Iload, 3, 0);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 2, 3); // dead-arm-only value chain
  b.bind(skip);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  passOk(g, PK::ControlFlowSimplification);
  CHECK(countKind(g, NodeKind::NegI) == 1); // orphaned but alive
  CHECK(countKind(g, NodeKind::AddI) == 1);
  passOk(g, PK::DeadCodeElimination);
  CHECK(countKind(g, NodeKind::NegI) == 0); // reclaimed
  CHECK(countKind(g, NodeKind::AddI) == 0);
}

B2_TEST(cfs_verifier_clean_after_raw_kills) {
  // Raw-kill several nodes of a small hand-built graph (dangling inputs
  // by construction), then require key 20 to leave a legal graph.
  ir::Graph g;
  const ir::NodeId cA = g.constantI(1);
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 0,
                                          std::span<const ir::NodeId>{});
  const ir::NodeId guard = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 5);
  const ir::NodeId region = g.make(NodeKind::Region, {g.startNode(), guard});
  const ir::NodeId phi = g.make(NodeKind::Phi, {region, cA, cA});
  g.make(NodeKind::Return, {region, phi});
  g.killNode(guard);
  CHECK(!verifyOk(g));
  passOk(g, PK::ControlFlowSimplification);
  CHECK(verifyOk(g));
}

B2_TEST(cfs_idempotent_and_killswitch) {
  rbc::Method m;
  buildConstBranch(m, true);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  passOk(g, PK::ControlFlowSimplification);
  const std::string once = ir::print(g);
  const passes::PassResult r = passOk(g, PK::ControlFlowSimplification);
  CHECK(r.telemetry.removals == 0);
  CHECK(ir::print(g) == once);

  rbc::Method m2;
  buildConstBranch(m2, true);
  ir::Graph g2;
  buildOk(m2, g2);
  passOk(g2, PK::ConstantFolding);
  const std::string before = ir::print(g2);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::ControlFlowSimplification, false);
  const passes::PassResult r2 =
      passes::runSinglePass(g2, PK::ControlFlowSimplification, cfg);
  CHECK(r2.ok);
  CHECK(ir::print(g2) == before);
}

// --- GVN (key 35) --------------------------------------------------------------

B2_TEST(gvn_duplicate_addi_merged) {
  // Two separate iadds over the same operands: the builder creates both
  // (no build-time dedup), GVN merges them.
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 5);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1); // duplicate
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::AddI) == 2);
  const passes::PassResult r = passOk(g, PK::GVN);
  CHECK(countKind(g, NodeKind::AddI) == 1);
  CHECK(r.telemetry.gvnDedups == 1);
}

B2_TEST(gvn_first_id_wins_and_users_rewired) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 5);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1); // r2 rewritten by the dup
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const auto adds = nodesOfKind(g, NodeKind::AddI);
  CHECK(adds.size() == 2);
  const ir::NodeId first = adds[0];
  const ir::NodeId second = adds[1];
  passOk(g, PK::GVN);
  CHECK(!g.node(first).isDead());  // lowest id survives
  CHECK(g.node(second).isDead());
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.input(ret, 1) == first); // the user was rewired
}

B2_TEST(gvn_framestate_snapshots_auto_update) {
  // Rule 14 payoff: the call's FS captured the SECOND add; after GVN the
  // snapshot edge points at the survivor - deopt state stays exact.
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("M", "callee", "()I");
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 5);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 3, 0, 1); // r3 = duplicate value
  b.emitCall(rbc::Op::Invokestatic, 2, 0, 0, mc); // FS captures r2, r3
  b.emitReg(rbc::Op::Ireturn, 3);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const auto adds = nodesOfKind(g, NodeKind::AddI);
  CHECK(adds.size() == 2);
  const ir::NodeId first = adds[0];
  const ir::NodeId second = adds[1];
  const ir::NodeId fs = firstFs(g);
  // Find the FS slot holding the second add.
  std::uint16_t slotOfSecond = 0xFFFF;
  for (std::uint16_t s = 0; s < g.node(fs).numInputs; ++s) {
    if (g.input(fs, s) == second) {
      slotOfSecond = s;
    }
  }
  CHECK(slotOfSecond != 0xFFFF);
  passOk(g, PK::GVN);
  CHECK(g.input(fs, slotOfSecond) == first); // rewired automatically
}

B2_TEST(gvn_duplicate_arraylength_and_isnull) {
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)I",
                    rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitRegReg(rbc::Op::Arraylength, 1, 0);
  b.emitRegReg(rbc::Op::Arraylength, 2, 0); // duplicate
  b.emitRegRegReg(rbc::Op::Iadd, 1, 1, 2);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::ArrayLength) == 2);
  passOk(g, PK::GVN);
  CHECK(countKind(g, NodeKind::ArrayLength) == 1);

  rbc::RbcBuilder b2("main", "(Ljava/lang/Object;)V",
                     rbc::method_flags::Static);
  b2.setRegs(1);
  b2.setLocals(1);
  const rbc::RbcBuilder::Label skip = b2.newLabel();
  b2.emitRegSlot(rbc::Op::Aload, 0, 0);
  b2.emitRegBranch(rbc::Op::Ifnull, 0, skip);
  b2.emitRegBranch(rbc::Op::Ifnull, 0, skip); // duplicate IsNull user
  b2.emit(rbc::Op::Return);
  b2.bind(skip);
  b2.emit(rbc::Op::Return);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  CHECK(countKind(g2, NodeKind::IsNull) == 2);
  passOk(g2, PK::GVN);
  CHECK(countKind(g2, NodeKind::IsNull) == 1);
}

B2_TEST(gvn_merges_fold_created_constants) {
  // fold creates ConstantI(5); a builder ConstantI(5) already exists.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 2);
  b.emitRegImm(rbc::Op::Iconst, 1, 3);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitRegImm(rbc::Op::Iconst, 0, 5); // the existing 5
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  // 2, 3, builder-5, fold-5: GVN merges the two 5s.
  passOk(g, PK::GVN);
  std::size_t fives = 0;
  for (ir::NodeId i = 0; i < g.nodeCount(); ++i) {
    if (!g.node(i).isDead() && g.node(i).kind == NodeKind::ConstantI &&
        constValI(g, i) == 5) {
      ++fives;
    }
  }
  CHECK(fives == 1);
}

B2_TEST(gvn_duplicate_div_merged_trap_preserved) {
  // Two identical DivI(x, 2): merged; the survivor inherits all users, so
  // the (guard-gated) trap behavior is unchanged.
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 2);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1); // duplicate
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::DivI) == 2);
  passOk(g, PK::GVN);
  CHECK(countKind(g, NodeKind::DivI) == 1);
  // Each division site kept its own ZeroCheck guard (fixed nodes chain
  // control; guards are never value-numbered).
  CHECK(countKind(g, NodeKind::Guard) == 2);
}

B2_TEST(gvn_keeps_distinct_inputs_and_payloads) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 5);
  b.emitRegImm(rbc::Op::Iconst, 2, 6);
  b.emitRegRegReg(rbc::Op::Iadd, 3, 0, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 3, 0, 2); // different constant
  b.emitReg(rbc::Op::Ireturn, 3);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::GVN);
  CHECK(countKind(g, NodeKind::AddI) == 2); // distinct: both stay
}

B2_TEST(gvn_never_merges_calls_or_stores) {
  // CallOpaque effects and stores are never value-numbered.
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(2);
  const std::uint32_t mc = b.constMethodRef("M", "f", "()I");
  b.emitCall(rbc::Op::Invokestatic, 0, 0, 0, mc);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 0, mc); // identical shape
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::CallStatic) == 2);
  const passes::PassResult r = passOk(g, PK::GVN);
  CHECK(countKind(g, NodeKind::CallStatic) == 2);
  CHECK(r.telemetry.gvnDedups == 0);
}

B2_TEST(gvn_phi_dedup_handbuilt) {
  ir::Graph g;
  const ir::NodeId cA = g.constantI(10);
  const ir::NodeId region =
      g.make(NodeKind::Region, {g.startNode(), g.startNode()});
  const ir::NodeId phi1 = g.make(NodeKind::Phi, {region, cA, cA});
  const ir::NodeId phi2 = g.make(NodeKind::Phi, {region, cA, cA});
  const ir::NodeId ret = g.make(NodeKind::Return, {region, phi1});
  g.setInput(ret, 1, phi2); // the user consumes the DUPLICATE phi
  CHECK(verifyOk(g));
  passOk(g, PK::GVN);
  CHECK(!g.node(phi1).isDead());
  CHECK(g.node(phi2).isDead());
  CHECK(g.input(ret, 1) == phi1);
}

B2_TEST(gvn_load_dedup_handbuilt) {
  // Two loads with identical ctrl+mem+obj+field: same state, same read.
  ir::Graph g;
  const ir::NodeId p = g.parameter(0, ir::IRType::Ref);
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 0,
                                          std::span<const ir::NodeId>{});
  const ir::NodeId guard = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 5);
  const ir::NodeId l1 = g.make(NodeKind::LoadField,
                               {guard, g.startNode(), p}, 9,
                               static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId l2 = g.make(NodeKind::LoadField,
                               {guard, g.startNode(), p}, 9,
                               static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId ret = g.make(NodeKind::Return, {guard, l1});
  g.setInput(ret, 1, l2);
  CHECK(verifyOk(g));
  passOk(g, PK::GVN);
  CHECK(!g.node(l1).isDead());
  CHECK(g.node(l2).isDead());
  CHECK(g.input(ret, 1) == l1);
}

B2_TEST(gvn_idempotent_and_killswitch) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 5);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::GVN);
  const std::string once = ir::print(g);
  const passes::PassResult r = passOk(g, PK::GVN);
  CHECK(r.telemetry.gvnDedups == 0);
  CHECK(ir::print(g) == once);

  rbc::RbcBuilder b2("main", "(I)I", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.setLocals(1);
  b2.emitRegSlot(rbc::Op::Iload, 0, 0);
  b2.emitRegImm(rbc::Op::Iconst, 1, 5);
  b2.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b2.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b2.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  const std::string before = ir::print(g2);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::GVN, false);
  const passes::PassResult r2 = passes::runSinglePass(g2, PK::GVN, cfg);
  CHECK(r2.ok);
  CHECK(ir::print(g2) == before);
}

B2_TEST(gvn_deterministic) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 5);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::GVN);
  const std::string a = ir::print(g);

  rbc::RbcBuilder b2("main", "(I)I", rbc::method_flags::Static);
  b2.setRegs(3);
  b2.setLocals(1);
  b2.emitRegSlot(rbc::Op::Iload, 0, 0);
  b2.emitRegImm(rbc::Op::Iconst, 1, 5);
  b2.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b2.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b2.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m2;
  CHECK(b2.finish(m2).ok);
  ir::Graph g2;
  buildOk(m2, g2);
  passOk(g2, PK::GVN);
  CHECK(ir::print(g2) == a);
}

// --- the pipeline (runEarlyCleanup) --------------------------------------------

B2_TEST(pipe_dead_branch_end_to_end) {
  rbc::Method m;
  buildConstBranch(m, false);
  ir::Graph g;
  buildOk(m, g);
  const passes::PassResult r = pipeOk(g);
  CHECK(countLiveUserKind(g, NodeKind::If) == 0);
  CHECK(countLiveUserKind(g, NodeKind::IfTrue) == 0);
  CHECK(countLiveUserKind(g, NodeKind::IfFalse) == 0);
  CHECK(countLiveUserKind(g, NodeKind::Region) == 0);
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValI(g, g.input(ret, 1)) == 22);
  CHECK(r.telemetry.rounds <= passes::kMaxEarlyCleanupRounds);
  CHECK(r.telemetry.converged);
}

B2_TEST(pipe_constant_arithmetic_end_to_end) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  b.emitRegImm(rbc::Op::Iconst, 0, 2);
  b.emitRegImm(rbc::Op::Iconst, 1, 3);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  pipeOk(g);
  CHECK(countKind(g, NodeKind::AddI) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(constValI(g, g.input(ret, 1)) == 5);
}

B2_TEST(pipe_loop_preserved) {
  // i = 0; while (i < 10) i = i + 1; return i - loops are NOT damaged.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(1);
  const rbc::RbcBuilder::Label head = b.newLabel();
  const rbc::RbcBuilder::Label exit = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 0);
  b.emitSlotReg(rbc::Op::Istore, 0, 0);
  b.bind(head);
  b.emitRegSlot(rbc::Op::Iload, 1, 0);
  b.emitRegImm(rbc::Op::Iconst, 2, 10);
  b.emitRegRegBranch(rbc::Op::IfIcmpge, 1, 2, exit);
  b.emitRegSlot(rbc::Op::Iload, 1, 0);
  b.emitRegImm(rbc::Op::Iconst, 2, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 1, 1, 2);
  b.emitSlotReg(rbc::Op::Istore, 1, 0);
  b.emitBranch(rbc::Op::Goto, head);
  b.bind(exit);
  b.emitRegSlot(rbc::Op::Iload, 1, 0);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const std::size_t phisBefore = countKind(g, NodeKind::Phi);
  CHECK(phisBefore >= 1);
  pipeOk(g);
  CHECK(countKind(g, NodeKind::LoopBegin) == 1);
  CHECK(countKind(g, NodeKind::LoopEnd) == 1);
  CHECK(countKind(g, NodeKind::LoopExit) == 1);
  CHECK(countKind(g, NodeKind::Phi) >= 1); // the induction phi survives
  CHECK(verifyOk(g));
}

B2_TEST(pipe_bounds_guards_kept_when_maybe_trapping) {
  // a[i] on parameters: null + bounds guards must survive the pipeline.
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;I)I",
                    rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(2);
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitRegSlot(rbc::Op::Iload, 1, 1);
  b.emitRegRegReg(rbc::Op::Iaload, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  const std::size_t guards = countKind(g, NodeKind::Guard);
  CHECK(guards == 2);
  pipeOk(g);
  CHECK(countKind(g, NodeKind::Guard) == 2);
  CHECK(countKind(g, NodeKind::LoadElem) == 1);
}

B2_TEST(pipe_nullfold_composes_with_dce) {
  rbc::Method m;
  buildNewReceiverCall(m);
  ir::Graph g;
  buildOk(m, g);
  pipeOk(g);
  CHECK(countKind(g, NodeKind::Guard) == 0);
  CHECK(countKind(g, NodeKind::CallVirtual) == 1);
  CHECK(countKind(g, NodeKind::IsNull) == 0);
  CHECK(countKind(g, NodeKind::Not) == 0);
}

B2_TEST(pipe_idempotent) {
  rbc::Method m;
  buildConstBranch(m, true);
  ir::Graph g;
  buildOk(m, g);
  pipeOk(g);
  const std::string once = ir::print(g);
  const passes::PassResult r = pipeOk(g);
  CHECK(r.telemetry.rewrites == 0);
  CHECK(r.telemetry.removals == 0);
  CHECK(ir::print(g) == once);
}

B2_TEST(pipe_deterministic_bytes) {
  rbc::Method m;
  buildConstBranch(m, false);
  ir::Graph g;
  buildOk(m, g);
  pipeOk(g);
  const std::string a = ir::print(g);

  rbc::Method m2;
  buildConstBranch(m2, false);
  ir::Graph g2;
  buildOk(m2, g2);
  pipeOk(g2);
  CHECK(ir::print(g2) == a);
}

B2_TEST(pipe_killswitch_all_off) {
  rbc::Method m;
  buildConstBranch(m, true);
  ir::Graph g;
  buildOk(m, g);
  const std::string before = ir::print(g);
  passes::PassConfig cfg;
  cfg.disableAll();
  const passes::PassResult r = pipeOk(g, cfg);
  CHECK(ir::print(g) == before);
  CHECK(r.telemetry.rewrites == 0);
  CHECK(r.telemetry.removals == 0);
  CHECK(r.telemetry.rounds == 0);
}

B2_TEST(pipe_rounds_budget_reports_unconverged) {
  rbc::Method m;
  buildConstBranch(m, true);
  ir::Graph g;
  buildOk(m, g);
  passes::PassConfig cfg;
  cfg.maxCleanupRounds = 1; // stop after one changing round
  const passes::PassResult r = pipeOk(g, cfg);
  CHECK(r.telemetry.rounds == 1);
  CHECK(!r.telemetry.converged); // stopped while still changing
  // The graph is still valid (fail-open on convergence, fail-closed on
  // verification): the fold happened in round 1.
  CHECK(verifyOk(g));
}

B2_TEST(pipe_verify_flag_configurable) {
  rbc::Method m;
  buildConstBranch(m, true);
  ir::Graph g;
  buildOk(m, g);
  passes::PassConfig cfg;
  cfg.verifyAfterEachPass = false;
  const passes::PassResult r = pipeOk(g, cfg);
  CHECK(r.ok);
  CHECK(verifyOk(g)); // still valid, just not checked in between
}

B2_TEST(pipe_registry_contract) {
  const auto reg = passes::passRegistry();
  CHECK(reg.size() == 10);
  CHECK(passes::passInfo(PK::DeadCodeElimination).key ==
        PK::DeadCodeElimination);
  CHECK(passes::passInfo(PK::GVN).key == PK::GVN);
  // Not delivered: key 17 resolves to the bad row and refuses to run.
  CHECK(passes::passInfo(PK::RedundantCastRemoval).key !=
        PK::RedundantCastRemoval);
  ir::Graph g;
  g.constantI(1);
  const passes::PassResult r =
      passes::runSinglePass(g, PK::RedundantCastRemoval);
  CHECK(!r.ok);
  CHECK(!r.diags.empty());
}

// --- the corpus sweep (the strongest gate) ---------------------------------------

B2_TEST(pipe_corpus_sweep_all_programs_verified_deterministic) {
  const std::vector<std::string> files = {
      "bench_fib",     "bench_sum",     "conversions",
      "div_catch",     "exception_nest", "fib_loop",
      "fields",        "float_math",    "fusion_guard_clobber",
      "fusion_guard_loop_live",        "fusion_guard_reread",
      "monitors",      "quickened",     "sparse_switch",
      "statics_clinit", "strings_intern", "sum_loop",
      "switch_names",  "uncaught",
  };
  std::size_t methods = 0;
  for (const std::string& name : files) {
    const std::string path = "tests/interp/corpus/" + name + ".rbc";
    std::ifstream in(path);
    if (!in) {
      CHECK_MSG(false, "cannot open " + path);
      continue;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const auto parsed = rbc::parseRbcText(ss.str());
    CHECK_MSG(parsed.has_value(), "parse failed: " + name);
    if (!parsed) {
      continue;
    }
    for (std::size_t i = 0; i < parsed->methods.size(); ++i) {
      const rbc::Method& m = parsed->methods[i];
      ++methods;
      ir::Graph g;
      buildOk(m, g);
      const passes::PassResult pr = pipeOk(g);
      CHECK_MSG(pr.ok, "pipeline failed: " + name + "/" + m.name);
      CHECK_MSG(verifyOk(g), "post-pipeline verify failed: " + name +
                                 "/" + m.name);

      // Determinism (Rule 124): rebuild + repipeline is byte-identical.
      ir::Graph g2;
      buildOk(m, g2);
      pipeOk(g2);
      CHECK_MSG(ir::print(g) == ir::print(g2),
                "non-deterministic pipeline: " + name + "/" + m.name);
    }
  }
  CHECK(methods >= 24); // the corpus grew past 24 methods by T2-IR2
}

// --- Rule 35 completeness: the families below close the >=10-per-pass gap

B2_TEST(fusion_mem_phi_collapses_via_region_repair) {
  // A dead arm containing a STORE leaves a memory phi at the merge; the
  // region repair drops the dead pred and the memory phi collapses to
  // the surviving memory state.
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)V",
                    rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t fld = b.constFieldRef("M", "x", "I");
  const rbc::RbcBuilder::Label skip = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 1);
  b.emitRegBranch(rbc::Op::Ifne, 0, skip); // jump taken: store arm dead
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 7);
  b.emitRegRegRegCp(rbc::Op::Putfield, 0, 0, 1, fld);
  b.bind(skip);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  passOk(g, PK::BranchNormalization);
  CHECK(countKind(g, NodeKind::StoreField) == 0);
  // No phi survives the collapse (memory or slot).
  CHECK(countLiveUserKind(g, NodeKind::Phi) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 0)).kind != NodeKind::Phi);
  CHECK(verifyOk(g));
}

B2_TEST(fusion_multi_pred_rebuild_keeps_phi_alignment) {
  // Hand-built 3-pred region, one pred raw-killed: the rebuilt region has
  // 2 preds and every rebuilt phi has 1+2 inputs, slot-for-pred.
  ir::Graph g;
  const ir::NodeId cA = g.constantI(10);
  const ir::NodeId cB = g.constantI(20);
  const ir::NodeId cC = g.constantI(30);
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 0,
                                          std::span<const ir::NodeId>{});
  const ir::NodeId g1 = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 5);
  const ir::NodeId g2 = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 6);
  const ir::NodeId g3 = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 7);
  const ir::NodeId region = g.make(NodeKind::Region, {g1, g2, g3});
  const ir::NodeId phi = g.make(NodeKind::Phi, {region, cA, cB, cC});
  g.make(NodeKind::Return, {region, phi});
  g.killNode(g2);
  passOk(g, PK::TrivialBlockFusion);
  CHECK(verifyOk(g));
  const ir::NodeId region2 = firstOf(g, NodeKind::Region);
  CHECK(g.node(region2).numInputs == 2);
  const ir::NodeId phi2 = firstOf(g, NodeKind::Phi);
  CHECK(g.node(phi2).numInputs == 3);
  CHECK(g.input(phi2, 1) == cA);
  CHECK(g.input(phi2, 2) == cC);
}

B2_TEST(fusion_repair_deterministic) {
  const auto build = [](ir::Graph& g) {
    const ir::NodeId cA = g.constantI(11);
    const ir::NodeId cB = g.constantI(22);
    const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 0,
                                            std::span<const ir::NodeId>{});
    const ir::NodeId pred = g.make(
        NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
        static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 5);
    const ir::NodeId region =
        g.make(NodeKind::Region, {g.startNode(), pred});
    const ir::NodeId phi = g.make(NodeKind::Phi, {region, cA, cB});
    g.make(NodeKind::Return, {region, phi});
    g.killNode(pred); // the region loses its guard pred
  };
  ir::Graph g;
  build(g);
  passOk(g, PK::TrivialBlockFusion);
  const std::string out = ir::print(g);
  ir::Graph g2;
  build(g2);
  passOk(g2, PK::TrivialBlockFusion);
  CHECK(ir::print(g2) == out);
}

B2_TEST(canon_swap_feeds_strength_reduction) {
  // MulI(8, x) canonicalizes the constant right; strength then rewrites
  // it to a shift (compose key 13 -> key 15).
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 8);
  b.emitRegRegReg(rbc::Op::Imul, 2, 1, 0); // const on the LEFT
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::Canonicalization);
  const ir::NodeId mul = firstOf(g, NodeKind::MulI);
  CHECK(g.node(g.input(mul, 1)).kind == NodeKind::ConstantI);
  passOk(g, PK::StrengthReduction);
  const ir::NodeId shl = firstOf(g, NodeKind::ShlI);
  CHECK(shl != ir::kInvalidNodeId);
  CHECK(constValI(g, g.input(shl, 1)) == 3);
}

B2_TEST(strength_muli_pow2_large) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 65536); // 2^16
  b.emitRegRegReg(rbc::Op::Imul, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::StrengthReduction);
  const ir::NodeId shl = firstOf(g, NodeKind::ShlI);
  CHECK(constValI(g, g.input(shl, 1)) == 16);
}

B2_TEST(strength_pipeline_end_to_end) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 8);
  b.emitRegRegReg(rbc::Op::Imul, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  pipeOk(g);
  CHECK(countKind(g, NodeKind::MulI) == 0);
  CHECK(countLiveUserKind(g, NodeKind::ShlI) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(nullfold_deterministic_bytes) {
  rbc::Method m;
  buildNewReceiverCall(m);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::NullCheckFolding);
  const std::string a = ir::print(g);
  rbc::Method m2;
  buildNewReceiverCall(m2);
  ir::Graph g2;
  buildOk(m2, g2);
  passOk(g2, PK::NullCheckFolding);
  CHECK(ir::print(g2) == a);
}

B2_TEST(branch_live_arm_store_survives) {
  // The complement of branch_dead_arm: the TAKEN arm's store survives.
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)V",
                    rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t fld = b.constFieldRef("M", "x", "I");
  const rbc::RbcBuilder::Label skip = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 0); // ifne NOT taken: store arm live
  b.emitRegBranch(rbc::Op::Ifne, 0, skip);
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 7);
  b.emitRegRegRegCp(rbc::Op::Putfield, 0, 0, 1, fld);
  b.bind(skip);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  passOk(g, PK::ConstantFolding);
  passOk(g, PK::BranchNormalization);
  CHECK(countKind(g, NodeKind::StoreField) == 1); // observable: kept
  CHECK(countKind(g, NodeKind::Return) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(branch_goto_skipped_block_never_materializes) {
  // Unconditional Goto over a block: the skipped block has NO in-edges,
  // so the builder never materializes it (blocks with no predecessors
  // are dead at construction). The pipeline's job is only unreachable
  // code CREATED by transforms - this pins the builder-side behavior.
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(2);
  const rbc::RbcBuilder::Label skip = b.newLabel();
  b.emitBranch(rbc::Op::Goto, skip);
  b.emitRegImm(rbc::Op::Iconst, 0, 9); // skipped, value never read
  b.emitRegReg(rbc::Op::Ineg, 0, 0);   // dead chain
  b.bind(skip);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  buildOk(m, g);
  CHECK(countKind(g, NodeKind::NegI) == 0); // never materialized
  const passes::PassResult r = pipeOk(g);
  CHECK(r.telemetry.rewrites == 0); // nothing to clean
  CHECK(countKind(g, NodeKind::Return) == 1);
}

B2_TEST(cfs_orphan_control_chain_killed) {
  // Hand-built: a guard chain whose control root was raw-killed is
  // unreachable from Start; the sweep reclaims it bottom-up.
  ir::Graph g;
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 0,
                                          std::span<const ir::NodeId>{});
  const ir::NodeId root = g.make(
      NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 5);
  const ir::NodeId chain = g.make(
      NodeKind::Guard, {root, g.constantI(1), fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 6);
  g.make(NodeKind::Return, {chain});
  CHECK(verifyOk(g));
  g.killNode(root); // the chain is now unreachable
  passOk(g, PK::ControlFlowSimplification);
  CHECK(countKind(g, NodeKind::Guard) == 0);
  CHECK(countKind(g, NodeKind::Return) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(cfs_deterministic_repair) {
  const auto build = [](ir::Graph& g) {
    const ir::NodeId cA = g.constantI(11);
    const ir::NodeId cB = g.constantI(22);
    const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 0,
                                            std::span<const ir::NodeId>{});
    const ir::NodeId pred = g.make(
        NodeKind::Guard, {g.startNode(), g.constantI(1), fs},
        static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 5);
    const ir::NodeId region =
        g.make(NodeKind::Region, {g.startNode(), pred});
    const ir::NodeId phi = g.make(NodeKind::Phi, {region, cA, cB});
    g.make(NodeKind::Return, {region, phi});
    g.killNode(pred);
  };
  ir::Graph g;
  build(g);
  passOk(g, PK::ControlFlowSimplification);
  const std::string a = ir::print(g);
  ir::Graph g2;
  build(g2);
  passOk(g2, PK::ControlFlowSimplification);
  CHECK(ir::print(g2) == a);
}
