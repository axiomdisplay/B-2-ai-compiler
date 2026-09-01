// B-2 passes tests - the CM-PEA engine (keys 65/66/67/69).
//
// Discipline: Rule 35 - at least ten tests per delivered pass key; the
// surfaces here are the escape classification (65), the escape-point
// materialization (66/69), the scalar replacement (67), and the shared
// engine contracts (kill switches, budgets, idempotency, determinism,
// the corpus gate). Every test runs the TOTAL IR verifier after the
// transformation (Rule 40) and pins:
//
//   - the classification table: every use shape lands in the documented
//     lattice grade, and every refusal reason is the catalog key;
//   - the materialization shape: Materialize [ctrl, mem, vobj] wired
//     into BOTH chains, the escape use rewired after it, the FrameState
//     locals edges auto-rewired onto the real reference (Rule 14), the
//     post-escape accesses kept as real-object semantics;
//   - the scalar replacement: load forwarding (nearest same-field store
//     on the mem chain), typed defaults for never-written fields, the
//     virtual stores spliced out of both chains, the ctrl-chain
//     downstream rewired onto the allocation's predecessor, IsNull
//     folded to 0, the allocation killed;
//   - the PEA Rule options 1-4: proven non-observable (scalarized),
//     materialized, or rejected - never half-applied.

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
#include <b2/passes/Inline.h>
#include <b2/passes/Passes.h>
#include <b2/rbc/RbcText.h>
#include <b2/rbc/Verifier.h>

using namespace b2;
using ir::NodeKind;
using ES = passes::EscapeState;

namespace {

using PK = passes::PassKey;

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

[[nodiscard]] bool verifyOk(const ir::Graph& g) {
  return !ir::verify(g).hasErrors();
}

[[nodiscard]] const passes::PeaDecision*
findDecision(const passes::PeaResult& r, ir::NodeId alloc) {
  for (const passes::PeaDecision& d : r.decisions) {
    if (d.alloc == alloc) {
      return &d;
    }
  }
  return nullptr;
}

// The canonical hand-built straight-line object lifetime:
//   New T5 ; StoreField(o, f3, c42) ; LoadField(o, f3) ; Return(load)
// ctrl chain Start -> New -> store -> load -> Return; mem chain
// Start -> store -> (load passes it through).
struct StraightObject {
  ir::Graph g;
  ir::NodeId start = ir::kInvalidNodeId;
  ir::NodeId c42 = ir::kInvalidNodeId;
  ir::NodeId alloc = ir::kInvalidNodeId;
  ir::NodeId store = ir::kInvalidNodeId;
  ir::NodeId load = ir::kInvalidNodeId;
  ir::NodeId ret = ir::kInvalidNodeId;

  StraightObject() {
    start = g.startNode();
    c42 = g.constantI(42);
    alloc = g.make(NodeKind::New, {start}, ir::TypeId{5});
    store = g.make(NodeKind::StoreField, {alloc, start, alloc, c42},
                   ir::FieldId{3});
    load = g.make(NodeKind::LoadField, {store, store, alloc}, ir::FieldId{3},
                  static_cast<std::uint32_t>(ir::IRType::Int));
    ret = g.make(NodeKind::Return, {load, load});
  }
};

} // namespace

// --- key 65: the escape classification -----------------------------------------

B2_TEST(pea_analysis_only_mode_rewrites_nothing) {
  StraightObject so;
  CHECK(verifyOk(so.g));
  const auto before = ir::print(so.g);
  passes::PassConfig cfg;
  const passes::PassResult r = passes::runSinglePass(
      so.g, PK::EscapeAnalysis, cfg);
  CHECK(r.ok);
  CHECK(r.telemetry.rewrites == 0);
  CHECK(r.telemetry.removals == 0);
  CHECK(r.telemetry.peaScalarized == 0);
  CHECK(r.telemetry.peaMaterialized == 0);
  CHECK(r.telemetry.peaRejected == 0); // analysis only: no disposition
  CHECK(ir::print(so.g) == before);    // byte-identical graph
  CHECK(verifyOk(so.g));
}

B2_TEST(pea_noescape_classification) {
  StraightObject so;
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(so.g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(d->state == ES::NoEscape);
  CHECK(std::strcmp(d->action, "scalarized") == 0);
  CHECK(d->fields == 1);
  CHECK(verifyOk(so.g));
}

B2_TEST(pea_call_arg_is_arg_escape) {
  StraightObject so;
  // fs with the alloc in locals; call taking it as an argument (the
  // fields.rbc shape).
  ir::Graph& g = so.g;
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 3,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc, so.alloc});
  const ir::NodeId call = g.make(NodeKind::CallStatic,
                                 {so.load, so.store, so.alloc, fs},
                                 ir::MethodId{1},
                                 static_cast<std::uint32_t>(ir::IRType::Bottom));
  so.ret = g.make(NodeKind::Return, {call});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(d->state == ES::ArgEscape);
  CHECK(std::strcmp(d->action, "materialized") == 0);
  CHECK(std::strcmp(d->reason, "call-arg") == 0);
  CHECK(verifyOk(g));
}

B2_TEST(pea_return_value_is_return_escape) {
  StraightObject so;
  // Return the object itself (value slot) instead of the loaded field.
  ir::Graph& g = so.g;
  g.setInput(so.ret, 1, so.alloc);
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(d->state == ES::ReturnEscape);
  CHECK(std::strcmp(d->action, "materialized") == 0);
  CHECK(std::strcmp(d->reason, "return-escape") == 0);
  CHECK(verifyOk(g));
}

B2_TEST(pea_foreign_store_is_field_escape) {
  StraightObject so;
  // Store the allocation into ANOTHER object's field.
  ir::Graph& g = so.g;
  const ir::NodeId other = g.make(NodeKind::New, {g.startNode()},
                                  ir::TypeId{7});
  const ir::NodeId fstore = g.make(NodeKind::StoreField,
                                   {so.load, so.store, other, so.alloc},
                                   ir::FieldId{9});
  so.ret = g.make(NodeKind::Return, {fstore});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(d->state == ES::FieldEscape);
  CHECK(std::strcmp(d->action, "materialized") == 0);
  CHECK(std::strcmp(d->reason, "foreign-store") == 0);
  // The other allocation escapes nowhere on its own: no uses at all
  // except the (now real) store object slot.
  CHECK(verifyOk(g));
}

