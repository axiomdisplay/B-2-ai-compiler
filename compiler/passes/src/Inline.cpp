// B-2 Passes - the T2 inlining driver: the ICDG direct-inline engine.
//
// WHY THIS FILE EXISTS:
// This is the executable side of docs/inlining.md (the normative v1
// contract) over docs/icdg.md (the decision engine design). The driver
// walks the caller graph in node-id order, resolves direct-call targets
// through the CalleeSource (the id-space authority), applies the v1 cost
// model (Rule 45: refusals are budgeted, not guessed), re-builds each
// profitable callee INTO the caller graph through the builder's inline
// seam (detail::buildInlineBody), merges the exit states, rewires the
// call's users by role, and kills the call + its exceptional projection
// under the tombstone law (detail::kill). Every decision - inline or
// refuse - lands in the decision log with a structured reason
// (icdg.md 19). The IR verifier runs after every site (Rule 40);
// failures stop the run fail-closed.
//
// Determinism (Rule 124): sites in id order; a site's body sites are
// processed immediately after it (depth + 1) in id order; trials are
// cached per callee body pointer; node creation order is the builder's.
//
// SOUNDNESS CORE (docs/inlining.md section 4):
// - control: the callee's entry control is the call's control
//   predecessor; every return exit becomes a predecessor of the exit
//   merge (Region; single exits wire directly).
// - memory: the callee's entry memory is the call's memory predecessor;
//   each exit's memory state merges through a memory Phi (memory-state
//   producers only - the sea-of-nodes memory chain never dangles).
// - values: parameters are the caller's argument defs (receiver first);
//   the merged return value replaces the call in value position.
// - deopt: every callee FrameState chains to the call-site snapshot
//   (FrameStateDesc.caller) so the deoptimizer reconstructs the inlined
//   frame stack (Rule 75; the runtime stitching contract is
//   interp_contract.md section 3).
// - exceptions: escapes route through the call-site policy - covered
//   sites deopt (T0 re-enters THE EXCEPTION ALGORITHM at the callee's
//   escape pc, finds no handler, unwinds the callee frame, and the deopt
//   runtime re-enters the caller at the call pc with the pending
//   exception); uncovered sites keep the callee's Unwind (the exception
//   value propagates out of the caller directly).

#include "b2/passes/Inline.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "PassInternal.h"
#include "b2/ir/Node.h"
#include "b2/ir/Types.h"
#include "b2/ir/Verifier.h"

