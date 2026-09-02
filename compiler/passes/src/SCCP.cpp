// B-2 Passes - sparse conditional constant propagation (registry key 38;
// one engine covering the charter's scalar rows 37 CPROP / 38 SCCP /
// 39 conditional constant propagation, exactly like GVN covers CSE 36).
//
// WHY THIS FILE EXISTS:
// constfold (14) sees only LITERAL constants: a node whose inputs are
// ConstantI nodes. That misses every constant that only EXISTS because
// of control flow:
//   - a phi merging the same constant from two different nodes
//     (trivial-phi needs identical NODES and GVN has not run yet),
//   - a loop phi whose backedge value stabilizes (phi = 0 entering, and
//     the body leaves it at 0 - no fold of phi(0, AddI(phi, 0)) exists),
//   - a merge whose dead arm is provably dead through a branch whose
//     condition became constant only AFTER a phi resolved.
// SCCP (Wegman-Zadeck, adapted to the sea of nodes) finds all of these
// with an OPTIMISTIC value lattice driven by executable control edges,
// then replaces the proven constants - and the SAME pipeline round's
// branchnorm/cfs/dce consume them (branch folding, unreachable sweep,
// orphan reclamation). The rewrite surface of this pass is deliberately
// minimal: values become constants, control stays branchnorm's business.
//
// THE LATTICE (per live node, values only DESCEND - monotone, Rule 10):
//   Top      unresolved (optimistic: may still become anything)
//   Const(c) provably c on EVERY executable path
//   Bot      not a compile-time constant (variable, trap, opaque op)
//   meet(Top, x) = x;  meet(c, c) = c;  meet(c1, c2) = Bot;  Bot absorbs.
//
// THE ADAPTATION TO SEA-OF-NODES:
//   - Pure values FLOAT (no control input), so they are evaluated from
//     their operands alone, eagerly seeded and re-evaluated on operand
//     descent - no schedule, no dominator walk needed. Top operands keep
//     the result Top (optimistic wait), Bot forces Bot, all-Const folds
//     through the SHARED evaluator (evalBinOp/evalUnaryOp - the same
//     Java-semantics table constfold uses; a fold refusal, e.g. div by
//     a constant zero, is Bot: the value does not exist at runtime).
//   - Phi meets ONLY over the region's EXECUTABLE inputs: a value that
//     flows solely on a never-executed edge does not exist. This is the
//     "conditional" in SCCP and it is where the optimism lives: a not-
//     yet-executable backedge contributes Top (ignored), which is what
//     makes loop-invariant phis resolvable (phi = meet(0, phi) = 0).
//   - Executability: Start is executable; an If whose condition lattice
//     is Const makes only the taken projection executable (Bot makes
//     both; Top waits); an always-failing Guard stops flow exactly like
//     the Deopt it becomes; Region/LoopBegin are executable when any
//     input is (the backedge only fires once the body does - natural
//     causality, no forward/backedge special case); Switch marks ALL
//     projections (case ordinals are frontend-opaque - conservatism (b));
//     CallExcept rides the call's Parent edge like every other
//     projection (an exception MAY be thrown - over-approximation only).
//
// THE COMPLETION RULE (the soundness bolt, the classic SCCP endgame):
// at the propagation fixpoint, a Top value with a LIVE user is NOT
// "unreachable code" (the classic CFG reading) - floating values have
// no block. It is UNRESOLVED, and an unresolved value feeding a live
// use must be treated as overdefined: forced to Bot and re-propagated
// until stable. This is what keeps the optimism honest: a phi whose
// executable edge carries a never-resolving value (hand-built shapes;
// builder output resolves everything) collapses to Bot instead of
// claiming the meet of the resolved edges alone. Iterated: each forcing
// round lowers at least one node permanently (monotone), bounded by the
// node count, and the step budget bounds the whole propagation.
//
// THE EXECUTION ORDER (Rule 124, determinism):
//   PROPAGATE (pure - no mutation; unique fixpoint independent of
//   worklist order) -> COMPLETE (force unresolved-but-used Top values
//   to Bot, id order, re-propagate) -> APPLY (id order; one interned
//   constant node per distinct value, created on first need). The
//   interned constants keep the print byte-stable; GVN merges them with
//   pre-existing identical constants later in the same round.
//
// THE BUDGET (Rule 23): the Budget is charged per worklist pop AND per
// replacement. A propagation/completion overrun aborts BEFORE any
// rewrite - claims from a partial fixpoint are not sound, so none are
// applied (the graph is untouched, telemetry reports the overrun). An
// apply overrun stops mid-apply at a valid point: every replacement is
// individually sound at the fixpoint, so a prefix is too.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "PassInternal.h"

