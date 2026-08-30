#pragma once
// B-2 codegen - the instantiator: StencilPlan + archive -> executable code.
//
// WHY THIS FILE EXISTS:
// docs/stencils.md SS6 is the normative instantiation flow: allocate a
// writable staging buffer, copy stencil bodies instance by instance while
// recording native offset -> RBC pc, apply patch sites, link internal
// branches, finalize metadata, publish W^X atomically. This header pins
// that contract: the CompiledCode artifact (code + REAL pc map + deopt
// metadata in native terms), the refusal taxonomy (Amendment A: always
// safe to abandon), and the activation record layout every stencil body
// addresses (docs/codegen_contract.md SS3/SS6).
//
// LAW PINS (docs/laws.md):
// - Stencil Rule 3: the instantiator writes ONLY through declared archive
//   holes. There is no code path that mutates a body byte outside a hole.
// - Stencil Rule 5: copy/patch in writable staging memory; publication is
//   mprotect(PROT_READ|PROT_EXEC); the block is never written after.
// - Stencil Rule 4: plan.set_version vs the set vs the archive must agree;
//   any mismatch refuses before a single byte is copied.
// - Amendment A: every failure is a refusal with a reason; the method
//   simply stays on T0 (Rule 96). NEVER a partial CompiledCode.
// - Rule 15: CompiledCode owns offsets/indices only; callers hold the
//   pointer, which is stable for the owning code cache's lifetime.

#include <cstdint>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "b2/baseline/Plan.h"
#include "b2/baseline/StencilSet.h"
#include "b2/codegen/Archive.h"
#include "b2/interp/Runtime.h"
#include "b2/rbc/Rbc.h"

namespace b2::codegen {

// --- the T1 activation record (docs/codegen_contract.md SS3) ---------------------
//
// Machine-visible layout; the offsets are the ABI the archive's bodies bake
// in (bumped abi_hash on any change). The slot array follows the control
// block: locals l0.. then regs r0.., one 16-byte interp::Value each.
inline constexpr std::uint64_t kT1ActivationMagic = 0xB271AC7Fu;
inline constexpr std::uint32_t kActOffMagic = 0;
inline constexpr std::uint32_t kActOffCode = 8;      // CompiledCode*
inline constexpr std::uint32_t kActOffStatus = 16;   // 0 normal / 1 deopt
inline constexpr std::uint32_t kActOffDeoptPc = 24;  // native offset
inline constexpr std::uint32_t kActOffDeoptId = 32;
inline constexpr std::uint32_t kActOffTrapKind = 40;
inline constexpr std::uint32_t kActOffPendingExc = 48; // ObjRef id, 0 = none
inline constexpr std::uint32_t kActOffReenter = 56;    // native offset to
                                                       // re-enter at (handler
                                                       // dispatch), 0 = none
inline constexpr std::uint32_t kActOffRetValue = 64;   // 16-byte Value
inline constexpr std::uint32_t kActOffMonCount = 80;
inline constexpr std::uint32_t kActOffMonIds = 88;     // u32 each
inline constexpr std::uint32_t kT1MaxMonitors = 16;
inline constexpr std::uint32_t kSlotsBase = 152;       // first slot byte

inline constexpr std::uint32_t kStatusNormal = 0;
inline constexpr std::uint32_t kStatusDeopt = 1;

// Trap kinds (the exception families helpers raise; SS8/SS9). Inline traps
// (idiv/ldiv by zero) use Arithmetic; helper traps carry the kind from the
// helper's return value.
enum class TrapKind : std::uint32_t {
  None = 0,
  Npe,
  ArrayBounds,
  Arithmetic,
  ClassCast,
  NegativeArraySize,
  ArrayStore,
  Monitor,
  StackOverflow,
  NoSuchMethod,
  Thrown,     // athrow / rethrow path: pending exception is pre-built
  Redispatch, // caller caught a callee throw: re-enter at act.reenter offset
};

// Getstatic/putstatic patch encoding: values >= this mark a builtin static
// (System.out/err) and carry the singleton ObjRef id in the low bits; below
// it they are FieldIds (statics storage keys).
inline constexpr std::uint32_t kStaticBuiltinBase = 0x8000'0000u;

// Exit statuses of one compiled invocation (RAX convention, SS6).
inline constexpr std::uint32_t kExitNormal = 0;
inline constexpr std::uint32_t kExitDeopt = 1;

// --- compiled code ----------------------------------------------------------------

// Entry points of a CompiledCode: the method entry plus every exception
// handler the engine can re-enter at (SS6). `native_offset` points at an
// emitted entry stub (push rbp; mov rbp, rdi; jmp target).
struct CodeEntry {
  std::uint32_t native_offset = 0;
  std::uint32_t rbc_pc = 0;      // 0 for the method entry; handler pc else
  bool is_method_entry = false;
};

// The REAL pc map in native terms (the plan's nominal offsets are re-laid
// out with archive sizes at instantiation; deopt translation uses THESE).
struct RealPcEntry {
  std::uint32_t native_offset = 0; // instance start in the code block
  std::uint32_t native_end = 0;    // instance end (start + true size)
  std::uint32_t rbc_pc = 0;        // rbc_pc_start of the instance
  std::uint32_t instance = 0;      // index into the plan's instances
};

// Deopt metadata in native terms: the plan's DeoptPoint translated to the
// real layout (thunk position + real trap-site offset).
struct RealDeoptPoint {
  std::uint32_t deopt_id = 0;
  std::uint32_t thunk_offset = 0;  // deopt thunk in the code block
  std::uint32_t native_offset = 0; // real offset of the site
  std::uint32_t rbc_pc = 0;
  baseline::DeoptReason reason = baseline::DeoptReason::Trap;
  bool pending_exception_possible = false;
};

// One instantiation of a method (owned by the code cache; Rule 15: stable
// pointer, engine-internal).
struct CompiledCode {
  std::vector<std::uint8_t> code;        // the executable bytes (RX after pub)
  std::vector<RealPcEntry> pc_map;       // sorted by native_offset
  std::vector<RealDeoptPoint> deopt_points; // sorted by deopt_id
  std::vector<CodeEntry> entries;        // [0] is the method entry
  baseline::StencilPlan plan;            // the recipe this code came from