namespace b2::passes {

namespace {

// --- reason strings (icdg.md 19: static, no per-decision allocation) --------

constexpr const char* kRInlined =
    "direct inline: resolvable single body (static proof)";
constexpr const char* kRUnresolved =
    "target unresolved (external method or ambiguous id)";
constexpr const char* kRFlags =
    "callee flags refuse inlining (synchronized/abstract/native)";
constexpr const char* kRInsns = "callee too large (instruction cap)";
constexpr const char* kRSlots = "callee frame too wide (slot cap)";
constexpr const char* kRNoReturn =
    "callee has no normal return path (v1 refuses)";
constexpr const char* kRBuild = "callee does not build (builder refusal)";
constexpr const char* kRNodes = "callee node cost over cap";
constexpr const char* kRArgs =
    "argument window does not cover the callee parameters";
constexpr const char* kRRet = "return type disagrees with the call site";
constexpr const char* kRRecursion =
    "recursive cycle (callee already on the inline stack)";
constexpr const char* kRDepth = "inline depth cap";
constexpr const char* kRSites = "graph site budget";
constexpr const char* kRNodesGraph = "graph node budget";
constexpr const char* kRShape =
    "call-site shape is not builder-canonical (missing/odd exceptional "
    "continuation)";
constexpr const char* kRInconsistent =
    "inline build failed after a successful trial (driver defect: "
    "fail-closed)";

// --- helpers ----------------------------------------------------------------

[[nodiscard]] ir::IRType localRtypeToIr(rbc::RType t) noexcept {
  switch (t) {
  case rbc::RType::Int:
    return ir::IRType::Int;
  case rbc::RType::Long:
    return ir::IRType::Long;
  case rbc::RType::Float:
    return ir::IRType::Float;
  case rbc::RType::Double:
    return ir::IRType::Double;
  case rbc::RType::Null:
    return ir::IRType::Null;
  case rbc::RType::Ref:
    return ir::IRType::Ref;
  case rbc::RType::Bottom:
  default:
    return ir::IRType::Bottom;
  }
}

// Highest deopt id allocated anywhere in the graph (Deopt payload /
// Guard payload2; tombstones included - ids stay assigned). The next
// inline body seeds its allocation above this.
[[nodiscard]] std::uint32_t maxDeoptId(const ir::Graph& g) {
  std::uint32_t max = 0;
  for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
    const ir::Node& nd = g.node(n);
    if (nd.kind == ir::NodeKind::Deopt) {
      max = std::max(max, nd.payload);
    } else if (nd.kind == ir::NodeKind::Guard) {
      max = std::max(max, nd.payload2);
    }
  }
  return max;
}

// The exceptional continuation of one call site (builder-canonical
// shape: one CallExcept projection of the call, with exactly one live
// continuation user - Deopt [except, fs] when the caller's handler table
// covers the call pc, Unwind [except, except] otherwise).
struct SiteCont {
  ir::NodeId except = ir::kInvalidNodeId;
  ir::NodeId node = ir::kInvalidNodeId; // the continuation (to be killed)
  bool covered = false;
  bool ok = false;
};

[[nodiscard]] SiteCont siteContinuation(const ir::Graph& g,
                                        ir::NodeId call) {
  SiteCont sc;
  ir::NodeId except = ir::kInvalidNodeId;
  for (const ir::Use& u : g.usesOf(call)) {
    if (g.node(u.user).kind == ir::NodeKind::CallExcept) {
      except = u.user;
      break;
    }
  }
  if (except == ir::kInvalidNodeId) {
    return sc; // no projection: not builder output
  }
  sc.except = except;
  // Exactly one live continuation user (terminals have no users; the
  // builder creates exactly one Deopt or one Unwind on the projection).
  ir::NodeId cont = ir::kInvalidNodeId;
  for (const ir::Use& u : g.usesOf(except)) {
    if (!g.node(u.user).isDead()) {
      if (cont != ir::kInvalidNodeId && cont != u.user) {
        return sc; // two distinct live users: not canonical
      }
      cont = u.user;
    }
  }
  if (cont == ir::kInvalidNodeId) {
    return sc; // continuation already gone: not canonical
  }
  if (g.node(cont).kind == ir::NodeKind::Deopt &&
      g.numInputs(cont) >= 1 && g.input(cont, 0) == except) {
    sc.node = cont;
    sc.covered = true;
    sc.ok = true;
  } else if (g.node(cont).kind == ir::NodeKind::Unwind &&
             g.numInputs(cont) >= 1 && g.input(cont, 0) == except) {
    sc.node = cont;
    sc.covered = false;
    sc.ok = true;
  }
  return sc;
}

// Trial cache entry: does the callee build standalone, and how big is it?
struct Trial {
  bool built = false;
  bool ok = false;
  std::uint32_t nodes = 0;   // trial graph size incl. Start/Parameters
  std::uint32_t returns = 0; // reachable return terminals
  std::string diag;
};

// --- the driver ---------------------------------------------------------------

class Inliner {
 public:
  Inliner(ir::Graph& g, InlineCalleeSource& src, const InlineConfig& cfg,
          InlineResult& out)
      : g_(g), src_(src), cfg_(cfg), out_(out), jk_(g) {}

  void run() {
    if (!cfg_.enabled) {
      return; // kill switch: successful no-op (Rules 132/144)
    }
    const ir::NodeId rootLimit = g_.nodeCount();
    std::vector<const rbc::Method*> stack;
    processRange(1, rootLimit, 1, stack);
    out_.telemetry.converged = out_.telemetry.budgetStops == 0;
  }

