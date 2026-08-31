// B-2 passes tests - the RBC->IR graph builder (T2-IR2).
//
// Discipline: every built graph passes the TOTAL IR verifier (Rules 40,
// 126) - these tests additionally pin the lowering CONTRACT: slot SSA
// aliasing, FrameState placement and arity, guard gating, loop phi shapes,
// the v1 exception-deopt policy, quickened ops, determinism, and the
// refusal set. The corpus sweep (all 19 interp-corpus programs, every
// method) is the strongest gate: build + verify with zero diagnostics.

#include "TestHarness.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <b2/ir/Node.h>
#include <b2/ir/Printer.h>
#include <b2/ir/Serialize.h>
#include <b2/ir/Verifier.h>
#include <b2/passes/GraphBuilder.h>
#include <b2/rbc/RbcBuilder.h>
#include <b2/rbc/RbcText.h>
#include <b2/rbc/Verifier.h>

using namespace b2;
using ir::NodeKind;

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

[[nodiscard]] bool onlyDiagAbout(const passes::BuildResult& r,
                                 std::string_view needle) {
  if (r.ok || r.diags.empty()) {
    return false;
  }
  return r.diags[0].message.find(needle) != std::string::npos;
}

} // namespace

// --- construction hygiene ------------------------------------------------------

B2_TEST(gb_rejects_used_graph_and_edges) {
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(1);
  b.setLocals(0);
  const rbc::RbcBuilder::Label done = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 0);
  b.emitRegBranch(rbc::Op::Ifne, 0, done);
  b.bind(done);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);

  ir::Graph g;
  g.constantI(1); // stale: node count > 1
  passes::CounterResolver res;
  const passes::BuildResult r = passes::buildGraph(m, res, g, 1);
  CHECK(onlyDiagAbout(r, "not fresh"));

  rbc::RbcBuilder sync("main", "()V",
                       rbc::method_flags::Static |
                           rbc::method_flags::Synchronized);
  sync.setRegs(1);
  sync.emit(rbc::Op::Return);
  rbc::Method sm;
  CHECK(sync.finish(sm).ok);
  ir::Graph g2;
  passes::CounterResolver res2;
  const passes::BuildResult r2 = passes::buildGraph(sm, res2, g2, 1);
  CHECK(onlyDiagAbout(r2, "synchronized"));
}

B2_TEST(gb_rejects_out_of_range_registers) {
  // RbcBuilder itself refuses out-of-range operands, so hand-assemble the
  // Ins records (the builder must be total on hostile input regardless).
  rbc::Method m;
  m.name = "main";
  m.descriptor = "()V";
  m.flags = rbc::method_flags::Static;
  m.numRegs = 1;
  m.numLocals = 0;
  m.code.push_back(rbc::Ins(rbc::Op::Iconst, 5, 0, 0, 3)); // r5: out of range
  m.code.push_back(rbc::Ins(rbc::Op::Return, 0, 0, 0, 0));
  ir::Graph g;
  passes::CounterResolver res;
  const passes::BuildResult r = passes::buildGraph(m, res, g, 1);
  CHECK(!r.ok);
  CHECK(r.diags[0].message.find("out of range") != std::string::npos);
}

// --- SSA discipline ---------------------------------------------------------------

B2_TEST(gb_loads_stores_moves_are_pure_aliasing) {
  // iconst/istore/iload/imove contribute NO nodes: slots are SSA values.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(2);
  b.setLocals(2);
  b.emitRegImm(rbc::Op::Iconst, 0, 41);
  b.emitSlotReg(rbc::Op::Istore, 0, 0);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegReg(rbc::Op::Imove, 1, 0);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  CHECK(rbc::verify(m).ok);
  ir::Graph g; buildOk(m, g);
  // Start + ONE shared Undef + ConstantI + Return: aliasing added nothing.
  CHECK(g.nodeCount() == 4);
  CHECK(countKind(g, NodeKind::Parameter) == 0);
  CHECK(countKind(g, NodeKind::Undef) == 1);
}