  std::uint32_t method_index = 0;
  std::string method_name;               // diagnostics only (Rule 16)
  std::string method_descriptor;
  std::uint32_t num_locals = 0;
  std::uint32_t num_regs = 0;

  // Published executable base (the mprotect'ed block). Entries above are
  // offsets into it. Null until publish() succeeds. Owned: freed on
  // destruction (the page-aligned allocation from instantiate()).
  std::uint8_t* exec_base = nullptr;
  std::size_t exec_alloc_size = 0; // page-rounded mprotect/free extent

  ~CompiledCode() {
    if (exec_base != nullptr) {
      // The block is PROT_READ|EXEC (W^X): the allocator writes free-list
      // metadata INTO the freed chunk, so flip it back to writable first
      // (Stencil Rule 5 in reverse: un-publish, then release).
      mprotect(exec_base, exec_alloc_size, PROT_READ | PROT_WRITE);
      std::free(exec_base);
      exec_base = nullptr;
    }
  }
  CompiledCode() = default;
  CompiledCode(const CompiledCode&) = delete;
  CompiledCode& operator=(const CompiledCode&) = delete;
  CompiledCode(CompiledCode&&) noexcept = default;
  CompiledCode& operator=(CompiledCode&&) noexcept = default;

  [[nodiscard]] bool published() const noexcept { return exec_base != nullptr; }

  // native offset -> rbc pc (binary search over pc_map; the containing
  // instance's rbc_pc_start - v0 fusions deopt at fusion starts, Plan.h).
  [[nodiscard]] std::uint32_t rbcPcAt(std::uint32_t native_offset) const noexcept;

