// B-2 codegen - the instantiator: StencilPlan + archive -> W^X code.
//
// WHY THIS FILE EXISTS:
// docs/stencils.md SS6 is this file's algorithm: stage writable, copy
// stencil bodies while recording native offset -> RBC pc, patch ONLY the
// declared archive holes (Stencil Rule 3), link branches, emit entry stubs
// and per-point deopt thunks as COPIES of archive templates (Stencil Rule
// 1: the instantiator emits no bytes of its own), then publish W^X
// (Stencil Rule 5). Every failure is a refusal with a reason - the method
// simply stays on T0 (Amendment A; Rule 96).
//
// LAYOUT (docs/codegen_contract.md SS6):
//   [entry stubs]      one per entry point (method entry + handlers)
//   [instance bodies]  archive records at TRUE offsets (switches expand
//                      into __switch_case chains + a goto for default)
//   [deopt thunks]     one per DeoptPoint (copies of __deopt_thunk)
//   [deopt exit]       one shared tail (copy of __deopt_exit)

#include "compiler/codegen/src/Helpers.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "compiler/codegen/src/CodeGenInternal.h"
#include "b2/baseline/Plan.h"
#include "b2/codegen/Archive.h"
#include "b2/codegen/Instantiate.h"
#include "b2/interp/Value.h"
#include "b2/rbc/Opcode.h"

namespace b2::codegen {

namespace {

using PS = PatchSource;

[[nodiscard]] std::string decimal(std::uint64_t v) {
  return std::to_string(v);
}

[[nodiscard]] InstantiationResult refused(InstantiationStatus status,
                                          std::string detail) {
  InstantiationResult r;
  r.status = status;
  r.detail = std::move(detail);
  return r;
}

// Binary search: the instance whose rbc_pc_start == pc (plan instances tile
// the method in rbc order; Plan.h invariant 1/3).
[[nodiscard]] std::size_t instanceAtPc(const baseline::StencilPlan& plan,
                                       std::uint32_t pc) noexcept {
  std::size_t lo = 0;
  std::size_t hi = plan.instances.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (plan.instances[mid].rbc_pc_start < pc) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < plan.instances.size() &&
      plan.instances[lo].rbc_pc_start == pc) {
    return lo;
  }
  return plan.instances.size();
}

// Cap for switch case-chain expansion (Rule 23; over -> BadSwitchTable
// refusal, the method stays on T0 which dispatches via the table lookup).
inline constexpr std::uint32_t kMaxSwitchCases = 1024;

// The switch payload (canonical layouts, Verifier.cpp pins):
// table [low, high, def, t(low..high)]; lookup [N, def, m, t, ...].
struct SwitchCases {
  struct Case {
    std::int32_t match;
    std::uint32_t target; // rbc pc
  };
  std::vector<Case> cases;
  std::uint32_t defaultTarget = 0;
};

[[nodiscard]] bool parseSwitch(const rbc::Method& m, const rbc::Ins& ins,
                               SwitchCases& out) {
  if (ins.imm >= m.cp.size()) {
    return false;
  }
  const std::vector<std::int32_t>& v = m.cp[ins.imm].ints;
  if (ins.opcode() == rbc::Op::Tableswitch) {
    if (v.size() < 3) {
      return false;
    }
    const std::int64_t low = v[0];
    const std::int64_t high = v[1];
    if (high < low) {
      return false;
    }
    const std::int64_t count = high - low + 1;
    if (static_cast<std::int64_t>(v.size()) < 3 + count) {
      return false;
    }
    if (count > kMaxSwitchCases) {
      return false;
    }
    out.defaultTarget = static_cast<std::uint32_t>(v[2]);
    out.cases.reserve(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
      out.cases.push_back(
          {static_cast<std::int32_t>(low + i),
           static_cast<std::uint32_t>(v[3 + static_cast<std::size_t>(i)])});
    }
    return true;
  }
  // Lookupswitch.
  if (v.size() < 2) {
    return false;
  }
  const std::size_t pairs = v[0] > 0 ? static_cast<std::size_t>(v[0]) : 0;
  if (v.size() < 2 + 2 * pairs) {
    return false;
  }
  if (pairs > kMaxSwitchCases) {
    return false;
  }
  out.defaultTarget = static_cast<std::uint32_t>(v[1]);
  out.cases.reserve(pairs);
  for (std::size_t i = 0; i < pairs; ++i) {
    out.cases.push_back({v[2 + 2 * i],
                         static_cast<std::uint32_t>(v[3 + 2 * i])});
  }
  return true;
}

void put32(std::uint8_t* buf, std::uint32_t at, std::uint32_t v) noexcept {
  buf[at] = static_cast<std::uint8_t>(v);
  buf[at + 1] = static_cast<std::uint8_t>(v >> 8);
  buf[at + 2] = static_cast<std::uint8_t>(v >> 16);
  buf[at + 3] = static_cast<std::uint8_t>(v >> 24);
}

void put32(std::vector<std::uint8_t>& buf, std::uint32_t at,
           std::uint32_t v) noexcept {
  put32(buf.data(), at, v);
}

void put64(std::vector<std::uint8_t>& buf, std::uint32_t at,
           std::uint64_t v) noexcept {
  for (unsigned i = 0; i < 8; ++i) {
    buf[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
  }
}

constexpr std::uint32_t kNoRealOffset = 0xFFFF'FFFFu;
// Value payload byte offset within a 16-byte slot (self-checking: it is
// offsetof(interp::Value, as), never a magic number).
constexpr std::uint32_t kPayloadDelta =
    static_cast<std::uint32_t>(offsetof(interp::Value, as));
static_assert(kPayloadDelta == 8);

[[nodiscard]] const RealDeoptPoint* findPointById(
    const std::vector<RealDeoptPoint>& points, std::uint32_t id) noexcept {
  for (const RealDeoptPoint& p : points) {
    if (p.deopt_id == id) {
      return &p;
    }
  }
  return nullptr;
}

} // namespace

// --- pc-map / deopt lookups (engine side) -------------------------------------

std::uint32_t CompiledCode::rbcPcAt(std::uint32_t native_offset) const noexcept {
  // Instances tile in native order (layout invariant); find the last entry
  // whose native_offset <= native_offset and clamp inside its range.
  std::size_t lo = 0;
  std::size_t hi = pc_map.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (pc_map[mid].native_offset <= native_offset) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) {
    return pc_map.empty() ? 0 : pc_map.front().rbc_pc;
  }
  const RealPcEntry& e = pc_map[lo - 1];
  if (native_offset < e.native_end) {
    return e.rbc_pc;
  }
  // Between instances (entry stub / thunk area): the containing method's
  // first instruction is the only honest answer; callers translate trap
  // sites, which always fall inside an instance.
  return pc_map.front().rbc_pc;
}