B2_TEST(pea_refeq_identity_rejected) {
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId eq = g.make(NodeKind::RefEq, {so.alloc, so.alloc});
  const ir::NodeId c0 = g.constantI(0);
  const ir::NodeId ne = g.make(NodeKind::NeI, {eq, c0});
  const ir::NodeId iff = g.make(NodeKind::If, {so.load, ne});
  const ir::NodeId t = g.make(NodeKind::IfTrue, {iff});
  const ir::NodeId f = g.make(NodeKind::IfFalse, {iff});
  const ir::NodeId reg = g.make(NodeKind::Region, {t, f});
  so.ret = g.make(NodeKind::Return, {reg, so.load});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(d->state == ES::GlobalEscape);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "identity-observable") == 0);
  // Rejected means untouched: the New is still there.
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(pea_monitor_identity_rejected) {
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId mon = g.make(NodeKind::MonitorEnter,
                                {so.load, so.store, so.alloc});
  const ir::NodeId monx = g.make(NodeKind::MonitorExit,
                                 {mon, mon, so.alloc});
  so.ret = g.make(NodeKind::Return, {monx, so.load});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "identity-observable") == 0);
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(pea_unwind_exception_rejected) {
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId unw = g.make(NodeKind::Unwind, {so.load, so.alloc});
  CHECK(unw != ir::kInvalidNodeId);
  so.ret = g.make(NodeKind::Return, {so.load});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "exception-observable") == 0);
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(pea_phi_merge_rejected) {
  StraightObject so;
  ir::Graph& g = so.g;
  // phi(o, null) - the allocation flows into a merge.
  const ir::NodeId cnull = g.constantNull();
  const ir::NodeId iff = g.make(NodeKind::If, {so.load, g.constantI(0)});
  const ir::NodeId t = g.make(NodeKind::IfTrue, {iff});
  const ir::NodeId f = g.make(NodeKind::IfFalse, {iff});
  const ir::NodeId reg = g.make(NodeKind::Region, {t, f});
  const ir::NodeId phi = g.make(NodeKind::Phi, {reg, so.alloc, cnull});
  so.ret = g.make(NodeKind::Return, {reg, phi});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "phi-merge") == 0);
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(pea_fs_only_reference_scalarizes_with_listing) {
  StraightObject so;
  ir::Graph& g = so.g;
  // The allocation's ONLY escape-relevant use is a live FrameState
  // consumed by a deopt point (an unconditional Deopt). The fs-escape
  // listing (MSG-20260901-006): the allocation scalarizes, the fs's
  // slot edges rebase onto a per-instant vobj, and the desc lists it.
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 3,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc, so.alloc});
  const ir::NodeId dpt = g.make(NodeKind::Deopt, {so.load, fs}, 7u);
  so.ret = dpt; // a terminal: nothing may chain after a Deopt
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "scalarized") == 0);
  CHECK(std::strcmp(d->reason, "fs-escape") == 0);
  CHECK(pr.telemetry.peaScalarized == 1);
  CHECK(countKind(g, NodeKind::New) == 0);
  // One vobj, listed on the fs, carrying the store-visible field state
  // (the deopt's instant is the load's mem chain: the store's 42).
  const auto vobjs = nodesOfKind(g, NodeKind::VirtualObjectState);
  CHECK(vobjs.size() == 1);
  const ir::NodeId vo = vobjs[0];
  CHECK(g.numInputs(vo) == 1);
  CHECK(g.input(vo, 0) == so.c42);
  // BOTH slot edges referencing the allocation rebased onto the vobj.
  CHECK(g.input(fs, 0) == vo);
  CHECK(g.input(fs, 1) == vo);
  // The desc lists the vobj (the deopt-materialization channel).
  const auto list = g.frameStateVobjs(g.node(fs).payload);
  CHECK(list.size() == 1);
  CHECK(list[0] == vo);
  // The reader still forwarded (the load is gone); the deopt survives.
  CHECK(countKind(g, NodeKind::LoadField) == 0);
  CHECK(countKind(g, NodeKind::StoreField) == 0);
  CHECK(countKind(g, NodeKind::Deopt) == 1);
  CHECK(g.input(dpt, 1) == fs);
  CHECK(verifyOk(g));
}

B2_TEST(pea_fs_escape_per_instant_values) {
  StraightObject so;
  ir::Graph& g = so.g;
  // Two LIVE snapshots at different program points with a store
  // between them: each is its own deopt point (its own call), so each
  // gets its OWN vobj carrying the field state visible at that
  // instant - fs1 sees 42, fs2 sees 9.
  const ir::NodeId fs1 = g.makeFrameState(ir::MethodId{0}, 3,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc});
  const ir::NodeId c0 = g.constantI(0);
  const ir::NodeId call1 = g.make(NodeKind::CallStatic,
                                  {so.load, so.store, c0, fs1},
                                  ir::MethodId{1},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId c9 = g.constantI(9);
  const ir::NodeId store2 = g.make(NodeKind::StoreField,
                                   {call1, call1, so.alloc, c9},
                                   ir::FieldId{3});
  const ir::NodeId load2 = g.make(NodeKind::LoadField,
                                  {store2, store2, so.alloc}, ir::FieldId{3},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId fs2 = g.makeFrameState(ir::MethodId{0}, 8,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc});
  const ir::NodeId call2 = g.make(NodeKind::CallStatic,
                                  {load2, store2, c0, fs2},
                                  ir::MethodId{1},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  so.ret = g.make(NodeKind::Return, {call2, load2});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "scalarized") == 0);
  CHECK(std::strcmp(d->reason, "fs-escape") == 0);
  CHECK(countKind(g, NodeKind::New) == 0);
  const auto vobjs = nodesOfKind(g, NodeKind::VirtualObjectState);
  CHECK(vobjs.size() == 2);
  // fs1's instant (call1's mem = the first store) sees 42; fs2's
  // (call2's mem = store2) sees 9.
  CHECK(g.input(fs1, 0) == vobjs[0]);
  CHECK(g.input(vobjs[0], 0) == so.c42);
  CHECK(g.input(fs2, 0) == vobjs[1]);
  CHECK(g.input(vobjs[1], 0) == c9);
  // Each desc lists its own vobj.
  CHECK(g.frameStateVobjs(g.node(fs1).payload).size() == 1);
  CHECK(g.frameStateVobjs(g.node(fs1).payload)[0] == vobjs[0]);
  CHECK(g.frameStateVobjs(g.node(fs2).payload)[0] == vobjs[1]);
  // The readers forwarded: load -> 42, load2 -> 9; the spliced chain
  // collapses (call2's ctrl rebases through the removed store2/load2
  // onto call1).
  CHECK(g.input(call2, 0) == call1);
  CHECK(g.input(so.ret, 1) == c9);
  CHECK(g.input(so.ret, 0) == call2);
  CHECK(verifyOk(g));
}

B2_TEST(pea_fs_escape_instant_disagreement_rejected) {
  StraightObject so;
  ir::Graph& g = so.g;
  // ONE snapshot consumed by both a guard (before the second store)
  // and a call (after it): the two deopt points observe different
  // field states, and one vobj cannot carry two states - refuse.
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 3,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc});
  const ir::NodeId cond = g.make(NodeKind::IsNull, {so.alloc});
  const ir::NodeId guard = g.make(
      NodeKind::Guard, {so.load, cond, fs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 9u);
  const ir::NodeId c0 = g.constantI(0);
  const ir::NodeId c9 = g.constantI(9);
  const ir::NodeId store2 = g.make(NodeKind::StoreField,
                                   {guard, so.store, so.alloc, c9},
                                   ir::FieldId{3});
  const ir::NodeId call = g.make(NodeKind::CallStatic,
                                 {store2, store2, c0, fs},
                                 ir::MethodId{1},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  so.ret = g.make(NodeKind::Return, {call, c0});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "fs-multi-deopt") == 0);
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(nodesOfKind(g, NodeKind::VirtualObjectState).empty());
  CHECK(verifyOk(g));
}