#include "b2/ir/Printer.h"

namespace b2::passes::detail {

namespace {

// --- the lattice ---------------------------------------------------------------

enum class LV : std::uint8_t { Top = 0, Const = 1, Bot = 2 };

struct LatVal {
  LV lv = LV::Top;
  ConstVal c; // valid iff lv == Const

  [[nodiscard]] bool sameConst(const ConstVal& o) const noexcept {
    if (c.flavor != o.flavor) {
      return false;
    }
    switch (c.flavor) {
    case CV::I:
      return c.i == o.i;
    case CV::L:
      return c.l == o.l;
    case CV::F:
      return c.fb == o.fb;
    case CV::D:
      return c.db == o.db;
    case CV::Null:
      return true; // there is exactly one null
    default:
      return false;
    }
  }
};

[[nodiscard]] LatVal topLat() { return LatVal{}; }

[[nodiscard]] LatVal constLat(const ConstVal& c) {
  LatVal v;
  v.lv = LV::Const;
  v.c = c;
  return v;
}

[[nodiscard]] LatVal botLat() { return LatVal{LV::Bot, ConstVal{}}; }

// Lattice meet (greatest lower bound in Top > Const > Bot).
[[nodiscard]] LatVal meet(const LatVal& a, const LatVal& b) {
  if (a.lv == LV::Top) {
    return b;
  }
  if (b.lv == LV::Top) {
    return a;
  }
  if (a.lv == LV::Bot || b.lv == LV::Bot) {
    return botLat();
  }
  if (a.sameConst(b.c)) {
    return a;
  }
  return botLat();
}

// --- kind triage -----------------------------------------------------------------

// One-input kinds the shared evaluator understands (foldUnary's catalog).
[[nodiscard]] bool isUnaryEvalK(ir::NodeKind k) {
  using K = ir::NodeKind;
  switch (k) {
  case K::NegI:
  case K::Not:
  case K::NegL:
  case K::NegF:
  case K::NegD:
  case K::I2L:
  case K::L2I:
  case K::I2B:
  case K::I2C:
  case K::I2S:
  case K::I2F:
  case K::I2D:
  case K::L2F:
  case K::L2D:
  case K::F2I:
  case K::F2L:
  case K::F2D:
  case K::D2I:
  case K::D2L:
  case K::D2F:
    return true;
  default:
    return false;
  }
}

// Two-input kinds the shared evaluator understands (foldIntBin /
// foldLongBin / foldFloatBin / foldDoubleBin + the NaN-tolerant Cmp*).
[[nodiscard]] bool isBinaryEvalK(ir::NodeKind k) {
  using K = ir::NodeKind;
  switch (k) {
  case K::AddI: case K::SubI: case K::MulI: case K::DivI: case K::RemI:
  case K::ShlI: case K::ShrI: case K::UShrI:
  case K::AndI: case K::OrI: case K::XorI:
  case K::AddL: case K::SubL: case K::MulL: case K::DivL: case K::RemL:
  case K::ShlL: case K::ShrL: case K::UShrL:
  case K::AndL: case K::OrL: case K::XorL:
  case K::AddF: case K::SubF: case K::MulF: case K::DivF: case K::RemF:
  case K::AddD: case K::SubD: case K::MulD: case K::DivD: case K::RemD:
  case K::CmpI: case K::CmpL:
  case K::CmpFl: case K::CmpFg: case K::CmpDl: case K::CmpDg:
  case K::EqI: case K::NeI:
  case K::LtI: case K::LeI: case K::GtI: case K::GeI:
    return true;
  default:
    return false;
  }
}

// The lattice-evaluable set: shared-evaluator kinds + the value facts.
[[nodiscard]] bool isEvalK(ir::NodeKind k) {
  using K = ir::NodeKind;
  return isUnaryEvalK(k) || isBinaryEvalK(k) || k == K::IsNull ||
         k == K::RefEq || k == K::InstanceOf;
}

[[nodiscard]] bool isRegionLikeK(ir::NodeKind k) {
  return k == ir::NodeKind::Region || k == ir::NodeKind::LoopBegin;
}

// The apply gate (defensive - only Eval/Phi kinds can ever be Const):
// floating values only; nothing pinned or control-consuming is replaced.
[[nodiscard]] bool isSccpReplaceableK(ir::NodeKind k) {
  return isEvalK(k) || k == ir::NodeKind::Phi;
}

// --- the interned-constant cache key -------------------------------------------

struct ConstKey {
  CV flavor = CV::None;
  std::int32_t i = 0;
  std::int64_t l = 0;
  std::uint32_t fb = 0;
  std::uint64_t db = 0;