  // deopt id -> RealDeoptPoint (linear; deopt is the cold path).
  [[nodiscard]] const RealDeoptPoint* deoptPoint(
      std::uint32_t deopt_id) const noexcept;
};

// --- instantiation result -----------------------------------------------------------

enum class InstantiationStatus : std::uint8_t {
  Ok = 0,
  VersionMismatch,   // Stencil Rule 4: set/plan/archive disagreement
  NoArchiveRecord,   // a stencil in the plan has no archive record
  BadHole,           // a hole cannot be filled (width/range/source)
  BadBranchTarget,   // branch target is not an instance start
  NoDeoptCoverage,   // trap-capable instance without a covering DeoptPoint
  BadSwitchTable,    // switch payload missing/malformed
  WxPublishFailed,   // mprotect failure (Stencil Rule 5)
  BudgetExceeded,    // code bytes over kMaxCodeBytes
};

[[nodiscard]] constexpr std::string_view instantiationStatusName(
    InstantiationStatus s) noexcept {
  switch (s) {
    case InstantiationStatus::Ok: return "ok";
    case InstantiationStatus::VersionMismatch: return "version-mismatch";
    case InstantiationStatus::NoArchiveRecord: return "no-archive-record";
    case InstantiationStatus::BadHole: return "bad-hole";
    case InstantiationStatus::BadBranchTarget: return "bad-branch-target";
    case InstantiationStatus::NoDeoptCoverage: return "no-deopt-coverage";
    case InstantiationStatus::BadSwitchTable: return "bad-switch-table";
    case InstantiationStatus::WxPublishFailed: return "wx-publish-failed";
    case InstantiationStatus::BudgetExceeded: return "budget-exceeded";
  }
  return "?";
}

struct InstantiationResult {
  InstantiationStatus status = InstantiationStatus::Ok;
  std::string detail;                  // refusal diagnostics; never empty on failure
  std::unique_ptr<CompiledCode> code;  // valid only when Ok

  [[nodiscard]] bool ok() const noexcept {
    return status == InstantiationStatus::Ok && code != nullptr;
  }
};

// Per-method code budget (kill switch; Rule 10/45 discipline, Rule 23 named).
inline constexpr std::uint32_t kMaxCodeBytes = 2u << 20; // 2 MiB per method

// --- the instantiator ----------------------------------------------------------------
//
// NORMATIVE ALGORITHM (docs/stencils.md SS6 / docs/codegen_contract.md SS6;
// changes are an RFC + contract version bump):
//   1. Validate versions: plan.set_version == set.version; archive header
//      identity vs kStencilArchive* constants + abi hash; record count ==
//      set.size(). Any mismatch -> VersionMismatch.
//   2. Re-layout with TRUE sizes: entry-stub area (one stub per entry
//      point), then instances at running sums of archive record sizes,
//      then one deopt thunk per DeoptPoint, then the shared deopt tail.
//      Build the REAL pc map and translated deopt points.
//   3. Copy each archive record into staging; patch holes in three passes:
//      Plan holes from PatchValues (converting slot numbers to byte
//      offsets, resolving runtime-pending field/class/method refs through
//      `rt`), Stream holes from the RBC stream (register operands ->
//      slot byte offsets), Helper holes with helper addresses, Layout
//      holes with entry/thunk offsets. Every write goes through a declared
//      hole (Stencil Rule 3) and is width/range checked (BadHole).
//   4. Branch discipline: every branch target must be an instance start
//      (BadBranchTarget); every trap-capable instance must be covered by
//      a DeoptPoint (NoDeoptCoverage).
//   5. Publish: mprotect the block READ|EXEC, set exec_base (WxPublishFailed
//      on failure). The staging buffer IS the block (no second copy).
[[nodiscard]] InstantiationResult instantiate(const rbc::Program& program,
                                              std::uint32_t method_index,
                                              const baseline::StencilPlan& plan,
                                              const baseline::StencilSet& set,
                                              const Archive& archive,
                                              interp::Runtime& rt);

// Deterministic disassembly-free dump for golden tests: the code bytes as
// hex plus the real pc map / deopt points / entries (Rule 124 form).
[[nodiscard]] std::string dumpCode(const CompiledCode& code);

} // namespace b2::codegen