B2_TEST(gb_merge_phi_only_when_defs_differ) {
  // if (c) l0=1 else l0=2; return l0 -> one Phi over two constants.
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const rbc::RbcBuilder::Label thenL = b.newLabel();
  const rbc::RbcBuilder::Label elseL = b.newLabel();
  const rbc::RbcBuilder::Label endL = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 0);
  b.emitRegBranch(rbc::Op::Ifne, 0, thenL);
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
  ir::Graph g; buildOk(m, g);

  // Two phis (l0's and r0's) over the same ConstantI pair; the iload
  // after the merge aliases l0's phi.
  const std::vector<ir::NodeId> phis = nodesOfKind(g, NodeKind::Phi);
  CHECK(phis.size() == 2);
  const ir::NodeId phi = phis[0];
  CHECK(g.node(phi).numInputs == 3);
  CHECK(g.node(g.input(phi, 0)).kind == NodeKind::Region);
  CHECK(g.node(g.input(phi, 1)).kind == NodeKind::ConstantI);
  CHECK(g.node(g.input(phi, 2)).kind == NodeKind::ConstantI);
  CHECK(ir::resultTypeOf(g, phi) == ir::IRType::Int);
  // The return consumes a phi.
  const ir::NodeId ret = nodesOfKind(g, NodeKind::Return).at(0);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::Phi);
}

B2_TEST(gb_loop_phi_backedge_and_trivial_mem_phi) {
  // i = 0; while (i < 10) { i = i + 1; } return i
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
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
  ir::Graph g; buildOk(m, g);

  CHECK(countKind(g, NodeKind::LoopBegin) == 1);
  CHECK(countKind(g, NodeKind::LoopEnd) == 1);
  CHECK(countKind(g, NodeKind::LoopExit) == 1);
  const ir::NodeId lb = nodesOfKind(g, NodeKind::LoopBegin).at(0);
  CHECK(g.node(lb).numInputs == 2); // entry + backedge
  // l0's loop phi: entry constant + AddI backedge, arity = region preds+1.
  const ir::NodeId phi = nodesOfKind(g, NodeKind::Phi).at(0);
  CHECK(g.node(phi).numInputs == 3);
  CHECK(g.node(g.input(phi, 0)).kind == NodeKind::LoopBegin);
  CHECK(g.node(g.input(phi, 1)).kind == NodeKind::ConstantI);
  CHECK(g.node(g.input(phi, 2)).kind == NodeKind::AddI);
  // The memory phi collapsed to Start (no memory ops in the loop).
  CHECK(g.node(g.input(nodesOfKind(g, NodeKind::Return).at(0), 0)).kind !=
        NodeKind::Phi);
}

// --- FrameState policy (Rules 5, 126) ----------------------------------------------

B2_TEST(gb_framestate_at_calls_guards_loopexits) {
  rbc::RbcBuilder c("main", "(I)I", rbc::method_flags::Static);
  c.setRegs(4);
  c.setLocals(2);
  const std::uint32_t mc = c.constMethodRef("Main", "callee", "(I)I");
  c.emitRegSlot(rbc::Op::Iload, 1, 0);
  c.emitCall(rbc::Op::Invokestatic, 2, 1, 1, mc);
  c.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m2;
  CHECK(c.finish(m2).ok);

  ir::Graph g; buildOk(m2, g);
  // FS arity = numLocals(2) + numRegs(4) = 6; pc = the call's pc (1).
  const std::vector<ir::NodeId> fss = nodesOfKind(g, NodeKind::FrameState);
  CHECK(fss.size() == 1);
  const ir::NodeId fs = fss[0];
  CHECK(g.node(fs).numInputs == 6);
  CHECK(g.frameState(g.node(fs).payload).pc == 1);
  CHECK(g.frameState(g.node(fs).payload).method == 7);
  CHECK(g.frameState(g.node(fs).payload).caller == ir::kInvalidFrameState);
  // Slot order: locals first. l0 = Parameter(I); r1's def feeds slot 3.
  CHECK(g.node(g.input(fs, 0)).kind == NodeKind::Parameter);
  // The call consumes the FS as its trailing input.
  const ir::NodeId call = nodesOfKind(g, NodeKind::CallStatic).at(0);
  CHECK(g.input(call, g.node(call).numInputs - 1) == fs);
  // Uncaught call: CallExcept -> Unwind (exception policy v1).
  CHECK(countKind(g, NodeKind::CallExcept) == 1);
  CHECK(countKind(g, NodeKind::Unwind) == 1);
  const ir::NodeId uw = nodesOfKind(g, NodeKind::Unwind).at(0);
  CHECK(g.node(g.input(uw, 0)).kind == NodeKind::CallExcept);
  CHECK(g.node(g.input(uw, 1)).kind == NodeKind::CallExcept);
}