  [[nodiscard]] bool operator==(const ConstKey& o) const noexcept {
    return flavor == o.flavor && i == o.i && l == o.l && fb == o.fb &&
           db == o.db;
  }
};

[[nodiscard]] ConstKey keyOf(const ConstVal& c) noexcept {
  ConstKey k;
  k.flavor = c.flavor;
  k.i = c.i;
  k.l = c.l;
  k.fb = c.fb;
  k.db = c.db;
  return k;
}

struct ConstKeyHash {
  [[nodiscard]] static std::uint64_t mix(std::uint64_t h,
                                         std::uint64_t v) noexcept {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
  }

  [[nodiscard]] std::size_t operator()(const ConstKey& k) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    h = mix(h, static_cast<std::uint64_t>(k.flavor));
    h = mix(h, static_cast<std::uint32_t>(k.i));
    h = mix(h, static_cast<std::uint64_t>(k.l));
    h = mix(h, k.fb);
    h = mix(h, k.db);
    return static_cast<std::size_t>(h);
  }
};

// --- the propagation state -------------------------------------------------------

struct Sccp {
  ir::Graph& g;
  std::vector<LatVal> val;
  std::vector<std::uint8_t> exec;
  // FIFO worklists (unique fixpoint regardless of order; FIFO drains
  // control before values for tighter early precision).
  std::vector<ir::NodeId> cfgQ;
  std::vector<ir::NodeId> ssaQ;
  std::size_t cfgHead = 0;
  std::size_t ssaHead = 0;
  std::vector<std::uint8_t> inCfg;
  std::vector<std::uint8_t> inSsa;
  PassTelemetry& t;
  Budget& b;
  const Junk& jk;

  Sccp(ir::Graph& graph, PassTelemetry& tel, Budget& bud, const Junk& junk)
      : g(graph), val(graph.nodeCount()), exec(graph.nodeCount(), 0),
        inCfg(graph.nodeCount(), 0), inSsa(graph.nodeCount(), 0), t(tel),
        b(bud), jk(junk) {}

  [[nodiscard]] std::uint32_t count() const noexcept {
    return g.nodeCount();
  }

  [[nodiscard]] bool live(ir::NodeId n) const {
    return n < count() && !g.node(n).isDead();
  }

  [[nodiscard]] const LatVal& valAt(ir::NodeId n) {
    if (n >= count()) {
      return botLatStatic;
    }
    return val[n];
  }

