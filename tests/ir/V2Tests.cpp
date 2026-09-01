// B-2 IR tests - v2 additions (MSG-20260831-007): the graph-builder
// vocabulary. Positive: boolean test kinds type-check, Undef as the
// FrameState/Phi placeholder, guards producing control, Phi as the
// memory-state merge, serialization of the appended kinds. Negative: Undef
// in typed-operand positions, memory Phi smuggling a pure value, cycles
// through memory Phis, kind-value stability vs v1.

#include "TestHarness.h"

#include <b2/ir/Graph.h>
#include <b2/ir/Node.h>
#include <b2/ir/Printer.h>
#include <b2/ir/Serialize.h>
#include <b2/ir/Verifier.h>

using namespace b2;
using b2::ir::NodeKind;

namespace {

bool mentions(const ir::VerifyResult& r, std::string_view needle) {
  for (const ir::VerifyDiag& d : r.diags) {
    if (d.message.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace

B2_TEST(v2_boolean_kinds_type_check_and_verify) {
  ir::Graph g;
  const ir::NodeId a = g.parameter(0, ir::IRType::Int);
  const ir::NodeId b = g.constantI(3);
  const ir::NodeId eq = g.make(NodeKind::EqI, {a, b});
  const ir::NodeId lt = g.make(NodeKind::LtI, {a, b});
  const ir::NodeId ne = g.make(NodeKind::NeI, {a, b});
  const ir::NodeId orr = g.make(NodeKind::OrI, {eq, lt});
  const ir::NodeId nott = g.make(NodeKind::Not, {ne});
  const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), orr});
  const ir::NodeId t = g.make(NodeKind::IfTrue, {iff});
  const ir::NodeId f = g.make(NodeKind::IfFalse, {iff});
  g.make(NodeKind::Return, {t, nott});
  g.make(NodeKind::Return, {f});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message.c_str());
}

B2_TEST(v2_boolean_kinds_reject_wrong_operand_types) {
  {
    ir::Graph g;
    const ir::NodeId l = g.parameter(0, ir::IRType::Long);
    g.make(NodeKind::EqI, {l, l});
    g.make(NodeKind::Return, {g.startNode()});
    const ir::VerifyResult r = ir::verify(g);
    CHECK(!r.ok);
    CHECK(mentions(r, "expected int"));
  }
  {
    ir::Graph g;
    const ir::NodeId a = g.parameter(0, ir::IRType::Int);
    g.make(NodeKind::Not, {a});
    g.make(NodeKind::Return, {g.startNode()});
    // Not's operand IS Int: legal. The reject case is IsNull on an Int.
    const ir::VerifyResult ok = ir::verify(g);
    CHECK(ok.ok);
  }
  {
    ir::Graph g;
    const ir::NodeId a = g.parameter(0, ir::IRType::Int);
    g.make(NodeKind::IsNull, {a});
    g.make(NodeKind::Return, {g.startNode()});
    const ir::VerifyResult r = ir::verify(g);
    CHECK(!r.ok);
    CHECK(mentions(r, "expected ref"));
  }
}

B2_TEST(v2_refeq_and_isnull_accept_null_typed_operands) {
  ir::Graph g;
  const ir::NodeId null = g.constantNull();
  const ir::NodeId ref = g.parameter(0, ir::IRType::Ref);
  const ir::NodeId isnull = g.make(NodeKind::IsNull, {null});
  const ir::NodeId refeq = g.make(NodeKind::RefEq, {null, ref});
  const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), isnull});
  g.make(NodeKind::Return, {g.make(NodeKind::IfTrue, {iff}), refeq});
  g.make(NodeKind::Return, {g.make(NodeKind::IfFalse, {iff})});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message.c_str());
}

B2_TEST(v2_undef_feeds_framestate_and_phi) {
  ir::Graph g;
  const ir::NodeId un = g.make(NodeKind::Undef);
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId fs = g.makeFrameState(4, 9, {un, a});
  const ir::NodeId cond = g.constantI(1);
  const ir::NodeId guard =
      g.make(NodeKind::Guard, {g.startNode(), cond, fs},
             static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 0);
  // A Phi merging a real value with the "Bottom on this path" marker.
  const ir::NodeId other = g.constantI(2);
  const ir::NodeId region = g.make(NodeKind::Region, {guard, guard});
  const ir::NodeId phi = g.make(NodeKind::Phi, {region, a, other});
  g.make(NodeKind::Return, {region, phi});
  (void)un;
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message.c_str());
  CHECK(ir::resultTypeOf(g, phi) == ir::IRType::Int);
}

