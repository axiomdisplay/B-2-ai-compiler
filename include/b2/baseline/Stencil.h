#pragma once
// B-2 baseline (T1) - stencil identity, categories, patch sites, descriptors.
//
// WHY THIS FILE EXISTS:
// T1 is the no-IR baseline JIT (Amendment A): it lowers verified RBC directly
// to a StencilPlan by copy-and-patch composition of precompiled stencils
// (docs/stencils.md). This header defines what a stencil IS at the metadata
// level - identity, category, declared patch holes, effect flags - because
// Stencil Rule 2 makes metadata mandatory, Stencil Rule 3 forbids patching
// anything not declared here, and Stencil Rule 4 requires versioning. The
// descriptor table (StencilSet, next header) is the manifest the codegen team
// fills with real machine bytes later; the plan builder consumes only this
// metadata, so T1 selection is testable before any target code exists.
//
// LAW PINS (docs/laws.md):
// - Rule 15: indices, never raw pointers, in every cross-object reference.
// - Rule 16: no strings in the hot path; names here are diagnostics/lookup
//   keys, resolved to StencilId once and then carried as indices.
// - Amendment A / B.1: T1 builds NO IR. A StencilDesc describes machine code
//   shape, not a compiler IR node; nothing in this file may grow analysis.
// - Amendment B.3: deopt direction is T1 -> T0. Every trap-capable stencil
//   carries HasDeopt and its DeoptId hole is declared here.

#include <cstdint>
#include <string_view>

#include "b2/rbc/Opcode.h"
#include "b2/rbc/Rbc.h"