B2_TEST(pea_fs_escape_chained_identity_shares_vobj) {
  StraightObject so;
  ir::Graph& g = so.g;
  // The inlined shape: a LIVE callee snapshot (guard-consumed) chained
  // to a userless caller snapshot, BOTH referencing the allocation.
  // The frames reconstruct together at one instant - the same object
  // in two frames - so they share ONE vobj (identity preserved).
  const ir::NodeId callerFs = g.makeFrameState(
      ir::MethodId{0}, 3, std::initializer_list<ir::NodeId>{so.alloc});
  const ir::NodeId calleeFs = g.makeFrameState(
      ir::MethodId{1}, 1, std::initializer_list<ir::NodeId>{so.alloc},
      g.node(callerFs).payload);
  const ir::NodeId cond = g.make(NodeKind::IsNull, {so.alloc});
  const ir::NodeId guard = g.make(
      NodeKind::Guard, {so.load, cond, calleeFs},
      static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 9u);
  so.ret = g.make(NodeKind::Return, {guard, so.c42});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "scalarized") == 0);
  CHECK(std::strcmp(d->reason, "fs-escape") == 0);
  CHECK(countKind(g, NodeKind::New) == 0);
  // ONE vobj for both frames; the state is the guard's instant (the
  // store's 42).
  const auto vobjs = nodesOfKind(g, NodeKind::VirtualObjectState);
  CHECK(vobjs.size() == 1);
  const ir::NodeId vo = vobjs[0];
  CHECK(g.numInputs(vo) == 1);
  CHECK(g.input(vo, 0) == so.c42);
  CHECK(g.input(calleeFs, 0) == vo);
  CHECK(g.input(callerFs, 0) == vo);
  CHECK(g.frameStateVobjs(g.node(calleeFs).payload).size() == 1);
  CHECK(g.frameStateVobjs(g.node(callerFs).payload).size() == 1);
  CHECK(g.frameStateVobjs(g.node(calleeFs).payload)[0] == vo);
  CHECK(g.frameStateVobjs(g.node(callerFs).payload)[0] == vo);
  CHECK(verifyOk(g));
}

B2_TEST(pea_fs_escape_array_snapshot) {
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId len = g.constantI(3);
  const ir::NodeId alloc = g.make(NodeKind::NewArray, {start, len},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId c7 = g.constantI(7);
  const ir::NodeId i1 = g.constantI(1);
  const ir::NodeId store = g.make(NodeKind::StoreElem,
                                  {alloc, start, alloc, i1, c7},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId load = g.make(NodeKind::LoadElem,
                                 {store, store, alloc, i1},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 4,
                                         std::initializer_list<ir::NodeId>{
                                             alloc});
  const ir::NodeId call = g.make(NodeKind::CallStatic,
                                 {load, store, load, fs}, ir::MethodId{1},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId ret = g.make(NodeKind::Return, {call, load});
  (void)ret;
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "scalarized") == 0);
  CHECK(std::strcmp(d->reason, "fs-escape") == 0);
  CHECK(countKind(g, NodeKind::NewArray) == 0);
  // The array vobj: [length, elem0(default), elem1(7)]. The default
  // is the plan's cached zero constant (constants are not interned -
  // compare structurally).
  const auto vobjs = nodesOfKind(g, NodeKind::VirtualObjectState);
  CHECK(vobjs.size() == 1);
  const ir::NodeId vo = vobjs[0];
  CHECK(g.numInputs(vo) == 3);
  CHECK(g.input(vo, 0) == len);
  CHECK(g.node(g.input(vo, 1)).kind == NodeKind::ConstantI);
  CHECK(g.node(g.input(vo, 1)).constValue == 0);
  CHECK(g.input(vo, 2) == c7);
  CHECK(g.input(fs, 0) == vo);
  CHECK(g.frameStateVobjs(g.node(fs).payload).size() == 1);
  CHECK(verifyOk(g));
}

B2_TEST(pea_fs_escape_unknown_consumer_rejected) {
  StraightObject so;
  ir::Graph& g = so.g;
  // A snapshot consumed by a Phi (a legal Data input, but not a deopt
  // point): the fs is not reconstructible there - refuse.
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 3,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc});
  const ir::NodeId iff = g.make(NodeKind::If, {so.load, g.constantI(0)});
  const ir::NodeId t = g.make(NodeKind::IfTrue, {iff});
  const ir::NodeId f = g.make(NodeKind::IfFalse, {iff});
  const ir::NodeId reg = g.make(NodeKind::Region, {t, f});
  const ir::NodeId phi = g.make(NodeKind::Phi, {reg, fs, so.c42});
  so.ret = g.make(NodeKind::Return, {reg, phi});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "fs-unknown-consumer") == 0);
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(pea_fs_escape_kill_switch_refuses) {
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 3,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc});
  const ir::NodeId dpt = g.make(NodeKind::Deopt, {so.load, fs}, 7u);
  so.ret = dpt;
  CHECK(verifyOk(g));
  const auto before = ir::print(g);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::ScalarReplacement, false);
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g, cfg);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "scalarize-disabled") == 0);
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(ir::print(g) == before);
  CHECK(verifyOk(g));
}

B2_TEST(pea_fs_escape_analysis_only_decides) {
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 3,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc});
  const ir::NodeId dpt = g.make(NodeKind::Deopt, {so.load, fs}, 7u);
  so.ret = dpt;
  CHECK(verifyOk(g));
  const auto before = ir::print(g);
  const passes::PassResult r = passes::runSinglePass(g, PK::EscapeAnalysis);
  CHECK(r.ok);
  CHECK(r.telemetry.rewrites == 0);
  CHECK(r.telemetry.removals == 0);
  CHECK(r.telemetry.peaScalarized == 0);
  CHECK(ir::print(g) == before);
  CHECK(verifyOk(g));
}

B2_TEST(pea_fs_escape_idempotent_and_deterministic) {
  // Build the same graph twice; both runs scalarize with the SAME
  // listing (byte-identical dumps); a second engine run on the
  // rewritten graph performs zero rewrites.
  const auto build = [](StraightObject& so) {
    ir::Graph& g = so.g;
    const ir::NodeId fs = g.makeFrameState(
        ir::MethodId{0}, 3, std::initializer_list<ir::NodeId>{so.alloc});
    const ir::NodeId dpt = g.make(NodeKind::Deopt, {so.load, fs}, 7u);
    so.ret = dpt;
  };
  StraightObject a;
  build(a);
  CHECK(verifyOk(a.g));
  const passes::PeaResult pr1 = passes::runPartialEscapeAnalysis(a.g);
  CHECK(pr1.ok);
  CHECK(pr1.telemetry.peaScalarized == 1);
  const std::string dump1 = ir::print(a.g);
  StraightObject b;
  build(b);
  const passes::PeaResult pr2 = passes::runPartialEscapeAnalysis(b.g);
  CHECK(pr2.ok);
  CHECK(pr2.telemetry.peaScalarized == 1);
  CHECK(ir::print(b.g) == dump1); // determinism (Rule 124)
  const passes::PeaResult pr3 = passes::runPartialEscapeAnalysis(a.g);
  CHECK(pr3.ok);
  CHECK(pr3.telemetry.peaScalarized == 0); // idempotent: no candidates
  CHECK(pr3.telemetry.rewrites == 0);
  CHECK(ir::print(a.g) == dump1);
  CHECK(verifyOk(a.g));
  CHECK(verifyOk(b.g));
}

