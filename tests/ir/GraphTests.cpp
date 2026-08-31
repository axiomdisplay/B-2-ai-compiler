// B-2 IR tests - graph construction, use-def integrity, replacement epochs
// (charter's testing responsibilities: construction, replacement and epoch
// behavior, use-def integrity under replacement, Rule 34 regression shape).

#include "TestHarness.h"

#include <b2/ir/Graph.h>
#include <b2/ir/Node.h>
#include <b2/ir/Printer.h>
#include <b2/ir/Verifier.h>

using namespace b2;
using b2::ir::NodeKind;

namespace {

// Builds a small sound diamond: Start -> If -> IfTrue/IfFalse -> Region ->
// Phi, with constants feeding the Phi. Returns via out-params for reuse.
struct Diamond {
  ir::Graph g;
  ir::NodeId c1;
  ir::NodeId c2;
  ir::NodeId cmp;
  ir::NodeId iff;
  ir::NodeId t;
  ir::NodeId f;
  ir::NodeId region;
  ir::NodeId phi;

  Diamond() {
    c1 = g.constantI(1);
    c2 = g.constantI(2);
    cmp = g.make(NodeKind::CmpI, {c1, c2});
    iff = g.make(NodeKind::If, {g.startNode(), cmp});
    t = g.make(NodeKind::IfTrue, {iff});
    f = g.make(NodeKind::IfFalse, {iff});
    region = g.make(NodeKind::Region, {t, f});
    phi = g.make(NodeKind::Phi, {region, c1, c2});
  }
};

bool useListContains(const ir::Graph& g, ir::NodeId def, ir::NodeId user,
                     std::uint16_t slot) {
  for (const ir::Use& u : g.usesOf(def)) {
    if (u.user == user && u.slot == slot) {
      return true;
    }
  }
  return false;
}

} // namespace

B2_TEST(graph_empty_has_only_start_and_verifies) {
  ir::Graph g;
  CHECK(g.nodeCount() == 1);
  CHECK(g.node(0).kind == NodeKind::Start);
  CHECK(g.startNode() == 0);
  CHECK(ir::verify(g).ok);
}

B2_TEST(graph_node_ids_are_dense_creation_order) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(7);
  const ir::NodeId b = g.constantI(8);
  const ir::NodeId c = g.make(NodeKind::AddI, {a, b});
  CHECK(a == 1);
  CHECK(b == 2);
  CHECK(c == 3);
  CHECK(g.nodeCount() == 4);
}

B2_TEST(graph_constants_preserve_exact_bits) {
  ir::Graph g;
  const ir::NodeId f = g.constantF(-0.0F);
  const ir::NodeId d = g.constantD(1.5);
  const std::uint32_t fbits =
      static_cast<std::uint32_t>(static_cast<std::uint64_t>(g.node(f).constValue));
  CHECK(fbits == 0x80000000u); // negative zero survives exactly
  const std::uint64_t dbits =
      static_cast<std::uint64_t>(g.node(d).constValue);
  CHECK(dbits == 0x3FF8000000000000ULL);
  // NaN payload round-trip.
  const float nan1 = std::bit_cast<float>(0x7FC00001u);
  const ir::NodeId n = g.constantF(nan1);
  const std::uint32_t nbits =
      static_cast<std::uint32_t>(static_cast<std::uint64_t>(g.node(n).constValue));
  CHECK(nbits == 0x7FC00001u);
}

B2_TEST(graph_use_def_lists_track_make_and_setInput) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  const ir::NodeId add = g.make(NodeKind::AddI, {a, b});
  CHECK(useListContains(g, a, add, 0));
  CHECK(useListContains(g, b, add, 1));
  CHECK(g.usesOf(a).size() == 1);
  CHECK(g.usesOf(b).size() == 1);

  const ir::NodeId c = g.constantI(3);
  g.setInput(add, 1, c);
  CHECK(!useListContains(g, b, add, 1)); // old use removed
  CHECK(useListContains(g, c, add, 1));  // new use present
  CHECK(g.usesOf(b).empty());
  CHECK(g.input(add, 1) == c);
}

B2_TEST(graph_appendInput_grows_region_and_updates_uses) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId r = g.make(NodeKind::Region, {a, a}); // ctrl preds (bogus
  // kinds are fine for the storage test; the verifier's role check is tested
  // separately)
  const ir::NodeId b = g.constantI(2);
  g.appendInput(r, b);
  CHECK(g.numInputs(r) == 3);
  CHECK(g.input(r, 2) == b);
  CHECK(useListContains(g, b, r, 2));
  // Original inputs still intact after slice relocation.
  CHECK(g.input(r, 0) == a);
  CHECK(g.input(r, 1) == a);
}

B2_TEST(graph_replaceNode_rewires_users_and_tags_epoch) {
  Diamond d;
  const ir::NodeId fresh = d.g.constantI(9);
  const std::uint32_t oldEpoch = d.g.epoch();
  const std::uint32_t ep = d.g.replaceNode(d.c1, fresh);
  CHECK(ep == oldEpoch + 1);
  CHECK(d.g.node(d.c1).isDead());
  CHECK(d.g.node(d.c1).flags.has(ir::NodeFlag::Replaced));
  CHECK(d.g.node(d.c1).epoch == ep);
  // All users rewired.
  CHECK(!useListContains(d.g, d.c1, d.cmp, 0));
  CHECK(useListContains(d.g, fresh, d.cmp, 0));
  CHECK(useListContains(d.g, fresh, d.phi, 1));
  CHECK(d.g.replacements().size() == 1);
  CHECK(d.g.replacements()[0].oldNode == d.c1);
  CHECK(d.g.replacements()[0].newNode == fresh);
  CHECK(d.g.replacements()[0].epoch == ep);
  // Dead node has no users left.
  CHECK(d.g.usesOf(d.c1).empty());
}

