// B-2 passes tests - the T2 inlining engine (T2-INL1).
//
// Discipline: Rule 35 - at least ten tests per delivered surface; the
// surfaces here are the direct-inline engine (include/b2/passes/Inline.h)
// and the DCE chained-FrameState protection. Every test runs the TOTAL IR
// verifier after the transformation (Rule 40 - the driver does too, after
// every site) and pins: the exit-merge shapes (Region/memory-phi/value
// phi), the role-based rewiring (control/memory/value), the FrameState
// caller chains (Rule 75), the exception-escape routing (covered sites
// deopt through the callee frame; uncovered sites keep the Unwind), the
// refusal catalog (icdg.md 12: a do-not-inline engine first), budgets,
// the kill switch, determinism (Rule 124), idempotency, and the corpus
// sweep (all 19 interp-corpus programs through build -> inline ->
// pipeline, verified at every step).

#include "TestHarness.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <b2/interp/Interp.h>
#include <b2/ir/Node.h>
#include <b2/ir/Printer.h>
#include <b2/ir/Verifier.h>
#include <b2/passes/GraphBuilder.h>
#include <b2/passes/Inline.h>
#include <b2/passes/Passes.h>
#include <b2/rbc/RbcBuilder.h>
#include <b2/rbc/RbcText.h>
#include <b2/rbc/Verifier.h>

// The ICDG Phase 2 ingestion bridge (T0 profile -> passes snapshot).
// Lives in the passes tool dir; only interp-linking integrators include
// it - this test binary is one (the end-to-end guard test runs T0).
#include "DispatchProfileSnapshot.h"

using namespace b2;
using ir::NodeKind;
using IA = passes::InlineAction;

namespace {

// --- shared helpers -----------------------------------------------------------

[[nodiscard]] std::size_t countKind(const ir::Graph& g, NodeKind k) {
  std::size_t n = 0;
  for (ir::NodeId i = 0; i < g.nodeCount(); ++i) {
    if (!g.node(i).isDead() && g.node(i).kind == k) {
      ++n;
    }
  }
  return n;
}

// Alive nodes of kind `k` with at least one LIVE user: excludes the
// tombstone-law junk anchors (flow-dead bookkeeping referenced only by
// Dead/Replaced tombstones - pass_contracts.md section 3).
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

[[nodiscard]] std::size_t countAction(const passes::InlineResult& r,
                                      IA a) {
  std::size_t n = 0;
  for (const passes::InlineDecision& d : r.decisions) {
    if (d.action == a) {
      ++n;
    }
  }
  return n;
}

[[nodiscard]] std::vector<ir::NodeId> chainedFsNodes(const ir::Graph& g) {
  // Live FrameState nodes whose descriptor chains to a caller frame
  // (the inlined-callee snapshots; FrameStateDesc.caller != invalid).
  std::vector<ir::NodeId> out;
  for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
    const ir::Node& nd = g.node(n);
    if (nd.isDead() || nd.kind != NodeKind::FrameState) {
      continue;
    }
    if (g.frameState(nd.payload).caller != ir::kInvalidFrameState) {
      out.push_back(n);
    }
  }
  return out;
}

// The FrameState NODE carrying descriptor `desc` (invalid if none).
[[nodiscard]] ir::NodeId fsNodeOfDesc(const ir::Graph& g,
                                       ir::FrameStateId desc) {
  for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
    const ir::Node& nd = g.node(n);
    if (!nd.isDead() && nd.kind == NodeKind::FrameState &&
        nd.payload == desc) {
      return n;
    }
  }
  return ir::kInvalidNodeId;
}

// Builds prog.methods[idx] with the source as the resolver (the unified
// id-space contract), verifies RBC + IR, runs the inliner, requires ok.
// Graph is non-movable: placement-new into `g` (the existing discipline).
void inlineOk(const rbc::Program& prog, std::size_t idx, ir::Graph& g,
              passes::InlineResult& out,
              const passes::InlineConfig& cfg = {}) {
  g.~Graph();
  new (&g) ir::Graph();
  passes::ProgramCalleeSource src(prog);
  const rbc::Method& m = prog.methods[idx];
  const rbc::VerifyResult vr = rbc::verify(m);
  CHECK_MSG(vr.ok, vr.diags.empty() ? "RBC verify failed"
                                    : vr.diags[0].message.c_str());
  passes::BuildResult br = passes::buildGraph(
      m, src, g, ir::MethodId{static_cast<std::uint32_t>(idx)});
  CHECK_MSG(br.ok, br.diags.empty() ? "build failed"
                                    : br.diags[0].message.c_str());
  const ir::VerifyResult iv = ir::verify(g);
  CHECK_MSG(iv.ok, iv.diags.empty() ? "IR verify failed"
                                    : iv.diags[0].message.c_str());
  out = passes::runInlining(g, src, cfg);
  CHECK_MSG(out.ok, out.diags.empty() ? "inline failed"
                                      : out.diags[0].message.c_str());
}

[[nodiscard]] bool verifyOk(const ir::Graph& g) {
  return ir::verify(g).ok;
}

// Fresh program assembly (the v0 one-class world).
[[nodiscard]] rbc::Program mkProgram(std::vector<rbc::Method> ms) {
  rbc::Program p;
  p.className = "Main";
  p.methods = std::move(ms);
  return p;
}