B2_TEST(pea_unknown_use_rejected) {
  StraightObject so;
  ir::Graph& g = so.g;
  // BoxPrimitive consumes a Data ref: not in the table -> conservative.
  const ir::NodeId box = g.make(NodeKind::BoxPrimitive, {so.alloc},
                                static_cast<std::uint32_t>(ir::IRType::Int),
                                static_cast<std::uint32_t>(ir::IRType::Int));
  so.ret = g.make(NodeKind::Return, {so.load, box});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "unknown-use") == 0);
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(verifyOk(g));
}

// --- key 67: scalar replacement -------------------------------------------------

B2_TEST(pea_scalar_load_forwards_store_value) {
  StraightObject so;
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(so.g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaScalarized == 1);
  // The load and the store are gone; the allocation is gone; the return
  // now reads the constant directly with control from Start.
  CHECK(countKind(so.g, NodeKind::New) == 0);
  CHECK(countKind(so.g, NodeKind::StoreField) == 0);
  CHECK(countKind(so.g, NodeKind::LoadField) == 0);
  const ir::Node& ret = so.g.node(so.ret);
  CHECK(ret.numInputs == 2);
  CHECK(so.g.input(so.ret, 1) == so.c42);        // value = the store value
  CHECK(so.g.input(so.ret, 0) == so.g.startNode()); // chain spliced to Start
  CHECK(verifyOk(so.g));
}

B2_TEST(pea_scalar_latest_store_wins) {
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId c7 = g.constantI(7);
  const ir::NodeId store2 = g.make(NodeKind::StoreField,
                                   {so.load, so.store, so.alloc, c7},
                                   ir::FieldId{3});
  const ir::NodeId load2 = g.make(NodeKind::LoadField,
                                  {store2, store2, so.alloc}, ir::FieldId{3},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  so.ret = g.make(NodeKind::Return, {load2, load2});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaScalarized == 1);
  // The second (nearest) store's value is what the load forwarded.
  CHECK(g.input(so.ret, 1) == c7);
  CHECK(countKind(g, NodeKind::StoreField) == 0);
  CHECK(countKind(g, NodeKind::LoadField) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(pea_scalar_default_field_value) {
  StraightObject so;
  ir::Graph& g = so.g;
  // Read a field that was never written (before the store's field).
  const ir::NodeId load2 = g.make(NodeKind::LoadField,
                                  {so.load, so.store, so.alloc},
                                  ir::FieldId{8},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  so.ret = g.make(NodeKind::Return, {load2, load2});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaScalarized == 1);
  // The never-written field reads its type default: the 0 constant.
  const ir::Node& v = g.node(g.input(so.ret, 1));
  CHECK(v.kind == NodeKind::ConstantI);
  CHECK(v.constValue == 0);
  CHECK(verifyOk(g));
}

B2_TEST(pea_scalar_isnull_folds_to_zero) {
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId isn = g.make(NodeKind::IsNull, {so.alloc});
  so.ret = g.make(NodeKind::Return, {so.load, isn});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaScalarized == 1);
  const ir::Node& v = g.node(g.input(so.ret, 1));
  CHECK(v.kind == NodeKind::ConstantI);
  CHECK(v.constValue == 0);
  CHECK(verifyOk(g));
}

B2_TEST(pea_scalar_chain_link_spliced) {
  // The builder chains downstream control THROUGH the allocation; the
  // scalar replacement must rewire those users onto the predecessor.
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId cinit = g.make(NodeKind::ClassInit, {so.load, so.store},
                                  ir::TypeId{9});
  so.ret = g.make(NodeKind::Return, {cinit, so.load});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaScalarized == 1);
  CHECK(countKind(g, NodeKind::New) == 0);
  // The ClassInit control predecessor is now the load's ctrl
  // predecessor chain - not the killed allocation, not a dead node.
  const ir::Node& ci = g.node(cinit);
  CHECK(ci.numInputs >= 1);
  CHECK(!g.node(g.input(cinit, 0)).isDead());
  CHECK(verifyOk(g));
}

