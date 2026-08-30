// B-2 baseline (T1) - the deterministic plan dump (golden-test format).
//
// WHY THIS FILE EXISTS:
// Rule 124 requires deterministic replay to be PROVABLE, and the Compiler.h
// contract pins the proof format: "line-oriented, stable, and complete".
// This file renders a StencilPlan plus its StencilSet as plain ASCII - the
// golden fixture every plan test diffs byte-for-byte. The format may only
// grow by APPENDING sections (versioned, per the Compiler.h comment), so
// every existing line below is frozen the moment tests pin it.
//
// FORMAT (see the Compiler.h dump contract; instance/patch line shapes are
// pinned verbatim by the T1-A task contract):
//   plan method=<name> descriptor=<desc> static=<0|1> index=<method_index>
//   frame regs=<num_regs> locals=<num_locals>
//   set magic=0x<magic> version=<version> target_arch=<arch> abi_hash=0x<abi>
//   summary code_size=<n> entry_native_offset=<n> instances=<n> fusions=<n>
//          patches=<n> deopt_points=<n> safepoints=<n>
//     inst <i> @<output_offset>+<size> <stencil-name> rbc[<start>,<end>) patches:<k> exc:<covers>
//       patch <site> <kind-name> <- <source-name>[ [pending]] = 0x<hex>
//   pc_map:
//     pcmap <i> native=<off> rbc=<pc> inst=<i>
//   stack_maps:
//     smap <i> native=<off> rbc=<pc> kind=<poll|call|trap> deopt=<idx|->
//   deopt_points:
//     dpt <i> id=0x<hex> native=<off> rbc=<pc> reason=<trap|guard|call-exception|uncommon> pending_exception=<0|1> stack_map=<idx|->
//   exception_edges:
//     edge <i> rbc[<start>,<end>) handler=<pc> catch=<cp|-1> native=<off>
//   end plan instances=<n> pc_map=<n> stack_maps=<n> deopt_points=<n> exception_edges=<n>
//
// STABILITY DISCIPLINE (Rule 124):
// - LF line endings, no trailing whitespace, exactly one final newline.
// - No pointers, no addresses, no locale-dependent formatting: integers via
//   std::format (locale-independent by construction), hex lowercase and
//   unpadded (same value => same bytes on every machine).
// - Enum spellings come from the local name tables below (lowercase-hyphen
//   forms); they are the golden words and must never be re-spelled.
// - Hostile/incomplete plans never crash the dumper: invalid stencil ids and
//   out-of-range enum values render as placeholders, not UB.

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "b2/baseline/Compiler.h"

namespace b2::baseline {
namespace {

// --- golden name tables (internal; also the reference spellings) -----------------

constexpr std::string_view kPatchKindNames[] = {
    "imm8",       "imm16",       "imm32",       "imm64",
    "branch-rel8", "branch-rel32", "call-rel32", "abs-target64",
    "slot-offset", "field-offset", "klass-id",   "method-id",
    "call-site-id", "deopt-id",    "ic-stub-addr", "const-pool-index",
    "runtime-helper-id",
};

constexpr std::string_view kPatchSourceNames[] = {
    "none",       "ins-dst",     "ins-a",       "ins-b",
    "ins-imm",    "frame-slot",  "cp-index",    "branch-target",
    "deopt-id",   "runtime-field", "runtime-class", "runtime-method",
    "runtime-ic-stub", "runtime-helper",
};

constexpr std::string_view kDeoptReasonNames[] = {
    "trap", "guard", "call-exception", "uncommon",
};

constexpr std::string_view kStackMapKindNames[] = {
    "poll", "call", "trap",
};

[[nodiscard]] std::string_view patchKindName(PatchKind k) noexcept {
  const auto idx = static_cast<std::size_t>(k);
  if (idx < sizeof(kPatchKindNames) / sizeof(kPatchKindNames[0])) {
    return kPatchKindNames[idx];
  }
  return "?";
}

[[nodiscard]] std::string_view patchSourceName(PatchSource s) noexcept {
  const auto idx = static_cast<std::size_t>(s);
  if (idx < sizeof(kPatchSourceNames) / sizeof(kPatchSourceNames[0])) {
    return kPatchSourceNames[idx];
  }
  return "?";
}

[[nodiscard]] std::string_view deoptReasonName(DeoptReason r) noexcept {
  const auto idx = static_cast<std::size_t>(r);
  if (idx < sizeof(kDeoptReasonNames) / sizeof(kDeoptReasonNames[0])) {
    return kDeoptReasonNames[idx];
  }
  return "?";
}

[[nodiscard]] std::string_view stackMapKindName(StackMapPoint::Kind k) noexcept {
  const auto idx = static_cast<std::size_t>(k);
  if (idx < sizeof(kStackMapKindNames) / sizeof(kStackMapKindNames[0])) {
    return kStackMapKindNames[idx];
  }
  return "?";
}

// --- append helpers (each line is terminated exactly once, here) ------------------

template <class... Args>
void appendLine(std::string& out, std::format_string<Args...> fmt, Args&&... args) {
  out += std::format(fmt, std::forward<Args>(args)...);
  out += '\n';
}

void appendCommaList(std::string& out, const std::vector<std::uint16_t>& values) {
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += std::format("{}", values[i]);
  }
}

// Renders the exception-coverage suffix: "exc:" plus the comma-joined edge
// indices (empty list = bare "exc:").
void appendExcCovers(std::string& out, const StencilInstance& inst) {
  out += " exc:";
  appendCommaList(out, inst.exc_covers);
}

[[nodiscard]] std::string_view stencilNameOf(const StencilSet& set, StencilId id) {
  if (static_cast<std::size_t>(id.id) >= set.stencils.size()) {
    return "<invalid-stencil>"; // hostile plan; never UB
  }
  return set.stencils[id.id].name;
}

} // namespace