// add(II)I with TWO exits (pos: a+b, neg: 0) - the multi-exit merge shape.
[[nodiscard]] rbc::Method mkAdd2() {
  rbc::RbcBuilder b("add", "(II)I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(2);
  const rbc::RbcBuilder::Label neg = b.newLabel();
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegBranch(rbc::Op::Ifle, 0, neg);
  b.emitRegSlot(rbc::Op::Iload, 2, 0);
  b.emitRegSlot(rbc::Op::Iload, 3, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 2, 3);
  b.emitReg(rbc::Op::Ireturn, 2);
  b.bind(neg);
  b.emitRegImm(rbc::Op::Iconst, 2, 0);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  return m;
}

// one(II)I with ONE exit (a+b) - the direct-wire shape (no merge nodes).
[[nodiscard]] rbc::Method mkAdd1() {
  rbc::RbcBuilder b("one", "(II)I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(2);
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitRegSlot(rbc::Op::Iload, 1, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  return m;
}

} // namespace

// --- core: the direct inline ---------------------------------------------------

B2_TEST(il_static_call_inlined_call_dies) {
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(6);
  const std::uint32_t add = b.constMethodRef("Main", "add", "(II)I");
  b.emitRegImm(rbc::Op::Iconst, 0, 3);
  b.emitRegImm(rbc::Op::Iconst, 1, 4);
  b.emitCall(rbc::Op::Invokestatic, 2, 0, 2, add);
  b.emit(rbc::Op::Return);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({mkAdd2(), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesConsidered == 1);
  CHECK(r.telemetry.sitesInlined == 1);
  CHECK(r.telemetry.sitesRefused == 0);
  CHECK(countAction(r, IA::DirectInline) == 1);
  // The call, its projection, and the site continuation are all dead;
  // the body + the exit merge are live and the graph verifies.
  CHECK(countLiveUserKind(g, NodeKind::CallStatic) == 0);
  CHECK(countLiveUserKind(g, NodeKind::CallExcept) == 0);
  CHECK(countLiveUserKind(g, NodeKind::Unwind) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(il_single_exit_direct_wiring_no_merge) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(6);
  const std::uint32_t one = b.constMethodRef("Main", "one", "(II)I");
  b.emitRegImm(rbc::Op::Iconst, 0, 3);
  b.emitRegImm(rbc::Op::Iconst, 1, 4);
  b.emitCall(rbc::Op::Invokestatic, 2, 0, 2, one);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({mkAdd1(), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  CHECK(r.telemetry.exitMerges == 0);
  // Single exit: no Region, no Phi (the exit state wires directly).
  CHECK(countKind(g, NodeKind::Region) == 0);
  CHECK(countKind(g, NodeKind::Phi) == 0);
  // The call's value user (the Return) now consumes the callee's AddI.
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::AddI);
  CHECK(verifyOk(g));
}

B2_TEST(il_multi_exit_region_and_phis) {
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(6);
  const std::uint32_t add = b.constMethodRef("Main", "add", "(II)I");
  b.emitRegImm(rbc::Op::Iconst, 0, 3);
  b.emitRegImm(rbc::Op::Iconst, 1, 4);
  b.emitCall(rbc::Op::Invokestatic, 2, 0, 2, add);
  b.emit(rbc::Op::Return);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({mkAdd2(), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.exitMerges == 1);
  CHECK(countKind(g, NodeKind::Region) == 1);
  // Memory phi + value phi at the merge.
  const auto phis = nodesOfKind(g, NodeKind::Phi);
  CHECK(phis.size() == 2);
  for (const ir::NodeId p : phis) {
    CHECK(g.node(g.input(p, 0)).kind == NodeKind::Region);
  }
  // The value phi feeds the caller's Return (the call's value user).
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  const ir::NodeId retCtrl = g.input(ret, 0);
  CHECK(g.node(retCtrl).kind == NodeKind::Region);
  CHECK(verifyOk(g));
}

B2_TEST(il_void_callee_value_free_inline) {
  // void callee with a store: no value phi; the memory chain threads.
  rbc::RbcBuilder cb("put", "(I)V", rbc::method_flags::Static);
  cb.setRegs(2);
  cb.setLocals(1);
  const std::uint32_t f = cb.constFieldRef("Main", "value", "I");
  cb.emitRegSlot(rbc::Op::Iload, 0, 0);
  cb.emitRegCp(rbc::Op::Putstatic, 0, f);
  cb.emit(rbc::Op::Return);
  rbc::Method put;
  CHECK(cb.finish(put).ok);

  rbc::RbcBuilder b("main", "(I)V", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("Main", "put", "(I)V");
  const std::uint32_t f2 = b.constFieldRef("Main", "value", "I");
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, mc);
  b.emitRegCp(rbc::Op::Getstatic, 2, f2);
  b.emit(rbc::Op::Return);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(put), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  CHECK(countLiveUserKind(g, NodeKind::CallStatic) == 0);
  // The caller's LoadStatic reads the memory the callee's StoreStatic
  // produced: the chain threads StoreStatic -> (the getstatic's own
  // ClassInit trigger) -> LoadStatic.
  const ir::NodeId load = firstOf(g, NodeKind::LoadStatic);
  const ir::NodeId hop = g.input(load, 1);
  CHECK(g.node(hop).kind == NodeKind::ClassInit);
  CHECK(g.node(g.input(hop, 1)).kind == NodeKind::StoreStatic);
  CHECK(verifyOk(g));
}

B2_TEST(il_loop_callee_inlines_loopbegin) {
  // sum(I)I: a counted loop (LoopBegin in the inlined body).
  rbc::RbcBuilder cb("sum", "(I)I", rbc::method_flags::Static);
  cb.setRegs(2);
  cb.setLocals(1);
  const rbc::RbcBuilder::Label loop = cb.newLabel();
  const rbc::RbcBuilder::Label done = cb.newLabel();
  cb.emitRegImm(rbc::Op::Iconst, 0, 0);
  cb.bind(loop);
  cb.emitRegSlot(rbc::Op::Iload, 1, 0);
  cb.emitRegBranch(rbc::Op::Ifle, 1, done);
  cb.emitRegRegReg(rbc::Op::Iadd, 0, 0, 1);
  cb.emitRegImm(rbc::Op::Iinc, 1, static_cast<std::uint32_t>(-1));
  cb.emitSlotReg(rbc::Op::Istore, 1, 0);
  cb.emitBranch(rbc::Op::Goto, loop);
  cb.bind(done);
  cb.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method sum;
  CHECK(cb.finish(sum).ok);

  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(4);
  const std::uint32_t mc = b.constMethodRef("Main", "sum", "(I)I");
  b.emitRegImm(rbc::Op::Iconst, 0, 10);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, mc);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(sum), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  CHECK(countKind(g, NodeKind::LoopBegin) == 1);
  CHECK(countLiveUserKind(g, NodeKind::CallStatic) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(il_nested_depth_two) {
  // leaf(I)I <- mid(I)I <- main: two levels, both inlined.
  rbc::RbcBuilder lb("leaf", "(I)I", rbc::method_flags::Static);
  lb.setRegs(2);
  lb.setLocals(1);
  lb.emitRegSlot(rbc::Op::Iload, 0, 0);
  lb.emitRegImm(rbc::Op::Iconst, 1, 1);
  lb.emitRegRegReg(rbc::Op::Iadd, 0, 0, 1);
  lb.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method leaf;
  CHECK(lb.finish(leaf).ok);

  rbc::RbcBuilder mb("mid", "(I)I", rbc::method_flags::Static);
  mb.setRegs(3);
  mb.setLocals(1);
  const std::uint32_t leafCp = mb.constMethodRef("Main", "leaf", "(I)I");
  mb.emitRegSlot(rbc::Op::Iload, 0, 0);
  mb.emitCall(rbc::Op::Invokestatic, 1, 0, 1, leafCp);
  mb.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method mid;
  CHECK(mb.finish(mid).ok);

  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  const std::uint32_t midCp = b.constMethodRef("Main", "mid", "(I)I");
  b.emitRegImm(rbc::Op::Iconst, 0, 5);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, midCp);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram(
      {std::move(leaf), std::move(mid), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 2, g, r);
  CHECK(r.telemetry.sitesInlined == 2);
  CHECK(r.telemetry.maxDepthReached == 2);
  CHECK(countLiveUserKind(g, NodeKind::CallStatic) == 0);
  // The whole chain collapsed to one AddI feeding the Return's value.
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::AddI);
  CHECK(verifyOk(g));
}

B2_TEST(il_classinit_stays_in_the_chain) {
  // The static-call site's ClassInit (JVMS 5.5) stays in the memory chain
  // before the inlined body - the body's statics read post-init memory.
  rbc::RbcBuilder cb("get", "()I", rbc::method_flags::Static);
  cb.setRegs(1);
  const std::uint32_t f = cb.constFieldRef("Main", "value", "I");
  cb.emitRegCp(rbc::Op::Getstatic, 0, f);
  cb.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method get;
  CHECK(cb.finish(get).ok);

  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(3);
  const std::uint32_t mc = b.constMethodRef("Main", "get", "()I");
  b.emitCall(rbc::Op::Invokestatic, 0, 0, 0, mc);
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(get), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  CHECK(countLiveUserKind(g, NodeKind::CallStatic) == 0);
  // Both ClassInit triggers stay (the call site's for the callee's
  // class, the body's own for the static read); the body's LoadStatic
  // reads memory through the body's trigger.
  CHECK(countKind(g, NodeKind::ClassInit) >= 1);
  const ir::NodeId load = firstOf(g, NodeKind::LoadStatic);
  CHECK(g.node(g.input(load, 1)).kind == NodeKind::ClassInit);
  CHECK(verifyOk(g));
}

B2_TEST(il_argument_values_flow_into_body) {
  // The callee's parameters are the caller's argument defs: add(const 3,
  // const 4) folds to 7 through the pipeline after the inline.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(6);
  const std::uint32_t one = b.constMethodRef("Main", "one", "(II)I");
  b.emitRegImm(rbc::Op::Iconst, 0, 3);
  b.emitRegImm(rbc::Op::Iconst, 1, 4);
  b.emitCall(rbc::Op::Invokestatic, 2, 0, 2, one);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({mkAdd1(), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  // The body's AddI reads the caller's constants.
  const ir::NodeId addN = firstOf(g, NodeKind::AddI);
  CHECK(g.node(g.input(addN, 0)).kind == NodeKind::ConstantI);
  CHECK(g.node(g.input(addN, 1)).kind == NodeKind::ConstantI);
  // The pipeline then folds the whole body (the inline unlocked constfold).
  const passes::PassResult pr = passes::runEarlyCleanup(g);
  CHECK(pr.ok);
  CHECK(verifyOk(g));
  CHECK(countKind(g, NodeKind::AddI) == 0);
  const ir::NodeId ret = firstOf(g, NodeKind::Return);
  CHECK(g.node(g.input(ret, 1)).kind == NodeKind::ConstantI);
  CHECK(g.node(g.input(ret, 1)).constValue == 7);
}

// --- FrameState caller chains (Rule 75) -----------------------------------------

B2_TEST(il_callee_framesates_chain_to_call_site) {
  // get(Ljava/lang/Object;)I reads a field through the (null-guarded)
  // parameter: the guard's FrameState is a CALLEE frame that must chain
  // to the call-site snapshot (deopt reconstructs the inlined stack).
  rbc::RbcBuilder cb("get", "(Ljava/lang/Object;)I",
                     rbc::method_flags::Static);
  cb.setRegs(2);
  cb.setLocals(1);
  const std::uint32_t f = cb.constFieldRef("Main", "x", "I");
  cb.emitRegSlot(rbc::Op::Aload, 0, 0);
  cb.emitRegRegCp(rbc::Op::Getfield, 1, 0, f);
  cb.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method get;
  CHECK(cb.finish(get).ok);

  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)I",
                    rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("Main", "get",
                                            "(Ljava/lang/Object;)I");
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, mc);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(get), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);

  // Every callee FrameState chains somewhere, and the chain target is a
  // LIVE FrameState node carrying the CALLER's method id (the snapshot
  // T0 needs to reconstruct the caller frame; the DCE protection test
  // pins that liveness).
  const auto chained = chainedFsNodes(g);
  CHECK(chained.size() >= 1);
  for (const ir::NodeId n : chained) {
    const ir::FrameStateDesc& d = g.frameState(g.node(n).payload);
    CHECK(d.method == 0); // the callee's frame (program index 0)
    const ir::NodeId callerFs = fsNodeOfDesc(g, d.caller);
    CHECK(callerFs != ir::kInvalidNodeId);
    CHECK(g.node(callerFs).kind == NodeKind::FrameState);
    CHECK(!g.node(callerFs).isDead());
    CHECK(g.frameState(g.node(callerFs).payload).method == 1);
  }
  CHECK(verifyOk(g));
}

B2_TEST(il_deopt_ids_stay_unique) {
  // The callee body's guards/deopts continue the caller graph's deopt id
  // allocation: every Deopt payload / Guard payload2 is distinct.
  rbc::RbcBuilder cb("get", "(Ljava/lang/Object;)I",
                     rbc::method_flags::Static);
  cb.setRegs(2);
  cb.setLocals(1);
  const std::uint32_t f = cb.constFieldRef("Main", "x", "I");
  cb.emitRegSlot(rbc::Op::Aload, 0, 0);
  cb.emitRegRegCp(rbc::Op::Getfield, 1, 0, f);
  cb.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method get;
  CHECK(cb.finish(get).ok);

  // main null-guards its own receiver too (getstatic of System.out is
  // overkill here; a second guarded call in main suffices).
  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)I",
                    rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("Main", "get",
                                            "(Ljava/lang/Object;)I");
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, mc);
  b.emitRegSlot(rbc::Op::Aload, 2, 0);
  b.emitCall(rbc::Op::Invokestatic, 3, 2, 1, mc);
  b.emitReg(rbc::Op::Ireturn, 3);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(get), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 2);
  // The callee bodies' null-guards carry fresh deopt ids (payload2).
  CHECK(countLiveUserKind(g, NodeKind::Guard) >= 2);

  std::vector<std::uint32_t> ids;
  for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
    const ir::Node& nd = g.node(n);
    if (nd.isDead()) {
      continue;
    }
    if (nd.kind == NodeKind::Deopt) {
      ids.push_back(nd.payload);
    } else if (nd.kind == NodeKind::Guard) {
      ids.push_back(nd.payload2);
    }
  }
  std::sort(ids.begin(), ids.end());
  CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
  CHECK(verifyOk(g));
}

// --- exception escapes (docs/inlining.md section 4) ------------------------------

B2_TEST(il_escape_uncovered_site_keeps_unwind) {
  // thrower(I)I: athrow escapes the callee; the caller site is uncovered
  // -> the callee's Unwind REMAINS (the exception value - the New node -
  // propagates out of the caller directly); the site's old Unwind dies.
  rbc::RbcBuilder cb("thrower", "(I)I", rbc::method_flags::Static);
  cb.setRegs(2);
  cb.setLocals(1);
  const std::uint32_t cls = cb.constClass("Main");
  const rbc::RbcBuilder::Label pos = cb.newLabel();
  cb.emitRegSlot(rbc::Op::Iload, 0, 0);
  cb.emitRegBranch(rbc::Op::Ifgt, 0, pos);
  cb.emitRegCp(rbc::Op::New, 0, cls);
  cb.emitReg(rbc::Op::Athrow, 0);
  cb.bind(pos);
  cb.emitRegSlot(rbc::Op::Iload, 1, 0);
  cb.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method thrower;
  CHECK(cb.finish(thrower).ok);

  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("Main", "thrower", "(I)I");
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, mc);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(thrower), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  // Exactly ONE live Unwind: the callee's escape (value = the New node),
  // not the site's old [CallExcept, CallExcept] shape.
  CHECK(countKind(g, NodeKind::Unwind) == 1);
  const ir::NodeId uw = firstOf(g, NodeKind::Unwind);
  CHECK(g.node(g.input(uw, 1)).kind == NodeKind::New);
  CHECK(countKind(g, NodeKind::CallExcept) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(il_escape_covered_site_deopts_through_callee_frame) {
  // Same thrower, but the caller's handler covers the call site: the
  // escape becomes a Deopt whose FrameState is the CALLEE's (chained to
  // the call-site snapshot) - T0 re-executes the athrow, finds no
  // handler in the callee, unwinds it, and the deopt runtime re-enters
  // the caller at the call pc with the pending exception.
  rbc::RbcBuilder cb("thrower", "(I)I", rbc::method_flags::Static);
  cb.setRegs(2);
  cb.setLocals(1);
  const std::uint32_t cls = cb.constClass("Main");
  const rbc::RbcBuilder::Label pos = cb.newLabel();
  cb.emitRegSlot(rbc::Op::Iload, 0, 0);
  cb.emitRegBranch(rbc::Op::Ifgt, 0, pos);
  cb.emitRegCp(rbc::Op::New, 0, cls);
  cb.emitReg(rbc::Op::Athrow, 0);
  cb.bind(pos);
  cb.emitRegSlot(rbc::Op::Iload, 1, 0);
  cb.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method thrower;
  CHECK(cb.finish(thrower).ok);

  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("Main", "thrower", "(I)I");
  const std::uint32_t ec = b.constClass("java/lang/Exception");
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  const std::uint32_t tryStart = b.here();
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, mc);
  const std::uint32_t tryEnd = b.here();
  b.emitReg(rbc::Op::Ireturn, 1);
  const std::uint32_t h = b.here();
  b.emitRegImm(rbc::Op::Iconst, 2, 0);
  b.emitReg(rbc::Op::Ireturn, 2);
  b.addHandler(tryStart, tryEnd, h, static_cast<std::int32_t>(ec));
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(thrower), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  // The old site continuation (Deopt [CallExcept, fs]) is dead; the
  // escape's Deopt is a normal deopt at the callee's athrow with a
  // CHAINED callee FrameState.
  CHECK(countKind(g, NodeKind::CallExcept) == 0);
  const auto deopts = nodesOfKind(g, NodeKind::Deopt);
  CHECK(deopts.size() == 1);
  const ir::NodeId dep = deopts[0];
  const ir::NodeId fs = g.input(dep, 1);
  CHECK(g.node(fs).kind == NodeKind::FrameState);
  const ir::FrameStateDesc& d = g.frameState(g.node(fs).payload);
  CHECK(d.method == 0); // the CALLEE's frame
  CHECK(d.caller != ir::kInvalidFrameState); // chained to the call site
  CHECK(g.node(g.input(dep, 0)).kind != NodeKind::CallExcept);
  CHECK(verifyOk(g));
}

B2_TEST(il_callee_caught_throw_is_untouched) {
  // The callee's OWN covered athrow keeps the builder's caught-athrow
  // deopt shape (T0 re-executes and the callee's handler runs): the
  // inlining only reroutes ESCAPES, never in-callee catches.
  rbc::RbcBuilder cb("inner", "(I)I", rbc::method_flags::Static);
  cb.setRegs(2);
  cb.setLocals(1);
  const std::uint32_t cls = cb.constClass("Main");
  const std::uint32_t ec = cb.constClass("java/lang/Exception");
  const rbc::RbcBuilder::Label pos = cb.newLabel();
  cb.emitRegSlot(rbc::Op::Iload, 0, 0);
  cb.emitRegBranch(rbc::Op::Ifgt, 0, pos);
  const std::uint32_t tryStart = cb.here();
  cb.emitRegCp(rbc::Op::New, 1, cls);
  cb.emitReg(rbc::Op::Athrow, 1);
  const std::uint32_t tryEnd = cb.here();
  cb.bind(pos);
  cb.emitRegSlot(rbc::Op::Iload, 1, 0);
  cb.emitReg(rbc::Op::Ireturn, 1);
  const std::uint32_t ch = cb.here();
  cb.emit(rbc::Op::Return);
  cb.addHandler(tryStart, tryEnd, ch, static_cast<std::int32_t>(ec));
  rbc::Method inner;
  CHECK(cb.finish(inner).ok);

  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("Main", "inner", "(I)I");
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, mc);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(inner), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  // The callee's caught-athrow Deopt is now an in-graph deopt with the
  // CALLEE's chained FrameState; no Unwind and no CallExcept survive.
  CHECK(countKind(g, NodeKind::Unwind) == 0);
  CHECK(countKind(g, NodeKind::CallExcept) == 0);
  const auto deopts = nodesOfKind(g, NodeKind::Deopt);
  CHECK(deopts.size() == 1);
  const ir::FrameStateDesc& d =
      g.frameState(g.node(g.input(deopts[0], 1)).payload);
  CHECK(d.method == 0);
  CHECK(d.caller != ir::kInvalidFrameState);
  CHECK(verifyOk(g));
}