B2_TEST(pea_scalar_array_length_forwards) {
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId len = g.constantI(4);
  const ir::NodeId alloc = g.make(NodeKind::NewArray, {start, len},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId alen = g.make(NodeKind::ArrayLength, {alloc});
  const ir::NodeId ret = g.make(NodeKind::Return, {start, alen});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaScalarized == 1);
  CHECK(g.input(ret, 1) == len);
  CHECK(countKind(g, NodeKind::NewArray) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(pea_scalar_array_const_index_elements) {
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId len = g.constantI(4);
  const ir::NodeId c1 = g.constantI(111);
  const ir::NodeId c3 = g.constantI(0); // element index
  const ir::NodeId alloc = g.make(NodeKind::NewArray, {start, len},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId s0 = g.make(NodeKind::StoreElem, {alloc, start, alloc,
                                                     c3, c1},
                               static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId c3b = g.constantI(0);
  const ir::NodeId l0 = g.make(NodeKind::LoadElem, {s0, s0, alloc, c3b},
                               static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId ret = g.make(NodeKind::Return, {l0, l0});
  (void)ret;
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaScalarized == 1);
  CHECK(g.input(ret, 1) == c1); // element 0 forwarded to its store value
  CHECK(countKind(g, NodeKind::NewArray) == 0);
  CHECK(countKind(g, NodeKind::StoreElem) == 0);
  CHECK(countKind(g, NodeKind::LoadElem) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(pea_scalar_array_unwritten_element_defaults) {
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId len = g.constantI(4);
  const ir::NodeId c1 = g.constantI(111);
  const ir::NodeId c0 = g.constantI(0);
  const ir::NodeId c2idx = g.constantI(2);
  const ir::NodeId alloc = g.make(NodeKind::NewArray, {start, len},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId s0 = g.make(NodeKind::StoreElem, {alloc, start, alloc,
                                                     c0, c1},
                               static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId l2 = g.make(NodeKind::LoadElem, {s0, s0, alloc, c2idx},
                               static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId ret = g.make(NodeKind::Return, {l2, l2});
  (void)ret;
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaScalarized == 1);
  const ir::Node& v = g.node(g.input(ret, 1));
  CHECK(v.kind == NodeKind::ConstantI); // slot 2 never written: 0 default
  CHECK(v.constValue == 0);
  CHECK(verifyOk(g));
}

B2_TEST(pea_scalar_branch_local_lifetime) {
  // The whole virtual lifetime inside ONE branch: the projection sets
  // match, so the allocation still scalarizes.
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId c0 = g.constantI(0);
  const ir::NodeId cond = g.make(NodeKind::NeI, {c0, c0});
  const ir::NodeId iff = g.make(NodeKind::If, {start, cond});
  const ir::NodeId t = g.make(NodeKind::IfTrue, {iff});
  const ir::NodeId f = g.make(NodeKind::IfFalse, {iff});
  const ir::NodeId alloc = g.make(NodeKind::New, {t}, ir::TypeId{5});
  const ir::NodeId c42 = g.constantI(42);
  const ir::NodeId store = g.make(NodeKind::StoreField,
                                  {alloc, start, alloc, c42}, ir::FieldId{3});
  const ir::NodeId load = g.make(NodeKind::LoadField, {store, store, alloc},
                                 ir::FieldId{3},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId reg = g.make(NodeKind::Region, {load, f});
  const ir::NodeId vphi = g.make(NodeKind::Phi, {reg, load, c0});
  const ir::NodeId ret = g.make(NodeKind::Return, {reg, vphi});
  (void)ret; // anchors the terminal in the graph
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaScalarized == 1);
  CHECK(countKind(g, NodeKind::New) == 0);
  // The value phi now reads the constant directly on the taken arm.
  CHECK(g.input(vphi, 1) == c42);
  CHECK(verifyOk(g));
}

B2_TEST(pea_scalar_merge_crossed_rejected) {
  // Store in the branch, load after the merge: the projections differ
  // ({} for the load vs {IfTrue} for the store) - v1 rejects (a phi of
  // stores would be needed; the growth path).
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId c0 = g.constantI(0);
  const ir::NodeId cond = g.make(NodeKind::NeI, {c0, c0});
  const ir::NodeId iff = g.make(NodeKind::If, {start, cond});
  const ir::NodeId t = g.make(NodeKind::IfTrue, {iff});
  const ir::NodeId f = g.make(NodeKind::IfFalse, {iff});
  const ir::NodeId alloc = g.make(NodeKind::New, {start}, ir::TypeId{5});
  const ir::NodeId c42 = g.constantI(42);
  const ir::NodeId store = g.make(NodeKind::StoreField, {t, start, alloc, c42},
                                  ir::FieldId{3});
  const ir::NodeId reg = g.make(NodeKind::Region, {store, f});
  const ir::NodeId load = g.make(NodeKind::LoadField, {reg, store, alloc},
                                 ir::FieldId{3},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId ret = g.make(NodeKind::Return, {load, load});
  (void)ret; // anchors the terminal in the graph
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "merge-crossed") == 0);
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(pea_too_many_fields_cost_gate) {
  // kPeaMaxFields + 1 distinct fields (17 chained stores): the cost
  // gate (Rule 45) rejects rather than plan a fat snapshot. The
  // instance-field shape is the gate's real target - it was pinned via
  // the array shape in v1 because the ir verifier's memory-chain DFS
  // step belt false-positived on chains longer than half the graph
  // (MSG-20260901-007, fixed by the ir team); the belt now admits
  // well-formed long chains, so the instance shape is restored as the
  // primary gate and the array shape stays as its companion below.
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId alloc = g.make(NodeKind::New, {start}, ir::TypeId{5});
  ir::NodeId ctrl = alloc;
  ir::NodeId mem = start;
  const ir::NodeId c1 = g.constantI(1);
  for (std::uint32_t f = 0; f <= passes::kPeaMaxFields; ++f) {
    ctrl = g.make(NodeKind::StoreField,
                  {ctrl, mem, alloc, c1}, ir::FieldId{f});
    mem = ctrl;
  }
  const ir::NodeId ret = g.make(NodeKind::Return, {ctrl, c1});
  (void)ret;
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "too-many-fields") == 0);
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(verifyOk(g));
}

B2_TEST(pea_too_many_elems_cost_gate) {
  // kPeaMaxArrayElems + 1 distinct constant indices: the array twin
  // of the field cost gate (Rule 45).
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId len = g.constantI(static_cast<std::int32_t>(
      passes::kPeaMaxArrayElems + 1));
  const ir::NodeId alloc = g.make(NodeKind::NewArray, {start, len},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  ir::NodeId ctrl = alloc;
  ir::NodeId mem = start;
  const ir::NodeId c1 = g.constantI(1);
  for (std::uint32_t i = 0; i <= passes::kPeaMaxArrayElems; ++i) {
    const ir::NodeId idx = g.constantI(static_cast<std::int32_t>(i));
    ctrl = g.make(NodeKind::StoreElem,
                  {ctrl, mem, alloc, idx, c1},
                  static_cast<std::uint32_t>(ir::IRType::Int));
    mem = ctrl;
  }
  const ir::NodeId ret = g.make(NodeKind::Return, {ctrl, c1});
  (void)ret;
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "too-many-elems") == 0);
  CHECK(countKind(g, NodeKind::NewArray) == 1);
  CHECK(verifyOk(g));
}

// --- keys 66/69: escape-point materialization -----------------------------------

B2_TEST(pea_materialize_shape_and_chain_wiring) {
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 3,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc, so.alloc});
  const ir::NodeId call = g.make(NodeKind::CallStatic,
                                 {so.load, so.store, so.alloc, fs},
                                 ir::MethodId{1},
                                 static_cast<std::uint32_t>(ir::IRType::Bottom));
  so.ret = g.make(NodeKind::Return, {call});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaMaterialized == 1);

  const auto mats = nodesOfKind(g, NodeKind::Materialize);
  CHECK(mats.size() == 1);
  const ir::NodeId m = mats[0];
  const ir::Node& mn = g.node(m);
  CHECK(mn.numInputs == 3);
  // The Materialize's vobj is a VirtualObjectState.
  const ir::NodeId vobj = g.input(m, 2);
  CHECK(g.node(vobj).kind == NodeKind::VirtualObjectState);
  // The escape use (the call) runs after the materialization on BOTH
  // chains: ctrl and mem inputs are the Materialize itself.
  CHECK(g.input(call, 0) == m);
  CHECK(g.input(call, 1) == m);
  // The call's argument edge reads the materialized reference.
  CHECK(g.input(call, 2) == m);
  // The FrameState locals edge was auto-rewired onto the reference
  // (Rule 14: replaceNode updates snapshot inputs).
  CHECK(g.input(fs, 0) == m);
  // The snapshot recorded the pre-escape store: field f3 = 42.
  CHECK(g.numInputs(vobj) == 1);
  CHECK(g.input(vobj, 0) == so.c42);
  // The allocation node itself is gone (replaced by the Materialize).
  CHECK(countKind(g, NodeKind::New) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(pea_materialize_snapshot_defaults_and_keeps_post_access) {
  // Pre-escape: one store + one load (forwarded). Post-escape: a second
  // store + load that must read the REAL materialized object.
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 3,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc});
  const ir::NodeId call = g.make(NodeKind::CallStatic,
                                 {so.load, so.store, so.alloc, fs},
                                 ir::MethodId{1},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId c9 = g.constantI(9);
  const ir::NodeId store2 = g.make(NodeKind::StoreField,
                                   {call, call, so.alloc, c9},
                                   ir::FieldId{3});
  const ir::NodeId load2 = g.make(NodeKind::LoadField,
                                  {store2, store2, so.alloc}, ir::FieldId{3},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  so.ret = g.make(NodeKind::Return, {load2, load2});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaMaterialized == 1);
  const auto mats = nodesOfKind(g, NodeKind::Materialize);
  CHECK(mats.size() == 1);
  const ir::NodeId m = mats[0];
  // The post-escape store targets the materialized reference (its obj
  // edge was rebased), and the post-escape load is KEPT as a real read.
  CHECK(g.input(store2, 2) == m);
  CHECK(g.input(load2, 2) == m);
  CHECK(countKind(g, NodeKind::LoadField) == 1); // only the post-escape one
  CHECK(countKind(g, NodeKind::StoreField) == 1);
  // The pre-escape load was forwarded (its value reached nothing since
  // the call consumed the control - it is simply gone).
  CHECK(verifyOk(g));
}

B2_TEST(pea_materialize_multi_escape_shares_first_point) {
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId fs1 = g.makeFrameState(ir::MethodId{0}, 3,
                                          std::initializer_list<ir::NodeId>{
                                              so.alloc});
  const ir::NodeId call1 = g.make(NodeKind::CallStatic,
                                  {so.load, so.store, so.alloc, fs1},
                                  ir::MethodId{1},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId fs2 = g.makeFrameState(ir::MethodId{0}, 5,
                                          std::initializer_list<ir::NodeId>{
                                              so.alloc});
  const ir::NodeId call2 = g.make(NodeKind::CallStatic,
                                  {call1, call1, so.alloc, fs2},
                                  ir::MethodId{1},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  so.ret = g.make(NodeKind::Return, {call2, call2});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaMaterialized == 1);
  // ONE materialization: the later escape reads the same reference.
  CHECK(nodesOfKind(g, NodeKind::Materialize).size() == 1);
  const ir::NodeId m = nodesOfKind(g, NodeKind::Materialize)[0];
  CHECK(g.input(call2, 2) == m);
  CHECK(verifyOk(g));
}

B2_TEST(pea_materialize_array_snapshot) {
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId len = g.constantI(4);
  const ir::NodeId c5 = g.constantI(5);
  const ir::NodeId c0 = g.constantI(0);
  const ir::NodeId alloc = g.make(NodeKind::NewArray, {start, len},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId s0 = g.make(NodeKind::StoreElem, {alloc, start, alloc,
                                                     c0, c5},
                               static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 2,
                                         std::initializer_list<ir::NodeId>{
                                             alloc});
  const ir::NodeId call = g.make(NodeKind::CallStatic,
                                 {s0, s0, alloc, fs}, ir::MethodId{1},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId ret = g.make(NodeKind::Return, {call, call});
  (void)ret; // anchors the terminal in the graph
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaMaterialized == 1);
  const auto mats = nodesOfKind(g, NodeKind::Materialize);
  CHECK(mats.size() == 1);
  const ir::NodeId vobj = g.input(mats[0], 2);
  const ir::Node& vn = g.node(vobj);
  CHECK(vn.kind == NodeKind::VirtualObjectState);
  CHECK(vn.payload2 == 1); // array vobj
  // Layout: [length, elem0] - the store preceded the escape point.
  CHECK(g.numInputs(vobj) == 2);
  CHECK(g.input(vobj, 0) == len);
  CHECK(g.input(vobj, 1) == c5);
  CHECK(countKind(g, NodeKind::NewArray) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(pea_materialize_dynamic_index) {
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId len = g.constantI(4);
  // Non-constant index: a parameter.
  const ir::NodeId idx = g.parameter(0, ir::IRType::Int);
  const ir::NodeId alloc = g.make(NodeKind::NewArray, {start, len},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId s0 = g.make(NodeKind::StoreElem, {alloc, start, alloc,
                                                     idx, len},
                               static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 2,
                                         std::initializer_list<ir::NodeId>{
                                             alloc});
  const ir::NodeId call = g.make(NodeKind::CallStatic,
                                 {s0, s0, alloc, fs}, ir::MethodId{1},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId ret = g.make(NodeKind::Return, {call, call});
  (void)ret;
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, alloc);
  CHECK(d != nullptr);
  CHECK(d->state == ES::ArgEscape);
  // The dynamic-index store IS the first materializing use (smaller id
  // than the call): materialization lands before it, and both the store
  // and the call then target the materialized reference.
  CHECK(std::strcmp(d->reason, "dynamic-index") == 0);
  CHECK(pr.telemetry.peaMaterialized == 1);
  const ir::NodeId m = nodesOfKind(g, NodeKind::Materialize)[0];
  CHECK(g.input(s0, 2) == m);
  CHECK(g.input(call, 2) == m);
  CHECK(verifyOk(g));
}

B2_TEST(pea_materialize_reader_before_escape_forwards) {
  StraightObject so;
  ir::Graph& g = so.g;
  // A second load BEFORE the escape (chain order) still forwards.
  const ir::NodeId load2 = g.make(NodeKind::LoadField,
                                  {so.load, so.store, so.alloc},
                                  ir::FieldId{3},
                                  static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 4,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc});
  const ir::NodeId call = g.make(NodeKind::CallStatic,
                                 {load2, so.store, so.alloc, fs},
                                 ir::MethodId{1},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  so.ret = g.make(NodeKind::Return, {call, call});
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaMaterialized == 1);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(d->loads == 2); // both pre-escape reads forwarded
  CHECK(d->stores == 1);
  // Both loads are gone; only the materialization remains.
  CHECK(countKind(g, NodeKind::LoadField) == 0);
  CHECK(verifyOk(g));
}

B2_TEST(pea_materialize_nested_inner_first) {
  // inner = new A; inner.x = 42; outer = new B; outer.f = inner;
  // escape(outer). The inner allocation is a foreign-store escape and
  // materializes FIRST (id order); the outer snapshot's field then holds
  // the inner Materialize reference (never a still-virtual vobj).
  ir::Graph g;
  const ir::NodeId start = g.startNode();
  const ir::NodeId c42 = g.constantI(42);
  const ir::NodeId inner = g.make(NodeKind::New, {start}, ir::TypeId{5});
  const ir::NodeId si = g.make(NodeKind::StoreField, {inner, start, inner,
                                                      c42},
                               ir::FieldId{1});
  const ir::NodeId outer = g.make(NodeKind::New, {si}, ir::TypeId{6});
  const ir::NodeId so_ = g.make(NodeKind::StoreField, {outer, si, outer,
                                                       inner},
                                ir::FieldId{2});
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 4,
                                         std::initializer_list<ir::NodeId>{
                                             outer});
  const ir::NodeId call = g.make(NodeKind::CallStatic,
                                 {so_, so_, outer, fs}, ir::MethodId{1},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  const ir::NodeId ret = g.make(NodeKind::Return, {call, call});
  (void)ret; // anchors the terminal in the graph
  CHECK(verifyOk(g));
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaMaterialized == 2); // inner AND outer
  const auto mats = nodesOfKind(g, NodeKind::Materialize);
  CHECK(mats.size() == 2);
  // The outer vobj's field edge must NOT be a VirtualObjectState (the
  // verifier's still-virtual rule): it is the inner Materialize.
  const passes::PeaDecision* do_ = findDecision(pr, outer);
  CHECK(do_ != nullptr);
  const ir::Node& ov = g.node(g.input(do_->materializeAt, 2));
  CHECK(ov.kind == NodeKind::VirtualObjectState);
  const ir::NodeId fieldVal = g.input(do_->materializeAt, 2);
  const ir::NodeId fv = g.input(fieldVal, 0);
  CHECK(g.node(fv).kind == NodeKind::Materialize);
  CHECK(verifyOk(g));
}

// --- kill switches, idempotency, determinism, the corpus gate -------------------

B2_TEST(pea_kill_switch_analysis_disables_all) {
  StraightObject so;
  const auto before = ir::print(so.g);
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::EscapeAnalysis, false);
  const passes::PassResult r = passes::runSinglePass(
      so.g, PK::PartialEscapeAnalysis, cfg);
  CHECK(r.ok); // a disabled pass is a successful no-op
  CHECK(r.telemetry.rewrites == 0);
  CHECK(ir::print(so.g) == before);
  CHECK(verifyOk(so.g));
}

B2_TEST(pea_kill_switch_scalar_downgrades) {
  StraightObject so;
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::ScalarReplacement, false);
  const passes::PeaResult pr =
      passes::runPartialEscapeAnalysis(so.g, cfg);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "scalarize-disabled") == 0);
  CHECK(countKind(so.g, NodeKind::New) == 1); // untouched
  CHECK(verifyOk(so.g));
}

B2_TEST(pea_kill_switch_materialize_downgrades) {
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 3,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc});
  const ir::NodeId call = g.make(NodeKind::CallStatic,
                                 {so.load, so.store, so.alloc, fs},
                                 ir::MethodId{1},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  so.ret = g.make(NodeKind::Return, {call, call});
  CHECK(verifyOk(g));
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::PartialEscapeAnalysis, false);
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g, cfg);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "rejected") == 0);
  CHECK(std::strcmp(d->reason, "materialize-disabled") == 0);
  CHECK(countKind(g, NodeKind::New) == 1);
  CHECK(nodesOfKind(g, NodeKind::Materialize).empty());
  CHECK(verifyOk(g));
}