B2_TEST(gb_guards_gate_control_and_share_framestate) {
  // int[] a = new int[3]; a[i] -> null guard + bounds guard + LoadElem,
  // all chaining control; guards and the load share one FS per pc... the
  // null+bounds guards share the pc's FrameState.
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(2);
  const std::uint32_t arr = b.newReg(); // r0
  (void)arr;
  const std::uint32_t len = b.newReg(); // r1
  b.emitRegImm(rbc::Op::Iconst, len, 3);
  b.emitRegRegImm(rbc::Op::NewArray, 0, len, static_cast<std::uint32_t>(rbc::Atype::Int));
  b.emitRegSlot(rbc::Op::Iload, 1, 0);
  b.emitRegRegReg(rbc::Op::Iaload, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g; buildOk(m, g);

  CHECK(countKind(g, NodeKind::NewArray) == 1);
  CHECK(countKind(g, NodeKind::ArrayLength) == 1);
  CHECK(countKind(g, NodeKind::Guard) == 2);
  const auto guards = nodesOfKind(g, NodeKind::Guard);
  CHECK(g.node(guards[0]).payload ==
        static_cast<std::uint32_t>(ir::GuardKind::NullCheck));
  CHECK(g.node(guards[1]).payload ==
        static_cast<std::uint32_t>(ir::GuardKind::BoundsCheck));
  // Control chains: LoadElem's ctrl is the bounds guard.
  const ir::NodeId load = nodesOfKind(g, NodeKind::LoadElem).at(0);
  CHECK(g.node(g.input(load, 0)).kind == NodeKind::Guard);
  // Guards share the FS node of their instruction.
  CHECK(g.input(guards[0], 2) == g.input(guards[1], 2));
}

B2_TEST(gb_div_zero_guard_and_deopt_ids) {
  rbc::RbcBuilder b("main", "(II)I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(2);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegSlot(rbc::Op::Iload, 1, 1);
  b.emitRegRegReg(rbc::Op::Idiv, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g; buildOk(m, g);
  CHECK(countKind(g, NodeKind::Guard) == 1);
  const ir::NodeId guard = nodesOfKind(g, NodeKind::Guard).at(0);
  CHECK(g.node(guard).payload ==
        static_cast<std::uint32_t>(ir::GuardKind::ZeroCheck));
  // Builder deopt ids start at kBuilderDeoptIdBase+1 (no stream collisions).
  CHECK(g.node(guard).payload2 == 0x80000001u);
  CHECK(g.node(g.input(guard, 1)).kind == NodeKind::NeI);
}

// --- exception policy v1 -----------------------------------------------------------

B2_TEST(gb_covered_call_deopts_and_handler_is_unreachable) {
  // try { call(); } catch (E e) { handler-body } - the call is covered:
  // CallExcept -> Deopt (exception deopt, class 2.3); the handler body is
  // not reachable in the compiled graph (deopt dispatches via T0).
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("Main", "callee", "()V");
  const std::uint32_t ec = b.constClass("java/lang/Exception");
  const std::uint32_t tryStart = b.here();
  b.emitCall(rbc::Op::Invokestatic, 0, 0, 0, mc);
  const std::uint32_t tryEnd = b.here();
  b.emit(rbc::Op::Return);
  const std::uint32_t h = b.here();
  b.emitRegImm(rbc::Op::Iconst, 0, 1);
  b.emit(rbc::Op::Return);
  b.addHandler(tryStart, tryEnd, h, static_cast<std::int32_t>(ec));
  rbc::Method m;
  CHECK(b.finish(m).ok);
  CHECK(rbc::verify(m).ok);

  ir::Graph g; buildOk(m, g);
  CHECK(countKind(g, NodeKind::CallStatic) == 1);
  CHECK(countKind(g, NodeKind::CallExcept) == 1);
  CHECK(countKind(g, NodeKind::Deopt) == 1);
  const ir::NodeId deopt = nodesOfKind(g, NodeKind::Deopt).at(0);
  // The Deopt's control is the CallExcept (the exception-deopt class
  // signal; the exception value is the CallExcept's value).
  CHECK(g.node(g.input(deopt, 0)).kind == NodeKind::CallExcept);
  // The handler body did not materialize: exactly ONE Return.
  CHECK(countKind(g, NodeKind::Return) == 1);
}

B2_TEST(gb_covered_athrow_deopts_uncaught_unwinds) {
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)V",
                    rbc::method_flags::Static);
  b.setRegs(2);
  b.setLocals(1);
  const std::uint32_t ec = b.constClass("java/lang/Exception");
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  const std::uint32_t tryStart = b.here();
  b.emitReg(rbc::Op::Athrow, 0);
  const std::uint32_t tryEnd = b.here();
  b.emit(rbc::Op::Return);
  const std::uint32_t h = b.here();
  b.emit(rbc::Op::Return);
  b.addHandler(tryStart, tryEnd, h, static_cast<std::int32_t>(ec));
  rbc::Method m;
  CHECK(b.finish(m).ok);
  CHECK(rbc::verify(m).ok);
  ir::Graph g; buildOk(m, g);
  // Covered athrow -> Deopt; the NPE guard is NOT emitted (T0's athrow
  // handles null itself on the re-executed path).
  CHECK(countKind(g, NodeKind::Deopt) == 1);
  CHECK(countKind(g, NodeKind::Guard) == 0);
  CHECK(countKind(g, NodeKind::Unwind) == 0);

  // Uncovered athrow: null guard + Unwind with the thrown value.
  rbc::RbcBuilder u("main", "(Ljava/lang/Object;)V",
                    rbc::method_flags::Static);
  u.setRegs(2);
  u.setLocals(1);
  u.emitRegSlot(rbc::Op::Aload, 0, 0);
  u.emitReg(rbc::Op::Athrow, 0);
  rbc::Method m2;
  CHECK(u.finish(m2).ok);
  CHECK(rbc::verify(m2).ok);
  ir::Graph g2; buildOk(m2, g2);
  CHECK(countKind(g2, NodeKind::Guard) == 1);
  CHECK(countKind(g2, NodeKind::Unwind) == 1);
  const ir::NodeId uw = nodesOfKind(g2, NodeKind::Unwind).at(0);
  CHECK(g2.node(g2.input(uw, 1)).kind == NodeKind::Parameter);
}

// --- switches ------------------------------------------------------------------------

B2_TEST(gb_switch_projections_match_table) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(2);
  b.setLocals(1);
  const rbc::RbcBuilder::Label a = b.newLabel();
  const rbc::RbcBuilder::Label c = b.newLabel();
  const rbc::RbcBuilder::Label dflt = b.newLabel();
  b.emitRegSlot(rbc::Op::Iload, 1, 0);
  b.emitSwitch(rbc::Op::Lookupswitch, 1, {10, 20, 30}, {a, c, dflt});
  b.bind(a);
  b.emitRegImm(rbc::Op::Iconst, 1, 1);
  b.emitReg(rbc::Op::Ireturn, 1);
  b.bind(c);
  b.emitRegImm(rbc::Op::Iconst, 1, 2);
  b.emitReg(rbc::Op::Ireturn, 1);
  b.bind(dflt);
  b.emitRegImm(rbc::Op::Iconst, 1, 3);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g; buildOk(m, g);
  CHECK(countKind(g, NodeKind::Switch) == 1);
  CHECK(countKind(g, NodeKind::SwitchCase) == 3);
  CHECK(countKind(g, NodeKind::SwitchDefault) == 1);
  CHECK(countKind(g, NodeKind::Return) == 3);
  // Case ordinals are 0..2 in table order.
  const auto cases = nodesOfKind(g, NodeKind::SwitchCase);
  for (std::size_t i = 0; i < cases.size(); ++i) {
    CHECK(g.node(cases[i]).payload == i);
  }
}