 private:
  // Processes live CallStatic sites with ids in [first, limit) at `depth`
  // (recursion: a successful inline's own body sites run at depth + 1
  // with the callee pushed on the stack - recursion control, icdg.md 12).
  // Junk sinks (this run's tombstone-law anchors, including the call
  // sinks the kills create) are never sites.
  void processRange(ir::NodeId first, ir::NodeId limit, std::uint32_t depth,
                    std::vector<const rbc::Method*>& stack) {
    for (ir::NodeId id = first; id < limit && out_.ok; ++id) {
      const ir::Node& nd = g_.node(id);
      if (nd.isDead() || nd.kind != ir::NodeKind::CallStatic) {
        continue;
      }
      if (detail::isJunkSink(jk_, id)) {
        continue; // a tombstone-law anchor, not a call site
      }
      ++out_.telemetry.sitesConsidered;
      const ir::NodeId before = g_.nodeCount();
      const rbc::Method* inlined = tryInlineSite(id, depth, stack);
      if (inlined == nullptr || !out_.ok) {
        continue;
      }
      // The body (and its exit merge) landed in [before, nodeCount()):
      // its own call sites are the next tier, recursion-controlled.
      stack.push_back(inlined);
      processRange(before, g_.nodeCount(), depth + 1, stack);
      stack.pop_back();
      if (depth > out_.telemetry.maxDepthReached) {
        out_.telemetry.maxDepthReached = depth;
      }
    }
  }