B2_TEST(pea_kill_switch_planning_downgrades) {
  StraightObject so;
  ir::Graph& g = so.g;
  const ir::NodeId fs = g.makeFrameState(ir::MethodId{0}, 3,
                                         std::initializer_list<ir::NodeId>{
                                             so.alloc});
  const ir::NodeId call = g.make(NodeKind::CallStatic,
                                 {so.load, so.store, so.alloc, fs},
                                 ir::MethodId{1},
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  so.ret = g.make(NodeKind::Return, {call, call});
  CHECK(verifyOk(g));
  passes::PassConfig cfg;
  cfg.setPassEnabled(PK::MaterializationPlanning, false);
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g, cfg);
  CHECK(pr.ok);
  const passes::PeaDecision* d = findDecision(pr, so.alloc);
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->reason, "materialize-disabled") == 0);
  CHECK(nodesOfKind(g, NodeKind::Materialize).empty());
  CHECK(verifyOk(g));
}

B2_TEST(pea_idempotent_second_run) {
  StraightObject so;
  const passes::PeaResult pr1 = passes::runPartialEscapeAnalysis(so.g);
  CHECK(pr1.ok);
  CHECK(pr1.telemetry.peaScalarized == 1);
  const auto after1 = ir::print(so.g);
  const passes::PeaResult pr2 = passes::runPartialEscapeAnalysis(so.g);
  CHECK(pr2.ok);
  CHECK(pr2.telemetry.rewrites == 0);
  CHECK(pr2.telemetry.removals == 0);
  CHECK(pr2.telemetry.peaScalarized == 0);
  CHECK(pr2.telemetry.peaMaterialized == 0);
  CHECK(pr2.telemetry.peaRejected == 0); // no candidates left at all
  CHECK(ir::print(so.g) == after1);
  CHECK(verifyOk(so.g));
}