B2_TEST(il_nested_external_call_escape_uncovered) {
  // The callee calls an EXTERNAL method (kept indirect) that may throw,
  // uncovered in the callee, at an uncovered caller site: the escape is
  // the callee's Unwind [CallExcept_C, CallExcept_C] - the exception
  // value of C propagates out of the caller.
  rbc::RbcBuilder cb("caller", "(I)I", rbc::method_flags::Static);
  cb.setRegs(2);
  cb.setLocals(1);
  const std::uint32_t ext =
      cb.constMethodRef("java/io/PrintStream", "flush", "()V");
  cb.emitCall(rbc::Op::Invokestatic, 0, 0, 0, ext);
  cb.emitRegSlot(rbc::Op::Iload, 1, 0);
  cb.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method caller;
  CHECK(cb.finish(caller).ok);

  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("Main", "caller", "(I)I");
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, mc);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(caller), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  CHECK(r.telemetry.sitesRefused == 1); // the external call stays
  // The external call is still live with its exceptional projection and
  // its Unwind escape (all now part of the inlined body).
  CHECK(countLiveUserKind(g, NodeKind::CallStatic) == 1);
  CHECK(countKind(g, NodeKind::CallExcept) == 1);
  CHECK(countKind(g, NodeKind::Unwind) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(il_nested_external_call_escape_covered) {
  // Same shape, caller site covered: the escape becomes the class-2.3
  // exception deopt Deopt [CallExcept_C, callee fs] - the existing
  // convention, now inside the inlined body with a chained FrameState.
  rbc::RbcBuilder cb("caller", "(I)I", rbc::method_flags::Static);
  cb.setRegs(2);
  cb.setLocals(1);
  const std::uint32_t ext =
      cb.constMethodRef("java/io/PrintStream", "flush", "()V");
  cb.emitCall(rbc::Op::Invokestatic, 0, 0, 0, ext);
  cb.emitRegSlot(rbc::Op::Iload, 1, 0);
  cb.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method caller;
  CHECK(cb.finish(caller).ok);

  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("Main", "caller", "(I)I");
  const std::uint32_t ec = b.constClass("java/lang/Exception");
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  const std::uint32_t tryStart = b.here();
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, mc);
  const std::uint32_t tryEnd = b.here();
  b.emitReg(rbc::Op::Ireturn, 1);
  const std::uint32_t h = b.here();
  b.emitRegImm(rbc::Op::Iconst, 2, 0);
  b.emitReg(rbc::Op::Ireturn, 2);
  b.addHandler(tryStart, tryEnd, h, static_cast<std::int32_t>(ec));
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(caller), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  // The nested external call is live with its CallExcept; the site's old
  // continuation is dead; the escape is ONE class-2.3 Deopt whose ctrl is
  // the CallExcept and whose fs is the callee's chained frame.
  CHECK(countLiveUserKind(g, NodeKind::CallStatic) == 1);
  const auto deopts = nodesOfKind(g, NodeKind::Deopt);
  CHECK(deopts.size() == 1);
  CHECK(g.node(g.input(deopts[0], 0)).kind == NodeKind::CallExcept);
  const ir::FrameStateDesc& d =
      g.frameState(g.node(g.input(deopts[0], 1)).payload);
  CHECK(d.method == 0);
  CHECK(d.caller != ir::kInvalidFrameState);
  CHECK(countKind(g, NodeKind::Unwind) == 0);
  CHECK(verifyOk(g));
}

// --- refusals (icdg.md 12: the do-not-inline engine) -------------------------------