  // One site. Returns the inlined callee body (null = refused or failed).
  [[nodiscard]] const rbc::Method* tryInlineSite(
      ir::NodeId call, std::uint32_t depth,
      const std::vector<const rbc::Method*>& stack) {
    InlineDecision d;
    d.call = call;
    d.depth = depth;
    d.target = g_.node(call).payload;

    // Graph-level budgets first (site cap; the node cap needs the trial).
    if (out_.telemetry.sitesInlined >= cfg_.maxSitesPerGraph) {
      (void)refuse(d, kRSites, /*budgetStop=*/true);
      return nullptr;
    }

    // Resolve the target through the id-space authority.
    InlineCallee callee = src_.resolve(d.target);
    if (!callee.resolved()) {
      (void)refuse(d, kRUnresolved, false);
      return nullptr;
    }
    const rbc::Method& m = *callee.method;
    d.target = callee.frameMethodId;

    // Clean flag refusals (the builder would refuse too; the reason is
    // better without a trial).
    if (m.isSynchronized() || m.isAbstract() ||
        (m.flags & rbc::method_flags::Native) != 0) {
      (void)refuse(d, kRFlags, false);
      return nullptr;
    }

    // Cost model, RBC-level caps (Rule 23 constants; Rule 45 cost model).
    d.calleeInsns = static_cast<std::uint32_t>(m.code.size());
    d.calleeSlots = m.numLocals + m.numRegs;
    if (d.calleeInsns > cfg_.maxCalleeInsns) {
      (void)refuse(d, kRInsns, false);
      return nullptr;
    }
    if (d.calleeSlots > cfg_.maxCalleeSlots) {
      (void)refuse(d, kRSlots, false);
      return nullptr;
    }

    // Recursion control (icdg.md 12): the callee on the current inline
    // stack is a recursive cycle; depth beyond the cap is refused.
    if (depth > cfg_.maxDepth) {
      (void)refuse(d, kRDepth, false);
      return nullptr;
    }
    for (const rbc::Method* s : stack) {
      if (s == callee.method) {
        (void)refuse(d, kRRecursion, false);
        return nullptr;
      }
    }

    // Site shape: the exceptional continuation must be builder-canonical.
    const SiteCont sc = siteContinuation(g_, call);
    if (!sc.ok) {
      (void)refuse(d, kRShape, false);
      return nullptr;
    }

    // Argument window + return-type consistency (the verified-RBC
    // guarantees, re-checked defensively: the site must feed the callee
    // exactly what its frame model expects).
    const std::uint16_t nIn = g_.numInputs(call);
    if (nIn < 3) {
      (void)refuse(d, kRShape, false);
      return nullptr;
    }
    const ir::NodeId fsNode = g_.input(call, nIn - 1);
    if (g_.node(fsNode).kind != ir::NodeKind::FrameState ||
        g_.node(fsNode).isDead()) {
      (void)refuse(d, kRShape, false);
      return nullptr;
    }
    std::vector<ir::NodeId> args;
    args.reserve(nIn - 3);
    for (std::uint16_t s = 2; s + 1 < nIn; ++s) {
      args.push_back(g_.input(call, s));
    }
    const std::uint32_t calleeNeed =
        rbc::paramCount(m.descriptor) + (m.isStatic() ? 0u : 1u);
    if (args.size() < calleeNeed) {
      (void)refuse(d, kRArgs, false);
      return nullptr;
    }
    const ir::IRType retCall =
        static_cast<ir::IRType>(g_.node(call).payload2);
    const ir::IRType retCallee =
        localRtypeToIr(rbc::parseReturn(m.descriptor));
    if (retCall != retCallee) {
      (void)refuse(d, kRRet, false);
      return nullptr;
    }

    // Trial build (per-callee cache): the callee must build standalone.
    const Trial& tr = trialOf(callee.method);
    d.calleeNodes = tr.nodes;
    if (!tr.ok) {
      (void)refuse(d, kRBuild, false);
      return nullptr;
    }
    if (tr.returns == 0) {
      (void)refuse(d, kRNoReturn, false);
      return nullptr;
    }
    if (tr.nodes > cfg_.maxCalleeNodes) {
      (void)refuse(d, kRNodes, false);
      return nullptr;
    }
    if (out_.telemetry.nodesAdded + tr.nodes > cfg_.maxNodesPerGraph) {
      (void)refuse(d, kRNodesGraph, /*budgetStop=*/true);
      return nullptr;
    }

    // The real build: the callee's RBC -> IR, into this graph, wired to
    // the call site (trusted seam; the trial guarantees buildability).
    detail::InlineSiteWiring w;
    w.entryCtrl = g_.input(call, 0);
    w.entryMem = g_.input(call, 1);
    w.args = std::span<const ir::NodeId>(args);
    w.fsCaller = g_.node(fsNode).payload;
    w.siteCovered = sc.covered;
    w.deoptIdSeed = 1 + maxDeoptId(g_);
    detail::InlineBodyResult body = detail::buildInlineBody(
        m, src_.resolver(), g_, callee.frameMethodId, w);
    if (!body.ok || body.exits.empty()) {
      // Must not happen after a successful trial: fail closed with the
      // builder's own diagnostics (never a silently wrong graph).
      failClosed(call, body.diags.empty()
                           ? std::string(kRInconsistent)
                           : ("inline body: " + body.diags[0].message));
      return nullptr;
    }

    // Exit merge: N == 1 wires directly; N >= 2 builds the Region, the
    // memory phi, and (non-void) the value phi. Every exit memory is a
    // memory-state producer by construction (the builder only chains
    // producers into curMem_).
    ir::NodeId exitCtrl = ir::kInvalidNodeId;
    ir::NodeId exitMem = ir::kInvalidNodeId;
    ir::NodeId exitValue = ir::kInvalidNodeId;
    const bool wantsValue = retCall != ir::IRType::Bottom;
    if (body.exits.size() == 1) {
      exitCtrl = body.exits[0].ctrl;
      exitMem = body.exits[0].mem;
      exitValue = body.exits[0].value;
      if (wantsValue && exitValue == ir::kInvalidNodeId) {
        failClosed(call, kRInconsistent);
        return nullptr;
      }
    } else {
      std::vector<ir::NodeId> ctrls;
      std::vector<ir::NodeId> mems;
      std::vector<ir::NodeId> vals;
      ctrls.reserve(body.exits.size());
      mems.reserve(body.exits.size());
      for (const detail::InlineExit& e : body.exits) {
        ctrls.push_back(e.ctrl);
        mems.push_back(e.mem);
        if (e.value != ir::kInvalidNodeId) {
          vals.push_back(e.value);
        }
      }
      if (wantsValue && vals.size() != body.exits.size()) {
        failClosed(call, kRInconsistent);
        return nullptr;
      }
      exitCtrl = g_.make(ir::NodeKind::Region, ctrls);
      std::vector<ir::NodeId> memPhi;
      memPhi.push_back(exitCtrl);
      memPhi.insert(memPhi.end(), mems.begin(), mems.end());
      exitMem = g_.make(ir::NodeKind::Phi, memPhi);
      if (wantsValue) {
        std::vector<ir::NodeId> valPhi;
        valPhi.push_back(exitCtrl);
        valPhi.insert(valPhi.end(), vals.begin(), vals.end());
        exitValue = g_.make(ir::NodeKind::Phi, valPhi);
      }
      ++out_.telemetry.exitMerges;
    }

    // Rewire every LIVE user of the call by its slot's role (the call
    // produced control AND memory AND a value; one node cannot replace
    // all three, so the rewire is per-role). The Parent slot (the
    // CallExcept) is left for the kill sequence. Use-list snapshot: the
    // rewire mutates it.
    {
      const std::vector<ir::Use> uses(g_.usesOf(call).begin(),
                                      g_.usesOf(call).end());
      for (const ir::Use& u : uses) {
        if (g_.node(u.user).isDead()) {
          continue; // tombstone referencers: the kill junks them
        }
        const ir::InputRole role = detail::roleOfSlot(g_, u.user, u.slot);
        switch (role) {
        case ir::InputRole::Ctrl:
          g_.setInput(u.user, u.slot, exitCtrl);
          break;
        case ir::InputRole::Mem:
          g_.setInput(u.user, u.slot, exitMem);
          break;
        case ir::InputRole::Data:
          if (exitValue == ir::kInvalidNodeId) {
            failClosed(call, "void call has live value users");
            return nullptr;
          }
          g_.setInput(u.user, u.slot, exitValue);
          break;
        case ir::InputRole::Parent:
        case ir::InputRole::FrameState:
        case ir::InputRole::None:
          break; // the projection parent and (impossible) fs producer
        }
        if (!out_.ok) {
          return nullptr;
        }
      }
    }

    // Kill sequence (tombstone law): the site's old exceptional
    // continuation dies first (terminals have no users), then the
    // CallExcept projection (its only live user just died), then the
    // call itself (all live users rewired above).
    {
      PassTelemetry killTel; // Rule 26 accounting folded into out_ below
      detail::Budget b{cfg_.maxCalleeNodes * 4 + 64};
      if (!detail::kill(g_, sc.node, killTel, b, jk_) ||
          !detail::kill(g_, sc.except, killTel, b, jk_) ||
          !detail::kill(g_, call, killTel, b, jk_)) {
        failClosed(call, "tombstone-law kill refused (live referencers "
                         "remained: driver defect)");
        return nullptr;
      }
      out_.telemetry.removals += killTel.removals;
    }

    // Rule 40: the verifier runs after every site; failure stops the run
    // fail-closed (the graph is left in the post-last-good-site state).
    {
      const ir::VerifyResult v = ir::verify(g_);
      if (!v.ok) {
        for (const ir::VerifyDiag& vd : v.diags) {
          if (out_.diags.size() >= 32) {
            break;
          }
          InlineDiag dgn;
          dgn.node = call;
          dgn.message = "IR verify failed at n" +
                        std::to_string(vd.node) + ": " + vd.message;
          out_.diags.push_back(std::move(dgn));
        }
        out_.ok = false;
        return nullptr;
      }
    }

    // Success: record the decision and the telemetry.
    d.action = InlineAction::DirectInline;
    d.reason = kRInlined;
    out_.decisions.push_back(d);
    ++out_.telemetry.sitesInlined;
    out_.telemetry.nodesAdded += body.nodesAdded;
    out_.telemetry.deoptsEmitted += body.deoptsEmitted;
    return callee.method;
  }