B2_TEST(pea_deterministic_double_run) {
  StraightObject so1;
  StraightObject so2;
  (void)passes::runPartialEscapeAnalysis(so1.g);
  (void)passes::runPartialEscapeAnalysis(so2.g);
  CHECK(ir::print(so1.g) == ir::print(so2.g));
  CHECK(verifyOk(so1.g));
}

B2_TEST(pea_no_allocations_is_noop) {
  ir::Graph g;
  const ir::NodeId c = g.constantI(1);
  const ir::NodeId ret = g.make(NodeKind::Return, {g.startNode(), c});
  (void)ret;
  CHECK(verifyOk(g));
  const auto before = ir::print(g);
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.decisions.empty());
  CHECK(ir::print(g) == before);
  CHECK(verifyOk(g));
}

// --- the pipeline + corpus integration ------------------------------------------

B2_TEST(pea_pipeline_integration_runs_after_gvn) {
  // The full pipeline (PEA between GVN and the final DCE) over the
  // canonical object program: the allocation materializes at the first
  // call (call-arg) and the graph verifies.
  const std::string path = "tests/interp/corpus/fields.rbc";
  std::ifstream in(path);
  CHECK(static_cast<bool>(in));
  std::stringstream ss;
  ss << in.rdbuf();
  const auto parsed = rbc::parseRbcText(ss.str());
  CHECK(static_cast<bool>(parsed));
  int failures = 0;
  for (std::size_t i = 0; i < parsed->methods.size(); ++i) {
    passes::ProgramCalleeSource res(*parsed);
    ir::Graph g;
    const passes::BuildResult br = passes::buildGraph(
        parsed->methods[i], res, g, static_cast<ir::MethodId>(i));
    CHECK(br.ok);
    const passes::PassResult pr = passes::runEarlyCleanup(g);
    if (!pr.ok) {
      ++failures;
    }
    CHECK(verifyOk(g));
  }
  CHECK(failures == 0);
}

