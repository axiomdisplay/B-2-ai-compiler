#pragma once
// B-2 IR - value type lattice for the sea-of-nodes IR.
//
// WHY THIS FILE EXISTS:
// The IR type system is what the verifier proves per data edge (Rules 40,
// 126) and what Rule 33 (no implicit conversions) is checked against: every
// type change must be an explicit conversion node, so the lattice itself can
// stay small and flat. Scalar types mirror rbc::RType one-to-one (Null <: Ref
// is the only subtype relation; boolean/byte/char/short fold into Int) so the
// RBC-to-IR graph builder is a total function on RBC types. Two extensions
// serve the Part XVIII first-class passes:
//   - vector lane types + vector result types (SWLP), where Int8/Int16 exist
//     ONLY as lane types (scalar byte/char/short values fold into Int);
//   - Tagged, the representation-polymorphic type produced by NaN-boxing
//     tag/untag nodes (Part XVIII NaN Boxing Rule).
//
// This header must stay dependency-free: the IR links nothing else.

#include <cstdint>

namespace b2::ir {

using NodeId = std::uint32_t; // Rule 15: index-based edges, never pointers

inline constexpr NodeId kInvalidNodeId = 0xFFFFFFFFu;

// Opaque interned-id types (Rule 16: no strings in the IR). The frontend
// intern tables own the mapping; the IR only carries the integer.
using SymbolId = std::uint32_t; // utf8 / name / descriptor
using TypeId = std::uint32_t;   // class / interface / array type id
using FieldId = std::uint32_t;  // resolved field id
using MethodId = std::uint32_t; // resolved method id
using DeoptId = std::uint32_t;  // deopt reason / plan id (frontend-owned)

enum class IRType : std::uint8_t {
  Bottom = 0, // unreachable / no value (control-only nodes)
  Int,        // 32-bit signed integer (boolean/byte/char/short folded)
  Long,       // 64-bit signed integer
  Float,      // 32-bit IEEE 754
  Double,     // 64-bit IEEE 754
  Null,       // the null reference (Null <: Ref)
  Ref,        // object reference (class/interface/array)
  Tagged,     // representation-polymorphic NaN-boxed/tagged value

  // Lane types (SWLP). NEVER legal as a scalar operand type - the verifier
  // rejects them outside vector payload contexts.
  Int8,
  Int16,

  // Vector result types. Lane count is a per-node payload, not part of the
  // type; the verifier checks lane-count agreement between operands.
  // Masks are VectorI8 values (lanes of 0/1 bytes) - SWLP convention.
  VectorI8,
  VectorI16,
  VectorI32,
  VectorI64,
  VectorF32,
  VectorF64,

