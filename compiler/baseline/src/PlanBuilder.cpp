// B-2 baseline (T1) - the stencil-plan builder: verified RBC in, StencilPlan out.
//
// WHY THIS FILE EXISTS:
// This implements the NORMATIVE ALGORITHM pinned in b2/baseline/Compiler.h:
// (1) rbc::verify hard gate, (2) ONE linear scan over code[] with greedy
// longest-match superinstruction fusion through a peephole window, (3) a
// second linear pass assembling the pc map, stack maps, deopt points, and
// exception edges, (4) the backedge-poll membership check, (5) a final
// verifyPlan() self-audit. There is NO IR, NO dataflow, NO worklist, NO
// fixpoint (Amendment A): every "analysis" below is a single linear pass
// building a membership set, exactly like the poll set the contract names.
// Effect order in the plan IS RBC order - nothing is reordered, eliminated,
// or speculated.
//
// The builder TRUSTS the verifier (structural bounds, cp kinds, branch
// targets, handler ranges) and re-checks only what selection itself needs:
// patch-value widths and its own plan invariants. Any budget ceiling, any op
// without an Available stencil, any invariant the builder cannot uphold =>
// Refused with a reason - never a partial plan (Amendment A: always safe to
// abandon; Rule 96: T0 runs the method).
//
// LAW PINS (docs/laws.md):
// - Rule 15: every cross-reference below is an index (StencilId, instance
//   index, handler index, deopt id).
// - Rule 16: std::string appears only on REFUSAL paths (diagnostics); the
//   happy path is indices and fixed-size vectors.
// - Rule 23: thresholds are the frozen PlanBudget fields (Compiler.h) plus
//   the named constants below.
// - Rule 124: the plan is a pure function of (RBC, StencilSet, options) -
//   deterministic allocation order, sorted vector for duplicate detection,
//   no hash-ordered containers anywhere.
//
// AMBIGUITY RESOLUTIONS (documented for the integrator; see worklog T1-A):
// - Fusion safety guard: a superinstruction window [pc, pc+len) is only
//   fused when NO branch target and NO exception-handler boundary (start,
//   end, or entry pc) falls STRICTLY INSIDE the window. This keeps every
//   fused trap reporting an rbc pc in the same handler-membership cell as
//   every step it covers (PcMapEntry pins the fusion's single observable
//   boundary at its START), keeps every branch target and handler entry at
//   a real instance boundary (instantiable branch/exception patching), and
//   makes "handler range covers instance range" unambiguous for exc_covers.
//   It is a bounded membership check over precomputed tables, not analysis.
// - GuardCp deopt ids live in Ins::b (Sig::GuardCp pin in rbc/Opcode.h), so
//   DeoptIdSource values read b for guard_class and imm elsewhere; DeoptId
//   holes on call stencils carry the plan-allocated CallException id.

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "b2/baseline/Compiler.h"