  static const LatVal botLatStatic;

  void pushCfg(ir::NodeId n) {
    if (!live(n) || n >= inCfg.size() || inCfg[n]) {
      return;
    }
    inCfg[n] = 1;
    cfgQ.push_back(n);
  }

  void pushSsa(ir::NodeId n) {
    if (!live(n) || n >= inSsa.size() || inSsa[n]) {
      return;
    }
    inSsa[n] = 1;
    ssaQ.push_back(n);
  }

  // Control flows into u (u is n's user in a Ctrl/Parent slot, or a
  // selected projection of n). Region-like nodes re-queue ALWAYS: a new
  // executable input means their phis must re-meet, even when the region
  // was already executable.
  void markCtrlUser(ir::NodeId u) {
    if (!live(u)) {
      return;
    }
    if (isRegionLikeK(g.node(u).kind)) {
      if (u < exec.size()) {
        exec[u] = 1; // an input is executable -> the merge is too
      }
      pushCfg(u);
      return;
    }
    if (u < exec.size() && !exec[u]) {
      exec[u] = 1;
      pushCfg(u);
    }
  }

  // Flow successors of control node n: live users consuming n in a Ctrl
  // or Parent slot (the exact successor rule the unreachable sweep uses;
  // memory edges are not control).
  void propagateCtrlUsers(ir::NodeId n) {
    for (const ir::Use& u : g.usesOf(n)) {
      if (!live(u.user)) {
        continue;
      }
      if (u.slot >= g.node(u.user).numInputs ||
          g.input(u.user, u.slot) != n) {
        continue; // stale entry
      }
      const ir::InputRole role = roleOfSlot(g, u.user, u.slot);
      if (role == ir::InputRole::Ctrl || role == ir::InputRole::Parent) {
        markCtrlUser(u.user);
      }
    }
  }

  // The live projection of `iff` with the wanted kind (Parent slot 0).
  [[nodiscard]] ir::NodeId projectionOf(ir::NodeId iff,
                                         ir::NodeKind want) const {
    for (const ir::Use& u : g.usesOf(iff)) {
      if (u.slot == 0 && live(u.user) && g.node(u.user).kind == want) {
        return u.user;
      }
    }
    return ir::kInvalidNodeId;
  }

  void markProjection(ir::NodeId iff, ir::NodeKind want) {
    const ir::NodeId p = projectionOf(iff, want);
    if (p != ir::kInvalidNodeId) {
      markCtrlUser(p);
    }
  }

  // --- control processing (exec[n] is set by the push protocol) ---------------