  _Count
};

[[nodiscard]] constexpr bool isScalarType(IRType t) noexcept {
  return t == IRType::Int || t == IRType::Long || t == IRType::Float ||
         t == IRType::Double || t == IRType::Null || t == IRType::Ref ||
         t == IRType::Tagged;
}

[[nodiscard]] constexpr bool isVectorType(IRType t) noexcept {
  return t >= IRType::VectorI8 && t <= IRType::VectorF64;
}

// Scalar type of the lanes of a vector type (VectorI32 -> Int).
// Bottom for non-vector input.
[[nodiscard]] constexpr IRType laneTypeOf(IRType t) noexcept {
  switch (t) {
  case IRType::VectorI8:
    return IRType::Int8;
  case IRType::VectorI16:
    return IRType::Int16;
  case IRType::VectorI32:
    return IRType::Int;
  case IRType::VectorI64:
    return IRType::Long;
  case IRType::VectorF32:
    return IRType::Float;
  case IRType::VectorF64:
    return IRType::Double;
  default:
    return IRType::Bottom;
  }
}

// Vector type over a given lane type (Int -> VectorI32). Bottom for
// non-lane-legal input.
[[nodiscard]] constexpr IRType vectorOfLane(IRType lane) noexcept {
  switch (lane) {
  case IRType::Int8:
    return IRType::VectorI8;
  case IRType::Int16:
    return IRType::VectorI16;
  case IRType::Int:
    return IRType::VectorI32;
  case IRType::Long:
    return IRType::VectorI64;
  case IRType::Float:
    return IRType::VectorF32;
  case IRType::Double:
    return IRType::VectorF64;
  default:
    return IRType::Bottom;
  }
}

// Least upper bound. Flat lattice except Null <: Ref; vector types and
// Tagged join only with themselves. Incompatible merges produce Bottom
// (the merge point is unreachable - same convention as rbc::join).
[[nodiscard]] constexpr IRType join(IRType a, IRType b) noexcept {
  if (a == b) {
    return a;
  }
  if ((a == IRType::Null && b == IRType::Ref) ||
      (a == IRType::Ref && b == IRType::Null)) {
    return IRType::Ref;
  }
  return IRType::Bottom;
}

// Can a value of type `from` feed an operand slot expecting `to`?
[[nodiscard]] constexpr bool isAssignable(IRType from, IRType to) noexcept {
  if (from == to) {
    return true;
  }
  if (from == IRType::Null && to == IRType::Ref) {
    return true;
  }
  return false;
}

[[nodiscard]] constexpr const char* typeName(IRType t) noexcept {
  switch (t) {
  case IRType::Bottom:
    return "bottom";
  case IRType::Int:
    return "int";
  case IRType::Long:
    return "long";
  case IRType::Float:
    return "float";
  case IRType::Double:
    return "double";
  case IRType::Null:
    return "null";
  case IRType::Ref:
    return "ref";
  case IRType::Tagged:
    return "tagged";
  case IRType::Int8:
    return "i8";
  case IRType::Int16:
    return "i16";
  case IRType::VectorI8:
    return "vec.i8";
  case IRType::VectorI16:
    return "vec.i16";
  case IRType::VectorI32:
    return "vec.i32";
  case IRType::VectorI64:
    return "vec.i64";
  case IRType::VectorF32:
    return "vec.f32";
  case IRType::VectorF64:
    return "vec.f64";
  default:
    return "?";
  }
}

// Value representations for the adaptive representation pass
// (docs/special_passes.md section 3.1 - the 9-rep space, verbatim).
enum class ValueRep : std::uint8_t {
  UnboxedInt32,    // raw int in GP register
  UnboxedInt64,    // raw long in GP register
  UnboxedFloat32,  // raw float in FP register
  UnboxedFloat64,  // raw double in FP register
  CompressedOop,   // 32-bit compressed reference
  NaNBoxedRef,     // reference in NaN payload
  TaggedInt31,     // 31-bit int with tag bit
  BoxedObject,     // heap-allocated box (fallback)
  Polymorphic,     // multiple reps at same site (megamorphic)
};

[[nodiscard]] constexpr bool isValidRep(ValueRep r) noexcept {
  return r >= ValueRep::UnboxedInt32 && r <= ValueRep::Polymorphic;
}

[[nodiscard]] constexpr const char* repName(ValueRep r) noexcept {
  switch (r) {
  case ValueRep::UnboxedInt32:
    return "i32";
  case ValueRep::UnboxedInt64:
    return "i64";
  case ValueRep::UnboxedFloat32:
    return "f32";
  case ValueRep::UnboxedFloat64:
    return "f64";
  case ValueRep::CompressedOop:
    return "oop";
  case ValueRep::NaNBoxedRef:
    return "nanbox";
  case ValueRep::TaggedInt31:
    return "int31";
  case ValueRep::BoxedObject:
    return "boxed";
  case ValueRep::Polymorphic:
    return "poly";
  default:
    return "?";
  }
}

// SWLP vector lane bounds. Law 23: thresholds are named constants.
inline constexpr std::uint32_t kMinVectorLanes = 2;
inline constexpr std::uint32_t kMaxVectorLanes = 64;

// Vector arithmetic / reduction op codes (payload of VectorOp /
// VectorMaskOp / VectorReduce). Kept as one shared enum; not every value is
// legal for every lane type (the verifier checks).
enum class VectorOp : std::uint8_t {
  Add = 0,
  Sub,
  Mul,
  Div,
  Rem,
  And,
  Or,
  Xor,
  Min,
  Max,
  Sum, // reduction-only (element-wise sums are meaningless)
  _Count
};

// Legal ELEMENT-WISE ops (VectorOp/VectorMaskOp payloads). Sum excluded:
// it is a reduction result shape, not a lane op.
[[nodiscard]] constexpr bool isLegalElementWiseOp(VectorOp op,
                                                 IRType lane) noexcept {
  switch (lane) {
  case IRType::Int8:
  case IRType::Int16:
  case IRType::Int:
  case IRType::Long:
    return op >= VectorOp::Add && op <= VectorOp::Max;
  case IRType::Float:
  case IRType::Double:
    switch (op) {
    case VectorOp::Add:
    case VectorOp::Sub:
    case VectorOp::Mul:
    case VectorOp::Div:
    case VectorOp::Rem:
    case VectorOp::Min:
    case VectorOp::Max:
      return true;
    default:
      return false; // no bitwise ops on FP lanes
    }
  default:
    return false;
  }
}

// Legal REDUCTION ops (VectorReduce payloads).
[[nodiscard]] constexpr bool isLegalReductionOp(VectorOp op,
                                                IRType lane) noexcept {
  switch (lane) {
  case IRType::Int8:
  case IRType::Int16:
  case IRType::Int:
  case IRType::Long:
    switch (op) {
    case VectorOp::Sum:
    case VectorOp::Min:
    case VectorOp::Max:
    case VectorOp::And:
    case VectorOp::Or:
    case VectorOp::Xor:
      return true;
    default:
      return false;
    }
  case IRType::Float:
  case IRType::Double:
    return op == VectorOp::Sum || op == VectorOp::Min || op == VectorOp::Max;
  default:
    return false;
  }
}

[[nodiscard]] constexpr const char* vectorOpName(VectorOp op) noexcept {
  switch (op) {
  case VectorOp::Add:
    return "add";
  case VectorOp::Sub:
    return "sub";
  case VectorOp::Mul:
    return "mul";
  case VectorOp::Div:
    return "div";
  case VectorOp::Rem:
    return "rem";
  case VectorOp::And:
    return "and";
  case VectorOp::Or:
    return "or";
  case VectorOp::Xor:
    return "xor";
  case VectorOp::Min:
    return "min";
  case VectorOp::Max:
    return "max";
  case VectorOp::Sum:
    return "sum";
  default:
    return "?";
  }
}

// Memory barrier kinds (payload of MemBar). Classic JMM fence flavors.
enum class MemBarKind : std::uint8_t {
  LoadLoad = 0,
  StoreStore,
  LoadStore,
  StoreLoad, // full sequential-consistency fence
  Acquire,
  Release,
};

[[nodiscard]] constexpr const char* memBarName(MemBarKind k) noexcept {
  switch (k) {
  case MemBarKind::LoadLoad:
    return "loadload";
  case MemBarKind::StoreStore:
    return "storestore";
  case MemBarKind::LoadStore:
    return "loadstore";
  case MemBarKind::StoreLoad:
    return "storeload";
  case MemBarKind::Acquire:
    return "acquire";
  case MemBarKind::Release:
    return "release";
  default:
    return "?";
  }
}

// Pack (lane type, lane count) into one u32 payload slot.
// lanes <= kMaxVectorLanes = 64 fits the high 24 bits.
[[nodiscard]] constexpr std::uint32_t packVecPayload(IRType lane,
                                                     std::uint32_t lanes) noexcept {
  return (lanes << 8) | static_cast<std::uint32_t>(lane);
}

// Unpack helpers. Returns Bottom / 0 for corrupt payloads.
[[nodiscard]] constexpr IRType unpackVecLane(std::uint32_t p) noexcept {
  return static_cast<IRType>(p & 0xFFu);
}

[[nodiscard]] constexpr std::uint32_t unpackVecLanes(std::uint32_t p) noexcept {
  return p >> 8;
}

} // namespace b2::ir
