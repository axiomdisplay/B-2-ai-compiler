// B-2 Passes - global value numbering (registry key 35, suite item 35).
//
// WHY THIS FILE EXISTS:
// GVN is the sea-of-nodes dedup law: two LIVE nodes with the same
// (kind, payload, payload2, constValue, exact input vector) compute the
// same value at every point where either is computable, because both are
// the same total function of the same inputs. Replacing the later node
// with the earlier one (lowest id wins - deterministic, Rule 124) is
// sound WITHOUT a schedule or dominator analysis precisely because pure
// nodes float: anywhere the removed node was computable, the survivor is
// too (identical inputs), and replaceNode rewires every user - including
// FrameState snapshots, so deopt states stay value-identical (Rule 14
// doing real work).
//
// Eligibility (isGvnEligible): pure values (constants, arithmetic,
// comparisons, conversions, parameters, phis, vector formation),
// ArrayLength, InstanceOf, and the Load* readers (loads chain control, so
// builder output rarely produces equal keys - they are eligible for
// transformed/hand-built graphs; identical ctrl+mem+address inputs yield
// the same read). Calls, stores, allocations, guards, control, and state
// nodes are never value-numbered.
//
// Trap preservation: identical trapping nodes (e.g. two DivI(c, x)) merge
// safely - the survivor inherits ALL users, so the trap fires exactly
// when any original would have.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "PassInternal.h"

namespace b2::passes::detail {

namespace {

struct Key {
  std::uint16_t kind = 0;
  std::uint32_t payload = 0;
  std::uint32_t payload2 = 0;
  std::uint64_t constValue = 0;
  std::vector<ir::NodeId> inputs;

  [[nodiscard]] bool operator==(const Key& o) const noexcept {
    return kind == o.kind && payload == o.payload &&
           payload2 == o.payload2 && constValue == o.constValue &&
           inputs == o.inputs;
  }
};

struct KeyHash {
  // Deterministic combination (splitmix-style mixing); the hash is never
  // part of the observable output (the map is only probed, never
  // iterated), so Rule 124 determinism rests on the id-order sweep alone.
  [[nodiscard]] static std::uint64_t mix(std::uint64_t h,
                                         std::uint64_t v) noexcept {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
  }

  [[nodiscard]] std::size_t operator()(const Key& k) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    h = mix(h, k.kind);
    h = mix(h, k.payload);
    h = mix(h, k.payload2);
    h = mix(h, k.constValue);
    for (const ir::NodeId i : k.inputs) {
      h = mix(h, i);
    }
    return static_cast<std::size_t>(h);
  }
};

[[nodiscard]] Key keyOf(const ir::Graph& g, ir::NodeId n) {
  const ir::Node& node = g.node(n);
  Key k;
  k.kind = static_cast<std::uint16_t>(node.kind);
  k.payload = node.payload;
  k.payload2 = node.payload2;
  k.constValue = static_cast<std::uint64_t>(node.constValue);
  k.inputs.reserve(node.numInputs);
  for (std::uint16_t s = 0; s < node.numInputs; ++s) {
    k.inputs.push_back(g.input(n, s));
  }
  return k;
}

} // namespace

void runGVN(ir::Graph& g, PassTelemetry& t, Budget& b, const Junk& jk) {
  // NOTE on junk sinks (jk): they are NOT protected here. A sink that
  // duplicates a real constant merges onto it - replaceNode rewires the
  // tombstone users to the survivor, so every tombstone edge stays alive
  // and legal, and the graph converges within one run instead of
  // churning fresh sinks on every pipeline application.
  // Determinism contract: nodes are processed in id order and the FIRST
  // (lowest-id) occurrence of a key is the survivor; the map is probed
  // only, so its internal layout cannot leak into the output.
  std::unordered_map<Key, ir::NodeId, KeyHash> seen;
  seen.reserve(g.nodeCount());

  for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
    if (b.exceeded) {
      return;
    }
    const ir::Node& node = g.node(n);
    if (node.isDead() || !isGvnEligible(node.kind)) {
      continue;
    }
    Key k = keyOf(g, n);
    const auto it = seen.find(k);
    if (it == seen.end()) {
      seen.emplace(std::move(k), n);
      continue;
    }
    const ir::NodeId survivor = it->second;
    if (survivor == n || g.node(survivor).isDead()) {
      continue; // paranoid: never rewire onto a tombstone
    }
    const std::uint32_t rewritesBefore = t.rewrites;
    replace(g, n, survivor, t, b, jk);
    if (t.rewrites != rewritesBefore) {
      ++t.gvnDedups;
    }
  }
}

} // namespace b2::passes::detail