  [[nodiscard]] const rbc::Method* refuse(InlineDecision& d,
                                          const char* reason,
                                          bool budgetStop) {
    d.action = InlineAction::KeepIndirect;
    d.reason = reason;
    out_.decisions.push_back(d);
    ++out_.telemetry.sitesRefused;
    if (budgetStop) {
      ++out_.telemetry.budgetStops;
    }
    return nullptr;
  }

  void failClosed(ir::NodeId call, std::string message) {
    InlineDiag dgn;
    dgn.node = call;
    dgn.message = std::move(message);
    if (out_.diags.size() < 32) {
      out_.diags.push_back(std::move(dgn));
    }
    out_.ok = false;
  }

  // Trial build cache (per body pointer; the program owns the bodies).
  [[nodiscard]] const Trial& trialOf(const rbc::Method* m) {
    const auto it = trials_.find(m);
    if (it != trials_.end()) {
      return it->second;
    }
    Trial t;
    ir::Graph scratch;
    const BuildResult br =
        buildGraph(*m, src_.resolver(), scratch, ir::MethodId{0});
    t.ok = br.ok;
    t.nodes = scratch.nodeCount();
    t.diag = br.diags.empty() ? std::string() : br.diags[0].message;
    for (ir::NodeId n = 0; n < scratch.nodeCount(); ++n) {
      if (!scratch.node(n).isDead() &&
          scratch.node(n).kind == ir::NodeKind::Return) {
        ++t.returns;
      }
    }
    return trials_.emplace(m, std::move(t)).first->second;
  }

