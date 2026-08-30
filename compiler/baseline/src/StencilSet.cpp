// B-2 baseline (T1) - the v0 target-neutral stencil-set manifest.
//
// WHY THIS FILE EXISTS:
// StencilSet.h freezes WHAT a stencil descriptor is; this file is the complete
// v0 MANIFEST (Stencil Rule 2: every stencil has metadata; Stencil Rule 4: the
// version travels with it): one opcode stencil for EVERY rbc::Op (all 150,
// derived mechanically from rbc::info() so the manifest can never drift from
// the opcode table), the seven v0 superinstructions with their local
// producer-consumer links (docs/stencils.md SS3.2), and the manifest-only
// call/guard/deopt/helper entries the instantiator will need once machine
// stencils exist (pattern_len = 0: the plan builder never selects them).
//
// The table is TARGET-NEUTRAL (target_arch = 0): descriptors declare holes,
// effect flags, frame accounting, and NOMINAL sizes. Nominal sizes drive the
// plan's deterministic output_offset arithmetic (golden-testable) and become
// real sizes when the codegen team's stencil archive lands under a bumped
// version (docs/stencils.md SS4/SS5).
//
// AVAILABILITY (StencilSet.h): Available for everything T1 can plan today;
// NeedsRuntimeFeature for the ops whose execution path the runtime cannot
// back in compiled form. Every NeedsRuntimeFeature decision, cited:
//   - invokedynamic ......... interp_contract SS6 item 4 (raises
//                             BootstrapMethodError in v0; verifier-only op).
//   - guard_class ........... StencilSet.h: KlassId hole pending real
//                             class-id patching.
//   - deopt_trap ............ interp_contract SS6 item 2 (raises
//                             InternalError in T0 "without deopt metadata
//                             (v0)"); the compiled form unconditionally enters
//                             the deopt runtime, whose stubs are themselves
//                             NeedsRuntimeFeature manifest entries.
//   - multianewarray ........ v1 instantiation gap (codegen_contract SS12):
//                             dims-from-registers allocation has no archive
//                             body yet; methods containing it stay on T0.
//
// SET VERSION 2 (MSG-20260830-004, codegen RFC, baseline approved): the
// T1 runtime-helper seam landed, so the un-quickened invoke* and ldc become
// Available - calls resolve at instantiation (static/special: MethodId) or
// in the call helper (virtual/interface: builtin probe + (name,desc));
// ldc (String intern / Class materialize / MethodType-MethodHandle refusal)
// runs entirely in the LdcConst helper, mirroring interp SS6 exactly.
// The quickened variants were already Available (SS7 imm pins).
//
// LAW PINS (docs/laws.md):
// - Rule 23: every size/threshold below is a named, documented constant.
// - Rule 124: two calls to builtinStencilSetV0() build byte-identical tables
//   (fixed iteration order + std::stable_sort; no hash-ordered containers).
// - Rule 16: names are diagnostics/lookup keys only (StencilSet::find is the
//   documented cold path); selection is by StencilId index.
// - Amendment A: the manifest is metadata, not analysis - nothing here may
//   grow a dataflow, a worklist, or a cost model.
//
// HOLE CONVENTIONS (shared with PlanBuilder.cpp - the two files derive from
// the same rbc::Sig contract and MUST agree; golden tests pin the composed
// behavior end to end):
// - Register operands (dst/a/b) are NOT v0 patch holes: the plan's instances
//   carry their rbc ranges, and the instantiator re-reads register operands
//   from the verified stream when it lays out T0-compatible frame accesses
//   (docs/stencils.md SS8). Holes exist for the RELOCATION-relevant operands:
//   frame-slot numbers, constant-pool indices, branch targets, ids, offsets.
// - PatchValue.value semantics are the PatchSource's: FrameSlot carries the
//   local slot number, BranchTarget the rbc pc, CpIndex the pool index, and
//   Runtime* sources carry their semantic input (cp index / call pc) with
//   pending resolution at instantiation time.
// - Superinstruction holes are the CONCATENATION of the per-step holes in
//   step order (each step contributes exactly the holes its own op would
//   declare as an opcode stencil), so the builder can fill them step-wise.

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <tuple>
#include <vector>

#include "b2/baseline/StencilSet.h"