B2_TEST(il_external_call_stays_indirect) {
  // println is not a program method: unresolved -> KeepIndirect, the
  // call is byte-identically preserved.
  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(4);
  const std::uint32_t out =
      b.constFieldRef("java/lang/System", "out", "Ljava/io/PrintStream;");
  const std::uint32_t pn =
      b.constMethodRef("java/io/PrintStream", "println", "(I)V");
  b.emitRegCp(rbc::Op::Getstatic, 0, out);
  b.emitRegImm(rbc::Op::Iconst, 1, 42);
  b.emitCall(rbc::Op::Invokevirtual, 2, 0, 2, pn);
  b.emit(rbc::Op::Return);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 0, g, r);
  CHECK(r.telemetry.sitesConsidered == 0); // CallVirtual is not a v1 site
  CHECK(countKind(g, NodeKind::CallVirtual) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(il_recursion_control_one_level_only) {
  // f(I)I calls itself: the ROOT site inlines (the stack is empty - one
  // expansion is sound: the body's recursive call stays a call), and
  // the body's site is refused as a recursive cycle.
  rbc::RbcBuilder cb("f", "(I)I", rbc::method_flags::Static);
  cb.setRegs(3);
  cb.setLocals(1);
  const std::uint32_t self = cb.constMethodRef("Main", "f", "(I)I");
  const rbc::RbcBuilder::Label base = cb.newLabel();
  cb.emitRegSlot(rbc::Op::Iload, 0, 0);
  cb.emitRegBranch(rbc::Op::Ifle, 0, base);
  cb.emitRegSlot(rbc::Op::Iload, 0, 0);
  cb.emitRegImm(rbc::Op::Iconst, 1, 1);
  cb.emitRegRegReg(rbc::Op::Isub, 1, 0, 1);
  cb.emitCall(rbc::Op::Invokestatic, 2, 1, 1, self);
  cb.emitReg(rbc::Op::Ireturn, 2);
  cb.bind(base);
  cb.emitRegSlot(rbc::Op::Iload, 2, 0);
  cb.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method f;
  CHECK(cb.finish(f).ok);

  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("Main", "f", "(I)I");
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, mc);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(f), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesConsidered == 2);
  CHECK(r.telemetry.sitesInlined == 1);
  CHECK(r.telemetry.sitesRefused == 1);
  CHECK(std::strcmp(r.decisions[1].reason,
                    "recursive cycle (callee already on the inline stack)") ==
        0);
  // The body's recursive call remains a live call.
  CHECK(countLiveUserKind(g, NodeKind::CallStatic) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(il_depth_cap_refuses_third_level) {
  // leaf <- mid <- inner <- main with maxDepth 2: two inline, the third
  // site is refused by the depth cap.
  rbc::RbcBuilder lb("leaf", "(I)I", rbc::method_flags::Static);
  lb.setRegs(2);
  lb.setLocals(1);
  lb.emitRegSlot(rbc::Op::Iload, 0, 0);
  lb.emitRegImm(rbc::Op::Iconst, 1, 1);
  lb.emitRegRegReg(rbc::Op::Iadd, 0, 0, 1);
  lb.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method leaf;
  CHECK(lb.finish(leaf).ok);

  auto mkChain = [](const char* name) {
    rbc::RbcBuilder cb(name, "(I)I", rbc::method_flags::Static);
    cb.setRegs(2);
    cb.setLocals(1);
    const std::uint32_t tgt =
        cb.constMethodRef("Main", "leaf", "(I)I"); // patched below per level
    cb.emitRegSlot(rbc::Op::Iload, 0, 0);
    cb.emitCall(rbc::Op::Invokestatic, 1, 0, 1, tgt);
    cb.emitReg(rbc::Op::Ireturn, 1);
    return cb;
  };
  (void)mkChain;

  // mid calls leaf
  rbc::RbcBuilder mb("mid", "(I)I", rbc::method_flags::Static);
  mb.setRegs(2);
  mb.setLocals(1);
  const std::uint32_t leafCp = mb.constMethodRef("Main", "leaf", "(I)I");
  mb.emitRegSlot(rbc::Op::Iload, 0, 0);
  mb.emitCall(rbc::Op::Invokestatic, 1, 0, 1, leafCp);
  mb.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method mid;
  CHECK(mb.finish(mid).ok);

  // inner calls mid
  rbc::RbcBuilder ib("inner", "(I)I", rbc::method_flags::Static);
  ib.setRegs(2);
  ib.setLocals(1);
  const std::uint32_t midCp = ib.constMethodRef("Main", "mid", "(I)I");
  ib.emitRegSlot(rbc::Op::Iload, 0, 0);
  ib.emitCall(rbc::Op::Invokestatic, 1, 0, 1, midCp);
  ib.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method inner;
  CHECK(ib.finish(inner).ok);

  rbc::RbcBuilder b("main", "(I)I", rbc::method_flags::Static);
  b.setRegs(2);
  b.setLocals(1);
  const std::uint32_t innerCp = b.constMethodRef("Main", "inner", "(I)I");
  b.emitRegSlot(rbc::Op::Iload, 0, 0);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, innerCp);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram(
      {std::move(leaf), std::move(mid), std::move(inner), std::move(main)});

  passes::InlineConfig cfg;
  cfg.maxDepth = 2;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 3, g, r, cfg);
  CHECK(r.telemetry.sitesInlined == 2); // main->inner (d1), inner->mid (d2)
  CHECK(r.telemetry.sitesRefused == 1); // mid->leaf at depth 3: refused
  CHECK(std::strcmp(r.decisions[2].reason, "inline depth cap") == 0);
  CHECK(r.telemetry.maxDepthReached == 2);
  CHECK(verifyOk(g));
}

B2_TEST(il_insn_cap_refuses_large_callee) {
  // A 40-instruction callee: over the default instruction cap.
  rbc::RbcBuilder cb("big", "()I", rbc::method_flags::Static);
  cb.setRegs(2);
  for (int i = 0; i < 39; ++i) {
    cb.emitRegImm(rbc::Op::Iconst, 0, static_cast<std::uint32_t>(i));
  }
  cb.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method big;
  CHECK(cb.finish(big).ok);

  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(2);
  const std::uint32_t mc = b.constMethodRef("Main", "big", "()I");
  b.emitCall(rbc::Op::Invokestatic, 0, 0, 0, mc);
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(big), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 0);
  CHECK(std::strcmp(r.decisions[0].reason,
                    "callee too large (instruction cap)") == 0);
  CHECK(r.decisions[0].calleeInsns == 40);
  CHECK(countKind(g, NodeKind::CallStatic) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(il_never_returns_refused) {
  // thrower()V: only an athrow, no normal return - v1 refuses (the
  // zero-exit merge is the documented follow-up).
  rbc::RbcBuilder cb("die", "()V", rbc::method_flags::Static);
  cb.setRegs(1);
  const std::uint32_t cls = cb.constClass("Main");
  cb.emitRegCp(rbc::Op::New, 0, cls);
  cb.emitReg(rbc::Op::Athrow, 0);
  rbc::Method die;
  CHECK(cb.finish(die).ok);

  rbc::RbcBuilder b("main", "()V", rbc::method_flags::Static);
  b.setRegs(1);
  const std::uint32_t mc = b.constMethodRef("Main", "die", "()V");
  b.emitCall(rbc::Op::Invokestatic, 0, 0, 0, mc);
  b.emit(rbc::Op::Return);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(die), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 0);
  CHECK(std::strcmp(
            r.decisions[0].reason,
            "callee has no normal return path (v1 refuses)") == 0);
  CHECK(countKind(g, NodeKind::CallStatic) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(il_synchronized_refused) {
  // Synchronized callee: the FrameState cannot carry the held-monitor
  // record deopt must reproduce (interp_contract.md section 1).
  rbc::RbcBuilder cb("sync", "()I", rbc::method_flags::Static |
                                       rbc::method_flags::Synchronized);
  cb.setRegs(1);
  cb.emitRegImm(rbc::Op::Iconst, 0, 1);
  cb.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method sync;
  CHECK(cb.finish(sync).ok);

  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(1);
  const std::uint32_t mc = b.constMethodRef("Main", "sync", "()I");
  b.emitCall(rbc::Op::Invokestatic, 0, 0, 0, mc);
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(sync), std::move(main)});

  // inlineOk verifies RBC first; build the graph manually here to reach
  // the inliner's flag refusal with a builder-built caller.
  ir::Graph g;
  passes::ProgramCalleeSource src(prog);
  passes::BuildResult br =
      passes::buildGraph(prog.methods[1], src, g, ir::MethodId{1});
  CHECK(br.ok);
  passes::InlineResult r = passes::runInlining(g, src);
  CHECK(r.ok);
  CHECK(r.telemetry.sitesInlined == 0);
  CHECK(std::strcmp(r.decisions[0].reason,
                    "callee flags refuse inlining "
                    "(synchronized/abstract/native)") == 0);
  CHECK(verifyOk(g));
}

B2_TEST(il_budget_site_cap_reports_stop) {
  // Two inlinable sites with maxSitesPerGraph = 1: the second is a
  // budget stop, converged = false (Rule 26: never silent).
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(4);
  const std::uint32_t one = b.constMethodRef("Main", "one", "(II)I");
  b.emitRegImm(rbc::Op::Iconst, 0, 1);
  b.emitRegImm(rbc::Op::Iconst, 1, 2);
  b.emitCall(rbc::Op::Invokestatic, 2, 0, 2, one);
  b.emitRegImm(rbc::Op::Iconst, 0, 3);
  b.emitRegImm(rbc::Op::Iconst, 1, 4);
  b.emitCall(rbc::Op::Invokestatic, 3, 0, 2, one);
  b.emitReg(rbc::Op::Ireturn, 3);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({mkAdd1(), std::move(main)});

  passes::InlineConfig cfg;
  cfg.maxSitesPerGraph = 1;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r, cfg);
  CHECK(r.telemetry.sitesInlined == 1);
  CHECK(r.telemetry.budgetStops == 1);
  CHECK(!r.telemetry.converged);
  CHECK(std::strcmp(r.decisions[1].reason, "graph site budget") == 0);
  CHECK(countLiveUserKind(g, NodeKind::CallStatic) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(il_budget_node_cap_refuses) {
  // maxNodesPerGraph below the first callee's trial size: refused with
  // the graph node budget reason.
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(4);
  const std::uint32_t one = b.constMethodRef("Main", "one", "(II)I");
  b.emitRegImm(rbc::Op::Iconst, 0, 1);
  b.emitRegImm(rbc::Op::Iconst, 1, 2);
  b.emitCall(rbc::Op::Invokestatic, 2, 0, 2, one);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({mkAdd1(), std::move(main)});

  passes::InlineConfig cfg;
  cfg.maxNodesPerGraph = 4; // far below any trial
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r, cfg);
  CHECK(r.telemetry.sitesInlined == 0);
  CHECK(r.telemetry.budgetStops == 1);
  CHECK(std::strcmp(r.decisions[0].reason, "graph node budget") == 0);
  CHECK(countKind(g, NodeKind::CallStatic) == 1);
  CHECK(verifyOk(g));
}

// --- kill switch, determinism, idempotency (Rules 124, 132, 144) -------------------