const RealDeoptPoint* CompiledCode::deoptPoint(
    std::uint32_t deopt_id) const noexcept {
  for (const RealDeoptPoint& p : deopt_points) {
    if (p.deopt_id == deopt_id) {
      return &p;
    }
  }
  return nullptr;
}

// --- the instantiator ------------------------------------------------------------

InstantiationResult instantiate(const rbc::Program& program,
                                std::uint32_t method_index,
                                const baseline::StencilPlan& plan,
                                const baseline::StencilSet& set,
                                const Archive& archive,
                                interp::Runtime& rt) {
  if (method_index >= program.methods.size()) {
    return refused(InstantiationStatus::VersionMismatch,
                   "method index " + decimal(method_index) + " out of range");
  }
  const rbc::Method& m = program.methods[method_index];

  // 1. Version gates (Stencil Rule 4): plan vs set, set vs archive.
  if (plan.set_version.magic != set.version.magic ||
      plan.set_version.version != set.version.version) {
    return refused(InstantiationStatus::VersionMismatch,
                   "plan set version " + decimal(plan.set_version.version) +
                       " != set version " + decimal(set.version.version));
  }
  if (archive.header.magic != kStencilArchiveMagic ||
      archive.header.version != kStencilArchiveVersion ||
      archive.header.target_arch != kTargetArchX86_64 ||
      archive.header.abi_hash != kT1AbiHashV1) {
    return refused(InstantiationStatus::VersionMismatch,
                   "archive identity mismatch");
  }
  if (archive.header.manifest_count != set.stencils.size() ||
      archive.records.size() < archive.header.manifest_count) {
    return refused(InstantiationStatus::VersionMismatch,
                   "archive/set record misalignment");
  }

  const ArchiveRecord* entryTmpl = archive.internal(kInternalEntry);
  const ArchiveRecord* thunkTmpl = archive.internal(kInternalDeoptThunk);
  const ArchiveRecord* exitTmpl = archive.internal(kInternalDeoptExit);
  const ArchiveRecord* caseTmpl = archive.internal(kInternalSwitchCase);
  if (entryTmpl == nullptr || thunkTmpl == nullptr || exitTmpl == nullptr ||
      caseTmpl == nullptr) {
    return refused(InstantiationStatus::NoArchiveRecord,
                   "internal template missing from the archive");
  }

  const std::uint32_t numLocals = plan.num_locals;
  const std::uint32_t numRegs = plan.num_regs;
  const auto slotOffset = [&](std::uint32_t flatIndex) noexcept {
    return kSlotsBase + flatIndex * sizeof(interp::Value);
  };
  const auto localOffset = [&](std::uint32_t localIdx) noexcept {
    return slotOffset(localIdx);
  };
  const auto regOffset = [&](std::uint32_t regIdx) noexcept {
    return slotOffset(numLocals + regIdx);
  };

  // Per-instance lookup of the RBC instruction for a fusion step.
  const auto insAt = [&](const baseline::StencilInstance& inst,
                         std::uint8_t step) -> const rbc::Ins& {
    const std::uint32_t pc =
        inst.rbc_pc_start + (step < inst.rbc_pc_end - inst.rbc_pc_start
                                 ? step
                                 : 0);
    return m.code[pc];
  };

  // 2. Layout: entries, instances (with switch expansions), thunks, tail.
  struct InstLayout {
    std::uint32_t real = 0;
    std::uint32_t size = 0; // true emitted size (expansions included)
    std::uint32_t nominal = 0;
    SwitchCases sw; // valid when the instance is a switch
  };

  // Distinct handler entry points (re-entry targets), in first-seen order.
  std::vector<std::uint32_t> handlerPcs;
  for (const baseline::ExceptionEdge& e : plan.exception_edges) {
    if (std::find(handlerPcs.begin(), handlerPcs.end(), e.handler_pc) ==
        handlerPcs.end()) {
      handlerPcs.push_back(e.handler_pc);
    }
  }
  const std::uint32_t entryCount =
      1 + static_cast<std::uint32_t>(handlerPcs.size());
  const std::uint32_t entryArea =
      entryCount * static_cast<std::uint32_t>(entryTmpl->code.size());

  // The goto record (switch-default expansion target).
  const baseline::StencilId gotoId = set.find("goto");
  if (!gotoId.valid() ||
      archive.records[gotoId.id].code.empty()) {
    return refused(InstantiationStatus::NoArchiveRecord,
                   "the manifest 'goto' record is missing/empty");
  }
  const std::uint32_t kGotoRecordSize =
      static_cast<std::uint32_t>(archive.records[gotoId.id].code.size());

  // Parse switches first (they size the layout).
  std::vector<InstLayout> layout(plan.instances.size());
  for (std::size_t i = 0; i < plan.instances.size(); ++i) {
    const baseline::StencilInstance& inst = plan.instances[i];
    layout[i].nominal = inst.output_offset;
    const ArchiveRecord& rec =
        archive.records[inst.stencil.id]; // StencilId == record index
    if (!rec.code.empty()) {
      layout[i].size = static_cast<std::uint32_t>(rec.code.size());
      continue;
    }
    // Empty record: the documented switch expansion (or a refusal).
    const rbc::Op op = m.code[inst.rbc_pc_start].opcode();
    if (op != rbc::Op::Tableswitch && op != rbc::Op::Lookupswitch) {
      return refused(InstantiationStatus::NoArchiveRecord,
                     "stencil '" + std::string(rec.name) + "' at pc " +
                         decimal(inst.rbc_pc_start) +
                         " has no archive body (op not instantiable in v1)");
    }
    if (!parseSwitch(m, m.code[inst.rbc_pc_start], layout[i].sw)) {
      return refused(InstantiationStatus::BadSwitchTable,
                     "switch at pc " + decimal(inst.rbc_pc_start) +
                         " has a malformed/oversized payload");
    }
    // The default is a copy of the manifest goto record.
    layout[i].size = static_cast<std::uint32_t>(
        layout[i].sw.cases.size() * caseTmpl->code.size() +
        kGotoRecordSize);
  }

  // Running real offsets.
  std::uint32_t cur = entryArea;
  for (InstLayout& l : layout) {
    l.real = cur;
    cur += l.size;
  }
  const std::uint32_t thunkArea = cur;
  const std::uint32_t thunkSize =
      static_cast<std::uint32_t>(thunkTmpl->code.size());
  for (std::size_t k = 0; k < plan.deopt_points.size(); ++k) {
    cur += thunkSize;
  }
  const std::uint32_t exitOffset = cur;
  const std::uint32_t totalSize =
      cur + static_cast<std::uint32_t>(exitTmpl->code.size());
  if (totalSize > kMaxCodeBytes) {
    return refused(InstantiationStatus::BudgetExceeded,
                   "code size " + decimal(totalSize) + " exceeds budget " +
                       decimal(kMaxCodeBytes));
  }

  // Instance real offset by rbc pc (branch targets land on instance starts
  // - fusion-blocked positions guarantee it; refuse defensively otherwise).
  const auto realOfPc = [&](std::uint32_t rbcPc) -> std::uint32_t {
    const std::size_t i = instanceAtPc(plan, rbcPc);
    return i == plan.instances.size() ? kNoRealOffset
                                      : layout[i].real;
  };

  // Deopt point -> thunk real offset; covering point per instance.
  std::vector<RealDeoptPoint> realPoints(plan.deopt_points.size());
  {
    std::uint32_t off = thunkArea;
    for (std::size_t k = 0; k < plan.deopt_points.size(); ++k) {
      const baseline::DeoptPoint& p = plan.deopt_points[k];
      realPoints[k].deopt_id = p.deopt_id;
      realPoints[k].thunk_offset = off;
      realPoints[k].reason = p.reason;
      realPoints[k].pending_exception_possible = p.pending_exception_possible;
      realPoints[k].rbc_pc = p.rbc_pc;
      // The trap site's real offset: the instance containing the point's
      // nominal offset (points sit at their instance's nominal start).
      const std::size_t i = instanceAtPc(plan, p.rbc_pc);
      if (i == plan.instances.size()) {
        return refused(InstantiationStatus::NoDeoptCoverage,
                       "deopt point pc " + decimal(p.rbc_pc) +
                           " has no covering instance");
      }
      realPoints[k].native_offset = layout[i].real;
      off += thunkSize;
    }
  }
  // (internal status name; mapped below)
  const auto coveringPoint = [&](std::size_t instIdx) -> const RealDeoptPoint* {
    const std::uint32_t pc = plan.instances[instIdx].rbc_pc_start;
    for (const RealDeoptPoint& p : realPoints) {
      if (p.rbc_pc == pc) {
        return &p;
      }
    }
    return nullptr;
  };

  // 3. Stage + patch.
  auto out = std::make_unique<CompiledCode>();
  out->code.assign(totalSize, 0xCC);
  std::vector<std::uint8_t>& buf = out->code;

  // 3a. Entry stubs: [0] = method entry, then handlers in first-seen order.
  out->entries.resize(entryCount);
  {
    const std::uint32_t esz =
        static_cast<std::uint32_t>(entryTmpl->code.size());
    const std::uint32_t methodEntryTarget = realOfPc(0);
    if (methodEntryTarget == kNoRealOffset) {
      return refused(InstantiationStatus::BadBranchTarget,
                     "method entry target unresolved (empty plan?)");
    }
    out->entries[0].native_offset = 0;
    out->entries[0].rbc_pc = 0;
    out->entries[0].is_method_entry = true;
    for (std::uint32_t k = 0; k < entryCount; ++k) {
      std::memcpy(buf.data() + k * esz, entryTmpl->code.data(), esz);
      std::uint32_t target = methodEntryTarget;
      if (k > 0) {
        const std::uint32_t hpc = handlerPcs[k - 1];
        const std::uint32_t t = realOfPc(hpc);
        if (t == kNoRealOffset) {
          return refused(InstantiationStatus::BadBranchTarget,
                         "handler pc " + decimal(hpc) +
                             " is not an instance start");
        }
        out->entries[k].native_offset = k * esz;
        out->entries[k].rbc_pc = hpc;
        target = t;
      }
      for (const ArchiveHole& h : entryTmpl->holes) {
        // __entry carries exactly one Layout/BranchRel32 hole (validated).
        const std::uint32_t at = k * esz + h.code_offset;
        put32(buf, at, target - (at + 4));
      }
    }
  }

  // Helper call sites: patched in the executable block once its absolute
  // address is known (before mprotect - Stencil Rule 5 staging discipline).
  struct HelperSite {
    std::uint32_t offset;      // offset within the code block
    std::uint8_t helper_id;
  };
  std::vector<HelperSite> helperSites;

  // 3b. Instance bodies.
  const ArchiveRecord& gotoRec = archive.records[gotoId.id];
  for (std::size_t i = 0; i < plan.instances.size(); ++i) {
    const baseline::StencilInstance& inst = plan.instances[i];
    const ArchiveRecord& rec = archive.records[inst.stencil.id];
    const std::uint32_t base = layout[i].real;
    const rbc::Ins& first = m.code[inst.rbc_pc_start];

    if (rec.code.empty()) {
      // Switch expansion: case chain + default goto (Rule 1: copies of
      // archive templates, patched at their declared holes only).
      const SwitchCases& sw = layout[i].sw;
      const std::uint32_t csz =
          static_cast<std::uint32_t>(caseTmpl->code.size());
      const std::uint32_t gsz =
          static_cast<std::uint32_t>(gotoRec.code.size());
      const std::uint32_t selOff =
          regOffset(first.a) + kPayloadDelta; // selector payload slot
      for (std::size_t c = 0; c < sw.cases.size(); ++c) {
        const std::uint32_t at = base + static_cast<std::uint32_t>(c) * csz;
        std::memcpy(buf.data() + at, caseTmpl->code.data(), csz);
        const std::uint32_t target = realOfPc(sw.cases[c].target);
        if (target == kNoRealOffset) {
          return refused(InstantiationStatus::BadBranchTarget,
                         "switch case target pc " +
                             decimal(sw.cases[c].target) +
                             " is not an instance start");
        }
        for (const ArchiveHole& h : caseTmpl->holes) {
          const std::uint32_t pos = at + h.code_offset;
          switch (h.tag) {
            case HoleTag::Stream: // selector slot disp
              put32(buf, pos, selOff);
              break;
            case HoleTag::SwitchMatch:
              put32(buf, pos,
                    static_cast<std::uint32_t>(sw.cases[c].match));
              break;
            case HoleTag::SwitchTarget:
              put32(buf, pos, target - (pos + 4));
              break;
            default:
              return refused(InstantiationStatus::BadHole,
                             "switch_case template carries an unexpected "
                             "hole tag");
          }
        }
      }
      const std::uint32_t at =
          base + static_cast<std::uint32_t>(sw.cases.size()) * csz;
      std::memcpy(buf.data() + at, gotoRec.code.data(), gsz);
      const std::uint32_t dflt = realOfPc(sw.defaultTarget);
      if (dflt == kNoRealOffset) {
        return refused(InstantiationStatus::BadBranchTarget,
                       "switch default target pc " +
                           decimal(sw.defaultTarget) +
                           " is not an instance start");
      }
      for (const ArchiveHole& h : gotoRec.holes) {
        const std::uint32_t pos = at + h.code_offset;
        put32(buf, pos, dflt - (pos + 4));
      }
      continue;
    }

    std::memcpy(buf.data() + base, rec.code.data(), rec.code.size());

    // Plan-value matching cursor: the SAME greedy source walk validation
    // proved (Plan holes pair with PatchValues by source, in order).
    std::size_t mi = 0;
    const auto nextPlanValue = [&](PS src) -> const baseline::PatchValue* {
      while (mi < inst.patch_values.size() &&
             inst.patch_values[mi].source != src) {
        ++mi;
      }
      if (mi >= inst.patch_values.size()) {
        return nullptr;
      }
      return &inst.patch_values[mi++];
    };

    // The unique CpIndex-sourced plan value (CpPayload holes).
    const baseline::PatchValue* cpIndexValue = nullptr;
    for (const baseline::PatchValue& pv : inst.patch_values) {
      if (pv.source == PS::CpIndex) {
        cpIndexValue = &pv;
        break;
      }
    }

    // The field-reading op of this instance (step 0 for plain stencils;
    // the fusion's getfield step for superinstructions).
    rbc::Op op = first.opcode();
    {
      const baseline::StencilDesc& d = set.desc(inst.stencil);
      for (std::uint8_t s = 0; s < d.pattern_len; ++s) {
        const rbc::Op po = d.pattern[s].op;
        if (po == rbc::Op::Getfield || po == rbc::Op::Putfield ||
            po == rbc::Op::Getstatic || po == rbc::Op::Putstatic ||
            po == rbc::Op::GetfieldQuick || po == rbc::Op::PutfieldQuick ||
            po == rbc::Op::AnewArray || po == rbc::Op::New ||
            po == rbc::Op::Checkcast || po == rbc::Op::Instanceof ||
            po == rbc::Op::Ldc ||
            po == rbc::Op::Invokevirtual || po == rbc::Op::Invokespecial ||
            po == rbc::Op::Invokestatic || po == rbc::Op::Invokeinterface ||
            po == rbc::Op::InvokevirtualQuick ||
            po == rbc::Op::InvokespecialQuick ||
            po == rbc::Op::InvokestaticQuick ||
            po == rbc::Op::InvokeinterfaceQuick) {
          op = po; // the semantic op the plan holes belong to
          break;
        }
      }
    }

    for (const ArchiveHole& h : rec.holes) {
      const std::uint32_t pos = base + h.code_offset;
      switch (h.tag) {
        case HoleTag::Plan: {
          const baseline::PatchValue* pv = nextPlanValue(h.source);
          if (pv == nullptr) {
            return refused(InstantiationStatus::BadHole,
                           "stencil '" + std::string(rec.name) + "' pc " +
                               decimal(inst.rbc_pc_start) +
                               ": no plan value for source " +
                               decimal(static_cast<unsigned>(h.source)));
          }
          switch (h.source) {
            case PS::FrameSlot:
              put32(buf, pos, localOffset(static_cast<std::uint32_t>(
                                  pv->value)) + h.imm);
              break;
            case PS::InsImm:
              if (pv->kind == PatchKind::MethodId ||
                  pv->kind == PatchKind::CallSiteId) {
                // Quickened call target: pack the flavor.
                put32(buf, pos,
                      callFlavorOf(op) << kCallFlavorShift |
                          static_cast<std::uint32_t>(pv->value));
              } else {
                put32(buf, pos, static_cast<std::uint32_t>(pv->value));
              }
              break;
            case PS::BranchTarget: {
              const std::uint32_t target =
                  realOfPc(static_cast<std::uint32_t>(pv->value));
              if (target == kNoRealOffset) {
                return refused(InstantiationStatus::BadBranchTarget,
                               "branch target pc " + decimal(pv->value) +
                                   " is not an instance start");
              }
              put32(buf, pos, target - (pos + 4));
              break;
            }
            case PS::DeoptIdSource: {
              const RealDeoptPoint* p =
                  findPointById(realPoints,
                                static_cast<std::uint32_t>(pv->value));
              if (p == nullptr) {
                return refused(InstantiationStatus::NoDeoptCoverage,
                               "deopt id " + decimal(pv->value) +
                                   " has no deopt point");
              }
              put32(buf, pos, p->thunk_offset - (pos + 4));
              break;
            }
            case PS::CpIndex:
              put32(buf, pos, static_cast<std::uint32_t>(pv->value));
              break;
            case PS::RuntimeField: {
              // Instance field byte offset, static FieldId, or the builtin
              // singleton encoding - by op family (contract SS5). Statics
              // are keyed by FieldId (not object offsets); getfield/putfield
              // (and the quickened forms) use instance slot byte offsets.
              const rbc::Const& cref = m.cp[pv->value];
              const bool isStatic =
                  op == rbc::Op::Getstatic || op == rbc::Op::Putstatic;
              if (isStatic) {
                const std::optional<interp::ObjRef> builtin =
                    rt.builtinStatic(cref);
                if (builtin && op == rbc::Op::Getstatic) {
                  put32(buf, pos,
                        kStaticBuiltinBase | builtin->id);
                } else if (builtin) {
                  // Store to a builtin static: T0 raises InternalError
                  // ("store to builtin static"); refuse so T0 runs the
                  // method and raises it (identical observable behavior).
                  return refused(InstantiationStatus::BadHole,
                                 "store to builtin static (T0 raises it)");
                } else {
                  const std::optional<interp::ResolvedField> rf =
                      rt.resolveField(cref);
                  if (!rf) {
                    return refused(
                        InstantiationStatus::BadHole,
                        "getstatic cp " + decimal(pv->value) +
                            " does not resolve (method stays on T0)");
                  }
                  put32(buf, pos, rt.fieldIdOf(*rf).v);
                }
              } else {
                const std::optional<interp::ResolvedField> rf =
                    rt.resolveField(cref);
                if (!rf) {
                  return refused(InstantiationStatus::BadHole,
                                 "field cp " + decimal(pv->value) +
                                     " does not resolve (method stays on T0)");
                }
                // Getfield (un-quickened) carries the resolved RType in the
                // high nibble so the helper can apply the A5 unset-field
                // default (the quickened form cannot know it; T0's quickened
                // corner refuses identically).
                const std::uint32_t typeBits =
                    op == rbc::Op::Getfield
                        ? (static_cast<std::uint32_t>(rf->type) << 28)
                        : 0u;
                put32(buf, pos,
                      rt.fieldOffsetOf(rf->slot) | typeBits);
              }
              break;
            }
            case PS::RuntimeClass: {
              const rbc::Const& cref = m.cp[pv->value];
              put32(buf, pos, rt.classId(cref.str).v);
              break;
            }
            case PS::RuntimeMethod: {
              const rbc::Const& cref = m.cp[pv->value];
              const std::uint32_t flavor = callFlavorOf(op);
              if (flavor == kCallFlavorVirtual) {
                // Runtime dispatch needs the MethodRef (builtin probe +
                // (name,desc) resolution): the hole carries the cp index.
                put32(buf, pos,
                      flavor << kCallFlavorShift |
                          static_cast<std::uint32_t>(pv->value));
              } else {
                const std::optional<interp::MethodId> mid =
                    rt.resolveMethod(cref);
                if (!mid) {
                  return refused(
                      InstantiationStatus::BadHole,
                      "call cp " + decimal(pv->value) +
                          " does not resolve to a program method (method "
                          "stays on T0; T0 raises NoSuchMethodError)");
                }
                put32(buf, pos,
                      flavor << kCallFlavorShift | mid->v);
              }
              break;
            }
            case PS::RuntimeICStub:
              // Site id (call pc): unused by the v1 helper path; recorded
              // for the future IC. Patch the raw value.
              put32(buf, pos, static_cast<std::uint32_t>(pv->value));
              break;
            case PS::RuntimeHelper:
            case PS::None:
            default:
              put32(buf, pos, static_cast<std::uint32_t>(pv->value));
              break;
          }
          break;
        }
        case HoleTag::Stream: {
          // Two stream shapes: SlotOffset holes carry a REGISTER operand
          // (dst/a/b as frame-slot byte offsets); Imm32 holes carry the RAW
          // operand value (arg counts, atype codes).
          const rbc::Ins& ins = insAt(inst, h.step);
          const bool isSlot = h.kind == PatchKind::SlotOffset;
          switch (h.source) {
            case PS::InsDst:
              put32(buf, pos, isSlot ? regOffset(ins.dst) + h.imm
                                     : static_cast<std::uint32_t>(ins.dst));
              break;
            case PS::InsA:
              put32(buf, pos, isSlot ? regOffset(ins.a) + h.imm
                                     : static_cast<std::uint32_t>(ins.a));
              break;
            case PS::InsB:
              put32(buf, pos, isSlot ? regOffset(ins.b) + h.imm
                                     : static_cast<std::uint32_t>(ins.b));
              break;
            case PS::InsImm:
              put32(buf, pos, ins.imm);
              break;
            case PS::FrameSlot:
              put32(buf, pos, localOffset(ins.imm) + h.imm);
              break;
            default:
              return refused(InstantiationStatus::BadHole,
                             "unexpected stream hole source in '" +
                                 std::string(rec.name) + "'");
          }
          break;
        }
        case HoleTag::Helper: {
          // Resolved after the executable block is allocated (the rel32
          // needs the block's absolute address); recorded here.
          if (helperAddress(h.helper_id) == nullptr) {
            return refused(InstantiationStatus::BadHole,
                           "helper id " + decimal(h.helper_id) +
                               " has no address");
          }
          helperSites.push_back({base + h.code_offset, h.helper_id});
          break;
        }
        case HoleTag::Layout: {
          // Trap branch -> the covering deopt point's thunk.
          const RealDeoptPoint* p = coveringPoint(i);
          if (p == nullptr) {
            return refused(InstantiationStatus::NoDeoptCoverage,
                           "trap-capable instance at pc " +
                               decimal(inst.rbc_pc_start) +
                               " has no covering deopt point");
          }
          put32(buf, pos, p->thunk_offset - (pos + 4));
          break;
        }
        case HoleTag::CpPayload: {
          if (cpIndexValue == nullptr ||
              cpIndexValue->value >= m.cp.size()) {
            return refused(InstantiationStatus::BadHole,
                           "constant payload hole without a cp index");
          }
          const rbc::Const& c = m.cp[cpIndexValue->value];
          std::uint64_t bits = 0;
          if (op == rbc::Op::Lconst) {
            bits = static_cast<std::uint64_t>(c.i64);
          } else {
            double d = c.f64; // memcpy form (no type punning; -Wpedantic safe)
            std::memcpy(&bits, &d, sizeof(bits));
          }
          put64(buf, pos, bits);
          break;
        }
        case HoleTag::SwitchMatch:
        case HoleTag::SwitchTarget:
          return refused(InstantiationStatus::BadHole,
                         "switch holes outside the switch expansion");
      }
    }
  }

  // 3c. Deopt thunks (copies of __deopt_thunk; holes: id, trap-site, jmp).
  {
    for (std::size_t k = 0; k < realPoints.size(); ++k) {
      const RealDeoptPoint& p = realPoints[k];
      std::memcpy(buf.data() + p.thunk_offset, thunkTmpl->code.data(),
                  thunkTmpl->code.size());
      const std::uint32_t base = p.thunk_offset;
      // __deopt_thunk holes, in emission order: [0] id imm32, [1] trap-site
      // imm32, [2] jmp Layout -> the shared exit.
      if (thunkTmpl->holes.size() != 3) {
        return refused(InstantiationStatus::BadHole,
                       "__deopt_thunk template must carry 3 holes");
      }
      put32(buf, base + thunkTmpl->holes[0].code_offset, p.deopt_id);
      put32(buf, base + thunkTmpl->holes[1].code_offset, p.native_offset);
      {
        const std::uint32_t pos = base + thunkTmpl->holes[2].code_offset;
        put32(buf, pos, exitOffset - (pos + 4));
      }
    }
    std::memcpy(buf.data() + exitOffset, exitTmpl->code.data(),
                exitTmpl->code.size()); // __deopt_exit: no holes
  }

  // 4. Real pc map + metadata.
  out->pc_map.resize(plan.instances.size());
  for (std::size_t i = 0; i < plan.instances.size(); ++i) {
    out->pc_map[i].native_offset = layout[i].real;
    out->pc_map[i].native_end = layout[i].real + layout[i].size;
    out->pc_map[i].rbc_pc = plan.instances[i].rbc_pc_start;
    out->pc_map[i].instance = static_cast<std::uint32_t>(i);
  }
  out->deopt_points = std::move(realPoints);
  out->plan = plan;
  out->method_index = method_index;
  out->method_name = m.name;
  out->method_descriptor = m.descriptor;
  out->num_locals = numLocals;
  out->num_regs = numRegs;

  // 5. Publish W^X (Stencil Rule 5): page-aligned block still WRITABLE,
  //    copy the staged bytes, patch the helper call sites (they need the
  //    block's absolute address), THEN flip to READ|EXEC and flush.
  const std::size_t pageSize = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  const std::size_t allocSize =
      ((totalSize + pageSize - 1) / pageSize) * pageSize;
  void* exec = nullptr;
  if (posix_memalign(&exec, pageSize, allocSize) != 0 || exec == nullptr) {
    return refused(InstantiationStatus::WxPublishFailed,
                   "code page allocation failed (" + decimal(allocSize) +
                       " bytes)");
  }
  std::uint8_t* block = static_cast<std::uint8_t*>(exec);
  std::memcpy(block, buf.data(), totalSize);
  for (const HelperSite& site : helperSites) {
    void* addr = helperAddress(site.helper_id);
    if (addr == nullptr) {
      std::free(exec);
      return refused(InstantiationStatus::BadHole,
                     "helper id " + decimal(site.helper_id) +
                         " has no address");
    }
    // Absolute 8-byte address (the movabs operand): location-independent.
    const std::uint64_t addrBits =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(addr));
    for (unsigned i = 0; i < 8; ++i) {
      block[site.offset + i] = static_cast<std::uint8_t>(addrBits >> (8 * i));
    }
  }
  if (mprotect(exec, allocSize, PROT_READ | PROT_EXEC) != 0) {
    std::free(exec);
    return refused(InstantiationStatus::WxPublishFailed,
                   "mprotect(PROT_READ|PROT_EXEC) failed");
  }
  __builtin___clear_cache(reinterpret_cast<char*>(exec),
                          reinterpret_cast<char*>(exec) + totalSize);
  out->exec_base = block;
  out->exec_alloc_size = allocSize;

  InstantiationResult res;
  res.status = InstantiationStatus::Ok;
  res.code = std::move(out);
  return res;
}