// --- class-init triggers (T0 pins) -----------------------------------------------------

B2_TEST(gb_classinit_on_static_access_static_call_new_ldc_class) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(2);
  b.setLocals(1);
  const std::uint32_t fc = b.constFieldRef("Main", "x", "I");
  const std::uint32_t mc = b.constMethodRef("Main", "f", "()I");
  const std::uint32_t cls = b.constClass("Main");
  b.emitRegCp(rbc::Op::Getstatic, 0, fc);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 0, mc);
  b.emitRegCp(rbc::Op::New, 0, cls);
  b.emitRegCp(rbc::Op::Ldc, 0, cls); // dst re-typed Ref; no FieldRef probe
  b.emitRegImm(rbc::Op::Iconst, 1, 0);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g; buildOk(m, g);
  // T0's trigger set: getstatic, invokestatic, new, ldc-of-Class.
  CHECK(countKind(g, NodeKind::ClassInit) == 4);
}

// --- quickened ops -----------------------------------------------------------------------

B2_TEST(gb_quickened_field_and_call_ops) {
  // getfield_quick/putfield_quick carry resolved offsets; the dst type is
  // the pre-established register type; invokestatic_quick keeps the
  // class-init trigger and uses the quickened MethodId verbatim.
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(1);
  const std::uint32_t c0 = b.constClass("Main");
  const std::uint32_t c1 = b.constInt(0); // offset 0 via pool index 1
  (void)c1;
  b.emitRegCp(rbc::Op::New, 0, c0);
  b.emitRegImm(rbc::Op::Iconst, 1, 20);
  b.emitRegRegCp(rbc::Op::PutfieldQuick, 0, 0, c1);
  b.emitRegImm(rbc::Op::Iconst, 2, 0); // pre-type r2 Int for the quick load
  b.emitRegRegCp(rbc::Op::GetfieldQuick, 2, 0, c1);
  b.emitRegImm(rbc::Op::Iconst, 3, 0); // pre-type r3 Int for the quick call
  b.emitCall(rbc::Op::InvokestaticQuick, 3, 0, 0, 0);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  CHECK(rbc::verify(m).ok);
  ir::Graph g; buildOk(m, g);
  CHECK(countKind(g, NodeKind::StoreField) == 1);
  CHECK(countKind(g, NodeKind::LoadField) == 1);
  const ir::NodeId load = nodesOfKind(g, NodeKind::LoadField).at(0);
  // FieldId payload = the quickened offset, verbatim.
  CHECK(g.node(load).payload == 1);
  CHECK(ir::resultTypeOf(g, load) == ir::IRType::Int);
  const ir::NodeId call = nodesOfKind(g, NodeKind::CallStatic).at(0);
  CHECK(g.node(call).payload == 0); // MethodId from imm
  // The quickened static call still triggers class init (T0 pin).
  CHECK(countKind(g, NodeKind::ClassInit) == 2); // new + quick call
}