  void processCfg(ir::NodeId n) {
    if (!live(n) || n >= exec.size() || !exec[n]) {
      return; // re-pushed before its control arrived: nothing to do yet
    }
    const ir::Node& node = g.node(n);
    switch (node.kind) {
    case ir::NodeKind::If: {
      if (node.numInputs < 2) {
        return; // malformed: the verifier's business
      }
      const LatVal cond = valAt(g.input(n, 1));
      if (cond.lv == LV::Top) {
        return; // wait for the condition to resolve
      }
      if (cond.lv == LV::Const && cond.c.flavor == CV::I) {
        // Decided branch: only the taken projection flows.
        markProjection(n, cond.c.i != 0 ? ir::NodeKind::IfTrue
                                        : ir::NodeKind::IfFalse);
        return;
      }
      // Bot (or a malformed non-Int constant): both sides are possible.
      markProjection(n, ir::NodeKind::IfTrue);
      markProjection(n, ir::NodeKind::IfFalse);
      return;
    }
    case ir::NodeKind::Switch: {
      // Case ordinals are frontend-side opaque payloads (the documented
      // switch-folding blocker), so a constant selector cannot resolve
      // to a case: ALL projections are marked executable (sound
      // over-approximation - conservatism (b) in the contract).
      for (const ir::Use& u : g.usesOf(n)) {
        if (u.slot == 0 && live(u.user) &&
            (g.node(u.user).kind == ir::NodeKind::SwitchCase ||
             g.node(u.user).kind == ir::NodeKind::SwitchDefault)) {
          markCtrlUser(u.user);
        }
      }
      return;
    }
    case ir::NodeKind::Guard: {
      if (node.numInputs < 2) {
        return;
      }
      const LatVal cond = valAt(g.input(n, 1));
      if (cond.lv == LV::Top) {
        return; // wait for the condition to resolve
      }
      if (cond.lv == LV::Const && cond.c.flavor == CV::I &&
          cond.c.i == 0) {
        return; // always-deopt guard: flow stops (branchnorm makes the
                // Deopt terminal; the sweep reclaims the continuation)
      }
      propagateCtrlUsers(n); // may pass (Bot) or always passes (const!=0)
      return;
    }
    case ir::NodeKind::Region:
    case ir::NodeKind::LoopBegin: {
      // A new executable input arrived: the phis must re-meet (the
      // optimistic loop phi gains its backedge exactly here), then the
      // merged control flows onward.
      for (const ir::Use& u : g.usesOf(n)) {
        if (u.slot == 0 && live(u.user) &&
            g.node(u.user).kind == ir::NodeKind::Phi) {
          pushSsa(u.user);
        }
      }
      propagateCtrlUsers(n);
      return;
    }
    case ir::NodeKind::Return:
    case ir::NodeKind::Unwind:
    case ir::NodeKind::Deopt:
    case ir::NodeKind::End:
      return; // terminators: flow stops
    default:
      // Start, projections, calls (CallExcept rides the Parent edge),
      // loads/stores/allocations/monitors/LoopEnd/LoopExit/MemBar/
      // ClassInit/CheckCast/RepTransitionGuard: control flows through.
      // (RepTransitionGuard deopts on an unexpected TAG, which no value
      // lattice can decide - conservative pass-through.)
      propagateCtrlUsers(n);
      return;
    }
  }

  // --- value evaluation ---------------------------------------------------------

  // The operand's current lattice value (Bot for out-of-range inputs -
  // malformed graphs refuse to fold, which is the safe direction).
  [[nodiscard]] LatVal operand(ir::NodeId n, std::uint16_t slot) {
    const ir::Node& node = g.node(n);
    if (slot >= node.numInputs) {
      return botLat();
    }
    return valAt(g.input(n, slot));
  }

  [[nodiscard]] ir::NodeId inputOf(ir::NodeId n, std::uint16_t slot) const {
    const ir::Node& node = g.node(n);
    return slot < node.numInputs ? g.input(n, slot) : ir::kInvalidNodeId;
  }

  // Phi: meet over the values arriving on EXECUTABLE region inputs only
  // (the conditional in SCCP). A not-yet-executable edge contributes Top
  // - the optimism that resolves loop-invariant phis. A dead or
  // arity-mismatched input on a live edge contributes Bot (malformed:
  // refuse to claim). Values on non-executable edges are ignored.
  [[nodiscard]] LatVal evalPhi(ir::NodeId n) {
    const ir::Node& node = g.node(n);
    const ir::NodeId region = inputOf(n, 0);
    if (!live(region) || !isRegionLikeK(g.node(region).kind)) {
      return botLat(); // malformed: the verifier's business, be safe
    }
    if (region >= exec.size() || !exec[region]) {
      return topLat(); // no executable edge yet: wait
    }
    const ir::Node& reg = g.node(region);
    LatVal acc = topLat();
    for (std::uint16_t j = 0; j < reg.numInputs; ++j) {
      const ir::NodeId pred = g.input(region, j);
      if (!live(pred) || pred >= exec.size() || !exec[pred]) {
        continue; // this edge does not flow
      }
      if (j + 1 >= node.numInputs) {
        return botLat(); // arity mismatch: malformed, refuse
      }
      const ir::NodeId v = g.input(n, j + 1);
      acc = meet(acc, valAt(v));
    }
    return acc;
  }