B2_TEST(il_kill_switch_is_a_noop) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(4);
  const std::uint32_t one = b.constMethodRef("Main", "one", "(II)I");
  b.emitRegImm(rbc::Op::Iconst, 0, 1);
  b.emitRegImm(rbc::Op::Iconst, 1, 2);
  b.emitCall(rbc::Op::Invokestatic, 2, 0, 2, one);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({mkAdd1(), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  const std::string inlined = ir::print(g);

  // Fresh graph, disabled engine: byte-identical to the plain build.
  ir::Graph g2;
  passes::ProgramCalleeSource src2(prog);
  passes::BuildResult br2 =
      passes::buildGraph(prog.methods[1], src2, g2, ir::MethodId{1});
  CHECK(br2.ok);
  const std::string plain = ir::print(g2);
  passes::InlineConfig off;
  off.enabled = false;
  passes::InlineResult r2 = passes::runInlining(g2, src2, off);
  CHECK(r2.ok);
  CHECK(r2.decisions.empty());
  CHECK(r2.telemetry.sitesConsidered == 0);
  CHECK(ir::print(g2) == plain);
  CHECK(ir::print(g2) != inlined); // the enabled run did transform
}

B2_TEST(il_deterministic_double_run) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(4);
  const std::uint32_t add = b.constMethodRef("Main", "add", "(II)I");
  b.emitRegImm(rbc::Op::Iconst, 0, 3);
  b.emitRegImm(rbc::Op::Iconst, 1, 4);
  b.emitCall(rbc::Op::Invokestatic, 2, 0, 2, add);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({mkAdd2(), std::move(main)});

  ir::Graph gA;
  passes::InlineResult rA;
  inlineOk(prog, 1, gA, rA);
  ir::Graph gB;
  passes::InlineResult rB;
  inlineOk(prog, 1, gB, rB);
  CHECK(ir::print(gA) == ir::print(gB));
  CHECK(rA.decisions.size() == rB.decisions.size());
  CHECK(rA.telemetry.nodesAdded == rB.telemetry.nodesAdded);
  // The serialized bytes match too (Rule 124: same ids, same bytes).
  CHECK(ir::print(gA) == ir::print(gB));
}

B2_TEST(il_idempotent_second_run_inlines_nothing) {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(4);
  const std::uint32_t add = b.constMethodRef("Main", "add", "(II)I");
  b.emitRegImm(rbc::Op::Iconst, 0, 3);
  b.emitRegImm(rbc::Op::Iconst, 1, 4);
  b.emitCall(rbc::Op::Invokestatic, 2, 0, 2, add);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({mkAdd2(), std::move(main)});

  ir::Graph g;
  passes::InlineResult r1;
  inlineOk(prog, 1, g, r1);
  CHECK(r1.telemetry.sitesInlined == 1);
  const std::string once = ir::print(g);

  passes::ProgramCalleeSource src(prog);
  passes::InlineResult r2 = passes::runInlining(g, src);
  CHECK(r2.ok);
  CHECK(r2.telemetry.sitesInlined == 0);
  CHECK(ir::print(g) == once);
}

// --- DCE protection for chained FrameStates (docs/inlining.md section 5) -----------

B2_TEST(il_dce_keeps_chained_call_site_framesate) {
  // The call-site FrameState loses its last edge user (the dead call)
  // when the callee inlines, but the callee's frames chain to its DESC:
  // DCE must keep the node (its input edges ARE the caller-frame slot
  // values the deoptimizer needs) - the isCallerChained protection.
  rbc::RbcBuilder cb("get", "(Ljava/lang/Object;)I",
                     rbc::method_flags::Static);
  cb.setRegs(2);
  cb.setLocals(1);
  const std::uint32_t f = cb.constFieldRef("Main", "x", "I");
  cb.emitRegSlot(rbc::Op::Aload, 0, 0);
  cb.emitRegRegCp(rbc::Op::Getfield, 1, 0, f);
  cb.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method get;
  CHECK(cb.finish(get).ok);

  rbc::RbcBuilder b("main", "(Ljava/lang/Object;)I",
                    rbc::method_flags::Static);
  b.setRegs(3);
  b.setLocals(1);
  const std::uint32_t mc = b.constMethodRef("Main", "get",
                                            "(Ljava/lang/Object;)I");
  b.emitRegSlot(rbc::Op::Aload, 0, 0);
  b.emitCall(rbc::Op::Invokestatic, 1, 0, 1, mc);
  b.emitReg(rbc::Op::Ireturn, 1);
  rbc::Method main;
  CHECK(b.finish(main).ok);
  rbc::Program prog = mkProgram({std::move(get), std::move(main)});

  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r);
  CHECK(r.telemetry.sitesInlined == 1);
  const auto chained = chainedFsNodes(g);
  CHECK(chained.size() >= 1);
  const ir::FrameStateId siteDesc =
      g.frameState(g.node(chained[0]).payload).caller;
  ir::NodeId siteFs = fsNodeOfDesc(g, siteDesc);
  CHECK(siteFs != ir::kInvalidNodeId);

  // The full cleanup pipeline (DCE included) keeps the chained snapshot
  // alive with its edges intact.
  const passes::PassResult pr = passes::runEarlyCleanup(g);
  CHECK(pr.ok);
  CHECK(verifyOk(g));
  siteFs = fsNodeOfDesc(g, siteDesc);
  CHECK(siteFs != ir::kInvalidNodeId);
  CHECK(!g.node(siteFs).isDead());
  // And the chain itself is intact after the pipeline.
  const auto chained2 = chainedFsNodes(g);
  CHECK(chained2.size() >= 1);
  CHECK(g.frameState(g.node(chained2[0]).payload).caller == siteDesc);
}

// --- quickened payloads resolve through the unified space ---------------------------

B2_TEST(il_quickened_payload_inline) {
  // The corpus quickened program: invokestatic_quick carries the
  // program-table index (rbc_spec SS6.2); the unified source resolves it
  // exactly like an un-quickened MethodRef payload.
  const std::string path = "tests/interp/corpus/quickened.rbc";
  std::ifstream in(path);
  CHECK_MSG(static_cast<bool>(in), "cannot open " + path);
  if (!in) {
    return;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const auto parsed = rbc::parseRbcText(ss.str());
  CHECK(parsed.has_value());
  if (!parsed) {
    return;
  }
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(*parsed, 0, g, r);
  // main's quick site (payload 1 = bump's table index) inlined; bump is
  // small, has a normal return, and its guards chain to the call site.
  CHECK(r.telemetry.sitesConsidered == 1);
  CHECK(r.telemetry.sitesInlined == 1);
  CHECK(r.decisions.size() == 1);
  if (!r.decisions.empty()) {
    CHECK(r.decisions[0].target == 1);
    CHECK(r.decisions[0].action == IA::DirectInline);
  }
  CHECK(chainedFsNodes(g).size() >= 1);
  CHECK(verifyOk(g));
}

// --- GuardInline (ICDG Phase 2: the profile-driven devirtualization) ---------------
//
// The engine's virtual-site surface: classification over the snapshot
// (mono/bi/poly/mega, confidence, agreement), the guard construction
// (InstanceOf + TypeProfile Guard chained after the null guard), the
// Rule 122 SpecMeta + ClassHierarchy dependency, idempotency/
// determinism, the refusal catalog, and the end-to-end T0 snapshot.
//
// NOTE ON PROGRAM SHAPES: MSG-20260901-004 (the loop memory-phi backedge
// read as a "memory chain cycle") landed its fix, so call-in-loop shapes
// are back on the menu - see il_guard_call_in_loop_end_to_end, the
// canonical dispatch-profile loop program that shape previously blocked.

namespace {

// main()I with ONE straight-line virtual call: new Main; bump(5).
// Methods assemble as [main=0, bump=1]; the site is (0, pc=2).
[[nodiscard]] rbc::Method mkVirtMain() {
  rbc::RbcBuilder b("main", "()I", rbc::method_flags::Static);
  b.setRegs(4);
  b.setLocals(0);
  const std::uint32_t cls = b.constClass("Main");
  const std::uint32_t bump = b.constMethodRef("Main", "bump", "(I)I");
  b.emitRegCp(rbc::Op::New, 0, cls);
  b.emitRegImm(rbc::Op::Iconst, 1, 5);
  b.emitCall(rbc::Op::Invokevirtual, 2, 0, 2, bump);
  b.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  return m;
}

// The instance callee: bump(I)I = (i + 1) / 2. The division's zero
// guard is deliberate: it materializes a FrameState inside the inline
// body (the Rule 75 caller-chain shape the tests pin).
// l0 = this, l1 = i.
[[nodiscard]] rbc::Method mkVirtBump() {
  rbc::RbcBuilder b("bump", "(I)I", 0);
  b.setRegs(5);
  b.setLocals(2);
  b.emitRegSlot(rbc::Op::Iload, 0, 1);
  b.emitRegImm(rbc::Op::Iconst, 1, 1);
  b.emitRegRegReg(rbc::Op::Iadd, 2, 0, 1);
  b.emitRegImm(rbc::Op::Iconst, 3, 2);
  b.emitRegRegReg(rbc::Op::Idiv, 4, 2, 3);
  b.emitReg(rbc::Op::Ireturn, 4);
  rbc::Method m;
  CHECK(b.finish(m).ok);
  return m;
}

[[nodiscard]] rbc::Program mkVirtProgram() {
  rbc::Program p;
  p.className = "Main";
  rbc::Method ms[] = {mkVirtMain(), mkVirtBump()};
  p.methods.assign(ms, ms + 2);
  return p;
}

// A monomorphic profile row: `count` dispatches, all to (recv, target).
[[nodiscard]] passes::DispatchProfile
monoProfile(std::uint32_t method, std::uint32_t pc, std::uint32_t count,
            std::string_view recv, std::uint32_t target,
            passes::ProfileSiteKind kind = passes::ProfileSiteKind::Virtual) {
  passes::DispatchProfile p;
  p.sites.resize(method + 1);
  p.sites[method].resize(pc + 1);
  passes::ProfileSite& s = p.sites[method][pc];
  s.kind = kind;
  s.count = count;
  s.entries[0].recvClass.assign(recv);
  s.entries[0].target = target;
  s.entries[0].count = count; // monomorphic: the entry owns the total
  return p;
}

// One profile row at (method, pc) with an explicit entry list.
[[nodiscard]] passes::DispatchProfile
rowProfile(std::uint32_t method, std::uint32_t pc,
           passes::ProfileSiteKind kind, bool mega, std::uint32_t count,
           std::initializer_list<passes::ProfileEntry> entries) {
  passes::DispatchProfile p;
  p.sites.resize(method + 1);
  p.sites[method].resize(pc + 1);
  passes::ProfileSite& s = p.sites[method][pc];
  s.kind = kind;
  s.megamorphic = mega;
  s.count = count;
  std::size_t i = 0;
  for (const passes::ProfileEntry& e : entries) {
    if (i < 3) {
      s.entries[i] = e;
    }
    ++i;
  }
  return p;
}

// The live TypeProfile guards (kind filter over the Guard nodes).
[[nodiscard]] std::vector<ir::NodeId>
typeProfileGuards(const ir::Graph& g) {
  std::vector<ir::NodeId> out;
  for (ir::NodeId n : nodesOfKind(g, NodeKind::Guard)) {
    if (g.node(n).payload ==
        static_cast<std::uint32_t>(ir::GuardKind::TypeProfile)) {
      out.push_back(n);
    }
  }
  return out;
}

// The live NullCheck guards.
[[nodiscard]] std::vector<ir::NodeId> nullGuards(const ir::Graph& g) {
  std::vector<ir::NodeId> out;
  for (ir::NodeId n : nodesOfKind(g, NodeKind::Guard)) {
    if (g.node(n).payload ==
        static_cast<std::uint32_t>(ir::GuardKind::NullCheck)) {
      out.push_back(n);
    }
  }
  return out;
}

} // namespace

B2_TEST(il_guard_no_profile_virtual_untouched) {
  // The zero-regression property: with NO profile attached, a virtual
  // site is not even considered - no decision, no guard, the call stays,
  // and the run is v1-identical (the graph bytes pinned by determinism).
  const rbc::Program prog = mkVirtProgram();
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 0, g, r);
  CHECK(r.telemetry.sitesConsidered == 0); // the virtual site is invisible
  CHECK(r.telemetry.sitesInlined == 0);
  CHECK(r.telemetry.sitesGuardInlined == 0);
  CHECK(r.decisions.empty());
  CHECK(countKind(g, NodeKind::CallVirtual) == 1); // the call survives
  CHECK(typeProfileGuards(g).empty());
  const std::string baseline = ir::print(g);

  // An explicitly-null profile pointer is the same configuration.
  ir::Graph g2;
  passes::InlineResult r2;
  passes::InlineConfig cfg; // profile = nullptr (the default)
  inlineOk(prog, 0, g2, r2, cfg);
  CHECK(ir::print(g2) == baseline);
  CHECK(verifyOk(g));
}