namespace b2::baseline {
namespace {

using rbc::Op;
using rbc::Sig;

// PatternStep link sentinel (Stencil.h: 0xFFFF = unconstrained).
constexpr std::uint16_t kNoStepLink = 0xFFFF;

// PatchSource table width: one step-cursor per source enum value (the enum
// has 14 members; a fixed array avoids per-emit allocation).
constexpr std::size_t kPatchSourceCount = 14;

constexpr std::uint32_t kNoLink32 = 0xFFFF'FFFFu; // stack-map/deopt link sentinel

// --- refusal helpers -------------------------------------------------------------

[[nodiscard]] PlanResult refused(RefuseReason reason, std::string detail) {
  PlanResult r;
  r.ok = false;
  r.reason = reason;
  r.detail = std::move(detail); // never empty on refusal (Compiler.h)
  return r;
}

[[nodiscard]] std::string decimal(std::uint64_t v) {
  // Deterministic decimal formatting for diagnostics (no locale, no stream).
  if (v == 0) {
    return "0";
  }
  std::string out;
  while (v != 0) {
    out.push_back(static_cast<char>('0' + static_cast<int>(v % 10)));
    v /= 10;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

// --- step capability (must agree with StencilSet.cpp addHolesForOp) ---------------

[[nodiscard]] bool isQuickenedFieldOp(Op op) noexcept {
  return op == Op::GetfieldQuick || op == Op::PutfieldQuick;
}

[[nodiscard]] bool isCpResolvingSig(Sig sig) noexcept {
  switch (sig) {
    case Sig::RegCp:
    case Sig::RegRegCp:
    case Sig::RegRegRegCp:
    case Sig::RegCpBranch:
      return true;
    default:
      return false;
  }
}

// Does the step `op` supply a hole of the given source under the v0 hole
// conventions? Mirrors addHolesForOp in StencilSet.cpp - the two functions
// derive from the same rbc::Sig contract and must not drift.
[[nodiscard]] bool stepSuppliesSource(PatchSource src, Op op) noexcept {
  const Sig sig = rbc::info(op).sig;
  switch (src) {
    case PatchSource::InsDst:
    case PatchSource::InsA:
    case PatchSource::InsB:
      return true; // any step can carry a raw register-operand hole
    case PatchSource::InsImm:
      return sig == Sig::RegImm || sig == Sig::CallQuick || isQuickenedFieldOp(op);
    case PatchSource::FrameSlot:
      return sig == Sig::RegSlot || sig == Sig::SlotReg;
    case PatchSource::CpIndex:
      return isCpResolvingSig(sig) && !isQuickenedFieldOp(op);
    case PatchSource::BranchTarget:
      return sig == Sig::Branch || sig == Sig::RegBranch || sig == Sig::RegRegBranch;
    case PatchSource::DeoptIdSource:
      return false; // instance-level: the instance's own deopt id
    case PatchSource::RuntimeField:
      switch (op) {
        case Op::Getfield:
        case Op::Putfield:
        case Op::Getstatic:
        case Op::Putstatic:
          return true;
        default:
          return false;
      }
    case PatchSource::RuntimeClass:
      switch (op) {
        case Op::New:
        case Op::Checkcast:
        case Op::Instanceof:
        case Op::AnewArray:
        case Op::Multianewarray:
          return true;
        default:
          return false;
      }
    case PatchSource::RuntimeMethod:
      return sig == Sig::Call;
    case PatchSource::RuntimeICStub:
      return sig == Sig::Call || sig == Sig::CallQuick;
    case PatchSource::RuntimeHelper:
      return isCpResolvingSig(sig) && !isQuickenedFieldOp(op); // defensive
    case PatchSource::None:
      return false; // fixed at stencil-build time; nothing to source
  }
  return false;
}

[[nodiscard]] bool fitsWidth(std::uint64_t value, std::uint8_t widthBytes) noexcept {
  if (widthBytes >= 8) {
    return true;
  }
  return value <= ((std::uint64_t{1} << (8u * widthBytes)) - 1u);
}

// --- precomputed membership sets (single linear passes; NOT analysis) -------------

// Marks every pc that is a destination of some control transfer: branch
// targets (Branch/RegBranch/RegRegBranch sigs) plus every switch payload
// target (default included), used by the fusion guard.
void markControlTargets(const rbc::Method& m, std::vector<char>& marked) {
  const auto mark = [&marked](std::int32_t t) {
    if (t >= 0 && static_cast<std::uint64_t>(t) < marked.size()) {
      marked[static_cast<std::size_t>(t)] = 1;
    }
  };
  for (const rbc::Ins& ins : m.code) {
    const Sig sig = rbc::info(ins.opcode()).sig;
    if (sig == Sig::Branch || sig == Sig::RegBranch || sig == Sig::RegRegBranch) {
      mark(static_cast<std::int32_t>(ins.imm));
    } else if (sig == Sig::RegCpBranch) {
      // Canonical payloads (Verifier.cpp): table [low,high,def,targets...];
      // lookup [N,def,match,target,...]. The verify gate guarantees shape.
      if (ins.imm >= m.cp.size()) {
        continue; // cannot happen post-verify; defensive totality
      }
      const std::vector<std::int32_t>& v = m.cp[ins.imm].ints;
      if (ins.opcode() == Op::Tableswitch) {
        if (v.size() < 3) {
          continue;
        }
        mark(v[2]);
        for (std::size_t i = 3; i < v.size(); ++i) {
          mark(v[i]);
        }
      } else {
        if (v.size() < 2) {
          continue;
        }
        mark(v[1]);
        for (std::size_t i = 3; i < v.size(); i += 2) {
          mark(v[i]);
        }
      }
    }
  }
}

// The set of SafepointPoll instruction pcs (step 4's membership table).
[[nodiscard]] std::vector<char> computePollSet(const rbc::Method& m) {
  std::vector<char> poll(m.code.size(), 0);
  for (std::size_t i = 0; i < m.code.size(); ++i) {
    if (m.code[i].opcode() == Op::SafepointPoll) {
      poll[i] = 1;
    }
  }
  return poll;
}

// The fusion guard set: control destinations + handler boundaries/entries.
[[nodiscard]] std::vector<char> computeFusionBlocked(const rbc::Method& m) {
  std::vector<char> blocked(m.code.size() + 1, 0); // +1: h.end may be code.size()
  markControlTargets(m, blocked);
  for (const rbc::ExceptionHandler& h : m.handlers) {
    if (h.start < blocked.size()) {
      blocked[h.start] = 1;
    }
    if (h.end < blocked.size()) {
      blocked[h.end] = 1;
    }
    if (h.handler < blocked.size()) {
      blocked[h.handler] = 1;
    }
  }
  return blocked;
}

// --- superinstruction matching (the peephole window) -------------------------------

[[nodiscard]] bool matchSuper(const StencilDesc& cand, const rbc::Method& m,
                              std::uint32_t pc,
                              const std::vector<char>& blockedInside) noexcept {
  const std::uint32_t len = cand.pattern_len;
  if (len == 0 || len > StencilDesc::kMaxPattern) {
    return false;
  }
  if (len > m.code.size() - pc) { // pc < code.size() is the caller's invariant
    return false;
  }
  for (std::uint32_t k = 0; k < len; ++k) {
    if (m.code[pc + k].opcode() != cand.pattern[k].op) {
      return false;
    }
    // Producer-consumer links: an earlier step's dst must feed this step's
    // a/b operand. Links only point backwards (Stencil.h).
    const std::uint16_t linkA = cand.pattern[k].useAsA;
    if (linkA != kNoStepLink) {
      if (linkA >= k) {
        return false;
      }
      if (m.code[pc + linkA].dst != m.code[pc + k].a) {
        return false;
      }
    }
    const std::uint16_t linkB = cand.pattern[k].useAsB;
    if (linkB != kNoStepLink) {
      if (linkB >= k) {
        return false;
      }
      if (m.code[pc + linkB].dst != m.code[pc + k].b) {
        return false;
      }
    }
  }
  // Fusion-safety guard (see the file header): no observable boundary may
  // fall strictly inside the window.
  for (std::uint32_t t = pc + 1; t < pc + len; ++t) {
    if (blockedInside[t] != 0) {
      return false;
    }
  }
  return true;
}

// --- patch value computation --------------------------------------------------------

struct PatchFillResult {
  bool ok = true;
  RefuseReason reason = RefuseReason::Ok;
  std::string detail;
};

// Fills `inst.patch_values` 1:1 with the descriptor's declared holes. Per-
// source step cursors advance monotonically over the pattern steps: each
// hole of source S is supplied by the next step that can declare an S hole
// (the v0 convention: fused holes are the per-step holes in step order).
[[nodiscard]] PatchFillResult fillPatchValues(StencilInstance& inst,
                                              const StencilDesc& desc,
                                              const rbc::Method& m,
                                              std::uint32_t pc,
                                              std::uint32_t deoptId) {
  PatchFillResult out;
  inst.patch_values.resize(desc.patch_count);
  std::uint8_t cursor[kPatchSourceCount] = {};
  for (std::uint8_t k = 0; k < desc.patch_count; ++k) {
    const PatchSiteDesc& hole = desc.patches[k];
    PatchValue pv;
    pv.site = k;
    pv.kind = hole.kind;
    pv.source = hole.source;
    const std::size_t srcIdx = static_cast<std::size_t>(hole.source);
    std::uint32_t step = desc.pattern_len; // "no supplier" sentinel
    if (srcIdx < kPatchSourceCount && hole.source != PatchSource::DeoptIdSource &&
        hole.source != PatchSource::None) {
      std::uint8_t& pos = cursor[srcIdx];
      while (pos < desc.pattern_len &&
             !stepSuppliesSource(hole.source, desc.pattern[pos].op)) {
        ++pos;
      }
      if (pos < desc.pattern_len) {
        step = pos;
        ++pos;
      }
    }
    const rbc::Ins& ins = m.code[pc + (step < desc.pattern_len ? step : 0)];
    switch (hole.source) {
      case PatchSource::InsDst:
        pv.value = ins.dst;
        pv.pending_runtime = false;
        break;
      case PatchSource::InsA:
        pv.value = ins.a;
        pv.pending_runtime = false;
        break;
      case PatchSource::InsB:
        pv.value = ins.b;
        pv.pending_runtime = false;
        break;
      case PatchSource::InsImm:
      case PatchSource::FrameSlot:
      case PatchSource::CpIndex:
      case PatchSource::BranchTarget:
        pv.value = ins.imm;
        pv.pending_runtime = false;
        break;
      case PatchSource::DeoptIdSource:
        // The instance's own deopt id: rbc-declared for guards/deopt_trap,
        // plan-allocated for calls (see the emit loop).
        pv.value = deoptId;
        pv.pending_runtime = false;
        break;
      case PatchSource::RuntimeField:
      case PatchSource::RuntimeClass:
      case PatchSource::RuntimeMethod:
      case PatchSource::RuntimeHelper:
        // Runtime sources carry their semantic input (cp index) with
        // pending_runtime=true; the instantiator resolves at patch time.
        pv.value = ins.imm;
        pv.pending_runtime = true;
        break;
      case PatchSource::RuntimeICStub:
        // Semantic input = the call-site id, i.e. the call instruction's own
        // pc (the T0 IC site key is (MethodId, call pc), interp_contract SS8).
        pv.value = pc + (step < desc.pattern_len ? step : 0);
        pv.pending_runtime = true;
        break;
      case PatchSource::None:
        pv.value = 0;
        pv.pending_runtime = false;
        break;
    }
    if (step == desc.pattern_len &&
        hole.source != PatchSource::DeoptIdSource && hole.source != PatchSource::None) {
      // No step supplies this hole: the manifest declares more holes of this
      // source than the pattern can fill (possible only for custom sets).
      // Keep the 1:1 hole/value alignment with a zero value and let the
      // instantiator's manifest validation reject it - the plan itself stays
      // well-formed (invariants 1-6 hold regardless of the value).
      pv.value = 0;
      pv.pending_runtime = false;
    }
    if (!fitsWidth(pv.value, hole.width)) {
      out.ok = false;
      out.reason = RefuseReason::BadOperandRange;
      out.detail = "pc " + decimal(pc) + " stencil " + std::string(desc.name) +
                   " patch site " + decimal(k) + ": value " + decimal(pv.value) +
                   " exceeds hole width " + decimal(hole.width);
      return out;
    }
    inst.patch_values[k] = pv;
  }
  return out;
}

// --- the emit-time scan -------------------------------------------------------------

struct ScanState {
  std::vector<StencilInstance> instances;
  std::vector<std::uint32_t> instanceDeoptId; // parallel; valid when hasDeopt
  std::vector<char> instanceHasDeopt;         // parallel
  std::uint32_t outOffset = 0;
  std::uint32_t fusionCount = 0;
  std::uint32_t patchTotal = 0;
  std::uint32_t deoptCount = 0;
  std::uint32_t nextPlanDeoptId = kPlanAllocatedDeoptIdBase;
  // (rbc-declared deopt id -> first pc), sorted by id for duplicate checks.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> seenRbcDeoptIds;
};

[[nodiscard]] PlanResult emitInstance(ScanState& st, const StencilDesc& winner,
                                      const rbc::Method& m, std::uint32_t pc,
                                      const CompileOptions& options) {
  const std::uint32_t len = winner.pattern_len;

  // Budget checks at each emit (the kill switch), in a fixed order so the
  // refusal is deterministic: instances, output bytes, patches, deopt
  // points, fusions.
  if (st.instances.size() + 1 > options.budget.max_instances) {
    return refused(RefuseReason::BudgetExceeded,
                   "max_instances limit=" + decimal(options.budget.max_instances) +
                       " current=" + decimal(st.instances.size() + 1));
  }
  if (st.outOffset + winner.nominal_size > options.budget.max_output_bytes) {
    return refused(RefuseReason::BudgetExceeded,
                   "max_output_bytes limit=" + decimal(options.budget.max_output_bytes) +
                       " current=" + decimal(st.outOffset + winner.nominal_size));
  }
  if (st.patchTotal + winner.patch_count > options.budget.max_patches) {
    return refused(RefuseReason::BudgetExceeded,
                   "max_patches limit=" + decimal(options.budget.max_patches) +
                       " current=" + decimal(st.patchTotal + winner.patch_count));
  }
  const bool getsDeopt = hasFlag(winner.flags, StencilFlag::HasDeopt) ||
                         hasFlag(winner.flags, StencilFlag::CanTrap);
  if (getsDeopt && st.deoptCount + 1 > options.budget.max_deopt_points) {
    return refused(RefuseReason::BudgetExceeded,
                   "max_deopt_points limit=" + decimal(options.budget.max_deopt_points) +
                       " current=" + decimal(st.deoptCount + 1));
  }
  if (len > 1 && st.fusionCount + 1 > options.budget.max_fusions) {
    return refused(RefuseReason::BudgetExceeded,
                   "max_fusions limit=" + decimal(options.budget.max_fusions) +
                       " current=" + decimal(st.fusionCount + 1));
  }

  // Deopt id: rbc-declared for Guard/GuardCp/Trap sigs (GuardCp's id lives
  // in Ins::b per the frozen Sig contract), plan-allocated otherwise.
  std::uint32_t deoptId = 0;
  if (getsDeopt) {
    const Sig s0 = rbc::info(m.code[pc].opcode()).sig;
    if (s0 == Sig::Guard || s0 == Sig::GuardCp || s0 == Sig::Trap) {
      deoptId = (s0 == Sig::GuardCp) ? m.code[pc].b : m.code[pc].imm;
      if (deoptId >= kPlanAllocatedDeoptIdBase) {
        return refused(RefuseReason::BadOperandRange,
                       "pc " + decimal(pc) + ": rbc deopt id " + decimal(deoptId) +
                           " exceeds the rbc-declared id ceiling " +
                           decimal(kPlanAllocatedDeoptIdBase - 1));
      }
      const auto it = std::lower_bound(
          st.seenRbcDeoptIds.begin(), st.seenRbcDeoptIds.end(), deoptId,
          [](const std::pair<std::uint32_t, std::uint32_t>& p, std::uint32_t v) {
            return p.first < v;
          });
      if (it != st.seenRbcDeoptIds.end() && it->first == deoptId) {
        return refused(RefuseReason::InternalInvariant,
                       "pc " + decimal(pc) + ": duplicate rbc deopt id " +
                           decimal(deoptId) + " (first declared at pc " +
                           decimal(it->second) + ")");
      }
      st.seenRbcDeoptIds.insert(it, {deoptId, pc});
    } else {
      deoptId = st.nextPlanDeoptId++;
    }
    ++st.deoptCount;
  }

  StencilInstance inst;
  inst.stencil = winner.id;
  inst.output_offset = st.outOffset;
  inst.output_size = winner.nominal_size;
  inst.rbc_pc_start = pc;
  inst.rbc_pc_end = pc + len;

  // Exception coverage for ALL instances (Plan.h defines exc_covers as the
  // handler ranges containing the instance's rbc range; the fusion guard
  // makes containment coincide with overlap for fused windows).
  for (std::size_t hi = 0; hi < m.handlers.size(); ++hi) {
    const rbc::ExceptionHandler& h = m.handlers[hi];
    if (h.start <= pc && pc + len <= h.end) {
      inst.exc_covers.push_back(static_cast<std::uint16_t>(hi));
    }
  }

  // Patch values (1:1 with the declared holes, in declaration order).
  PatchFillResult fill = fillPatchValues(inst, winner, m, pc, deoptId);
  if (!fill.ok) {
    return refused(fill.reason, std::move(fill.detail));
  }

  st.patchTotal += winner.patch_count;
  if (len > 1) {
    ++st.fusionCount;
  }
  st.outOffset += winner.nominal_size;
  st.instances.push_back(std::move(inst));
  st.instanceDeoptId.push_back(deoptId);
  st.instanceHasDeopt.push_back(getsDeopt ? 1 : 0);
  return PlanResult{}; // success sentinel: reason == Ok (callers check .reason)
}

// --- step 4: the backedge-poll membership check -------------------------------------

[[nodiscard]] PlanResult checkBackedgePolls(const rbc::Method& m,
                                            const std::vector<char>& poll) {
  const auto checkTarget = [&](std::uint32_t branchPc, std::int32_t target) -> bool {
    if (target >= 0 && static_cast<std::uint32_t>(target) < branchPc) {
      // Backward edge: its destination must be an explicit SafepointPoll
      // instruction (CompileOptions pin: membership, not loop analysis).
      if (static_cast<std::size_t>(target) >= poll.size() ||
          poll[static_cast<std::size_t>(target)] == 0) {
        return false;
      }
    }
    return true;
  };
  for (std::uint32_t pc = 0; pc < m.code.size(); ++pc) {
    const rbc::Ins& ins = m.code[pc];
    const Sig sig = rbc::info(ins.opcode()).sig;
    if (sig == Sig::Branch || sig == Sig::RegBranch || sig == Sig::RegRegBranch) {
      if (!checkTarget(pc, static_cast<std::int32_t>(ins.imm))) {
        return refused(RefuseReason::MissingBackedgePoll,
                       "backward branch at pc " + decimal(pc) + " targets pc " +
                           decimal(ins.imm) + " which is not a safepoint_poll");
      }
    } else if (sig == Sig::RegCpBranch) {
      if (ins.imm >= m.cp.size()) {
        continue; // cannot happen post-verify; defensive totality
      }
      const std::vector<std::int32_t>& v = m.cp[ins.imm].ints;
      const bool table = ins.opcode() == Op::Tableswitch;
      const std::size_t count = v.size();
      const auto fail = [&](std::int32_t t) {
        return refused(RefuseReason::MissingBackedgePoll,
                       "backward switch edge at pc " + decimal(pc) + " targets pc " +
                           decimal(static_cast<std::uint32_t>(t)) +
                           " which is not a safepoint_poll");
      };
      if (table) {
        if (count >= 3) {
          for (std::size_t i = 2; i < count; ++i) {
            if (!checkTarget(pc, v[i])) {
              return fail(v[i]);
            }
          }
        }
      } else {
        if (count >= 2) {
          for (std::size_t i = 1; i < count; i += 2) {
            if (!checkTarget(pc, v[i])) {
              return fail(v[i]);
            }
          }
        }
      }
    }
  }
  return PlanResult{};
}

// --- verifyPlan internals ----------------------------------------------------------

[[nodiscard]] PlanCheckResult checkFailed(std::string error) {
  PlanCheckResult r;
  r.ok = false;
  r.error = std::move(error);
  return r;
}

[[nodiscard]] bool instanceCovers(std::uint32_t start, std::uint32_t end,
                                  const StencilInstance& inst) noexcept {
  return start <= inst.rbc_pc_start && inst.rbc_pc_end <= end;
}

// Binary search for the instance starting exactly at `rbcPc` (instances are
// in rbc order and tile the method, so rbc_pc_start is strictly ascending).
[[nodiscard]] std::size_t findInstanceAt(const std::vector<StencilInstance>& instances,
                                         std::uint32_t rbcPc) noexcept {
  std::size_t lo = 0;
  std::size_t hi = instances.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (instances[mid].rbc_pc_start < rbcPc) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < instances.size() && instances[lo].rbc_pc_start == rbcPc) {
    return lo;
  }
  return instances.size(); // not found
}

} // namespace

// --- compilePlan --------------------------------------------------------------------

PlanResult compilePlan(const rbc::Program& program, std::uint32_t method_index,
                       const StencilSet& set, const CompileOptions& options) {
  if (method_index >= program.methods.size()) {
    return refused(RefuseReason::InternalInvariant,
                   "method index " + decimal(method_index) + " out of range (program has " +
                       decimal(program.methods.size()) + " methods)");
  }
  const rbc::Method& method = program.methods[method_index];

  // 1. The hard gate - the same discipline as T0/b2run: only verified RBC.
  const rbc::VerifyResult vr = rbc::verify(method);
  if (!vr.ok) {
    std::string detail = "rbc::verify failed with no diagnostics";
    if (!vr.diags.empty()) {
      detail = "pc " + decimal(vr.diags.front().pc) + ": " + vr.diags.front().message;
    }
    return refused(RefuseReason::UnverifiableMethod, std::move(detail));
  }

  // Membership sets, each built in ONE linear pass (Amendment A discipline).
  const std::vector<char> poll = computePollSet(method);
  const std::vector<char> blocked = computeFusionBlocked(method);

  // exc_covers indices are u16 (Plan.h): a handler table beyond that space
  // cannot be represented; refuse loudly rather than truncate coverage.
  if (method.handlers.size() > 0xFFFFu) {
    return refused(RefuseReason::InternalInvariant,
                   "handler table size " + decimal(method.handlers.size()) +
                       " exceeds the u16 exception-edge index space");
  }

  // 2. The linear scan with greedy longest-match fusion.
  ScanState st;
  std::uint32_t pc = 0;
  while (pc < method.code.size()) {
    const Op op = method.code[pc].opcode();
    const std::span<const StencilDesc> cands = set.candidates(op);
    const StencilDesc* winner = nullptr;
    for (const StencilDesc& cand : cands) {
      if (cand.pattern_len == 0 || cand.pattern_len > StencilDesc::kMaxPattern) {
        continue; // manifest entries / hostile descriptors never match
      }
      if (cand.availability == StencilAvailability::NeedsRuntimeFeature) {
        continue; // never select; an Available fallback may follow in the span
      }
      if (!options.superinstructions && cand.category == StencilCategory::Superinstruction) {
        continue; // diagnostic mode pins opcode-stencil-only plans
      }
      if (cand.pattern_len == 1) {
        winner = &cand; // an opcode stencil always matches its own op
        break;
      }
      if (matchSuper(cand, method, pc, blocked)) {
        winner = &cand;
        break;
      }
    }
    if (winner == nullptr) {
      return refused(RefuseReason::NoStencilForOp,
                     "pc " + decimal(pc) + ": op " + rbc::opName(op) +
                         " has no Available stencil");
    }
    PlanResult emitted = emitInstance(st, *winner, method, pc, options);
    if (emitted.reason != RefuseReason::Ok) {
      return emitted; // budget / operand-range / invariant refusal
    }
    pc += winner->pattern_len;
  }

  // 3. Second linear pass: pc map, stack maps, deopt points, exception edges.
  StencilPlan plan;
  plan.method_index = method_index;
  plan.method_name = method.name;
  plan.method_descriptor = method.descriptor;
  plan.method_static = method.isStatic();
  plan.num_regs = method.numRegs;
  plan.num_locals = method.numLocals;
  plan.set_version = set.version;
  plan.instances = std::move(st.instances);

  plan.pc_map.resize(plan.instances.size());
  for (std::size_t i = 0; i < plan.instances.size(); ++i) {
    plan.pc_map[i].native_offset = plan.instances[i].output_offset;
    plan.pc_map[i].rbc_pc = plan.instances[i].rbc_pc_start;
    plan.pc_map[i].instance = static_cast<std::uint32_t>(i);
  }

  // Deopt points first (stack maps link to them). Built in instance order,
  // then sorted by (deopt_id, native_offset) per the Plan.h field contract.
  {
    std::vector<DeoptPoint> points;
    std::vector<std::uint32_t> pointInstance;
    points.reserve(plan.instances.size());
    for (std::size_t i = 0; i < plan.instances.size(); ++i) {
      if (st.instanceHasDeopt[i] == 0) {
        continue;
      }
      const StencilDesc& desc = set.desc(plan.instances[i].stencil);
      const Sig s0 = rbc::info(method.code[plan.instances[i].rbc_pc_start].opcode()).sig;
      DeoptReason reason = DeoptReason::Trap;
      if (s0 == Sig::Guard || s0 == Sig::GuardCp) {
        reason = DeoptReason::Guard;
      } else if (hasFlag(desc.flags, StencilFlag::CanCall)) {
        reason = DeoptReason::CallException;
      }
      DeoptPoint dp;
      dp.deopt_id = st.instanceDeoptId[i];
      dp.native_offset = plan.instances[i].output_offset;
      dp.rbc_pc = plan.instances[i].rbc_pc_start;
      dp.reason = reason;
      dp.pending_exception_possible = (reason == DeoptReason::CallException);
      dp.stack_map = kNoLink32;
      points.push_back(dp);
      pointInstance.push_back(static_cast<std::uint32_t>(i));
    }
    // Deterministic sort by (deopt_id, native_offset); ids are unique so the
    // order is total.
    std::vector<std::uint32_t> order(points.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&points](std::uint32_t a, std::uint32_t b) {
                if (points[a].deopt_id != points[b].deopt_id) {
                  return points[a].deopt_id < points[b].deopt_id;
                }
                return points[a].native_offset < points[b].native_offset;
              });
    plan.deopt_points.reserve(points.size());
    std::vector<std::uint32_t> deoptOfInstance(plan.instances.size(), kNoLink32);
    for (const std::uint32_t idx : order) {
      deoptOfInstance[pointInstance[idx]] =
          static_cast<std::uint32_t>(plan.deopt_points.size());
      plan.deopt_points.push_back(points[idx]);
    }

    // Stack maps: every IsSafepoint or CanCall instance (Compiler.h). The
    // kind is the site's dominant role: Call for call sites (the GC walks
    // the frame at a call; SS9.2), else Poll for poll sites; the Trap arm
    // is defensive - no v0 op is CanTrap without also being IsSafepoint or
    // CanCall, but custom sets may differ.
    plan.stack_maps.reserve(plan.instances.size());
    std::vector<std::uint32_t> stackMapOfInstance(plan.instances.size(), kNoLink32);
    for (std::size_t i = 0; i < plan.instances.size(); ++i) {
      const StencilDesc& desc = set.desc(plan.instances[i].stencil);
      if (!hasFlag(desc.flags, StencilFlag::IsSafepoint) &&
          !hasFlag(desc.flags, StencilFlag::CanCall)) {
        continue;
      }
      StackMapPoint sm;
      sm.native_offset = plan.instances[i].output_offset;
      sm.rbc_pc = plan.instances[i].rbc_pc_start;
      sm.kind = hasFlag(desc.flags, StencilFlag::CanCall)
                    ? StackMapPoint::Kind::Call
                    : (hasFlag(desc.flags, StencilFlag::IsSafepoint)
                           ? StackMapPoint::Kind::Poll
                           : StackMapPoint::Kind::Trap);
      sm.deopt_point = deoptOfInstance[i]; // kNoLink32 when the instance has none
      stackMapOfInstance[i] = static_cast<std::uint32_t>(plan.stack_maps.size());
      plan.stack_maps.push_back(sm);
    }
    // Back-fill the deopt points' stack_map links.
    for (std::size_t j = 0; j < plan.deopt_points.size(); ++j) {
      const std::uint32_t inst = pointInstance[order[j]];
      plan.deopt_points[j].stack_map = stackMapOfInstance[inst];
    }
  }