  // One floating value node's new lattice value from its operands.
  [[nodiscard]] LatVal evalNode(ir::NodeId n) {
    const ir::Node& node = g.node(n);
    const ir::NodeKind k = node.kind;
    if (k == ir::NodeKind::Phi) {
      return evalPhi(n);
    }

    if (k == ir::NodeKind::IsNull || k == ir::NodeKind::InstanceOf) {
      const ir::NodeId x = inputOf(n, 0);
      const LatVal a = operand(n, 0);
      if (a.lv == LV::Const && a.c.flavor == CV::Null) {
        // null == null, and null instanceof T is JLS-defined false.
        return constLat(constOfI(k == ir::NodeKind::IsNull ? 1 : 0));
      }
      if (live(x) && isNeverNullNode(g, x)) {
        return constLat(constOfI(0)); // a provably non-null receiver
      }
      if (a.lv == LV::Top) {
        return topLat();
      }
      return botLat();
    }

    if (k == ir::NodeKind::RefEq) {
      const ir::NodeId aIn = inputOf(n, 0);
      const ir::NodeId bIn = inputOf(n, 1);
      if (aIn != ir::kInvalidNodeId && aIn == bIn) {
        return constLat(constOfI(1)); // same SSA value: identity holds
      }
      const LatVal a = operand(n, 0);
      const LatVal bv = operand(n, 1);
      const bool aNull = a.lv == LV::Const && a.c.flavor == CV::Null;
      const bool bNull = bv.lv == LV::Const && bv.c.flavor == CV::Null;
      if (aNull && bNull) {
        return constLat(constOfI(1));
      }
      if ((aNull && live(bIn) && isNeverNullNode(g, bIn)) ||
          (bNull && live(aIn) && isNeverNullNode(g, aIn))) {
        return constLat(constOfI(0));
      }
      if (a.lv == LV::Top || bv.lv == LV::Top) {
        return topLat();
      }
      return botLat();
    }

    if (isUnaryEvalK(k)) {
      const LatVal a = operand(n, 0);
      if (a.lv == LV::Bot) {
        return botLat();
      }
      if (a.lv == LV::Top) {
        return topLat();
      }
      const auto r = evalUnaryOp(k, a.c);
      return r ? constLat(*r) : botLat(); // fold refusal (trap, NaN): Bot
    }

    // isBinaryEvalK(k)
    const LatVal a = operand(n, 0);
    const LatVal bv = operand(n, 1);
    if (a.lv == LV::Bot || bv.lv == LV::Bot) {
      return botLat();
    }
    if (a.lv == LV::Top || bv.lv == LV::Top) {
      return topLat();
    }
    const auto r = evalBinOp(k, a.c, bv.c);
    return r ? constLat(*r) : botLat();
  }

  // On a value descent, the consumers that must wake up: floating
  // values re-evaluate; If/Guard re-derive their branch decision (Switch
  // decisions are selector-independent in v1, but the requeue is cheap
  // and keeps the rule uniform).
  void pushUsersOf(ir::NodeId n) {
    for (const ir::Use& u : g.usesOf(n)) {
      if (!live(u.user)) {
        continue;
      }
      const ir::NodeKind uk = g.node(u.user).kind;
      if (uk == ir::NodeKind::If || uk == ir::NodeKind::Guard ||
          uk == ir::NodeKind::Switch) {
        pushCfg(u.user);
      } else if (isEvalK(uk) || uk == ir::NodeKind::Phi) {
        pushSsa(u.user);
      }
    }
  }

  void processSsa(ir::NodeId n) {
    if (!live(n)) {
      return;
    }
    const ir::Node& node = g.node(n);
    if (!(isEvalK(node.kind) || node.kind == ir::NodeKind::Phi)) {
      return; // defensive: only floating values live on this queue
    }
    const LatVal nv = evalNode(n);
    if (nv.lv > val[n].lv) { // strictly lower in the lattice: descent
      val[n] = nv;
      pushUsersOf(n);
    }
  }