B2_TEST(il_guard_mono_virtual_inlined) {
  // THE core test: a monomorphic site guard-inlines. The call dies, the
  // TypeProfile guard + the inlined body replace it, the decision
  // carries the profile numbers, and the IR verifier passes.
  const rbc::Program prog = mkVirtProgram();
  const passes::DispatchProfile prof = monoProfile(0, 2, 10, "Main", 1);
  passes::InlineConfig cfg;
  cfg.profile = &prof;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 0, g, r, cfg);
  CHECK(r.telemetry.sitesConsidered == 1);
  CHECK(r.telemetry.sitesInlined == 1);
  CHECK(r.telemetry.sitesGuardInlined == 1);
  CHECK(r.telemetry.guardsEmitted == 1);
  CHECK(countAction(r, IA::GuardInline) == 1);
  CHECK(r.decisions.size() == 1);
  if (!r.decisions.empty()) {
    const passes::InlineDecision& d = r.decisions[0];
    CHECK(d.action == IA::GuardInline);
    CHECK(d.target == 1);   // bump's table index
    CHECK(d.siteCount == 10);
    CHECK(d.recvCount == 10);
    CHECK(d.calleeNodes > 0);
  }
  CHECK(countKind(g, NodeKind::CallVirtual) == 0);  // killed
  CHECK(typeProfileGuards(g).size() == 1);          // the guard lives
  CHECK(chainedFsNodes(g).size() >= 1);             // the body chains
  CHECK(verifyOk(g));                               // Rule 40 (driver + here)
}

B2_TEST(il_guard_spec_meta_complete) {
  // Rule 122: the guard carries the COMPLETE speculative metadata; Rule
  // 42: a PGO source needs a valid dependency - here ClassHierarchy on
  // the profiled class. The Speculative flag rides the node.
  const rbc::Program prog = mkVirtProgram();
  const passes::DispatchProfile prof = monoProfile(0, 2, 10, "Main", 1);
  passes::InlineConfig cfg;
  cfg.profile = &prof;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 0, g, r, cfg);
  const std::vector<ir::NodeId> guards = typeProfileGuards(g);
  CHECK(guards.size() == 1);
  if (guards.empty()) {
    return;
  }
  const ir::Node& gn = g.node(guards[0]);
  CHECK(gn.flags.has(ir::NodeFlag::Speculative));
  CHECK(gn.specMeta != 0);
  if (gn.specMeta == 0) {
    return;
  }
  const ir::SpecMeta& sm = g.specMeta(gn.specMeta - 1);
  CHECK(sm.kind == ir::SpecMeta::Kind::TypeMonomorphic);
  CHECK(sm.source == ir::SpecMeta::Source::PGO);
  CHECK(sm.confidence == 10000); // 10/10
  CHECK(sm.guard == guards[0]);
  CHECK(sm.deoptTarget == gn.payload2);
  CHECK(sm.cost > 0);
  CHECK(sm.rollback == ir::SpecMeta::Rollback::None);
  CHECK(sm.dependency != ir::kInvalidDependency);
  CHECK(sm.dependency < g.dependencyCount());
  if (sm.dependency < g.dependencyCount()) {
    const ir::Dependency& dep = g.dependency(sm.dependency);
    CHECK(dep.kind == ir::Dependency::Kind::ClassHierarchy);
    // The dependency target is the profiled class's TypeId - the same
    // id the guard's InstanceOf payload carries.
    const ir::NodeId cond = g.input(guards[0], 1);
    CHECK(g.node(cond).kind == NodeKind::InstanceOf);
    CHECK(dep.target == g.node(cond).payload);
  }
  CHECK(g.specMetaCount() == 1);
  CHECK(g.dependencyCount() == 1);
}

B2_TEST(il_guard_shape_chains) {
  // The wiring: the TypeProfile guard chains AFTER the call's null
  // guard in the control flow; its FrameState is the call-site snapshot
  // (method + pc), so a guard deopt re-enters T0 at the call pc; the
  // InstanceOf reads the receiver def (the allocation).
  const rbc::Program prog = mkVirtProgram();
  const passes::DispatchProfile prof = monoProfile(0, 2, 10, "Main", 1);
  passes::InlineConfig cfg;
  cfg.profile = &prof;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 0, g, r, cfg);
  const std::vector<ir::NodeId> guards = typeProfileGuards(g);
  CHECK(guards.size() == 1);
  if (guards.empty()) {
    return;
  }
  // Control predecessor: the receiver null guard (builder output for
  // every receiver-op call site).
  const std::vector<ir::NodeId> nulls = nullGuards(g);
  CHECK(nulls.size() == 1); // exactly the call's receiver null guard
  if (!nulls.empty()) {
    CHECK(g.input(guards[0], 0) == nulls[0]);
  }
  // FrameState: descriptor (method = 0, pc = 2 - the invoke's pc).
  const ir::NodeId fs = g.input(guards[0], 2);
  CHECK(g.node(fs).kind == NodeKind::FrameState);
  const ir::FrameStateDesc& fd = g.frameState(g.node(fs).payload);
  CHECK(fd.method == 0);
  CHECK(fd.pc == 2);
  // Condition: InstanceOf(receiver, Main) - the payload through the
  // resolver's class space, the input the receiver def (the New).
  const ir::NodeId cond = g.input(guards[0], 1);
  CHECK(g.node(cond).kind == NodeKind::InstanceOf);
  const ir::NodeId recv = g.input(cond, 0);
  CHECK(g.node(recv).kind == NodeKind::New);
  passes::ProgramCalleeSource src(prog);
  CHECK(g.node(cond).payload == src.classId("Main"));
}

B2_TEST(il_guard_deopt_ids_unique) {
  // The guard's deopt id sits above every pre-existing id and the body's
  // ids continue above it: no collisions anywhere in the final graph.
  const rbc::Program prog = mkVirtProgram();
  const passes::DispatchProfile prof = monoProfile(0, 2, 10, "Main", 1);
  passes::InlineConfig cfg;
  cfg.profile = &prof;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 0, g, r, cfg);
  std::vector<std::uint32_t> ids;
  for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
    const ir::Node& nd = g.node(n);
    if (nd.isDead()) {
      continue;
    }
    if (nd.kind == NodeKind::Deopt) {
      ids.push_back(nd.payload);
    } else if (nd.kind == NodeKind::Guard) {
      ids.push_back(nd.payload2);
    }
  }
  std::sort(ids.begin(), ids.end());
  CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
  CHECK(ids.size() >= 2); // the type guard + the null guard at least
}

B2_TEST(il_guard_idempotent_deterministic) {
  // Idempotency: a second run (profile still attached) inlines nothing
  // and changes nothing. Determinism (Rule 124): rebuild + reinline +
  // re-print is byte-identical.
  const rbc::Program prog = mkVirtProgram();
  const passes::DispatchProfile prof = monoProfile(0, 2, 10, "Main", 1);
  passes::InlineConfig cfg;
  cfg.profile = &prof;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 0, g, r, cfg);
  const std::string after = ir::print(g);
  passes::ProgramCalleeSource src(prog);
  passes::InlineResult r2 = passes::runInlining(g, src, cfg);
  CHECK(r2.ok);
  CHECK(r2.telemetry.sitesInlined == 0);
  CHECK(ir::print(g) == after);
  ir::Graph g2;
  passes::InlineResult r2b;
  inlineOk(prog, 0, g2, r2b, cfg);
  CHECK(ir::print(g2) == after);
}

// --- the GuardInline refusal catalog (icdg.md 12: refuse first) ---------------------

namespace {

// Builds + verifies + inlines method 0 of the virtual program against
// `prof`, requiring ok; returns the decision for the (single) site.
[[nodiscard]] passes::InlineDecision
refuseWith(const passes::DispatchProfile& prof) {
  const rbc::Program prog = mkVirtProgram();
  passes::InlineConfig cfg;
  cfg.profile = &prof;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 0, g, r, cfg);
  CHECK(r.ok);
  CHECK(r.telemetry.sitesConsidered == 1);
  CHECK(r.telemetry.sitesInlined == 0);
  CHECK(r.telemetry.sitesGuardInlined == 0);
  CHECK(r.telemetry.guardsEmitted == 0);
  CHECK(countKind(g, NodeKind::CallVirtual) == 1); // the call survives
  CHECK(typeProfileGuards(g).empty());
  CHECK(r.decisions.size() == 1);
  passes::InlineDecision d;
  if (!r.decisions.empty()) {
    d = r.decisions[0];
    CHECK(d.action == IA::KeepIndirect);
  }
  CHECK(verifyOk(g));
  return d;
}

} // namespace