  // Exception edges: the method's handler table translated to native terms.
  plan.exception_edges.reserve(method.handlers.size());
  for (const rbc::ExceptionHandler& h : method.handlers) {
    const std::size_t idx = findInstanceAt(plan.instances, h.handler);
    if (idx >= plan.instances.size()) {
      return refused(RefuseReason::InternalInvariant,
                     "handler entry pc " + decimal(h.handler) +
                         " has no instance boundary (mid-fusion handler entry)");
    }
    ExceptionEdge edge;
    edge.start = h.start;
    edge.end = h.end;
    edge.handler_pc = h.handler;
    edge.catch_type = h.catchType;
    edge.native_handler = plan.instances[idx].output_offset;
    plan.exception_edges.push_back(edge);
  }

  // 4. require_backedge_polls: membership check against the poll set.
  if (options.require_backedge_polls) {
    PlanResult pollCheck = checkBackedgePolls(method, poll);
    if (pollCheck.reason != RefuseReason::Ok) {
      return pollCheck;
    }
  }

  // Header fields and telemetry counters (Rules 114/124: plain integers).
  plan.entry_native_offset =
      plan.instances.empty() ? 0 : plan.instances.front().output_offset;
  plan.code_size = st.outOffset;
  plan.fusion_count = st.fusionCount;
  plan.patch_count = st.patchTotal;
  plan.safepoint_count = static_cast<std::uint32_t>(plan.stack_maps.size());