B2_TEST(graph_replaceNode_refuses_replacement_with_dead_node) {
  ir::Graph g;
  const ir::NodeId a = g.constantI(1);
  const ir::NodeId dead = g.constantI(3);
  g.killNode(dead);
  const std::uint32_t ep = g.replaceNode(a, dead);
  CHECK(ep == 0); // refused
  CHECK(!g.node(a).isDead());
  CHECK(g.replacements().empty());
}

B2_TEST(graph_replacement_updates_framestate_snapshot_edges) {
  // The Rule 5 payoff: FrameState locals are EDGES, so replacing a local's
  // producer automatically updates every deopt snapshot that captured it.
  ir::Graph g;
  const ir::NodeId v1 = g.constantI(42);
  const ir::NodeId fs = g.makeFrameState(7, 12, {v1});
  CHECK(g.input(fs, 0) == v1);
  const ir::NodeId v2 = g.constantI(43);
  g.replaceNode(v1, v2);
  CHECK(g.input(fs, 0) == v2);
  CHECK(useListContains(g, v2, fs, 0));
}

B2_TEST(graph_virtual_object_fields_are_edges) {
  ir::Graph g;
  const ir::NodeId x = g.constantI(1);
  const ir::NodeId y = g.constantI(2);
  const ir::NodeId vo = g.makeVirtualObject(55, {x, y});
  CHECK(g.node(vo).kind == NodeKind::VirtualObjectState);
  CHECK(g.node(vo).payload == 55);
  CHECK(g.numInputs(vo) == 2);
  // Replacing a field value rewires the vobj automatically (the PEA soundness
  // seam: side-table ids would silently miss this).
  const ir::NodeId z = g.constantI(3);
  g.replaceNode(y, z);
  CHECK(g.input(vo, 1) == z);
}

B2_TEST(graph_specmeta_and_dependency_attachment) {
  ir::Graph g;
  const ir::NodeId dep = g.addDependency({ir::Dependency::Kind::ClassHierarchy, 17});
  ir::SpecMeta sm;
  sm.kind = ir::SpecMeta::Kind::TypeMonomorphic;
  sm.source = ir::SpecMeta::Source::PGO;
  sm.confidence = 9500;
  sm.deoptTarget = 4;
  sm.cost = 12;
  sm.dependency = dep;
  sm.rollback = ir::SpecMeta::Rollback::None;
  const ir::SpecMetaId sid = g.addSpecMeta(sm);

  const ir::NodeId cond = g.constantI(1);
  const ir::NodeId fs = g.makeFrameState(1, 0, {});
  const ir::NodeId guard =
      g.make(NodeKind::Guard, {g.startNode(), cond, fs},
             static_cast<std::uint32_t>(ir::GuardKind::NullCheck), 4);
  g.attachSpecMeta(guard, sid);
  CHECK(g.node(guard).specMeta == sid + 1);
  CHECK(g.node(guard).flags.has(ir::NodeFlag::Speculative));
  CHECK(g.specMeta(sid).dependency == dep);
  CHECK(g.dependency(dep).kind == ir::Dependency::Kind::ClassHierarchy);
}

B2_TEST(graph_live_node_count_tracks_deaths) {
  ir::Graph g;
  [[maybe_unused]] const ir::NodeId a = g.constantI(1);
  const ir::NodeId b = g.constantI(2);
  CHECK(g.liveNodeCount() == 3); // Start + 2 constants
  g.killNode(b);
  CHECK(g.liveNodeCount() == 2);
  CHECK(g.nodeCount() == 3); // tombstone stays
}

B2_TEST(graph_diamond_verifies_and_types) {
  Diamond d;
  const ir::VerifyResult r = ir::verify(d.g);
  CHECK(r.ok);
  CHECK(ir::resultTypeOf(d.g, d.phi) == ir::IRType::Int);
  CHECK(ir::resultTypeOf(d.g, d.cmp) == ir::IRType::Int);
}

B2_TEST(graph_smallvector_inline_then_heap_growth) {
  std::pmr::monotonic_buffer_resource arena;
  ir::SmallVector<ir::NodeId, 3> v(&arena);
  for (int i = 0; i < 100; ++i) {
    v.push_back(static_cast<ir::NodeId>(i));
  }
  CHECK(v.size() == 100);
  const unsigned first = 0;
  const unsigned last = static_cast<unsigned>(v.size()) - 1;
  const unsigned mid = 50;
  CHECK(v[first] == 0);
  CHECK(v[last] == 99);
  CHECK(v.remove(mid));
  CHECK(v.size() == 99);
  CHECK(v[mid] == 51);
  CHECK(!v.remove(1000));
  // Copy keeps contents and stays usable.
  ir::SmallVector<ir::NodeId, 3> w(v);
  const unsigned wlast = static_cast<unsigned>(w.size()) - 1;
  CHECK(w.size() == 99);
  CHECK(w[wlast] == 99);
}
