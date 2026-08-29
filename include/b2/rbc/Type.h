#pragma once
// B-2 RBC - verification type system.
//
// WHY THIS FILE EXISTS:
// RBC is the shared intermediate format between the source path (frontend
// lowering), the classfile path (loader/verifier/quickener), and all four
// execution tiers (T0/T1/T2/T3). Type correctness of RBC is a hard gate: T1
// composes stencils directly from RBC without any IR, so by the time RBC
// exists it must already be type-sound. These are the types the RBC verifier
// proves per register and per local slot at every program point.

#include <cstdint>

namespace b2::rbc {

// Verification type lattice for RBC values (registers and local slots).
//
// WHY: Unlike the JVM stack verifier, RBC registers hold exactly one value
// each (longs/doubles do NOT occupy two slots). The lattice is therefore flat
// except Null <: Ref.
enum class RType : std::uint8_t {
  Bottom = 0, // unreachable / uninitialized
  Int,        // 32-bit signed integer (boolean/byte/char/short fold into Int)
  Long,       // 64-bit signed integer
  Float,      // 32-bit IEEE 754
  Double,     // 64-bit IEEE 754
  Null,       // the null reference (subtype of Ref)
  Ref,        // object reference (class/interface/array)
};

// Least upper bound.
[[nodiscard]] constexpr RType join(RType a, RType b) noexcept {
  if (a == b) {
    return a;
  }
  if ((a == RType::Null && b == RType::Ref) ||
      (a == RType::Ref && b == RType::Null)) {
    return RType::Ref;
  }
  return RType::Bottom; // incompatible merges mark the point unreachable
}

// Can a value of type `from` be used where `to` is expected?
[[nodiscard]] constexpr bool isAssignable(RType from, RType to) noexcept {
  if (from == to) {
    return true;
  }
  // Null is assignable to Ref.
  if (from == RType::Null && to == RType::Ref) {
    return true;
  }
  return false;
}

// Is this a primitive numeric type (Int/Long/Float/Double)?
[[nodiscard]] constexpr bool isNumeric(RType t) noexcept {
  return t == RType::Int || t == RType::Long || t == RType::Float ||
         t == RType::Double;
}

[[nodiscard]] constexpr bool isCategory2(RType t) noexcept {
  // Kept for spec parity with the JVM even though RBC registers are single-slot.
  return t == RType::Long || t == RType::Double;
}

[[nodiscard]] constexpr const char* typeName(RType t) noexcept {
  switch (t) {
  case RType::Bottom: return "bottom";
  case RType::Int: return "int";
  case RType::Long: return "long";
  case RType::Float: return "float";
  case RType::Double: return "double";
  case RType::Null: return "null";
  case RType::Ref: return "ref";
  }
  return "?";
}

} // namespace b2::rbc