namespace b2::baseline {

// --- stencil identity -------------------------------------------------------
//
// Strong index into a StencilSet (Rule 15). 0 is the null stencil (the set
// reserves it so validity is one compare, mirroring interp ObjRef discipline).
struct StencilId {
  std::uint32_t id = 0;
  [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
  [[nodiscard]] constexpr bool operator==(const StencilId&) const noexcept = default;
};

// Stencil archive/ABI version (Stencil Rule 4). A plan may only instantiate
// against a set whose version matches the one the plan was built for; the
// field travels with every StencilSet and every serialized StencilPlan.
struct StencilSetVersion {
  std::uint32_t magic = 0;        // format identity
  std::uint32_t version = 1;      // descriptor-table schema version
  std::uint32_t target_arch = 0;  // 0 = target-neutral descriptors (v0)
  std::uint32_t abi_hash = 0;     // target ABI hash once machine bytes exist
};

// --- categories (docs/stencils.md SS3) ---------------------------------------

enum class StencilCategory : std::uint8_t {
  Opcode,           // one stencil per RBC operation (SS3.1)
  Superinstruction, // fused common RBC sequences (SS3.2)
  Call,             // method calls: ABI, stack map, IC site (SS3.3)
  InlineCache,      // patchable dispatch stubs (SS3.4)
  Guard,            // lightweight T1 assumptions -> deopt (SS3.5)
  Deopt,            // stubs entering the deopt runtime (SS3.6)
  RuntimeHelper,    // trampolines to runtime services (SS3.7)
  NanBox,           // encode/decode/guard if NaN boxing is enabled (SS3.8)
  Vector,           // SWLP support stencils, T2+ only (SS3.9)
};

[[nodiscard]] constexpr std::string_view categoryName(StencilCategory c) noexcept {
  switch (c) {
    case StencilCategory::Opcode: return "opcode";
    case StencilCategory::Superinstruction: return "superinstruction";
    case StencilCategory::Call: return "call";
    case StencilCategory::InlineCache: return "ic";
    case StencilCategory::Guard: return "guard";
    case StencilCategory::Deopt: return "deopt";
    case StencilCategory::RuntimeHelper: return "helper";
    case StencilCategory::NanBox: return "nanbox";
    case StencilCategory::Vector: return "vector";
  }
  return "?";
}

// --- patch holes (docs/stencils.md SS4.2) ------------------------------------
//
// Stencil Rule 3: the runtime may patch ONLY holes declared with these kinds.
// The kinds are the closed set from SS4.2; adding one is a stencil-format RFC.

enum class PatchKind : std::uint8_t {
  Imm8, Imm16, Imm32, Imm64,       // raw immediate widths
  BranchRel8, BranchRel32,         // internal branch to another instance
  CallRel32,                       // call to another stencil/runtime
  AbsTarget64,                     // absolute code address
  SlotOffset,                      // byte offset of a frame slot (T0 layout)
  FieldOffset,                     // resolved instance-field byte offset
  KlassId,                         // resolved class id
  MethodId,                        // resolved method id (call target)
  CallSiteId,                      // inline-cache site id (= call rbc pc)
  DeoptId,                         // deopt point id
  ICStubAddr,                      // address of an IC stub
  ConstPoolIndex,                  // index into the method constant pool
  RuntimeHelperId,                 // id of a runtime helper trampoline
};

// Where a patch value comes from. PLAN-COMPUTABLE sources are filled by the
// plan builder from RBC alone (deterministic, golden-testable). RUNTIME
// sources are recorded here and resolved by the instantiator against the
// live runtime (field offsets, class/method ids, IC stubs) - the plan never
// blocks on them and the value stays PendingRuntime until instantiation.
enum class PatchSource : std::uint8_t {
  None,             // hole is fixed at stencil-build time (no patch needed)
  InsDst,           // value = instruction dst register number
  InsA,             // value = instruction a operand
  InsB,             // value = instruction b operand
  InsImm,           // value = instruction imm operand
  FrameSlot,        // value = imm as a T0 frame slot number
  CpIndex,          // value = imm as a constant-pool index
  BranchTarget,     // value = imm as an rbc pc (resolved at instantiation)
  DeoptIdSource,    // value = imm as the rbc DeoptId (guards/deopt_trap)
  RuntimeField,     // FieldOffset resolved from cp[imm] FieldRef
  RuntimeClass,     // KlassId resolved from cp[imm] Class/MethodRef
  RuntimeMethod,    // MethodId resolved from cp[imm] MethodRef
  RuntimeICStub,    // ICStubAddr for the call site (imm = call pc)
  RuntimeHelper,    // RuntimeHelperId (trap/alloc/monitor helpers)
};

// One declared hole in a stencil's code bytes (Stencil Rule 3: only described
// holes may be patched). `code_offset` is relative to the stencil start and
// is populated when machine bytes exist; v0 descriptors declare width+kind+
// source so plans and golden dumps are stable across that arrival.
struct PatchSiteDesc {
  std::uint32_t code_offset = 0;  // hole position in the stencil body
  PatchKind kind = PatchKind::Imm32;
  std::uint8_t width = 4;         // hole byte width (1/2/4/8)
  PatchSource source = PatchSource::None; // how instantiation fills it
};

// --- stencil flags ------------------------------------------------------------
//
// Mirrors rbc::Eff so the plan builder can check "can this point trap / call
// / safepoint" from the descriptor alone, and so the plan-level effect order
// is exactly RBC's effect order (Amendment A: no speculative reordering).
enum class StencilFlag : std::uint32_t {
  None = 0,
  ReadsHeap = 1u << 0,
  WritesHeap = 1u << 1,
  CanCall = 1u << 2,      // may invoke Java code (call/interface/builtin)
  CanTrap = 1u << 3,      // may throw (NPE/AIOOBE/arith/CCE...)
  CanAllocate = 1u << 4,  // may allocate (new/newarray/...)
  CanBranch = 1u << 5,    // control transfer (internal or deopt)
  IsSafepoint = 1u << 6,  // polls + requires a stack-map point
  HasDeopt = 1u << 7,     // owns a DeoptId hole / deopt path
  HasIC = 1u << 8,        // owns an inline-cache site
  WritesResult = 1u << 9, // writes a value to a dst frame slot
  ReadsFrame = 1u << 10,  // reads T0 frame slots
  QuickenedOnly = 1u << 11, // serves a quickened opcode
};

[[nodiscard]] constexpr StencilFlag operator|(StencilFlag a, StencilFlag b) noexcept {
  return static_cast<StencilFlag>(static_cast<std::uint32_t>(a) |
                                  static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool hasFlag(StencilFlag v, StencilFlag f) noexcept {
  return (static_cast<std::uint32_t>(v) & static_cast<std::uint32_t>(f)) != 0;
}

// --- stencil availability -----------------------------------------------------
//
// The v0 descriptor table is the COMPLETE manifest (one entry per servable
// Op / fusion), but not every entry can back a plan yet: real IC patching,
// invokedynamic linkage, and NaN boxing wait on runtime features. The plan
// builder refuses methods containing ops whose best stencil is not Available
// (T1 is always safe to abandon - never guess, never half-compile).
enum class StencilAvailability : std::uint8_t {
  Available,           // plannable now
  NeedsRuntimeFeature, // manifest entry only; plan builder must refuse
};

// --- the descriptor -----------------------------------------------------------

// Operand-shape matcher for superinstructions. A step matches an RBC
// instruction with opcode `op` plus optional LOCAL producer-consumer links to
// earlier matched steps (a peephole-window check, not dataflow analysis -
// the fusion is legal only when the window really is producer/consumer):
//   useAsA/useAsB = index of an earlier step whose dst register feeds this
//                   instruction's a/b operand (0xFFFF = unconstrained)
//   thenDstOf     = reserved for chained dst constraints (0xFFFF = none)
struct PatternStep {
  rbc::Op op = rbc::Op::Nop;
  std::uint16_t useAsA = 0xFFFF; // earlier step index feeding operand a
  std::uint16_t useAsB = 0xFFFF; // earlier step index feeding operand b
  std::uint16_t thenDstOf = 0xFFFF; // reserved (0xFFFF = none)
};

// Metadata for one stencil (Stencil Rule 2). Everything the plan builder and
// the instantiator need; nothing they don't. No raw pointers (Rule 15).
struct StencilDesc {
  StencilId id;                       // dense index in the owning StencilSet
  std::string_view name;              // "iadd_rrr", "aload_getfield_nullcheck"
  StencilCategory category = StencilCategory::Opcode;

  // What RBC this stencil serves. Opcode stencils: exactly one step. Super-
  // instructions: the exact matched sequence (SS3.2). Empty for categories
  // the plan builder never selects directly (Deopt/Helper/NanBox/Vector).
  static constexpr std::uint8_t kMaxPattern = 8;
  PatternStep pattern[kMaxPattern] = {};
  std::uint8_t pattern_len = 0;

  StencilFlag flags = StencilFlag::None;
  StencilAvailability availability = StencilAvailability::Available;

  // Declared holes, in code order (Stencil Rule 3). The k-th hole of an
  // instance is filled from the k-th PatchValue of that instance.
  static constexpr std::uint8_t kMaxPatches = 8;
  PatchSiteDesc patches[kMaxPatches] = {};
  std::uint8_t patch_count = 0;

  // Nominal code size in bytes. v0: layout accounting figure driving the
  // plan's deterministic output_offset arithmetic (golden-testable); becomes
  // the real stencil size once machine bytes exist (Rule 4 versioned tables).
  std::uint32_t nominal_size = 0;

  // Frame effects: how many T0 frame slots this stencil reads/writes at most
  // (result + sources). The T1 frame model keeps values in T0-layout frame
  // slots (docs/stencils.md SS8; interp_contract v1.0.0 SS1), so "register
  // allocation" for T1 is: pick frame slots from the RBC operands. Nothing
  // here may grow into global allocation (Amendment A).
  std::uint8_t frame_reads = 0;
  std::uint8_t frame_writes = 0;

  [[nodiscard]] constexpr bool servesOp(rbc::Op op) const noexcept {
    return pattern_len >= 1 && pattern[0].op == op;
  }
};

} // namespace b2::baseline
