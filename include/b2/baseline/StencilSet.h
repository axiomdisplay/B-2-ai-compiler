#pragma once
// B-2 baseline (T1) - the stencil set: the versioned descriptor registry the
// plan builder selects from.
//
// WHY THIS FILE EXISTS:
// Stencil Rule 2 (every stencil must have metadata) needs a home for the
// metadata; Stencil Rule 4 (versioning) needs the version to travel with it.
// The StencilSet is that home: the ordered table of StencilDesc entries plus
// the lookup paths selection needs. In v0 the table is target-neutral
// (target_arch = 0): descriptors describe holes, effects, and nominal sizes,
// but carry no machine bytes. When the codegen team's stencil generator
// (docs/stencils.md SS5) delivers real archives, the SAME descriptors gain
// code offsets and true sizes under a bumped version, and plans rebuild.
//
// LAW PINS (docs/laws.md):
// - Rule 15: StencilId is an index into this table; no pointer handles.
// - Rule 16: name lookup happens ONCE per selection decision (cold path);
//   the hot builder path indexes by (Op -> first candidate) jump table.
// - Rule 23 (via the manifest): nominal sizes and availability are named,
//   documented constants - not per-call magic numbers.
// - Stencil Rule 4: version mismatch between plan and set is a hard
//   instantiation refusal, never a best-effort patch.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "b2/baseline/Stencil.h"
#include "b2/rbc/Opcode.h"

namespace b2::baseline {

// Selection quality hint for competing stencils that serve the same first
// opcode: superinstructions are preferred (SS3.2: fewer patches, less code,
// less branch overhead); among equals, longer patterns win. The builder
// implements greedy longest-match; this order is the tie-break contract.
struct StencilSet {
  StencilSetVersion version{};
  std::vector<StencilDesc> stencils; // id == index; entry 0 is the null stencil

  // Cold lookup by canonical name ("iadd_rrr"); invalid if absent.
  [[nodiscard]] StencilId find(std::string_view name) const noexcept;

  // Stencils whose pattern STARTS with `op`, longest-pattern-first, Available
  // entries before NeedsRuntimeFeature ones. The hot selection path calls
  // this once per pc and walks the (tiny) candidate list.
  [[nodiscard]] std::span<const StencilDesc> candidates(rbc::Op op) const;

  [[nodiscard]] const StencilDesc& desc(StencilId id) const noexcept {
    return stencils[id.id];
  }
  [[nodiscard]] bool empty() const noexcept { return stencils.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return stencils.size(); }
};

// The v0 built-in set (target-neutral manifest):
//
// - One opcode stencil for EVERY rbc::Op, named "<mnemonic>" (e.g. "iadd",
//   "iload", "if_icmplt", "getfield_quick"). Availability: Available for all
//   ops T1 can plan today; NeedsRuntimeFeature for the ops whose execution
//   path the v0 runtime cannot back in compiled form (invokevirtual/
//   invokeinterface without IC patching, invokedynamic, ldc of non-string
//   constants, guard_class KlassId holes pending real class-id patching).
//   The exact availability map mirrors interp_contract SS6/SS7 - the honest
//   list of what actually runs.
// - Superinstructions (Available), all local producer-consumer fusions:
//     iload_iload_iadd        (l0,l1 -> iadd)
//     iload_iload_isub / imul (same shape, other arithmetic)
//     aload_getfield          (receiver load + field read incl. null trap)
//     aload_arraylength_if    (load + length + bounds-shaped compare)
//     iinc_goto               (counter bump + backedge, the loop idiom)
//     aload_getfield_ireturn  (getter idiom)
//   Each declares the fused hole set (two slot offsets, one FieldOffset,
//   one BranchRel32, ...) and the union of the parts' flags.
// - Call stencils (NeedsRuntimeFeature until IC patching lands):
//   call_static, call_virtual_monomorphic, call_virtual_bimorphic,
//   call_interface_megamorphic, call_invokedynamic.
// - Guard stencils: guard_non_null (Available: hole = DeoptId),
//   guard_class_id (NeedsRuntimeFeature: KlassId hole).
// - Deopt stencils: deopt_stub_eager, deopt_stub_uncommon_trap,
//   deopt_stub_exception (NeedsRuntimeFeature: they are the SS6 stubs the
//   instantiator links; plans reference the DeoptPoint ids, not the stubs).
// - Runtime helpers: allocation_slow_path, throw_null_pointer,
//   throw_array_index_oob, throw_arithmetic, class_init_check,
//   safepoint_slow_path (NeedsRuntimeFeature: trampolines to real runtime
//   entry points that do not exist before instantiation).
// - NaN box and Vector categories: NOT in v0 (Part XVIII: NaN boxing stays
//   disabled by default; SWLP is T2+). The manifest reserves their absence.
//
// Deterministic: two calls build byte-identical tables (Rule 124).
[[nodiscard]] StencilSet builtinStencilSetV0();

// Current set version constants (Stencil Rule 4). Version 2 flips the
// un-quickened invoke* + ldc to Available (MSG-20260830-004: the T1
// runtime-helper seam) and marks multianewarray NeedsRuntimeFeature (v1
// instantiation gap). Version 1 plans must not instantiate against it.
inline constexpr std::uint32_t kStencilSetMagic = 0x32737463u; // "2stc"
inline constexpr std::uint32_t kStencilSetVersionV0 = 2;
// Historical: the pre-flip manifest (frozen for golden replay of old plans).
inline constexpr std::uint32_t kStencilSetVersionV1 = 1;

} // namespace b2::baseline
