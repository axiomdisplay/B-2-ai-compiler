#pragma once
// B-2 codegen - constants shared by the codegen team's sources (stencil
// bodies, instantiator, engine). Internal to compiler/codegen/src; NOT a
// public contract (public constants live in include/b2/codegen/*).

#include <cstdint>

#include "b2/rbc/Opcode.h"

namespace b2::codegen {

// Call helper packing: packed = flavor << kCallFlavorShift | id.
// Flavors (docs/codegen_contract.md SS8):
//   0 static   - direct MethodId + JVMS 5.5 program-class init
//   1 virtual  - receiver-class builtin probe + (name,desc) resolution
//   2 special  - direct MethodId, no init
// The flavor is chosen by the INSTANTIATOR from the op (the packed value is
// what the Plan-tagged target hole carries).
inline constexpr std::uint32_t kCallFlavorStatic = 0;
inline constexpr std::uint32_t kCallFlavorVirtual = 1;
inline constexpr std::uint32_t kCallFlavorSpecial = 2;
inline constexpr std::uint32_t kCallFlavorShift = 28;

[[nodiscard]] inline std::uint32_t callFlavorOf(rbc::Op op) noexcept {
  switch (op) {
    case rbc::Op::Invokestatic:
    case rbc::Op::InvokestaticQuick:
      return kCallFlavorStatic;
    case rbc::Op::Invokevirtual:
    case rbc::Op::Invokeinterface:
    case rbc::Op::InvokevirtualQuick:
    case rbc::Op::InvokeinterfaceQuick:
      return kCallFlavorVirtual;
    case rbc::Op::Invokespecial:
    case rbc::Op::InvokespecialQuick:
      return kCallFlavorSpecial;
    default:
      return kCallFlavorStatic;
  }
}

} // namespace b2::codegen