// --- dumpPlan -----------------------------------------------------------------------

std::string dumpPlan(const StencilPlan& plan, const StencilSet& set) {
  std::string out;
  // Deterministic reservation (Rule 124: output must not depend on allocator
  // growth patterns; the reserve only affects capacity, never bytes).
  out.reserve(1024 + plan.instances.size() * 96);

  // Header block: method identity, frame layout, set version, counters.
  appendLine(out, "plan method={} descriptor={} static={} index={}",
             plan.method_name, plan.method_descriptor,
             plan.method_static ? 1 : 0, plan.method_index);
  appendLine(out, "frame regs={} locals={}", plan.num_regs, plan.num_locals);
  appendLine(out, "set magic=0x{:x} version={} target_arch={} abi_hash=0x{:x}",
             plan.set_version.magic, plan.set_version.version,
             plan.set_version.target_arch, plan.set_version.abi_hash);
  appendLine(out,
             "summary code_size={} entry_native_offset={} instances={} fusions={} "
             "patches={} deopt_points={} safepoints={}",
             plan.code_size, plan.entry_native_offset, plan.instances.size(),
             plan.fusion_count, plan.patch_count, plan.deopt_points.size(),
             plan.safepoint_count);

  // One line per instance, then one indented line per patch value.
  for (std::size_t i = 0; i < plan.instances.size(); ++i) {
    const StencilInstance& inst = plan.instances[i];
    std::string line = std::format("  inst {} @{}+{} {} rbc[{},{}) patches:{}",
                                   i, inst.output_offset, inst.output_size,
                                   stencilNameOf(set, inst.stencil),
                                   inst.rbc_pc_start, inst.rbc_pc_end,
                                   inst.patch_values.size());
    appendExcCovers(line, inst);
    appendLine(out, "{}", line);
    for (const PatchValue& pv : inst.patch_values) {
      appendLine(out, "    patch {} {} <- {}{} = 0x{:x}", pv.site,
                 patchKindName(pv.kind), patchSourceName(pv.source),
                 pv.pending_runtime ? " [pending]" : "", pv.value);
    }
  }

  // pc map.
  appendLine(out, "pc_map:");
  for (std::size_t i = 0; i < plan.pc_map.size(); ++i) {
    const PcMapEntry& e = plan.pc_map[i];
    appendLine(out, "  pcmap {} native={} rbc={} inst={}", i, e.native_offset,
               e.rbc_pc, e.instance);
  }

  // Stack maps.
  appendLine(out, "stack_maps:");
  for (std::size_t i = 0; i < plan.stack_maps.size(); ++i) {
    const StackMapPoint& sm = plan.stack_maps[i];
    appendLine(out, "  smap {} native={} rbc={} kind={} deopt={}", i,
               sm.native_offset, sm.rbc_pc, stackMapKindName(sm.kind),
               sm.deopt_point == 0xFFFF'FFFFu
                   ? std::string("-")
                   : std::format("{}", sm.deopt_point));
  }

  // Deopt points.
  appendLine(out, "deopt_points:");
  for (std::size_t i = 0; i < plan.deopt_points.size(); ++i) {
    const DeoptPoint& dp = plan.deopt_points[i];
    appendLine(out,
               "  dpt {} id=0x{:x} native={} rbc={} reason={} pending_exception={} "
               "stack_map={}",
               i, dp.deopt_id, dp.native_offset, dp.rbc_pc,
               deoptReasonName(dp.reason), dp.pending_exception_possible ? 1 : 0,
               dp.stack_map == 0xFFFF'FFFFu ? std::string("-")
                                            : std::format("{}", dp.stack_map));
  }

  // Exception edges.
  appendLine(out, "exception_edges:");
  for (std::size_t i = 0; i < plan.exception_edges.size(); ++i) {
    const ExceptionEdge& e = plan.exception_edges[i];
    appendLine(out, "  edge {} rbc[{},{}) handler={} catch={} native={}", i,
               e.start, e.end, e.handler_pc, e.catch_type, e.native_handler);
  }

  // Trailer: the metadata section counters (the summary line above already
  // carries the plan-level counters).
  appendLine(out, "end plan instances={} pc_map={} stack_maps={} deopt_points={} "
                  "exception_edges={}",
             plan.instances.size(), plan.pc_map.size(), plan.stack_maps.size(),
             plan.deopt_points.size(), plan.exception_edges.size());
  return out;
}

} // namespace b2::baseline