// --- determinism (Rule 124) ------------------------------------------------------------

B2_TEST(gb_deterministic_build_and_serialization) {
  const std::string text = R"RBC(.class Main

.method static main (I)I
.regs 4
.locals 2
.const c0 = field Main obj LMain;
.const c1 = method Main f (I)I
.const c2 = class "Main"
getstatic r0 c0
iload r1 l0
invokevirtual r2 r0 r1 c1
iadd r3 r2 r1
ireturn r3
.end)RBC";
  const auto parsed = rbc::parseRbcText(text);
  CHECK(parsed.has_value());
  CHECK(rbc::verify(parsed->methods[0]).ok);

  ir::Graph g1; buildOk(parsed->methods[0], g1);
  const std::string p1 = ir::print(g1);
  const std::vector<std::uint8_t> b1 = ir::serialize(g1);
  ir::Graph g2; buildOk(parsed->methods[0], g2);
  const std::string p2 = ir::print(g2);
  const std::vector<std::uint8_t> b2 = ir::serialize(g2);
  CHECK(p1 == p2);
  CHECK(b1 == b2);
  CHECK(!p1.empty());
}

// --- refusals ---------------------------------------------------------------------------

B2_TEST(gb_refuses_v1_scope_gaps) {
  struct Case {
    const char* name;
    std::string text;
    const char* needle;
  };
  const Case cases[] = {
      {"invokedynamic",
       ".class Main\n.method static main ()V\n.regs 1\n.locals 0\n"
       ".const c0 = indy f ()Ljava/lang/Object;\n"
       "invokedynamic r0 r0 0 c0\nreturn\n.end",
       "invokedynamic"},
      {"guard_class",
       ".class Main\n.method static main ()V\n.regs 1\n.locals 0\n"
       ".const c0 = class \"Main\"\n"
       "guard_class r0 1 c0\nreturn\n.end",
       "guard_class"},
  };
  for (const Case& c : cases) {
    const auto parsed = rbc::parseRbcText(c.text);
    if (!parsed.has_value()) {
      // Parse-level refusal is also fine (the op is verifier-only).
      continue;
    }
    ir::Graph g;
    passes::CounterResolver res;
    const passes::BuildResult r = passes::buildGraph(
        parsed->methods[0], res, g, 1);
    CHECK_MSG(!r.ok, c.name);
    if (!r.ok) {
      CHECK_MSG(r.diags[0].message.find(c.needle) != std::string::npos,
                c.name);
    }
  }
}