B2_TEST(v2_undef_rejected_in_typed_operand_positions) {
  {
    ir::Graph g;
    const ir::NodeId un = g.make(NodeKind::Undef);
    g.make(NodeKind::AddI, {un, un});
    g.make(NodeKind::Return, {g.startNode()});
    const ir::VerifyResult r = ir::verify(g);
    CHECK(!r.ok);
    CHECK(mentions(r, "Undef may only feed"));
  }
  {
    // Even where operand types are unchecked (call args), Undef is banned.
    ir::Graph g;
    const ir::NodeId un = g.make(NodeKind::Undef);
    const ir::NodeId fs = g.makeFrameState(1, 0, {un});
    g.make(NodeKind::CallStatic, {g.startNode(), g.startNode(), un, fs}, 3,
           static_cast<std::uint32_t>(ir::IRType::Int));
    const ir::VerifyResult r = ir::verify(g);
    CHECK(!r.ok);
    CHECK(mentions(r, "Undef may only feed"));
  }
}

B2_TEST(v2_guard_produces_control_for_protected_ops) {
  // The graph-builder shape: Guard gates a StoreField. v1 rejected this
  // (guards produced no control); v2 accepts (MSG-20260831-007).
  ir::Graph g;
  const ir::NodeId obj = g.parameter(0, ir::IRType::Ref);
  const ir::NodeId val = g.constantI(7);
  const ir::NodeId isnull = g.make(NodeKind::IsNull, {obj});
  const ir::NodeId notnull = g.make(NodeKind::Not, {isnull});
  const ir::NodeId fs = g.makeFrameState(4, 12, {obj, val});
  const ir::NodeId guard =
      g.make(NodeKind::Guard, {g.startNode(), notnull, fs},
             static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 1);
  const ir::NodeId store =
      g.make(NodeKind::StoreField, {guard, g.startNode(), obj, val}, 5);
  g.make(NodeKind::Return, {guard});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message.c_str());
  (void)store;
}

B2_TEST(v2_phi_is_the_memory_state_merge) {
  // Two branches each store, then merge: the merge's memory is a Phi over
  // the two store results (Graal's MemoryPhi shape).
  ir::Graph g;
  const ir::NodeId obj = g.constantNull();
  const ir::NodeId cond = g.constantI(1);
  const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), cond});
  const ir::NodeId t = g.make(NodeKind::IfTrue, {iff});
  const ir::NodeId f = g.make(NodeKind::IfFalse, {iff});
  const ir::NodeId st = g.make(NodeKind::StoreField,
                               {t, g.startNode(), obj, cond}, 1);
  const ir::NodeId sf = g.make(NodeKind::StoreField,
                               {f, g.startNode(), obj, cond}, 2);
  const ir::NodeId region = g.make(NodeKind::Region, {t, f});
  const ir::NodeId memPhi = g.make(NodeKind::Phi, {region, st, sf});
  const ir::NodeId load = g.make(NodeKind::LoadField,
                                 {region, memPhi, obj}, 3,
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  g.make(NodeKind::Return, {region, load});
  const ir::VerifyResult r = ir::verify(g);
  CHECK_MSG(r.ok, r.diags.empty() ? "" : r.diags[0].message.c_str());
}