namespace b2::baseline {
namespace {

using rbc::Op;
using rbc::Sig;

// --- nominal sizes (Rule 23: named, documented, per family) -------------------
// v0 layout-accounting figures; "same op family = same size" keeps plan bytes
// deterministic. They are NOT tuning knobs (Amendment A: T1 has no quality
// axis, only coverage).
constexpr std::uint32_t kOpcodeStencilNominalBytes = 16; // one-op stencils
constexpr std::uint32_t kCallStencilNominalBytes = 48;   // ABI + IC site + exception path
constexpr std::uint32_t kGuardStencilNominalBytes = 12;  // compare + deopt branch
constexpr std::uint32_t kDeoptStencilNominalBytes = 24;  // deopt stubs (save regs, call runtime)
constexpr std::uint32_t kHelperStencilNominalBytes = 32; // runtime helper trampolines
constexpr std::uint32_t kFusionSavingBytes = 4;          // fusion discount per superinstruction

// Frame accounting ceiling for call-shaped stencils (StencilDesc::frame_reads
// is a fixed u8, but a call reads one slot per argument): the JVMS 4.3.3
// parameter ceiling stands in as the documented "at most" figure.
constexpr std::uint8_t kMaxCallFrameReads = 255;

// --- effect/flag mapping ------------------------------------------------------

[[nodiscard]] bool opHasICSite(Op op) noexcept {
  // The T0 inline-cache site families (interp_contract SS8): field sites
  // (getfield/putfield) and virtual/interface call sites. Quickened FIELD ops
  // resolve without ICs (SS7: "no cp, no IC"); quickened virtual/interface
  // calls keep their IC (that is their whole point); static/special calls are
  // direct dispatch and never own a site.
  switch (op) {
    case Op::Getfield:
    case Op::Putfield:
    case Op::Invokevirtual:
    case Op::Invokeinterface:
    case Op::InvokevirtualQuick:
    case Op::InvokeinterfaceQuick:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool sigReadsRegisters(Sig sig) noexcept {
  // Every signature except the operand-less ones touches at least one frame
  // slot (dst for RMW/putstatic-style reads, a/b for sources). Conservative
  // for pure writers (iconst reads nothing) - ReadsFrame is a capability bit,
  // and the at-most reading keeps the per-Sig table mechanical.
  switch (sig) {
    case Sig::None:
    case Sig::Branch:
    case Sig::Trap:
      return false;
    default:
      return true;
  }
}

[[nodiscard]] StencilFlag flagsForOp(Op op) noexcept {
  const rbc::OpInfo& oi = rbc::info(op);
  StencilFlag f = StencilFlag::None;
  if (rbc::hasEff(oi.effects, rbc::Eff::ReadsHeap)) {
    f = f | StencilFlag::ReadsHeap;
  }
  if (rbc::hasEff(oi.effects, rbc::Eff::WritesHeap)) {
    f = f | StencilFlag::WritesHeap;
  }
  if (rbc::hasEff(oi.effects, rbc::Eff::CanCall)) {
    f = f | StencilFlag::CanCall;
  }
  if (rbc::hasEff(oi.effects, rbc::Eff::CanAllocate)) {
    f = f | StencilFlag::CanAllocate;
  }
  if (rbc::hasEff(oi.effects, rbc::Eff::CanBranch)) {
    f = f | StencilFlag::CanBranch;
  }
  if (rbc::hasEff(oi.effects, rbc::Eff::IsSafepoint)) {
    f = f | StencilFlag::IsSafepoint;
  }
  if (rbc::hasEff(oi.effects, rbc::Eff::Quickened)) {
    f = f | StencilFlag::QuickenedOnly;
  }
  if (rbc::hasEff(oi.effects, rbc::Eff::CanTrap)) {
    // Stencil.h (Amendment B.3 pin): every trap-capable stencil carries
    // HasDeopt; its trap path enters the T0-directed deopt runtime.
    f = f | StencilFlag::CanTrap | StencilFlag::HasDeopt;
  }
  // WritesResult derives from OpInfo::result; descriptor-derived results
  // (getfield/getstatic/invoke*, all quickened results) are Bottom in OpInfo
  // and so do not set the bit in v0 - a documented manifest limitation.
  if (oi.result != rbc::RType::Bottom) {
    f = f | StencilFlag::WritesResult;
  }
  if (sigReadsRegisters(oi.sig)) {
    f = f | StencilFlag::ReadsFrame;
  }
  if (opHasICSite(op)) {
    f = f | StencilFlag::HasIC;
  }
  return f;
}

// --- frame accounting (per Sig; documented approximation) ---------------------

struct FrameAccounting {
  std::uint8_t reads;
  std::uint8_t writes;
};

[[nodiscard]] FrameAccounting frameAccountingFor(Sig sig) noexcept {
  switch (sig) {
    case Sig::None:
    case Sig::Branch:
    case Sig::Trap:
      return {0, 0};
    case Sig::Reg:
    case Sig::RegBranch:
    case Sig::Guard:
    case Sig::GuardCp:
      return {1, 0};
    case Sig::RegRegBranch:
      return {2, 0};
    case Sig::RegCpBranch:
      return {1, 0};
    case Sig::RegImm:
    case Sig::RegCp:
    case Sig::RegSlot:
    case Sig::SlotReg:
    case Sig::RegReg:
    case Sig::RegRegImm:
    case Sig::RegRegCp:
      return {1, 1};
    case Sig::RegRegRegCp:
      return {2, 1};
    case Sig::RegRegReg:
      // Array stores (P3) read dst+a+b and write nothing; arithmetic reads
      // a+b and writes dst. The per-Sig table takes the task-pinned
      // arithmetic shape (2 reads / 1 write) - an "at most" accounting.
      return {2, 1};
    case Sig::Call:
    case Sig::CallQuick:
      return {kMaxCallFrameReads, 1};
  }
  return {0, 0};
}

// --- nominal size --------------------------------------------------------------

[[nodiscard]] std::uint32_t nominalSizeForOp(Op op) noexcept {
  switch (rbc::info(op).sig) {
    case Sig::Call:
    case Sig::CallQuick:
      return kCallStencilNominalBytes;
    case Sig::Guard:
    case Sig::GuardCp:
    case Sig::Trap:
      return kGuardStencilNominalBytes;
    default:
      return kOpcodeStencilNominalBytes;
  }
}

// --- availability (every decision cited; see the file header) ------------------

[[nodiscard]] StencilAvailability availabilityFor(Op op) noexcept {
  switch (op) {
    case Op::Invokedynamic:
      return StencilAvailability::NeedsRuntimeFeature;
    case Op::GuardClass:
      return StencilAvailability::NeedsRuntimeFeature;
    case Op::DeoptTrap:
      return StencilAvailability::NeedsRuntimeFeature;
    case Op::Multianewarray:
      return StencilAvailability::NeedsRuntimeFeature;
    default:
      // Includes invoke* (all forms) and ldc since set version 2: the T1
      // runtime-helper seam executes them (MSG-20260830-004).
      return StencilAvailability::Available;
  }
}

// --- hole declaration (must agree with PlanBuilder.cpp stepSuppliesSource) -----

[[nodiscard]] bool readsFieldRef(Op op) noexcept {
  switch (op) {
    case Op::Getfield:
    case Op::Putfield:
    case Op::Getstatic:
    case Op::Putstatic:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool readsClass(Op op) noexcept {
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
}

void addHole(StencilDesc& d, PatchKind kind, PatchSource source,
             std::uint8_t width) noexcept {
  if (d.patch_count >= StencilDesc::kMaxPatches) {
    return; // defensive: the widest v0 hole set is 3; never trips for builtin v0
  }
  PatchSiteDesc& h = d.patches[d.patch_count];
  h.code_offset = 0; // populated when machine bytes exist (Stencil.h)
  h.kind = kind;
  h.width = width;
  h.source = source;
  ++d.patch_count;
}

// Declares the relocation holes one op needs. Widths: every v0 hole is 4 bytes
// (slot numbers, cp indices, branch pcs, ids all fit u32; the plan builder's
// width check is the defensive mechanism for custom sets with narrower holes).
void addHolesForOp(StencilDesc& d, Op op) noexcept {
  const Sig sig = rbc::info(op).sig;
  switch (sig) {
    case Sig::None:
    case Sig::Reg:
    case Sig::RegReg:
    case Sig::RegRegReg:
    case Sig::RegRegImm:
      // No relocation-relevant operands (newarray's atype is re-read from the
      // stream by the instantiator, like every register operand).
      break;
    case Sig::RegImm:
      // iconst/iinc/fconst (float bits)/aconst_null (canonical 0).
      addHole(d, PatchKind::Imm32, PatchSource::InsImm, 4);
      break;
    case Sig::RegSlot:
    case Sig::SlotReg:
      addHole(d, PatchKind::SlotOffset, PatchSource::FrameSlot, 4);
      break;
    case Sig::RegCp:
    case Sig::RegRegCp:
    case Sig::RegRegRegCp: {
      if (op == Op::GetfieldQuick || op == Op::PutfieldQuick) {
        // Quickened field ops: imm IS the resolved byte offset (SS7 pin:
        // slot * sizeof(Value) = slot * 16) - plan-computable, no cp, no IC.
        addHole(d, PatchKind::FieldOffset, PatchSource::InsImm, 4);
        break;
      }
      addHole(d, PatchKind::ConstPoolIndex, PatchSource::CpIndex, 4);
      if (readsFieldRef(op)) {
        addHole(d, PatchKind::FieldOffset, PatchSource::RuntimeField, 4);
      } else if (readsClass(op)) {
        addHole(d, PatchKind::KlassId, PatchSource::RuntimeClass, 4);
      }
      break;
    }
    case Sig::RegCpBranch:
      // Switch: the pool index of the canonical SwitchTable payload; the
      // instantiator expands the jump table from cp[imm].ints (Verifier.cpp
      // pins the [low,high,def,targets...] / [N,def,pairs...] layouts).
      addHole(d, PatchKind::ConstPoolIndex, PatchSource::CpIndex, 4);
      break;
    case Sig::Branch:
    case Sig::RegBranch:
    case Sig::RegRegBranch:
      addHole(d, PatchKind::BranchRel32, PatchSource::BranchTarget, 4);
      break;
    case Sig::Call:
      // Un-quickened call: MethodId resolved from cp[imm] MethodRef;
      // CallSiteId/ICStubAddr for the (MethodId, call pc) site key; DeoptId
      // carries the plan-allocated CallException point id.
      addHole(d, PatchKind::MethodId, PatchSource::RuntimeMethod, 4);
      addHole(d, PatchKind::CallSiteId, PatchSource::RuntimeICStub, 4);
      addHole(d, PatchKind::DeoptId, PatchSource::DeoptIdSource, 4);
      break;
    case Sig::CallQuick: {
      // Quickened call: imm already carries the resolved id per SS7 -
      // MethodId (method-table index) for static/special, IC site id (= call
      // pc) for virtual/interface - so the first hole is plan-computable and
      // its KIND follows the family to keep the manifest honest.
      if (op == Op::InvokevirtualQuick || op == Op::InvokeinterfaceQuick) {
        addHole(d, PatchKind::CallSiteId, PatchSource::InsImm, 4);
      } else {
        addHole(d, PatchKind::MethodId, PatchSource::InsImm, 4);
      }
      addHole(d, PatchKind::CallSiteId, PatchSource::RuntimeICStub, 4);
      addHole(d, PatchKind::DeoptId, PatchSource::DeoptIdSource, 4);
      break;
    }
    case Sig::Guard:
      addHole(d, PatchKind::DeoptId, PatchSource::DeoptIdSource, 4);
      break;
    case Sig::GuardCp:
      // guard_class: the rbc DeoptId lives in Ins::b (Sig::GuardCp pin in
      // rbc/Opcode.h), the class in cp[imm].
      addHole(d, PatchKind::DeoptId, PatchSource::DeoptIdSource, 4);
      addHole(d, PatchKind::KlassId, PatchSource::RuntimeClass, 4);
      break;
    case Sig::Trap:
      addHole(d, PatchKind::DeoptId, PatchSource::DeoptIdSource, 4);
      break;
  }
}

// --- opcode stencil ------------------------------------------------------------

[[nodiscard]] StencilDesc makeOpStencil(Op op) {
  StencilDesc d;
  d.name = rbc::opName(op);
  d.category = StencilCategory::Opcode;
  d.pattern[0].op = op;
  d.pattern_len = 1;
  d.flags = flagsForOp(op);
  d.availability = availabilityFor(op);
  addHolesForOp(d, op);
  const FrameAccounting fa = frameAccountingFor(rbc::info(op).sig);
  d.frame_reads = fa.reads;
  d.frame_writes = fa.writes;
  d.nominal_size = nominalSizeForOp(op);
  return d;
}

// --- superinstructions (SS3.2) --------------------------------------------------

struct StepSpec {
  Op op;
  std::uint16_t useAsA; // earlier step whose dst feeds this step's a operand
  std::uint16_t useAsB; // ... feeds this step's b operand
};

struct SuperSpec {
  const char* name;
  StepSpec steps[3];
  std::uint8_t stepCount;
};

constexpr std::uint16_t kNoLink = 0xFFFF; // PatternStep sentinel

// The seven v0 superinstructions from the StencilSet.h spec, in spec order.
// Producer-consumer links are LOCAL window checks (Amendment A): the fusion
// is only legal when the window really is producer/consumer.
constexpr SuperSpec kSuperSpecs[] = {
    {"iload_iload_iadd",
     {{Op::Iload, kNoLink, kNoLink},
      {Op::Iload, kNoLink, kNoLink},
      {Op::Iadd, 0, 1}},
     3},
    {"iload_iload_isub",
     {{Op::Iload, kNoLink, kNoLink},
      {Op::Iload, kNoLink, kNoLink},
      {Op::Isub, 0, 1}},
     3},
    {"iload_iload_imul",
     {{Op::Iload, kNoLink, kNoLink},
      {Op::Iload, kNoLink, kNoLink},
      {Op::Imul, 0, 1}},
     3},
    {"aload_getfield",
     {{Op::Aload, kNoLink, kNoLink},
      {Op::Getfield, 0, kNoLink},
      {Op::Nop, kNoLink, kNoLink}},
     2},
    {"aload_arraylength_if",
     {{Op::Aload, kNoLink, kNoLink},
      {Op::Arraylength, 0, kNoLink},
      {Op::IfIcmpgt, 1, kNoLink}},
     3},
    {"iinc_goto",
     {{Op::Iinc, kNoLink, kNoLink},
      {Op::Goto, kNoLink, kNoLink},
      {Op::Nop, kNoLink, kNoLink}},
     2},
    {"aload_getfield_ireturn",
     {{Op::Aload, kNoLink, kNoLink},
      {Op::Getfield, 0, kNoLink},
      {Op::Ireturn, 1, kNoLink}},
     3},
};

static_assert(sizeof(kSuperSpecs) / sizeof(kSuperSpecs[0]) == 7,
              "the v0 manifest carries exactly the seven spec'd superinstructions");

[[nodiscard]] std::uint8_t saturatingAddU8(std::uint8_t a, std::uint8_t b) noexcept {
  const unsigned sum = static_cast<unsigned>(a) + static_cast<unsigned>(b);
  return static_cast<std::uint8_t>(sum > 255u ? 255u : sum);
}

[[nodiscard]] StencilDesc makeSuperStencil(const SuperSpec& spec) {
  StencilDesc d;
  d.name = spec.name;
  d.category = StencilCategory::Superinstruction;
  d.availability = StencilAvailability::Available;
  d.flags = StencilFlag::None;
  d.frame_reads = 0;
  d.frame_writes = 0;
  std::uint64_t sizeSum = 0;
  for (std::uint8_t k = 0; k < spec.stepCount; ++k) {
    // Each part is built by the SAME makeOpStencil pipeline, then combined:
    // pattern step, union of flags, summed frame accounting, concatenated
    // holes (in step order - the builder fills them step-wise), summed sizes.
    const StencilDesc part = makeOpStencil(spec.steps[k].op);
    d.pattern[k].op = spec.steps[k].op;
    d.pattern[k].useAsA = spec.steps[k].useAsA;
    d.pattern[k].useAsB = spec.steps[k].useAsB;
    d.pattern[k].thenDstOf = kNoLink; // reserved (Stencil.h)
    d.flags = d.flags | part.flags;
    d.frame_reads = saturatingAddU8(d.frame_reads, part.frame_reads);
    d.frame_writes = saturatingAddU8(d.frame_writes, part.frame_writes);
    sizeSum += part.nominal_size;
    for (std::uint8_t h = 0; h < part.patch_count; ++h) {
      addHole(d, part.patches[h].kind, part.patches[h].source, part.patches[h].width);
    }
  }
  d.pattern_len = spec.stepCount;
  d.nominal_size =
      static_cast<std::uint32_t>(sizeSum) - kFusionSavingBytes;
  return d;
}

// --- manifest-only entries (pattern_len = 0; the builder never selects them) ----
//
// Placeholder descriptors for the codegen-team artifacts (SS3.3/SS3.5-
// SS3.7): they carry representative metadata (holes, effects, nominal sizes)
// so the manifest is complete (Stencil Rule 2) and instantiation-time
// consumers can already look them up by name, but no plan ever instantiates
// them - their availability is NeedsRuntimeFeature and candidates() never
// returns them.

[[nodiscard]] constexpr StencilFlag operatorOrAll(
    std::initializer_list<StencilFlag> fs) noexcept {
  StencilFlag out = StencilFlag::None;
  for (const StencilFlag f : fs) {
    out = out | f;
  }
  return out;
}

struct ManifestSpec {
  const char* name;
  StencilCategory category;
  StencilFlag flags;
  std::uint32_t nominalSize;
  struct Hole {
    PatchKind kind;
    PatchSource source;
    std::uint8_t width;
  } holes[3];
  std::uint8_t holeCount;
  std::uint8_t frameReads;
  std::uint8_t frameWrites;
  // LAST so the pre-existing positional entries (which omit it) keep their
  // shape; unspecified means NeedsRuntimeFeature.
  StencilAvailability availability = StencilAvailability::NeedsRuntimeFeature;
};

constexpr std::uint8_t kW4 = 4; // every v0 hole is 4 bytes wide

constexpr ManifestSpec kManifestSpecs[] = {
    // Call stencils (SS3.3). call_static: direct dispatch, no IC site.
    {"call_static", StencilCategory::Call,
     operatorOrAll({StencilFlag::CanCall, StencilFlag::CanTrap, StencilFlag::HasDeopt,
                    StencilFlag::IsSafepoint, StencilFlag::ReadsFrame}),
     kCallStencilNominalBytes,
     {{PatchKind::MethodId, PatchSource::RuntimeMethod, kW4},
      {PatchKind::DeoptId, PatchSource::DeoptIdSource, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     2, kMaxCallFrameReads, 1},
    // Dispatched call forms: method + IC site + exception deopt.
    {"call_virtual_monomorphic", StencilCategory::Call,
     operatorOrAll({StencilFlag::CanCall, StencilFlag::CanTrap, StencilFlag::HasDeopt,
                    StencilFlag::IsSafepoint, StencilFlag::HasIC, StencilFlag::ReadsFrame}),
     kCallStencilNominalBytes,
     {{PatchKind::MethodId, PatchSource::RuntimeMethod, kW4},
      {PatchKind::CallSiteId, PatchSource::RuntimeICStub, kW4},
      {PatchKind::DeoptId, PatchSource::DeoptIdSource, kW4}},
     3, kMaxCallFrameReads, 1},
    {"call_virtual_bimorphic", StencilCategory::Call,
     operatorOrAll({StencilFlag::CanCall, StencilFlag::CanTrap, StencilFlag::HasDeopt,
                    StencilFlag::IsSafepoint, StencilFlag::HasIC, StencilFlag::ReadsFrame}),
     kCallStencilNominalBytes,
     {{PatchKind::MethodId, PatchSource::RuntimeMethod, kW4},
      {PatchKind::CallSiteId, PatchSource::RuntimeICStub, kW4},
      {PatchKind::DeoptId, PatchSource::DeoptIdSource, kW4}},
     3, kMaxCallFrameReads, 1},
    {"call_interface_megamorphic", StencilCategory::Call,
     operatorOrAll({StencilFlag::CanCall, StencilFlag::CanTrap, StencilFlag::HasDeopt,
                    StencilFlag::IsSafepoint, StencilFlag::HasIC, StencilFlag::ReadsFrame}),
     kCallStencilNominalBytes,
     {{PatchKind::MethodId, PatchSource::RuntimeMethod, kW4},
      {PatchKind::CallSiteId, PatchSource::RuntimeICStub, kW4},
      {PatchKind::DeoptId, PatchSource::DeoptIdSource, kW4}},
     3, kMaxCallFrameReads, 1},
    {"call_invokedynamic", StencilCategory::Call,
     operatorOrAll({StencilFlag::CanCall, StencilFlag::CanTrap, StencilFlag::HasDeopt,
                    StencilFlag::IsSafepoint, StencilFlag::HasIC, StencilFlag::ReadsFrame}),
     kCallStencilNominalBytes,
     {{PatchKind::MethodId, PatchSource::RuntimeMethod, kW4},
      {PatchKind::CallSiteId, PatchSource::RuntimeICStub, kW4},
      {PatchKind::DeoptId, PatchSource::DeoptIdSource, kW4}},
     3, kMaxCallFrameReads, 1},
    // Guard primitives (SS3.5/SS9.1). guard_non_null is the Available one:
    // its single hole is the DeoptId, plan-computable from the guard's imm.
    // NOTE (name collision, spec-inherent): the OPCODE stencil for
    // Op::GuardNonNull is also named "guard_non_null" (the manifest names
    // opcode stencils by mnemonic), so StencilSet::find answers that name
    // with the opcode stencil - it sorts ahead of the manifest tail and is
    // the plannable twin of this standalone Guard-category primitive.
    {"guard_non_null", StencilCategory::Guard,
     operatorOrAll({StencilFlag::CanTrap, StencilFlag::CanBranch, StencilFlag::HasDeopt,
                    StencilFlag::ReadsFrame}),
     kGuardStencilNominalBytes,
     {{PatchKind::DeoptId, PatchSource::DeoptIdSource, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     1, 1, 0, StencilAvailability::Available},
    // Guard primitive (SS3.5/SS9.1: cmp klass-id, jne deopt).
    {"guard_class_id", StencilCategory::Guard,
     operatorOrAll({StencilFlag::CanTrap, StencilFlag::CanBranch, StencilFlag::HasDeopt,
                    StencilFlag::ReadsFrame}),
     kGuardStencilNominalBytes,
     {{PatchKind::KlassId, PatchSource::RuntimeClass, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     1, 1, 0},
    // Deopt stubs (SS3.6): save registers, build the deopt context, enter the
    // runtime; never return to compiled code.
    {"deopt_stub_eager", StencilCategory::Deopt,
     operatorOrAll({StencilFlag::CanBranch, StencilFlag::HasDeopt, StencilFlag::ReadsFrame}),
     kDeoptStencilNominalBytes,
     {{PatchKind::DeoptId, PatchSource::DeoptIdSource, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     1, 0, 0},
    {"deopt_stub_uncommon_trap", StencilCategory::Deopt,
     operatorOrAll({StencilFlag::CanBranch, StencilFlag::HasDeopt, StencilFlag::ReadsFrame}),
     kDeoptStencilNominalBytes,
     {{PatchKind::DeoptId, PatchSource::DeoptIdSource, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     1, 0, 0},
    {"deopt_stub_exception", StencilCategory::Deopt,
     operatorOrAll({StencilFlag::CanBranch, StencilFlag::HasDeopt, StencilFlag::ReadsFrame}),
     kDeoptStencilNominalBytes,
     {{PatchKind::DeoptId, PatchSource::DeoptIdSource, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     1, 0, 0},
    // Runtime helper trampolines (SS3.7).
    {"allocation_slow_path", StencilCategory::RuntimeHelper,
     operatorOrAll({StencilFlag::CanAllocate, StencilFlag::IsSafepoint,
                    StencilFlag::HasDeopt, StencilFlag::ReadsFrame}),
     kHelperStencilNominalBytes,
     {{PatchKind::RuntimeHelperId, PatchSource::RuntimeHelper, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     1, 0, 1},
    {"throw_null_pointer", StencilCategory::RuntimeHelper,
     operatorOrAll({StencilFlag::CanTrap, StencilFlag::HasDeopt, StencilFlag::ReadsFrame}),
     kHelperStencilNominalBytes,
     {{PatchKind::RuntimeHelperId, PatchSource::RuntimeHelper, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     1, 0, 0},
    {"throw_array_index_oob", StencilCategory::RuntimeHelper,
     operatorOrAll({StencilFlag::CanTrap, StencilFlag::HasDeopt, StencilFlag::ReadsFrame}),
     kHelperStencilNominalBytes,
     {{PatchKind::RuntimeHelperId, PatchSource::RuntimeHelper, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     1, 0, 0},
    {"throw_arithmetic", StencilCategory::RuntimeHelper,
     operatorOrAll({StencilFlag::CanTrap, StencilFlag::HasDeopt, StencilFlag::ReadsFrame}),
     kHelperStencilNominalBytes,
     {{PatchKind::RuntimeHelperId, PatchSource::RuntimeHelper, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     1, 0, 0},
    {"class_init_check", StencilCategory::RuntimeHelper,
     operatorOrAll({StencilFlag::CanCall, StencilFlag::CanTrap, StencilFlag::HasDeopt,
                    StencilFlag::ReadsFrame}),
     kHelperStencilNominalBytes,
     {{PatchKind::RuntimeHelperId, PatchSource::RuntimeHelper, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     1, 0, 0},
    {"safepoint_slow_path", StencilCategory::RuntimeHelper,
     operatorOrAll({StencilFlag::IsSafepoint, StencilFlag::CanBranch,
                    StencilFlag::ReadsFrame}),
     kHelperStencilNominalBytes,
     {{PatchKind::RuntimeHelperId, PatchSource::RuntimeHelper, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4},
      {PatchKind::Imm32, PatchSource::None, kW4}},
     1, 0, 0},
};

static_assert(sizeof(kManifestSpecs) / sizeof(kManifestSpecs[0]) == 16,
              "the v0 manifest carries exactly the sixteen spec'd manifest-only entries");

} // namespace

// --- builtinStencilSetV0 --------------------------------------------------------

StencilSet builtinStencilSetV0() {
  StencilSet set;
  set.version.magic = kStencilSetMagic;
  set.version.version = kStencilSetVersionV0;
  set.version.target_arch = 0; // 0 = target-neutral descriptors (v0)
  set.version.abi_hash = 0;    // no machine bytes yet

  std::vector<StencilDesc>& stencils = set.stencils;

  // Entry 0: the null stencil (Stencil.h: validity is one compare).
  stencils.push_back(StencilDesc{});

  // Pattern-bearing entries: every opcode stencil (in Op order) plus the
  // superinstructions (in spec order). The final order below is produced by
  // ONE stable sort keyed on (first op, pattern_len DESC, Available before
  // NeedsRuntimeFeature) with the construction order as tie-break, so
  // candidates(op) can return the contiguous, correctly-ordered span of
  // entries whose pattern starts with `op` (Rule 124: deterministic).
  std::vector<StencilDesc> patterned;
  patterned.reserve(static_cast<std::size_t>(rbc::opCount()) +
                    sizeof(kSuperSpecs) / sizeof(kSuperSpecs[0]));
  for (std::uint16_t o = 0; o < rbc::opCount(); ++o) {
    patterned.push_back(makeOpStencil(static_cast<Op>(o)));
  }
  for (const SuperSpec& spec : kSuperSpecs) {
    patterned.push_back(makeSuperStencil(spec));
  }
  const auto patternKey = [](const StencilDesc& d) {
    return std::tuple(static_cast<std::uint16_t>(d.pattern[0].op),
                      -static_cast<int>(d.pattern_len),
                      static_cast<int>(d.availability));
  };
  std::stable_sort(patterned.begin(), patterned.end(),
                   [&patternKey](const StencilDesc& a, const StencilDesc& b) {
                     return patternKey(a) < patternKey(b);
                   });
  for (StencilDesc& d : patterned) {
    stencils.push_back(std::move(d));
  }
  patterned.clear();
  patterned.shrink_to_fit();

  // Manifest-only entries (spec order; default NeedsRuntimeFeature,
  // pattern_len = 0 - the builder never selects them, so they only need to
  // BE; guard_non_null is the Available exception per the StencilSet.h spec).
  for (const ManifestSpec& spec : kManifestSpecs) {
    StencilDesc d;
    d.name = spec.name;
    d.category = spec.category;
    d.flags = spec.flags;
    d.availability = spec.availability;
    d.nominal_size = spec.nominalSize;
    d.frame_reads = spec.frameReads;
    d.frame_writes = spec.frameWrites;
    for (std::uint8_t h = 0; h < spec.holeCount; ++h) {
      addHole(d, spec.holes[h].kind, spec.holes[h].source, spec.holes[h].width);
    }
    stencils.push_back(std::move(d));
  }

  // Dense ids: StencilId == vector index (Rule 15), assigned once here so
  // candidates()/desc() are pure index arithmetic afterwards.
  for (std::size_t i = 0; i < stencils.size(); ++i) {
    stencils[i].id = StencilId{static_cast<std::uint32_t>(i)};
  }

  return set;
}

// --- StencilSet lookups ----------------------------------------------------------

StencilId StencilSet::find(std::string_view name) const noexcept {
  // Cold path (Rule 16): one linear scan by canonical name. First match wins;
  // the null stencil answers "" with the invalid StencilId{0}.
  for (std::size_t i = 0; i < stencils.size(); ++i) {
    if (stencils[i].name == name) {
      return StencilId{static_cast<std::uint32_t>(i)};
    }
  }
  return StencilId{0};
}

std::span<const StencilDesc> StencilSet::candidates(rbc::Op op) const {
  // The vector follows the layout discipline builtinStencilSetV0() produces
  // (and any deterministic builder should mirror):
  //   [0]                 the null stencil        (pattern_len == 0)
  //   [1 .. M)            pattern-bearing entries, sorted by (first op asc,
  //                       pattern_len desc, availability asc, construction order)
  //   [M .. end)          manifest-only entries   (pattern_len == 0)
  // "pattern_len == 0" is monotone over [1, end) under that discipline, so M
  // is found with one binary search, and the pattern[0].op ordering inside
  // [1, M) with a second. The returned span is therefore the candidate list
  // in contract order (longest-pattern-first, Available-before-NeedsRuntime
  // Feature) with zero per-call allocation. Sets that violate the discipline
  // get degraded (never UB: every index stays in bounds) results.
  const std::size_t n = stencils.size();
  if (n <= 1) {
    return {};
  }
  std::size_t lo = 1;
  std::size_t hi = n;
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (stencils[mid].pattern_len == 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  const std::size_t m = lo; // first manifest/null-tail entry
  const std::uint16_t want = static_cast<std::uint16_t>(op);
  std::size_t blo = 1;
  std::size_t bhi = m;
  while (blo < bhi) {
    const std::size_t mid = blo + (bhi - blo) / 2;
    if (static_cast<std::uint16_t>(stencils[mid].pattern[0].op) < want) {
      blo = mid + 1;
    } else {
      bhi = mid;
    }
  }
  std::size_t end = blo;
  while (end < m && static_cast<std::uint16_t>(stencils[end].pattern[0].op) == want) {
    ++end;
  }
  if (blo >= end) {
    return {};
  }
  return {stencils.data() + blo, end - blo};
}

} // namespace b2::baseline
