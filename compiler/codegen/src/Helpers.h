#pragma once
// B-2 codegen - the T1 activation record and the runtime-helper ABI shared
// by the instantiator (patch-time resolution) and the engine (execution).
//
// WHY THIS FILE EXISTS:
// docs/codegen_contract.md SS3/SS8 pin the machine-visible frame and the
// helper calling convention. Both sides must agree byte-for-byte with the
// archive's baked constants, so the layout lives here as ONE struct with
// static_asserts against the public kActOff* offsets (a change here is an
// ABI break: bump kT1AbiHashV1 and regenerate the archive).
//
// The activation is RAW MEMORY the compiled code addresses through RBP; it
// is never constructed/destructed as a C++ object (reinterpret over an
// engine-owned buffer - Value is trivially copyable by contract, so the
// layout is stable).

#include <cstddef>
#include <cstdint>

#include "b2/codegen/Instantiate.h"
#include "b2/interp/Value.h"

namespace b2::codegen {

// The machine-visible activation (SS3). `slots` is over-allocated at
// runtime: numLocals + numRegs entries (locals first, then regs).
struct T1Activation {
  std::uint64_t magic;        // kT1ActivationMagic (defensive)
  void* code;                 // CompiledCode* (owning instantiation)
  std::uint64_t status;       // kStatusNormal / kStatusDeopt
  std::uint64_t deopt_pc;     // native offset within the code block
  std::uint64_t deopt_id;
  std::uint64_t trap_kind;    // TrapKind (helper-raised)
  std::uint64_t pending_exc;  // ObjRef id, 0 = none
  std::uint64_t reenter;      // native offset to re-enter at, 0 = none
  interp::Value ret_value;    // 16 bytes
  std::uint64_t mon_count;
  std::uint32_t mon_ids[kT1MaxMonitors];
  interp::Value slots[1];     // FAM: (numLocals + numRegs) values
};

static_assert(sizeof(T1Activation) == kSlotsBase + sizeof(interp::Value));
static_assert(offsetof(T1Activation, magic) == kActOffMagic);
static_assert(offsetof(T1Activation, code) == kActOffCode);
static_assert(offsetof(T1Activation, status) == kActOffStatus);
static_assert(offsetof(T1Activation, deopt_pc) == kActOffDeoptPc);
static_assert(offsetof(T1Activation, deopt_id) == kActOffDeoptId);
static_assert(offsetof(T1Activation, trap_kind) == kActOffTrapKind);
static_assert(offsetof(T1Activation, pending_exc) == kActOffPendingExc);
static_assert(offsetof(T1Activation, reenter) == kActOffReenter);
static_assert(offsetof(T1Activation, ret_value) == kActOffRetValue);
static_assert(offsetof(T1Activation, mon_count) == kActOffMonCount);
static_assert(offsetof(T1Activation, mon_ids) == kActOffMonIds);
static_assert(offsetof(T1Activation, slots) == kSlotsBase);

// The activation allocation size for a frame layout.
[[nodiscard]] inline std::size_t activationBytes(std::uint32_t numLocals,
                                                 std::uint32_t numRegs) noexcept {
  return static_cast<std::size_t>(kSlotsBase) +
         (static_cast<std::size_t>(numLocals) + numRegs) * sizeof(interp::Value);
}

} // namespace b2::codegen

// --- runtime helpers (extern "C"; defined in Engine.cpp) --------------------------
//
// ABI (SS8): RDI = T1Activation*, ESI/EDX/ECX/R8D = u32 args; EAX returns 0
// (success) or a TrapKind value. Value results are written by the helper
// into the activation slot it receives. On trap, the helper leaves
// act->pending_exc (a fully built exception ObjRef) and returns nonzero;
// the compiled body then branches to its deopt thunk.
extern "C" {

std::uint32_t b2cg_get_field(b2::codegen::T1Activation* act,
                             std::uint32_t objSlotOff, std::uint32_t fieldOff,
                             std::uint32_t dstSlotOff);
std::uint32_t b2cg_put_field(b2::codegen::T1Activation* act,
                             std::uint32_t objSlotOff, std::uint32_t fieldOff,
                             std::uint32_t valSlotOff);
std::uint32_t b2cg_get_static(b2::codegen::T1Activation* act,
                              std::uint32_t fieldIdOrBuiltin,
                              std::uint32_t dstSlotOff);
std::uint32_t b2cg_put_static(b2::codegen::T1Activation* act,
                              std::uint32_t fieldId, std::uint32_t valSlotOff);
std::uint32_t b2cg_array_load(b2::codegen::T1Activation* act,
                              std::uint32_t arrSlotOff, std::uint32_t idxSlotOff,
                              std::uint32_t dstSlotOff, std::uint32_t elemKind);
std::uint32_t b2cg_array_store(b2::codegen::T1Activation* act,
                               std::uint32_t arrSlotOff, std::uint32_t idxSlotOff,
                               std::uint32_t valSlotOff, std::uint32_t elemKind);
std::uint32_t b2cg_array_length(b2::codegen::T1Activation* act,
                                std::uint32_t arrSlotOff,
                                std::uint32_t dstSlotOff);
std::uint32_t b2cg_new_object(b2::codegen::T1Activation* act,
                              std::uint32_t classId, std::uint32_t dstSlotOff);
std::uint32_t b2cg_new_array(b2::codegen::T1Activation* act,
                             std::uint32_t lenSlotOff,
                             std::uint32_t atypeOrClassId,
                             std::uint32_t dstSlotOff, std::uint32_t flags);
std::uint32_t b2cg_check_cast(b2::codegen::T1Activation* act,
                              std::uint32_t classId, std::uint32_t srcSlotOff,
                              std::uint32_t dstSlotOff);
std::uint32_t b2cg_instance_of(b2::codegen::T1Activation* act,
                               std::uint32_t classId, std::uint32_t srcSlotOff,
                               std::uint32_t dstSlotOff);
std::uint32_t b2cg_monitor_enter(b2::codegen::T1Activation* act,
                                 std::uint32_t objSlotOff);
std::uint32_t b2cg_monitor_exit(b2::codegen::T1Activation* act,
                                std::uint32_t objSlotOff);
std::uint32_t b2cg_athrow(b2::codegen::T1Activation* act,
                          std::uint32_t excSlotOff);
std::uint32_t b2cg_call(b2::codegen::T1Activation* act,
                        std::uint32_t argBaseOff, std::uint32_t argCount,
                        std::uint32_t packedTarget, std::uint32_t dstSlotOff);
std::uint32_t b2cg_ldc_const(b2::codegen::T1Activation* act,
                             std::uint32_t cpIndex, std::uint32_t dstSlotOff);

// Float-ABI helpers (XMM0/XMM1 in, XMM0 out; never trap).
float b2cg_fmod_f(b2::codegen::T1Activation* act, float a, float b);
double b2cg_fmod_d(b2::codegen::T1Activation* act, double a, double b);

} // extern "C"

namespace b2::codegen {

// HelperId -> function address (the instantiator's Helper-hole resolver).
// Returns nullptr for unknown ids (a BadHole refusal at use).
[[nodiscard]] void* helperAddress(std::uint8_t helperId) noexcept;

} // namespace b2::codegen
