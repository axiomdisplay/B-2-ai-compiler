#pragma once
// B-2 codegen - the build-time stencil archive: precompiled machine-code
// bodies + declared holes, the codegen team's Stencil Rule 1 artifact.
//
// WHY THIS FILE EXISTS:
// docs/stencils.md SS5: stencils are generated at BUILD time by a stencil
// generator and shipped as a versioned archive; the runtime only copies and
// patches them (Stencil Rule 1). This header is the archive's public shape:
// what a record contains, how it is validated against a StencilSet (Stencil
// Rule 4), and how hole tags classify the fill channel (SS4/SS5 of
// docs/codegen_contract.md). The archive is embedded into libb2_codegen by
// the build (generated ArchiveData.cpp) so no file I/O is needed at runtime.
//
// LAW PINS (docs/laws.md):
// - Stencil Rule 1: stencil bodies are immutable build artifacts; nothing
//   here can create or mutate a body at runtime.
// - Stencil Rule 2: every record carries its hole table - the metadata IS
//   the record, not an afterthought.
// - Stencil Rule 3: only declared holes may be patched. HoleTag makes the
//   fill channel explicit so the instantiator can audit every write.
// - Stencil Rule 4: magic/version/target/abi-hash validation is mandatory
//   at load AND at instantiation; a mismatch refuses, never best-efforts.
// - Rule 124: the serialized archive is deterministic; byte-stability is
//   golden-tested (regenerate and compare).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "b2/baseline/Stencil.h"
#include "b2/baseline/StencilSet.h"

namespace b2::codegen {

// The manifest's patch vocabulary is the archive's too (SS5: Plan-tagged
// holes pair with plan PatchValues by kind/source).
using baseline::PatchKind;
using baseline::PatchSource;

// --- archive identity (Stencil Rule 4) -----------------------------------------

inline constexpr std::uint32_t kStencilArchiveMagic = 0x32736172u; // "2sar"
inline constexpr std::uint32_t kStencilArchiveVersion = 1;

// target_arch values (0 = the target-neutral v0 manifest; the archive is
// always concrete). v1 ships x86-64 only; new targets get new ids + a new
// archive artifact, never an overload of this one.
inline constexpr std::uint32_t kTargetArchX86_64 = 1;

// Folds every constant the bodies bake in: Value size, RType enumerator
// values, activation offsets, pointer size (docs/codegen_contract.md SS4).
// A change to any pinned constant is an ABI break and bumps this hash.
inline constexpr std::uint32_t kT1AbiHashV1 = 0xB2C0DE01u;

// --- hole tags (SS5: the fill channel) ------------------------------------------
//
// The manifest's holes (the plan-value channel) are tagged Plan and keep
// 1:1 alignment with StencilInstance::patch_values. The archive's extra
// holes are classified by where the instantiator takes the value from:
enum class HoleTag : std::uint8_t {
  Plan,    // value from the instance's k-th PatchValue (k = Plan-hole index)
  Stream,  // value from the RBC stream (register operands / raw imm)
  Helper,  // CallRel32 to a runtime helper; helper_id selects the target
  Layout,  // branch resolved from instantiation state (entries, thunks)
  CpPayload,     // constant payload from cp[the instance's CpIndex value]
  SwitchMatch,   // internal switch_case template: cmp immediate from the table
  SwitchTarget,  // internal switch_case template: case branch target
};

// Internal template names (records AFTER the manifest-aligned records;
// they are instantiation building blocks, never plan-selected).
inline constexpr std::string_view kInternalEntry = "__entry";
inline constexpr std::string_view kInternalDeoptThunk = "__deopt_thunk";
inline constexpr std::string_view kInternalDeoptExit = "__deopt_exit";
inline constexpr std::string_view kInternalSwitchCase = "__switch_case";

// Which runtime helper a Helper-tagged call site reaches (SS8). The ids are
// baked in the archive records; the engine maps id -> function address.
// Helper ABI (docs/codegen_contract.md SS8): RDI = activation, then up to
// four u32 args in ESI/EDX/ECX/R8D; EAX returns 0 (success) or a TrapKind.
// Value-producing helpers write their results DIRECTLY into the activation
// slot they receive as an argument. FmodF/FmodD are the two float-ABI
// exceptions (XMM0/XMM1 in, XMM0 out; they never trap).
enum class HelperId : std::uint8_t {
  None = 0,
  GetField,     // (act, objSlotOff, fieldOff, dstSlotOff)
  PutField,     // (act, objSlotOff, fieldOff, valSlotOff)
  GetStatic,    // (act, fieldIdOrBuiltin, dstSlotOff)
  PutStatic,    // (act, fieldId, valSlotOff)
  ArrayLoad,    // (act, arrSlotOff, idxSlotOff, dstSlotOff, elemKind)
  ArrayStore,   // (act, arrSlotOff, idxSlotOff, valSlotOff, elemKind)
  ArrayLength,  // (act, arrSlotOff, dstSlotOff)
  NewObject,    // (act, classId, dstSlotOff)
  NewArray,     // (act, lenSlotOff, atypeOrClassId, dstSlotOff, flags)
  CheckCast,    // (act, classId, srcSlotOff, dstSlotOff)
  InstanceOf,   // (act, classId, srcSlotOff, dstSlotOff)
  MonitorEnter, // (act, objSlotOff)
  MonitorExit,  // (act, objSlotOff)
  Athrow,       // (act, excSlotOff) - always traps out
  Call,         // (act, argBaseOff, argCount, packedTarget, dstSlotOff)
  LdcConst,     // (act, cpIndex, dstSlotOff)
  FmodF,        // float ABI: fmodf(xmm0, xmm1) -> xmm0
  FmodD,        // float ABI: fmod(xmm0, xmm1) -> xmm0
  Count
};

// One declared hole in an archive record (Stencil Rule 3's unit of audit).
// `step` selects the pattern instruction (fusion step) a Stream/Plan value
// is read from; `imm` carries SwitchMatch immediates for internal templates.
struct ArchiveHole {
  std::uint32_t code_offset = 0; // byte position in the record's code
  PatchKind kind = PatchKind::Imm32;
  std::uint8_t width = 4;        // 1/2/4/8
  PatchSource source = PatchSource::None;
  HoleTag tag = HoleTag::Plan;
  std::uint8_t helper_id = 0;    // HelperId when tag == Helper
  std::uint8_t step = 0;         // fusion step index (Stream/Plan values)
  std::uint32_t imm = 0;         // baked immediate (SwitchMatch payload)
};

// One archive record: the stencil body + its full hole table.
struct ArchiveRecord {
  std::string_view name;         // manifest name (validation + dumps)
  std::vector<std::uint8_t> code;
  std::vector<ArchiveHole> holes;
  // Manifest holes are exactly holes[0 .. manifest_holes) in manifest order.
  std::uint32_t manifest_holes = 0;