// --- dump (golden format) --------------------------------------------------------

std::string dumpCode(const CompiledCode& code) {
  std::string out;
  out += "code method=" + code.method_name + code.method_descriptor +
         " index=" + std::to_string(code.method_index) +
         " locals=" + std::to_string(code.num_locals) +
         " regs=" + std::to_string(code.num_regs) + "\n";
  for (const CodeEntry& e : code.entries) {
    out += "  entry native=" + std::to_string(e.native_offset) + " rbc=" +
           std::to_string(e.rbc_pc) +
           (e.is_method_entry ? " method" : " handler") + "\n";
  }
  for (const RealPcEntry& e : code.pc_map) {
    out += "  pcmap native=" + std::to_string(e.native_offset) + ".." +
           std::to_string(e.native_end) + " rbc=" + std::to_string(e.rbc_pc) +
           "\n";
  }
  for (const RealDeoptPoint& p : code.deopt_points) {
    out += "  deopt id=" + std::to_string(p.deopt_id) + " thunk=" +
           std::to_string(p.thunk_offset) + " native=" +
           std::to_string(p.native_offset) +
           " rbc=" + std::to_string(p.rbc_pc) + "\n";
  }
  static constexpr char kHex[] = "0123456789abcdef";
  out += "  bytes";
  for (std::size_t i = 0; i < code.code.size(); ++i) {
    if (i % 16 == 0) {
      out += "\n    ";
    }
    out += " ";
    out += kHex[code.code[i] >> 4];
    out += kHex[code.code[i] & 0xF];
  }
  out += "\n";
  return out;
}

} // namespace b2::codegen