B2_TEST(gb_refuses_ldc_method_type) {
  const std::string text =
      ".class Main\n"
      ".method static main ()V\n.regs 1\n.locals 0\n"
      ".const c0 = methodtype ()V\n"
      "ldc r0 c0\nreturn\n.end\n";
  const auto parsed = rbc::parseRbcText(text);
  if (!parsed.has_value()) {
    return; // parse-level refusal is acceptable (v0 has no MH runtime)
  }
  ir::Graph g;
  passes::CounterResolver res;
  const passes::BuildResult r =
      passes::buildGraph(parsed->methods[0], res, g, 1);
  CHECK(onlyDiagAbout(r, "methodtype"));
}

B2_TEST(gb_refuses_irreducible_control_flow) {
  // A true two-entry cycle: entry branches into A or B directly, and
  // A -> B -> A closes a cycle where neither dominates the other, so the
  // cycle lives on FORWARD edges and no topological order exists.
  //   0 iconst; 1 ifne r0 -> B; 2 goto A; 3 A: goto B; 4 B: ifne r0 -> A;
  //   5 return
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(1);
  b.setLocals(0);
  const rbc::RbcBuilder::Label a = b.newLabel();
  const rbc::RbcBuilder::Label bb = b.newLabel();
  b.emitRegImm(rbc::Op::Iconst, 0, 0);
  b.emitRegBranch(rbc::Op::Ifne, 0, bb);
  b.emitBranch(rbc::Op::Goto, a);
  b.bind(a);
  b.emitBranch(rbc::Op::Goto, bb);
  b.bind(bb);
  b.emitRegBranch(rbc::Op::Ifne, 0, a);
  b.emit(rbc::Op::Return);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g;
  passes::CounterResolver res;
  const passes::BuildResult r = passes::buildGraph(m, res, g, 1);
  CHECK(onlyDiagAbout(r, "irreducible"));
}

// --- printer golden (format pin, Rule 31/124) --------------------------------------------

B2_TEST(gb_printer_golden_minimal_method) {
  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(2);
  b.setLocals(1);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegImm(rbc::Op::Iconst, 1, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 0, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  ir::Graph g; buildOk(m, g);
  const std::string p = ir::print(g);
  const std::string exact = p;
  const std::string want =
      "; B-2 IR v2 nodes=6 live=6 epoch=0\n"
      "n0   Start                 \n"
      "n1   Parameter             #0 : int\n"
      "n2   Undef                 \n"
      "n3   ConstantI             1\n"
      "n4   AddI                   n1 n3\n"
      "n5   Return                 n0 n4\n";
  CHECK(exact == want);
}

// --- the corpus sweep (the strongest gate) ------------------------------------------------

B2_TEST(gb_corpus_sweep_all_programs_all_methods) {
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
    const std::string path =
        "tests/interp/corpus/" + name + ".rbc";
    std::ifstream in(path);
    if (!in) {
      CHECK_MSG(false, "cannot open " + path);
      continue;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const auto parsed = rbc::parseRbcText(ss.str());
    CHECK_MSG(parsed.has_value(), name + ": parse");
    if (!parsed.has_value()) {
      continue;
    }
    for (std::size_t i = 0; i < parsed->methods.size(); ++i) {
      const rbc::Method& m = parsed->methods[i];
      const rbc::VerifyResult vr = rbc::verify(m);
      CHECK_MSG(vr.ok, name + ":" + m.name + ": RBC verify");
      if (!vr.ok) {
        continue;
      }
      ir::Graph g;
      passes::CounterResolver res;
      const passes::BuildResult br = passes::buildGraph(
          m, res, g, static_cast<ir::MethodId>(i));
      CHECK_MSG(br.ok, name + ":" + m.name + ": build" +
                           (br.diags.empty()
                                ? std::string()
                                : " pc " + std::to_string(br.diags[0].pc) +
                                      ": " + br.diags[0].message));
      if (!br.ok) {
        continue;
      }
      const ir::VerifyResult iv = ir::verify(g);
      CHECK_MSG(iv.ok, name + ":" + m.name + ": IR verify" +
                           (iv.diags.empty()
                                ? std::string()
                                : " n" + std::to_string(iv.diags[0].node) +
                                      ": " + iv.diags[0].message));
      // Determinism per corpus method: a second build is byte-identical.
      ir::Graph g2;
      passes::CounterResolver res2;
      (void)passes::buildGraph(m, res2, g2, static_cast<ir::MethodId>(i));
      CHECK(ir::print(g) == ir::print(g2));
      ++methods;
    }
  }
  CHECK(methods >= 19); // every program has at least main
  std::printf("  corpus sweep: %zu methods built+verified\n", methods);
}