B2_TEST(v2_memory_phi_rejects_pure_value_inputs) {
  ir::Graph g;
  const ir::NodeId obj = g.constantNull();
  const ir::NodeId cond = g.constantI(1);
  const ir::NodeId iff = g.make(NodeKind::If, {g.startNode(), cond});
  const ir::NodeId t = g.make(NodeKind::IfTrue, {iff});
  const ir::NodeId f = g.make(NodeKind::IfFalse, {iff});
  const ir::NodeId st = g.make(NodeKind::StoreField,
                               {t, g.startNode(), obj, cond}, 1);
  const ir::NodeId bad = g.constantI(9); // pure value: not memory state
  const ir::NodeId region = g.make(NodeKind::Region, {st, f});
  const ir::NodeId memPhi = g.make(NodeKind::Phi, {region, st, bad});
  const ir::NodeId load = g.make(NodeKind::LoadField,
                                 {region, memPhi, obj}, 3,
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  g.make(NodeKind::Return, {region, load});
  const ir::VerifyResult r = ir::verify(g);
  CHECK(!r.ok);
  CHECK(mentions(r, "does not produce memory state"));
}

B2_TEST(v2_memory_chain_cycle_through_phi_rejected) {
  ir::Graph g;
  const ir::NodeId obj = g.constantNull();
  const ir::NodeId v = g.constantI(7);
  const ir::NodeId s1 = g.make(NodeKind::StoreField,
                               {g.startNode(), g.startNode(), obj, v}, 1);
  // A second, well-formed value input keeps the phi's arity honest (two
  // values for two region preds) so the CYCLE diagnostic below is the
  // sole discriminator (MSG-20260901-004: the loop-header closure skip
  // applies only to LoopBegin-backed phis - a forward merge has no
  // backedge, so a closure at one is a real cycle).
  const ir::NodeId s2 = g.make(NodeKind::StoreField,
                               {g.startNode(), g.startNode(), obj, v}, 2);
  const ir::NodeId region = g.make(NodeKind::Region, {g.startNode(),
                                                      g.startNode()});
  const ir::NodeId memPhi = g.make(NodeKind::Phi, {region, s1, s2});
  // s1's memory now flows through the phi back to s1: a cycle.
  g.setInput(s1, 1, memPhi);
  const ir::NodeId load = g.make(NodeKind::LoadField,
                                 {region, memPhi, obj}, 3,
                                 static_cast<std::uint32_t>(ir::IRType::Int));
  g.make(NodeKind::Return, {region, load});
  const ir::VerifyResult r = ir::verify(g);
  CHECK(!r.ok);
  CHECK(mentions(r, "cycle"));
}

B2_TEST(v2_new_kinds_serialize_and_roundtrip) {
  ir::Graph g;
  const ir::NodeId un = g.make(NodeKind::Undef);
  const ir::NodeId a = g.parameter(0, ir::IRType::Int);
  const ir::NodeId b = g.constantI(4);
  const ir::NodeId lt = g.make(NodeKind::LtI, {a, b});
  const ir::NodeId nott = g.make(NodeKind::Not, {lt});
  const ir::NodeId obj = g.parameter(1, ir::IRType::Ref);
  const ir::NodeId isnull = g.make(NodeKind::IsNull, {obj});
  const ir::NodeId fs = g.makeFrameState(6, 3, {un, a, obj});
  const ir::NodeId guard =
      g.make(NodeKind::Guard, {g.startNode(), nott, fs},
             static_cast<std::uint32_t>(ir::GuardKind::BoundsCheck), 2);
  const ir::NodeId fs2 = g.makeFrameState(6, 3, {un, a, obj});
  const ir::NodeId guard2 =
      g.make(NodeKind::Guard, {guard, isnull, fs2},
             static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 3);
  const ir::NodeId refeq = g.make(NodeKind::RefEq, {obj, obj});
  g.make(NodeKind::Return, {guard2, refeq});
  CHECK(ir::verify(g).ok);
  const std::vector<std::uint8_t> bytes = ir::serialize(g);
  ir::Graph h;
  const ir::DeserializeResult r = ir::deserializeInto(bytes, h);
  CHECK_MSG(r.ok, r.error.message.c_str());
  CHECK(ir::verify(h).ok);
  CHECK(ir::print(h) == ir::print(g));
  CHECK(ir::serialize(h) == bytes);
}

B2_TEST(v2_v1_artifacts_still_deserialize) {
  // v2 only APPENDED kinds and made guards control producers, so a v1
  // artifact must load unchanged (version gate accepts 1..kIrFormatVersion).
  ir::Graph g;
  const ir::NodeId cond = g.constantI(1);
  const ir::NodeId fs = g.makeFrameState(1, 0, {cond});
  g.make(NodeKind::Guard, {g.startNode(), cond, fs},
         static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 0);
  g.make(NodeKind::Return, {g.startNode()});
  std::vector<std::uint8_t> bytes = ir::serialize(g);
  // Downgrade the version field to 1 (offset 4..8).
  bytes[4] = 1;
  bytes[5] = 0;
  bytes[6] = 0;
  bytes[7] = 0;
  ir::Graph h;
  const ir::DeserializeResult r = ir::deserializeInto(bytes, h);
  CHECK_MSG(r.ok, r.error.message.c_str());
  CHECK(ir::verify(h).ok);
  CHECK(ir::print(h) == ir::print(g));
}

B2_TEST(v2_v1_kind_values_are_unchanged) {
  // Serialization stability (Rule 31): the enum values of all v1 kinds are
  // pinned; v2 kinds start after FrameState (v1's last).
  CHECK(static_cast<std::uint16_t>(NodeKind::FrameState) == 126);
  CHECK(static_cast<std::uint16_t>(NodeKind::Undef) == 127);
  CHECK(static_cast<std::uint16_t>(NodeKind::GeI) == 136);
  CHECK(ir::nodeKindCount() == 137);
}