  [[nodiscard]] std::span<const ArchiveHole> planHoles() const noexcept {
    return {holes.data(), manifest_holes};
  }
  [[nodiscard]] std::span<const ArchiveHole> extraHoles() const noexcept {
    return {holes.data() + manifest_holes, holes.size() - manifest_holes};
  }
};

// --- the archive -----------------------------------------------------------------

struct ArchiveHeader {
  std::uint32_t magic = kStencilArchiveMagic;
  std::uint32_t version = kStencilArchiveVersion;
  std::uint32_t target_arch = kTargetArchX86_64;
  std::uint32_t abi_hash = kT1AbiHashV1;
  std::uint32_t manifest_count = 0;  // == set.size(); records align here
  std::uint32_t record_count = 0;    // manifest records + internal templates
};

struct Archive {
  ArchiveHeader header{};
  std::vector<ArchiveRecord> records; // [0, manifest_count) align with the
                                      // manifest StencilIds; the rest are
                                      // internal templates (kInternal*)

  [[nodiscard]] bool empty() const noexcept { return records.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return records.size(); }

  // Internal template lookup by name; nullptr when absent.
  [[nodiscard]] const ArchiveRecord* internal(
      std::string_view name) const noexcept {
    for (std::size_t i = header.manifest_count; i < records.size(); ++i) {
      if (records[i].name == name) {
        return &records[i];
      }
    }
    return nullptr;
  }
};

// --- load / validate ---------------------------------------------------------------

struct ArchiveCheckResult {
  bool ok = false;
  std::string error; // first violation, human-readable
};

// Deserializes archive bytes (little-endian layout per
// docs/codegen_contract.md SS4). Rejects malformed input; never throws.
[[nodiscard]] Archive loadArchive(std::span<const std::uint8_t> bytes);

// Full validation against a StencilSet (Stencil Rule 4): identity fields,
// record count/order/names, manifest-hole alignment (kind/source/width in
// manifest order for every record), hole bounds inside the code bytes.
// Used by stencilgen's self-check, tests, and Tier1 at startup.
[[nodiscard]] ArchiveCheckResult validateArchive(const Archive& archive,
                                                 const baseline::StencilSet& set);

// Serializes (deterministically; Rule 124) - stencilgen's output path and
// the determinism golden test both call this.
[[nodiscard]] std::vector<std::uint8_t> serializeArchive(const Archive& archive);

// The embedded v1 x86-64 archive (built by tools/stencilgen at build time).
[[nodiscard]] std::span<const std::uint8_t> embeddedArchiveX86_64();

// Convenience: load + validate the embedded archive against a set.
[[nodiscard]] ArchiveCheckResult loadEmbeddedX86_64(const baseline::StencilSet& set,
                                                    Archive& out);

// Builds the in-memory archive from a StencilSet (the emitter lives in the
// codegen team's sources; tools/stencilgen calls this at BUILD time, and
// the determinism test calls it at test time to compare against the
// embedded bytes). Deterministic (Rule 124).
[[nodiscard]] Archive buildArchive(const baseline::StencilSet& set);

} // namespace b2::codegen