  ir::Graph& g_;
  InlineCalleeSource& src_;
  const InlineConfig& cfg_;
  InlineResult& out_;
  detail::Junk jk_;
  std::unordered_map<const rbc::Method*, Trial> trials_;
};

} // namespace

// --- ProgramCalleeSource -------------------------------------------------------

namespace {

// First-encounter interning (deterministic; linear scan is fine for the
// small pools the v0 programs produce).
[[nodiscard]] std::uint32_t internKey(std::vector<std::string>& keys,
                                      std::string_view key) {
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (keys[i] == key) {
      return static_cast<std::uint32_t>(i + 1);
    }
  }
  keys.emplace_back(key);
  return static_cast<std::uint32_t>(keys.size());
}

} // namespace

ProgramCalleeSource::ProgramCalleeSource(const rbc::Program& program)
    : program_(program) {}

InlineCallee ProgramCalleeSource::resolve(ir::MethodId callTarget) {
  // The unified space: a payload below the table size IS the program
  // method at that index (un-quickened MethodRef payloads, quickened
  // payloads, and the tool's FrameState method ids all agree); anything
  // at or above the table is an external method - unresolved.
  InlineCallee out;
  const std::size_t idx = callTarget;
  if (idx < program_.methods.size()) {
    out.method = &program_.methods[idx];
    out.frameMethodId = callTarget;
  }
  return out;
}

ir::TypeId ProgramCalleeSource::classId(std::string_view internalName) {
  return internKey(otherKeys_, internalName);
}

ir::FieldId ProgramCalleeSource::fieldId(std::string_view cls,
                                         std::string_view name,
                                         std::string_view descriptor) {
  return internKey(otherKeys_, std::string(cls) + "\x01" +
                                  std::string(name) + "\x01" +
                                  std::string(descriptor));
}

ir::MethodId ProgramCalleeSource::methodId(std::string_view cls,
                                           std::string_view name,
                                           std::string_view descriptor) {
  // Program methods: the table index (the unified space; matches the
  // quickened payload pin and the tool FrameState convention).
  if (cls == program_.className) {
    for (std::size_t i = 0; i < program_.methods.size(); ++i) {
      const rbc::Method& m = program_.methods[i];
      if (m.name == name && m.descriptor == descriptor) {
        return static_cast<ir::MethodId>(i);
      }
    }
  }
  // External methods: interned above the table (disjoint from program
  // indices by construction - methods.size() is the first external id).
  return static_cast<ir::MethodId>(
      program_.methods.size() +
      internKey(extKeys_, std::string(cls) + "\x02" + std::string(name) +
                              "\x02" + std::string(descriptor)));
}

ir::SymbolId ProgramCalleeSource::symbolId(std::string_view payload) {
  return internKey(otherKeys_, payload);
}

std::uint32_t
ProgramCalleeSource::switchTableId(const std::vector<std::int32_t>& payload) {
  std::string key;
  key.reserve(payload.size() * 4);
  for (const std::int32_t v : payload) {
    key.append(reinterpret_cast<const char*>(&v), sizeof(v));
  }
  return internKey(otherKeys_, key);
}

// --- the entry -------------------------------------------------------------------

InlineResult runInlining(ir::Graph& g, InlineCalleeSource& src,
                         const InlineConfig& cfg) {
  InlineResult out;
  Inliner driver(g, src, cfg, out);
  driver.run();
  return out;
}

} // namespace b2::passes