B2_TEST(il_guard_megamorphic_refused) {
  // The sticky flag: 4+ distinct classes froze the histogram - the site
  // stays indirect no matter which class a guard would name.
  passes::DispatchProfile p = rowProfile(
      0, 2, passes::ProfileSiteKind::Virtual, true, 4,
      {passes::ProfileEntry{"Main", 1, 1},
       passes::ProfileEntry{"java/lang/String", 1, 1},
       passes::ProfileEntry{"[I", 1, 1}});
  const passes::InlineDecision d = refuseWith(p);
  CHECK(std::string_view(d.reason) ==
        "megamorphic dispatch site (sticky frozen histogram)");
}

B2_TEST(il_guard_bimorphic_refused) {
  // Two receiver classes: the two-target guard is charter item 26 (the
  // documented follow-up); v2 refuses.
  passes::DispatchProfile p = rowProfile(
      0, 2, passes::ProfileSiteKind::Virtual, false, 10,
      {passes::ProfileEntry{"Main", 1, 8},
       passes::ProfileEntry{"java/lang/String", 1, 2}});
  const passes::InlineDecision d = refuseWith(p);
  CHECK(std::string_view(d.reason) ==
        "bimorphic dispatch site (two-target guard is the follow-up)");
}

B2_TEST(il_guard_polymorphic_refused) {
  // Three receiver classes: polymorphic, refused.
  passes::DispatchProfile p = rowProfile(
      0, 2, passes::ProfileSiteKind::Virtual, false, 9,
      {passes::ProfileEntry{"Main", 1, 3},
       passes::ProfileEntry{"java/lang/String", 1, 3},
       passes::ProfileEntry{"[I", 1, 3}});
  const passes::InlineDecision d = refuseWith(p);
  CHECK(std::string_view(d.reason) ==
        "polymorphic dispatch site (3 receiver classes)");
}

B2_TEST(il_guard_thin_profile_refused) {
  // Two dispatches are below the observation floor (Rule 44): the site
  // has not said enough yet.
  const passes::DispatchProfile p = monoProfile(0, 2, 2, "Main", 1);
  const passes::InlineDecision d = refuseWith(p);
  CHECK(std::string_view(d.reason) ==
        "site total below the observation floor (Rule 44)");
}

B2_TEST(il_guard_low_confidence_refused) {
  // 5/10 = 50%: below the 9000bp ratio threshold (Rule 44) - the site
  // mixes this class with other dispatch behavior (builtin executions
  // or other classes under saturation).
  const passes::DispatchProfile p = monoProfile(0, 2, 10, "Main", 1);
  passes::DispatchProfile prof = p;
  prof.sites[0][2].entries[0].count = 5; // half the site total
  const passes::InlineDecision d = refuseWith(prof);
  CHECK(std::string_view(d.reason) ==
        "profile confidence below the guard threshold (Rule 44)");
}

B2_TEST(il_guard_target_mismatch_refused) {
  // The profile names a DIFFERENT target than the call resolved to: the
  // snapshot does not describe this build; the only safe action is
  // KeepIndirect (never a mis-inline).
  const passes::DispatchProfile p = monoProfile(0, 2, 10, "Main", 0);
  const passes::InlineDecision d = refuseWith(p);
  CHECK(std::string_view(d.reason) ==
        "profile target disagrees with the call-site resolution");
}

B2_TEST(il_guard_object_class_refused) {
  // java/lang/Object: InstanceOf is a subtype check - for Object it
  // admits every non-array receiver (including receivers that dispatch
  // this site differently); the guard would not be exact.
  const passes::DispatchProfile p =
      monoProfile(0, 2, 10, "java/lang/Object", 1);
  const passes::InlineDecision d = refuseWith(p);
  CHECK(std::string_view(d.reason) ==
        "profiled receiver class is java/lang/Object (guard not exact)");
}

B2_TEST(il_guard_no_row_refused) {
  // The site never dispatched in the training run (empty snapshot):
  // no data, no decision to inline.
  const passes::DispatchProfile empty;
  const passes::InlineDecision d = refuseWith(empty);
  CHECK(std::string_view(d.reason) ==
        "no dispatch profile row for the site (not executed in T0)");
}

B2_TEST(il_guard_kind_mismatch_refused) {
  // The row's invoke family disagrees with the IR node (an Interface
  // row for a CallVirtual site): the snapshot is not this program's.
  const passes::DispatchProfile p = monoProfile(
      0, 2, 10, "Main", 1, passes::ProfileSiteKind::Interface);
  const passes::InlineDecision d = refuseWith(p);
  CHECK(std::string_view(d.reason) ==
        "profile row kind disagrees with the call site (mismatched "
        "profile)");
}

B2_TEST(il_guard_no_entry_refused) {
  // A count-only row (builtin executions record no receiver entry): the
  // site dispatches externally; there is no body to inline.
  passes::DispatchProfile p;
  p.sites.resize(1);
  p.sites[0].resize(3);
  p.sites[0][2].kind = passes::ProfileSiteKind::Virtual;
  p.sites[0][2].count = 7; // counted, but entries stay empty
  const passes::InlineDecision d = refuseWith(p);
  CHECK(std::string_view(d.reason) ==
        "profile has no receiver entry (builtin/count-only site)");
}

B2_TEST(il_guard_interface_site_inlined) {
  // CallInterface sites take the same path. The interface ref needs the
  // TEXT RBC form ("imethod" parses as an InterfaceMethodRef - the C++
  // RbcBuilder has no interface-ref factory; the graph builder demands
  // the kind for invokeinterface).
  const auto parsed = rbc::parseRbcText(R"RBC(.class Main
.method static main ()I
.regs 4
.const c0 = class "Main"
.const c1 = imethod Main bump (I)I
new r0 c0
iconst r1 5
invokeinterface r2 r0 r2 c1
ireturn r2
.end
.method bump (I)I
.regs 5
.locals 2
iload r0 l1
iconst r1 1
iadd r2 r0 r1
iconst r3 2
idiv r4 r2 r3
ireturn r4
.end
)RBC");
  CHECK(parsed.has_value());
  if (!parsed) {
    return;
  }

  const passes::DispatchProfile prof = monoProfile(
      0, 2, 10, "Main", 1, passes::ProfileSiteKind::Interface);
  passes::InlineConfig cfg;
  cfg.profile = &prof;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(*parsed, 0, g, r, cfg);
  CHECK(r.telemetry.sitesGuardInlined == 1);
  CHECK(countKind(g, NodeKind::CallInterface) == 0);
  CHECK(typeProfileGuards(g).size() == 1);
  CHECK(verifyOk(g));
}

B2_TEST(il_guard_kill_switch_with_profile) {
  // Rules 132/144: the kill switch is a successful no-op even with a
  // perfectly monomorphic profile attached - zero decisions, zero
  // telemetry, byte-identical graph.
  const rbc::Program prog = mkVirtProgram();
  const passes::DispatchProfile prof = monoProfile(0, 2, 10, "Main", 1);
  passes::InlineConfig cfg;
  cfg.profile = &prof;
  cfg.enabled = false;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 0, g, r, cfg);
  CHECK(r.decisions.empty());
  CHECK(r.telemetry.sitesConsidered == 0);
  CHECK(r.telemetry.guardsEmitted == 0);
  CHECK(countKind(g, NodeKind::CallVirtual) == 1);
  CHECK(typeProfileGuards(g).empty());
  // Byte-identical to a no-profile, enabled run of the same program.
  ir::Graph g2;
  passes::InlineResult r2;
  inlineOk(prog, 0, g2, r2);
  CHECK(ir::print(g) == ir::print(g2));
}

B2_TEST(il_guard_shared_v1_cost_caps) {
  // With a profile attached, virtual sites still flow through the SHARED
  // v1 cost model: an oversized callee refuses by the instruction cap,
  // not by anything profile-related. (The callee is named bump so the
  // call's MethodRef resolves to IT - external refs refuse unresolved
  // before the cost model ever runs.)
  rbc::RbcBuilder b("bump", "(I)I", 0);
  b.setRegs(3);
  b.setLocals(2);
  for (std::uint32_t i = 0; i < 40; ++i) { // > kMaxInlineCalleeInsns (35)
    b.emitRegImm(rbc::Op::Iconst, 0, 1);
  }
  b.emitRegSlot(rbc::Op::Iload, 0, 1);
  b.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method big;
  CHECK(b.finish(big).ok);
  rbc::Program prog;
  prog.className = "Main";
  rbc::Method ms[] = {mkVirtMain(), big};
  prog.methods.assign(ms, ms + 2);

  const passes::DispatchProfile prof = monoProfile(0, 2, 10, "Main", 1);
  passes::InlineConfig cfg;
  cfg.profile = &prof;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 0, g, r, cfg);
  CHECK(countAction(r, IA::KeepIndirect) == 1);
  CHECK(!r.decisions.empty());
  if (!r.decisions.empty()) {
    CHECK(r.decisions[0].action == IA::KeepIndirect);
    CHECK(std::string_view(r.decisions[0].reason) ==
          "callee too large (instruction cap)");
    CHECK(r.decisions[0].calleeInsns > 35);
  }
  CHECK(countKind(g, NodeKind::CallVirtual) == 1);
  CHECK(verifyOk(g));
}

// --- the end-to-end path: T0 training run -> snapshot -> guard-inline -----------------