  // Drain both queues; control first (tighter early executability).
  void propagate() {
    while (cfgHead < cfgQ.size() || ssaHead < ssaQ.size()) {
      if (!b.charge()) {
        return; // exceeded flagged: the caller aborts before any rewrite
      }
      if (cfgHead < cfgQ.size()) {
        const ir::NodeId n = cfgQ[cfgHead++];
        if (n < inCfg.size()) {
          inCfg[n] = 0;
        }
        processCfg(n);
      } else {
        const ir::NodeId n = ssaQ[ssaHead++];
        if (n < inSsa.size()) {
          inSsa[n] = 0;
        }
        processSsa(n);
      }
    }
  }

  // The completion rule: unresolved (Top) floating values with at least
  // one LIVE user are overdefined - forced to Bot and re-propagated.
  // Userless Top values (dead-code leftovers) stay Top: nobody observes
  // them, and leaving them alone keeps the pass idempotent-cheap.
  // Returns true when anything was forced (caller re-propagates).
  bool complete() {
    bool forced = false;
    for (ir::NodeId n = 0; n < count(); ++n) {
      if (!live(n) || val[n].lv != LV::Top) {
        continue;
      }
      const ir::NodeKind k = g.node(n).kind;
      if (!(isEvalK(k) || k == ir::NodeKind::Phi)) {
        continue; // control/state/seeded nodes never resolve further
      }
      if (liveUseCount(g, n) == 0) {
        continue;
      }
      val[n] = botLat();
      pushUsersOf(n);
      forced = true;
    }
    return forced;
  }

  // --- the apply phase ------------------------------------------------------------

  // Cycle-safe phi typing for the belt below: ir::resultTypeOf joins a
  // phi's INPUT types, and a loop phi's backedge value can be the phi
  // ITSELF (the loop-invariant self marker) - the naive walk would
  // recurse forever. On-path nodes contribute nothing to the join (the
  // self marker's type is by definition the phi's own join).
  [[nodiscard]] static ir::IRType typeOfNoCycle(const ir::Graph& g,
                                                ir::NodeId n,
                                                std::vector<ir::NodeId>& onPath) {
    const ir::Node& node = g.node(n);
    if (node.kind != ir::NodeKind::Phi) {
      return resultTypeOf(g, n);
    }
    for (const ir::NodeId p : onPath) {
      if (p == n) {
        return ir::IRType::Bottom; // revisit: contribute nothing
      }
    }
    onPath.push_back(n);
    ir::IRType t = ir::IRType::Bottom;
    bool seen = false;
    for (std::uint16_t s = 1; s < node.numInputs; ++s) {
      const ir::NodeId in = g.input(n, s);
      bool onPathAlready = false;
      for (const ir::NodeId p : onPath) {
        if (p == in) {
          onPathAlready = true;
          break;
        }
      }
      if (onPathAlready) {
        continue; // the self/recursive marker's type IS this phi's type:
                  // exclude it (Bottom is NOT the join identity - an
                  // on-path contribution would poison the join to Bottom)
      }
      const ir::IRType v = typeOfNoCycle(g, in, onPath);
      t = seen ? join(t, v) : v;
      seen = true;
    }
    onPath.pop_back();
    return t;
  }

  // The replacement's type must equal the node's result type (verified
  // graphs guarantee this by construction - the evaluator's flavors and
  // the phi meet derive from verifier-typed inputs; the belt refuses a
  // retyping replacement on hand-built drift).
  [[nodiscard]] static ir::IRType flavorType(CV f) {
    switch (f) {
    case CV::I:
      return ir::IRType::Int;
    case CV::L:
      return ir::IRType::Long;
    case CV::F:
      return ir::IRType::Float;
    case CV::D:
      return ir::IRType::Double;
    case CV::Null:
      return ir::IRType::Null;
    default:
      return ir::IRType::Bottom;
    }
  }