B2_TEST(pea_corpus_sweep_verified_idempotent) {
  const std::vector<std::string> files = {
      "bench_fib",     "bench_sum",     "conversions",
      "div_catch",     "exception_nest", "fib_loop",
      "fields",        "float_math",    "fusion_guard_clobber",
      "fusion_guard_loop_live",        "fusion_guard_reread",
      "monitors",      "quickened",     "sparse_switch",
      "statics_clinit", "strings_intern", "sum_loop",
      "switch_names",  "uncaught",
  };
  for (const std::string& name : files) {
    const std::string path = "tests/interp/corpus/" + name + ".rbc";
    std::ifstream in(path);
    CHECK(static_cast<bool>(in));
    std::stringstream ss;
    ss << in.rdbuf();
    const auto parsed = rbc::parseRbcText(ss.str());
    CHECK(static_cast<bool>(parsed));
    for (std::size_t i = 0; i < parsed->methods.size(); ++i) {
      passes::ProgramCalleeSource res(*parsed);
      ir::Graph g;
      const passes::BuildResult br = passes::buildGraph(
          parsed->methods[i], res, g, static_cast<ir::MethodId>(i));
      CHECK(br.ok);
      const passes::PassResult pr1 = passes::runEarlyCleanup(g);
      CHECK(pr1.ok); // fail-closed: verified after every pass
      CHECK(verifyOk(g));
      const auto after1 = ir::print(g);
      // Idempotent: a second full pipeline performs zero rewrites.
      const passes::PassResult pr2 = passes::runEarlyCleanup(g);
      CHECK(pr2.ok);
      CHECK(pr2.telemetry.rewrites + pr2.telemetry.removals == 0);
      // Rejected classifications are re-derived (they rewrite nothing);
      // only the one-shot dispositions must be zero on the second run.
      CHECK(pr2.telemetry.peaScalarized +
            pr2.telemetry.peaMaterialized == 0);
      CHECK(ir::print(g) == after1);
    }
  }
}

B2_TEST(pea_fields_rbc_materializes_call_arg_e2e) {
  // The fields.rbc main(): new Main + 3 x invokestatic bump(o). Without
  // inlining the first call is an argument escape: one materialization,
  // the FrameState edges rewired, the verifier clean.
  const std::string path = "tests/interp/corpus/fields.rbc";
  std::ifstream in(path);
  CHECK(static_cast<bool>(in));
  std::stringstream ss;
  ss << in.rdbuf();
  const auto parsed = rbc::parseRbcText(ss.str());
  CHECK(static_cast<bool>(parsed));
  CHECK(!parsed->methods.empty());
  passes::ProgramCalleeSource res(*parsed);
  ir::Graph g;
  const passes::BuildResult br = passes::buildGraph(
      parsed->methods[0], res, g, ir::MethodId{0});
  CHECK(br.ok);
  // The guards the builder emitted for the field accesses must be
  // folded first (the pipeline does this before PEA; here the engine
  // precondition): run nullfold + DCE, then PEA.
  {
    passes::PassResult r = passes::runSinglePass(g, PK::NullCheckFolding);
    CHECK(r.ok);
    r = passes::runSinglePass(g, PK::DeadCodeElimination);
    CHECK(r.ok);
  }
  const passes::PeaResult pr = passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  CHECK(pr.telemetry.peaMaterialized == 1);
  CHECK(pr.telemetry.peaScalarized == 0);
  CHECK(nodesOfKind(g, NodeKind::Materialize).size() == 1);
  CHECK(nodesOfKind(g, NodeKind::VirtualObjectState).size() == 1);
  CHECK(verifyOk(g));
}

B2_TEST(pea_fields_rbc_inline_fs_escape_zero_allocations) {
  // After inlining, the call-site FrameStates become chained snapshots
  // (side-table caller references, no live edge consumers): the
  // allocation's deopt-relevant uses are those snapshots plus the
  // reader/writer pairs of the inlined bodies. The fs-escape listing
  // (MSG-20260901-006): the deopt-unreachable chained snapshots share
  // one final-state vobj, main's own live println snapshot gets its
  // per-instant vobj, and the allocation SCALARIZES TO ZERO - the
  // post-inline end state the ICDG inline engine + CM-PEA were built
  // to reach.
  const std::string path = "tests/interp/corpus/fields.rbc";
  std::ifstream in(path);
  CHECK(static_cast<bool>(in));
  std::stringstream ss;
  ss << in.rdbuf();
  const auto parsed = rbc::parseRbcText(ss.str());
  CHECK(static_cast<bool>(parsed));
  passes::ProgramCalleeSource res(*parsed);
  ir::Graph g;
  const passes::BuildResult br = passes::buildGraph(
      parsed->methods[0], res, g, ir::MethodId{0});
  CHECK(br.ok);
  // Inline the three bump() sites first (the cross-method data flow).
  const b2::passes::InlineResult irn = b2::passes::runInlining(g, res,
                                                               {});
  CHECK(irn.ok);
  CHECK(irn.telemetry.sitesInlined == 3);
  // nullfold + DCE first (the engine precondition: the guards the
  // builder emitted must be folded), then PEA.
  {
    b2::passes::PassResult r =
        b2::passes::runSinglePass(g, PK::NullCheckFolding);
    CHECK(r.ok);
    r = b2::passes::runSinglePass(g, PK::DeadCodeElimination);
    CHECK(r.ok);
  }
  const b2::passes::PeaResult pr = b2::passes::runPartialEscapeAnalysis(g);
  CHECK(pr.ok);
  const passes::PeaDecision* d = nullptr;
  for (const passes::PeaDecision& dd : pr.decisions) {
    if (dd.kind == NodeKind::New) {
      d = &dd;
    }
  }
  CHECK(d != nullptr);
  CHECK(std::strcmp(d->action, "scalarized") == 0);
  CHECK(std::strcmp(d->reason, "fs-escape") == 0);
  CHECK(pr.telemetry.peaScalarized == 1);
  // Zero allocations: the New is gone, no Materialize was needed.
  CHECK(countKind(g, NodeKind::New) == 0);
  CHECK(countKind(g, NodeKind::Materialize) == 0);
  // Every field access forwarded away: the inlined bodies' loads AND
  // main's own count read all forward (the printstream fetch is a
  // LoadStatic, a different kind).
  CHECK(countKind(g, NodeKind::LoadField) == 0);
  CHECK(countKind(g, NodeKind::StoreField) == 0);
  // Two vobjs: main's live println snapshot carries the final count
  // state (the getstatic guard folded away - its receiver is the
  // never-null allocation - and DCE reclaimed that fs), and the
  // deopt-unreachable chained call-site snapshots share one
  // final-state vobj. Every referencing desc lists its vobj.
  const auto vobjs = nodesOfKind(g, NodeKind::VirtualObjectState);
  CHECK(vobjs.size() == 2);
  std::uint32_t listed = 0;
  for (ir::NodeId v : vobjs) {
    for (ir::FrameStateId i = 0; i < g.frameStateCount(); ++i) {
      for (ir::VirtualObjectId e : g.frameStateVobjs(i)) {
        if (e == v) {
          ++listed;
        }
      }
    }
  }
  CHECK(listed == 4); // 3 chained snapshots share one + 1 live snapshot
  // Every fs slot that referenced the New now references a vobj.
  for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
    if (g.node(n).kind != NodeKind::FrameState || g.node(n).isDead()) {
      continue;
    }
    for (std::uint16_t s = 0; s < g.numInputs(n); ++s) {
      CHECK(g.node(g.input(n, s)).kind != NodeKind::New);
    }
  }
  CHECK(verifyOk(g));
}