  // 5. Final self-audit; failure is a builder bug => refuse loudly.
  const PlanCheckResult check = verifyPlan(plan, method, set);
  if (!check.ok) {
    return refused(RefuseReason::InternalInvariant,
                   "verifyPlan rejected the emitted plan: " + check.error);
  }

  PlanResult result;
  result.ok = true;
  result.reason = RefuseReason::Ok;
  result.plan = std::move(plan);
  return result;
}

// --- compilePlanFor -------------------------------------------------------------------

PlanResult compilePlanFor(const rbc::Program& program, std::string_view name,
                          std::string_view descriptor, const StencilSet& set,
                          const CompileOptions& options) {
  // Method-not-found is an internal-caller error (the caller asked for a
  // method that does not exist in this program): the plan pipeline itself is
  // fine, so the refusal is InternalInvariant, not a plan-quality signal.
  for (std::size_t i = 0; i < program.methods.size(); ++i) {
    const rbc::Method& m = program.methods[i];
    if (m.name == name && m.descriptor == descriptor) {
      return compilePlan(program, static_cast<std::uint32_t>(i), set, options);
    }
  }
  return refused(RefuseReason::InternalInvariant,
                 "method not found: " + std::string(name) + std::string(descriptor));
}

// --- verifyPlan ------------------------------------------------------------------------

PlanCheckResult verifyPlan(const StencilPlan& plan, const rbc::Method& method,
                           const StencilSet& set) {
  // Invariant 7 (determinism) is deliberately NOT checked here: it is a
  // property of the BUILDER (same inputs => same plan), testable only by
  // compiling twice and comparing - callers and CI do exactly that. A single
  // plan in isolation carries no evidence either way.

  const auto descOf = [&set](const StencilInstance& inst) -> const StencilDesc* {
    if (static_cast<std::size_t>(inst.stencil.id) >= set.stencils.size()) {
      return nullptr;
    }
    return &set.stencils[inst.stencil.id];
  };

  // 1. Coverage / tiling: the instances tile [0, code.size()) in rbc order
  //    with no gaps or overlaps, and each instance's stencil pattern really
  //    serves the instructions it claims (ops + producer-consumer links) -
  //    that is what "covering" those pcs means.
  {
    std::uint32_t expected = 0;
    for (std::size_t i = 0; i < plan.instances.size(); ++i) {
      const StencilInstance& inst = plan.instances[i];
      const StencilDesc* desc = descOf(inst);
      if (desc == nullptr) {
        return checkFailed("instance " + decimal(i) + " references invalid stencil id " +
                           decimal(inst.stencil.id));
      }
      if (inst.rbc_pc_start != expected) {
        return checkFailed("instance " + decimal(i) + " starts at rbc pc " +
                           decimal(inst.rbc_pc_start) + ", expected " + decimal(expected) +
                           " (coverage tiling)");
      }
      if (inst.rbc_pc_end <= inst.rbc_pc_start ||
          inst.rbc_pc_end > method.code.size()) {
        return checkFailed("instance " + decimal(i) + " has empty or out-of-range rbc range [" +
                           decimal(inst.rbc_pc_start) + "," + decimal(inst.rbc_pc_end) + ")");
      }
      const std::uint32_t len = inst.rbc_pc_end - inst.rbc_pc_start;
      if (desc->pattern_len != len || len > StencilDesc::kMaxPattern) {
        return checkFailed("instance " + decimal(i) + " covers " + decimal(len) +
                           " instructions but stencil " + std::string(desc->name) +
                           " pattern has " + decimal(desc->pattern_len));
      }
      for (std::uint32_t k = 0; k < len; ++k) {
        const rbc::Ins& ins = method.code[inst.rbc_pc_start + k];
        if (ins.opcode() != desc->pattern[k].op) {
          return checkFailed("instance " + decimal(i) + " step " + decimal(k) +
                             " expects op " + rbc::opName(desc->pattern[k].op) +
                             " but rbc pc " + decimal(inst.rbc_pc_start + k) + " is " +
                             rbc::opName(ins.opcode()));
        }
        const std::uint16_t linkA = desc->pattern[k].useAsA;
        if (linkA != kNoStepLink) {
          if (linkA >= k) {
            return checkFailed("instance " + decimal(i) + " step " + decimal(k) +
                               " has a forward useAsA link");
          }
          if (method.code[inst.rbc_pc_start + linkA].dst != ins.a) {
            return checkFailed("instance " + decimal(i) + " step " + decimal(k) +
                               " useAsA link does not hold");
          }
        }
        const std::uint16_t linkB = desc->pattern[k].useAsB;
        if (linkB != kNoStepLink) {
          if (linkB >= k) {
            return checkFailed("instance " + decimal(i) + " step " + decimal(k) +
                               " has a forward useAsB link");
          }
          if (method.code[inst.rbc_pc_start + linkB].dst != ins.b) {
            return checkFailed("instance " + decimal(i) + " step " + decimal(k) +
                               " useAsB link does not hold");
          }
        }
      }
      expected = inst.rbc_pc_end;
    }
    if (expected != method.code.size()) {
      return checkFailed("instances end at rbc pc " + decimal(expected) +
                         " but code size is " + decimal(method.code.size()));
    }
  }

  // 2. Layout: contiguous output offsets starting at 0, sizes equal to the
  //    descriptors' nominal sizes, code_size = end of the last instance.
  {
    std::uint32_t offset = 0;
    for (std::size_t i = 0; i < plan.instances.size(); ++i) {
      const StencilInstance& inst = plan.instances[i];
      const StencilDesc* desc = descOf(inst);
      if (inst.output_offset != offset) {
        return checkFailed("instance " + decimal(i) + " output offset " +
                           decimal(inst.output_offset) + ", expected " + decimal(offset));
      }
      if (inst.output_size != desc->nominal_size) {
        return checkFailed("instance " + decimal(i) + " output size " +
                           decimal(inst.output_size) + " != stencil nominal size " +
                           decimal(desc->nominal_size));
      }
      offset += inst.output_size;
    }
    if (plan.code_size != offset) {
      return checkFailed("code_size " + decimal(plan.code_size) +
                         " != end of last instance " + decimal(offset));
    }
    const std::uint32_t entry =
        plan.instances.empty() ? 0 : plan.instances.front().output_offset;
    if (plan.entry_native_offset != entry) {
      return checkFailed("entry_native_offset " + decimal(plan.entry_native_offset) +
                         " != first instance offset " + decimal(entry));
    }
  }

  // 3. Pc map: one entry per instance, in plan order, aligned with the
  //    instances' offsets and rbc starts.
  if (plan.pc_map.size() != plan.instances.size()) {
    return checkFailed("pc_map has " + decimal(plan.pc_map.size()) + " entries for " +
                       decimal(plan.instances.size()) + " instances");
  }
  for (std::size_t i = 0; i < plan.pc_map.size(); ++i) {
    const PcMapEntry& e = plan.pc_map[i];
    if (e.native_offset != plan.instances[i].output_offset ||
        e.rbc_pc != plan.instances[i].rbc_pc_start || e.instance != i) {
      return checkFailed("pc_map entry " + decimal(i) +
                         " does not match its instance (coverage shape)");
    }
    if (i > 0 && e.native_offset <= plan.pc_map[i - 1].native_offset) {
      return checkFailed("pc_map is not sorted by native offset at entry " + decimal(i));
    }
  }

  // 4. Deopt ids: unique; RBC-declared ids (< kPlanAllocatedDeoptIdBase)
  //    match the guard/deopt_trap instruction at their rbc pc (GuardCp's id
  //    lives in Ins::b, Guard/Trap's in imm - the frozen Sig contract).
  {
    std::vector<std::uint32_t> ids;
    ids.reserve(plan.deopt_points.size());
    for (const DeoptPoint& dp : plan.deopt_points) {
      ids.push_back(dp.deopt_id);
    }
    std::sort(ids.begin(), ids.end());
    for (std::size_t i = 1; i < ids.size(); ++i) {
      if (ids[i] == ids[i - 1]) {
        return checkFailed("duplicate deopt id " + decimal(ids[i]));
      }
    }
    for (const DeoptPoint& dp : plan.deopt_points) {
      if (dp.deopt_id >= kPlanAllocatedDeoptIdBase) {
        continue; // plan-allocated; nothing to match in the stream
      }
      if (dp.rbc_pc >= method.code.size()) {
        return checkFailed("deopt id " + decimal(dp.deopt_id) + " has out-of-range rbc pc " +
                           decimal(dp.rbc_pc));
      }
      const rbc::Ins& ins = method.code[dp.rbc_pc];
      const Sig sig = rbc::info(ins.opcode()).sig;
      const std::uint32_t streamId =
          (sig == Sig::GuardCp) ? ins.b : ins.imm;
      if (sig != Sig::Guard && sig != Sig::GuardCp && sig != Sig::Trap) {
        return checkFailed("deopt id " + decimal(dp.deopt_id) +
                           " is rbc-declared but rbc pc " + decimal(dp.rbc_pc) +
                           " holds non-guard op " + rbc::opName(ins.opcode()));
      }
      if (streamId != dp.deopt_id) {
        return checkFailed("deopt id " + decimal(dp.deopt_id) +
                           " does not match the instruction's id " + decimal(streamId) +
                           " at rbc pc " + decimal(dp.rbc_pc));
      }
    }
  }

  // 5. Exception edges: every edge's native handler is the output offset of
  //    the instance starting exactly at handler_pc, and every CanTrap
  //    instance lists exactly the edges whose ranges contain its rbc range.
  for (std::size_t ei = 0; ei < plan.exception_edges.size(); ++ei) {
    const ExceptionEdge& edge = plan.exception_edges[ei];
    const std::size_t idx = findInstanceAt(plan.instances, edge.handler_pc);
    if (idx >= plan.instances.size()) {
      return checkFailed("exception edge " + decimal(ei) + " handler pc " +
                         decimal(edge.handler_pc) + " has no instance boundary");
    }
    if (edge.native_handler != plan.instances[idx].output_offset) {
      return checkFailed("exception edge " + decimal(ei) + " native handler " +
                         decimal(edge.native_handler) + " != instance offset " +
                         decimal(plan.instances[idx].output_offset));
    }
  }
  for (std::size_t i = 0; i < plan.instances.size(); ++i) {
    const StencilInstance& inst = plan.instances[i];
    const StencilDesc* desc = descOf(inst);
    if (desc == nullptr) {
      return checkFailed("instance " + decimal(i) + " references invalid stencil id");
    }
    // Invariant 5 speaks of CanTrap instances; non-trap instances carry
    // their coverage list as (header-defined) information and are not
    // audited here.
    if (!hasFlag(desc->flags, StencilFlag::CanTrap)) {
      continue;
    }
    std::size_t cursor = 0; // exc_covers is ascending by construction
    for (std::size_t ei = 0; ei < plan.exception_edges.size(); ++ei) {
      const bool covered =
          instanceCovers(plan.exception_edges[ei].start, plan.exception_edges[ei].end, inst);
      const bool listed = cursor < inst.exc_covers.size() &&
                          static_cast<std::size_t>(inst.exc_covers[cursor]) == ei;
      if (covered && !listed) {
        return checkFailed("CanTrap instance " + decimal(i) +
                           " does not list covering exception edge " + decimal(ei));
      }
      if (listed && !covered) {
        return checkFailed("CanTrap instance " + decimal(i) + " lists exception edge " +
                           decimal(ei) + " which does not cover its rbc range");
      }
      if (listed) {
        ++cursor;
      }
    }
    if (cursor != inst.exc_covers.size()) {
      return checkFailed("CanTrap instance " + decimal(i) +
                         " exc_covers is not the ascending list of covering "
                         "exception edges");
    }
  }

  // 6. Patch sites: every patch value aligns 1:1 with the descriptor's
  //    declared holes (site index == position, kind and source agreement).
  for (std::size_t i = 0; i < plan.instances.size(); ++i) {
    const StencilInstance& inst = plan.instances[i];
    const StencilDesc* desc = descOf(inst);
    if (inst.patch_values.size() != desc->patch_count) {
      return checkFailed("instance " + decimal(i) + " has " + decimal(inst.patch_values.size()) +
                         " patch values for " + decimal(desc->patch_count) + " declared holes");
    }
    for (std::uint8_t k = 0; k < desc->patch_count; ++k) {
      const PatchValue& pv = inst.patch_values[k];
      const PatchSiteDesc& hole = desc->patches[k];
      if (pv.site != k) {
        return checkFailed("instance " + decimal(i) + " patch value " + decimal(k) +
                           " has site index " + decimal(pv.site));
      }
      if (pv.source != hole.source) {
        return checkFailed("instance " + decimal(i) + " patch value " + decimal(k) +
                           " source disagrees with the declared hole");
      }
      if (pv.kind != hole.kind) {
        return checkFailed("instance " + decimal(i) + " patch value " + decimal(k) +
                           " kind disagrees with the declared hole");
      }
    }
  }

  PlanCheckResult ok;
  ok.ok = true;
  return ok;
}

} // namespace b2::baseline