  void apply() {
    std::unordered_map<ConstKey, ir::NodeId, ConstKeyHash> intern;
    // Frozen at entry: interned constants append NEW nodes while this
    // loop runs, and the new nodes are already constants (never replace
    // candidates). An unfrozen bound would walk past val[]'s size.
    const std::uint32_t applyCount = count();
    for (ir::NodeId n = 0; n < applyCount && !b.exceeded; ++n) {
      if (!live(n) || val[n].lv != LV::Const) {
        continue;
      }
      const ir::Node& node = g.node(n);
      if (!isSccpReplaceableK(node.kind)) {
        continue; // constants themselves, control, state: never replaced
      }
      std::vector<ir::NodeId> onPath;
      if (flavorType(val[n].c.flavor) != typeOfNoCycle(g, n, onPath)) {
        continue; // defensive type belt
      }
      const ConstKey key = keyOf(val[n].c);
      auto it = intern.find(key);
      if (it == intern.end() || !live(it->second)) {
        const ir::NodeId rep = makeConstNode(g, val[n].c);
        if (rep == ir::kInvalidNodeId) {
          continue; // flavor None cannot happen for Const lattice values
        }
        it = intern.insert_or_assign(intern.begin(), key, rep);
      }
      const std::uint32_t rewritesBefore = t.rewrites;
      replace(g, n, it->second, t, b, jk);
      if (t.rewrites != rewritesBefore) {
        ++t.sccpConstants;
      }
    }
  }
};

const LatVal Sccp::botLatStatic = LatVal{LV::Bot, ConstVal{}};

} // namespace

void runSCCP(ir::Graph& g, PassTelemetry& t, Budget& b, const Junk& jk) {
  Sccp s(g, t, b, jk);

  // Seed (id order, Rule 124): constants resolve to themselves, floating
  // evaluable values + phis start Top (optimistic), everything else is
  // Bot from the start (parameters, symbols, Undef, vectors, tags, and
  // every control/pinned/memory/call value - loads, calls, allocations,
  // Start's memory state: never compile-time constants).
  const ir::NodeId start = g.startNode();
  for (ir::NodeId n = 0; n < s.count(); ++n) {
    const ir::Node& node = g.node(n);
    if (node.isDead()) {
      continue;
    }
    const ir::NodeKind k = node.kind;
    if (k == ir::NodeKind::ConstantI || k == ir::NodeKind::ConstantL ||
        k == ir::NodeKind::ConstantF || k == ir::NodeKind::ConstantD ||
        k == ir::NodeKind::ConstantNull) {
      s.val[n] = constLat(constOf(g, n));
      continue;
    }
    if (n == start) {
      if (n < s.exec.size()) {
        s.exec[n] = 1;
      }
      s.pushCfg(n);
      // Start also produces the initial memory state, so it can appear
      // as a mem-phi value input; its lattice entry stays Top (control
      // nodes are never forced by completion), which the mem-phi meet
      // treats as unresolved: memory phis never claim a constant. Sound.
      continue;
    }
    if (isEvalK(k) || k == ir::NodeKind::Phi) {
      s.val[n] = topLat();
      s.pushSsa(n); // eager first evaluation
      continue;
    }
    // Control/state/pinned/opaque: Bot (a live FrameState user of a Top
    // value would churn; Bot is the honest "no constant" answer).
    s.val[n] = botLat();
  }

  // PROPAGATE -> COMPLETE -> (stable) -> APPLY.
  s.propagate();
  if (b.exceeded) {
    return; // no rewrite from a partial fixpoint (unsound claims)
  }
  while (s.complete()) {
    s.propagate();
    if (b.exceeded) {
      return;
    }
  }
  s.apply();
}

} // namespace b2::passes::detail