B2_TEST(il_guard_end_to_end_t0_snapshot) {
  // The full Phase 2 loop on a real interpreter: main calls f three
  // times; f holds ONE straight-line virtual site; T0 dispatches it
  // three times (mono, Main); the snapshot carries that row; f's graph
  // guard-inlines bump behind a guard naming the class T0 actually saw.
  rbc::RbcBuilder fm("f", "()I", rbc::method_flags::Static);
  fm.setRegs(4);
  fm.setLocals(0);
  const std::uint32_t cls = fm.constClass("Main");
  const std::uint32_t bump = fm.constMethodRef("Main", "bump", "(I)I");
  fm.emitRegCp(rbc::Op::New, 0, cls);
  fm.emitRegImm(rbc::Op::Iconst, 1, 5);
  fm.emitCall(rbc::Op::Invokevirtual, 2, 0, 2, bump);
  fm.emitReg(rbc::Op::Ireturn, 2);
  rbc::Method f;
  CHECK(fm.finish(f).ok);

  rbc::RbcBuilder mm("main", "()I", rbc::method_flags::Static);
  mm.setRegs(2);
  mm.setLocals(0);
  const std::uint32_t fRef = mm.constMethodRef("Main", "f", "()I");
  mm.emitCall(rbc::Op::Invokestatic, 0, 0, 0, fRef);
  mm.emitCall(rbc::Op::Invokestatic, 0, 0, 0, fRef);
  mm.emitCall(rbc::Op::Invokestatic, 0, 0, 0, fRef);
  mm.emitReg(rbc::Op::Ireturn, 0);
  rbc::Method mainM;
  CHECK(mm.finish(mainM).ok);

  rbc::Program prog;
  prog.className = "Main";
  rbc::Method ms[] = {mainM, f, mkVirtBump()};
  prog.methods.assign(ms, ms + 3);

  // The T0 training run (b2run's entry inference shape; main()I).
  b2::interp::Interpreter interp(prog, b2::interp::InterpConfig{});
  const b2::interp::RunResult run =
      interp.run("main", "()I", std::vector<b2::interp::Value>{});
  CHECK(run.status == b2::interp::RunStatus::Returned);

  // The snapshot (the shared converter - the same code b2graph uses).
  passes::DispatchProfile prof;
  passes::snapshotDispatchProfile(interp, prof);
  // T0 dispatched f's site 3 times, all Main receivers, all to bump (2).
  CHECK(prof.sites.size() >= 2);
  if (prof.sites.size() >= 2) {
    const passes::ProfileSite& s = prof.sites[1][2];
    CHECK(s.kind == passes::ProfileSiteKind::Virtual);
    CHECK(!s.megamorphic);
    CHECK(s.count == 3);
    CHECK(s.entries[0].recvClass == "Main");
    CHECK(s.entries[0].target == 2);
    CHECK(s.entries[0].count == 3);
    CHECK(s.entries[1].count == 0);
  }

  // f's graph guard-inlines bump behind the guard.
  passes::InlineConfig cfg;
  cfg.profile = &prof;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 1, g, r, cfg);
  CHECK(r.telemetry.sitesGuardInlined == 1);
  CHECK(countAction(r, IA::GuardInline) == 1);
  if (!r.decisions.empty()) {
    CHECK(r.decisions[0].siteCount == 3);
    CHECK(r.decisions[0].recvCount == 3);
  }
  const std::vector<ir::NodeId> guards = typeProfileGuards(g);
  CHECK(guards.size() == 1);
  if (!guards.empty()) {
    // The guard names the class T0 actually observed.
    const ir::NodeId cond = g.input(guards[0], 1);
    passes::ProgramCalleeSource src(prog);
    CHECK(g.node(cond).payload == src.classId("Main"));
  }
  CHECK(countKind(g, NodeKind::CallVirtual) == 0);
  CHECK(verifyOk(g));

  // main's graph: the three invokestatic f sites direct-inline (f is
  // tiny), and each inlined copy's virtual site guard-inlines at depth
  // 2 against the SAME profile row (the site key is (f=1, pc=2)).
  ir::Graph gm;
  passes::InlineResult rm;
  inlineOk(prog, 0, gm, rm, cfg);
  CHECK(countAction(rm, IA::DirectInline) == 3);
  CHECK(countAction(rm, IA::GuardInline) == 3);
  CHECK(rm.telemetry.sitesGuardInlined == 3);
  CHECK(typeProfileGuards(gm).size() == 3);
  CHECK(countKind(gm, NodeKind::CallVirtual) == 0);
  CHECK(verifyOk(gm));
}

// --- the corpus sweep (the strongest gate) ------------------------------------------

B2_TEST(il_guard_call_in_loop_end_to_end) {
  // THE MSG-20260901-004 unblock: the canonical dispatch-profile LOOP
  // program (main { while (i < 3) sum += obj.bump(5); }). A call is a
  // memory producer, so this shape previously could not build a
  // VERIFIABLE graph at all - the header memory phi's backedge read as
  // a "memory chain cycle". Now the full Phase 2 loop runs on it: T0
  // trains inside the loop, the snapshot carries the (main, pc=6) row,
  // and the call INSIDE the loop guard-inlines with everything green.
  rbc::RbcBuilder mm("main", "()I", rbc::method_flags::Static);
  mm.setRegs(6);
  mm.setLocals(0);
  const std::uint32_t cls = mm.constClass("Main");
  const std::uint32_t bump = mm.constMethodRef("Main", "bump", "(I)I");
  mm.emitRegCp(rbc::Op::New, 0, cls);
  mm.emitRegImm(rbc::Op::Iconst, 1, 5);   // the bump argument
  mm.emitRegImm(rbc::Op::Iconst, 2, 0);   // i
  mm.emitRegImm(rbc::Op::Iconst, 3, 0);   // sum
  mm.emitRegImm(rbc::Op::Iconst, 4, 3);   // the bound
  const rbc::RbcBuilder::Label loop = mm.newLabel();
  const rbc::RbcBuilder::Label done = mm.newLabel();
  mm.bind(loop);
  mm.emitRegRegBranch(rbc::Op::IfIcmpge, 2, 4, done);
  mm.emitCall(rbc::Op::Invokevirtual, 5, 0, 2, bump); // pc=6, args (r0,r1)
  mm.emitRegRegReg(rbc::Op::Iadd, 3, 3, 5);
  mm.emitRegImm(rbc::Op::Iinc, 2, 1);
  mm.emitBranch(rbc::Op::Goto, loop);
  mm.bind(done);
  mm.emitReg(rbc::Op::Ireturn, 3);
  rbc::Method mainM;
  CHECK(mm.finish(mainM).ok);

  rbc::Program prog;
  prog.className = "Main";
  rbc::Method ms[] = {mainM, mkVirtBump()};
  prog.methods.assign(ms, ms + 2);

  // The T0 training run: 3 dispatches, each bump(5) = (5+1)/2 = 3, so
  // the program returns 9 (a real execution, not just a status check).
  b2::interp::Interpreter interp(prog, b2::interp::InterpConfig{});
  const b2::interp::RunResult run =
      interp.run("main", "()I", std::vector<b2::interp::Value>{});
  CHECK(run.status == b2::interp::RunStatus::Returned);
  CHECK(run.result.as.i == 9);

  // The snapshot: the site row is (main=0, pc=6), mono Main -> bump (1).
  passes::DispatchProfile prof;
  passes::snapshotDispatchProfile(interp, prof);
  CHECK(prof.sites.size() >= 1);
  if (prof.sites.size() >= 1 && prof.sites[0].size() > 6) {
    const passes::ProfileSite& s = prof.sites[0][6];
    CHECK(s.kind == passes::ProfileSiteKind::Virtual);
    CHECK(!s.megamorphic);
    CHECK(s.count == 3);
    CHECK(s.entries[0].recvClass == "Main");
    CHECK(s.entries[0].target == 1);
    CHECK(s.entries[0].count == 3);
    CHECK(s.entries[1].count == 0);
  }

  // The graph: the call INSIDE the loop guard-inlines; the loop shape
  // (LoopBegin + the header memory phi's backedge through the inlined
  // body's producers) verifies end to end.
  passes::InlineConfig cfg;
  cfg.profile = &prof;
  ir::Graph g;
  passes::InlineResult r;
  inlineOk(prog, 0, g, r, cfg);
  CHECK(r.telemetry.sitesConsidered == 1);
  CHECK(r.telemetry.sitesGuardInlined == 1);
  CHECK(countAction(r, IA::GuardInline) == 1);
  if (!r.decisions.empty()) {
    CHECK(r.decisions[0].siteCount == 3);
    CHECK(r.decisions[0].recvCount == 3);
  }
  CHECK(countKind(g, NodeKind::CallVirtual) == 0);
  CHECK(typeProfileGuards(g).size() == 1);
  CHECK(countKind(g, NodeKind::LoopBegin) == 1);
  CHECK(chainedFsNodes(g).size() >= 1);
  CHECK(verifyOk(g));
}

B2_TEST(il_corpus_sweep_all_programs_verified_deterministic) {
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
      passes::InlineResult r;
      inlineOk(*parsed, i, g, r);
      CHECK_MSG(verifyOk(g), "post-inline verify failed: " + name +
                                 "/" + m.name);
      const std::string afterInline = ir::print(g); // pre-pipeline baseline
      // The full T2 shape: inline THEN the cleanup pipeline.
      const passes::PassResult pr = passes::runEarlyCleanup(g);
      CHECK_MSG(pr.ok, "pipeline failed: " + name + "/" + m.name);
      CHECK_MSG(verifyOk(g), "post-pipeline verify failed: " + name +
                                 "/" + m.name);
      const std::string afterPipe = ir::print(g);

      // Idempotency: a second inline run over the post-pipeline graph
      // inlines nothing new and changes nothing.
      passes::ProgramCalleeSource src(*parsed);
      passes::InlineResult r2 = passes::runInlining(g, src);
      CHECK_MSG(r2.ok, "second inline failed: " + name + "/" + m.name);
      CHECK_MSG(r2.telemetry.sitesInlined == 0,
                "second inline inlined: " + name + "/" + m.name);
      CHECK_MSG(ir::print(g) == afterPipe,
                "second inline changed the graph: " + name + "/" + m.name);

      // Determinism (Rule 124): rebuild + reinline + repipeline is
      // byte-identical.
      ir::Graph g2;
      passes::InlineResult r2b;
      inlineOk(*parsed, i, g2, r2b);
      const passes::PassResult pr2 = passes::runEarlyCleanup(g2);
      CHECK_MSG(pr2.ok, "determinism pipeline failed: " + name + "/" + m.name);
      CHECK_MSG(ir::print(g) == ir::print(g2),
                "non-deterministic inline+pipeline: " + name + "/" +
                    m.name);

      // The PGO variant: an EMPTY dispatch profile attached. Every
      // virtual/interface site is now CONSIDERED but refuses (no row);
      // the graphs must stay byte-identical to the no-profile run
      // (refusals change the decision log, never the graph), and the
      // run must stay deterministic with the profile attached.
      passes::DispatchProfile empty;
      passes::InlineConfig pcfg;
      pcfg.profile = &empty;
      ir::Graph g3;
      passes::InlineResult r3;
      inlineOk(*parsed, i, g3, r3, pcfg);
      CHECK_MSG(verifyOk(g3), "post-inline verify (pgo) failed: " + name +
                                  "/" + m.name);
      CHECK_MSG(ir::print(g3) == afterInline,
                "profile-attached refusals changed the graph: " + name +
                    "/" + m.name);
      ir::Graph g4;
      passes::InlineResult r4;
      inlineOk(*parsed, i, g4, r4, pcfg);
      CHECK_MSG(ir::print(g4) == ir::print(g3),
                "non-deterministic with profile: " + name + "/" + m.name);
    }
  }
  CHECK(methods >= 24); // the corpus grew past 24 methods by T2-IR2
}
