#pragma once
// B-2 RBC - instruction encoding, constant pool, methods, programs.
//
// WHY THIS FILE EXISTS:
// This is the in-memory shape of RBC, the convergence point of both entry
// paths (frontend lowering, classfile loader/quickener) and the input of all
// four tiers. Everything downstream - the T0 interpreter frame, T1 stencil
// plans, T2 IR construction, deopt pc-maps - is defined against these
// structures. Laws 15 (stable indices) and 124 (deterministic replay) apply:
// indices only, no pointers into containers that can reallocate.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "b2/rbc/Opcode.h"

namespace b2::rbc {

// One RBC instruction: fixed 12 bytes, field order = logical operand order.
//
// WHY fixed-width: trivial pc arithmetic (pc is an index into code[]),
// trivial native-pc -> RBC-pc maps for T1/T2 deopt, and in-place quickening
// by opcode swap without relocating anything.
//
// Operand meaning by Sig (see Opcode.h). Branch targets are instruction
// indices. Constants/field refs/method refs are constant-pool indices.
struct Ins {
  std::uint16_t op = 0;  // Op
  std::uint16_t dst = 0; // destination register / result
  std::uint16_t a = 0;   // first source register (or arg base for calls)
  std::uint16_t b = 0;   // second source register (or arg count for calls)
  std::uint32_t imm = 0; // immediate / cp index / branch target / deopt id

  constexpr Ins() = default;
  constexpr Ins(Op o, std::uint16_t dst_, std::uint16_t a_, std::uint16_t b_,
                std::uint32_t imm_)
      : op(static_cast<std::uint16_t>(o)), dst(dst_), a(a_), b(b_), imm(imm_) {}

  [[nodiscard]] constexpr Op opcode() const noexcept {
    return static_cast<Op>(op);
  }
};

// Primitive array element type codes (JVM atype codes, kept for familiarity).
enum class Atype : std::uint8_t {
  Boolean = 4, Char = 5, Float = 6, Double = 7, Byte = 8, Short = 9,
  Int = 10, Long = 11,
};

// Constant pool entry. v0 keeps one flat struct; kinds constrain which fields
// are meaningful. All indices are into the owning method's pool.
struct Const {
  enum class Kind : std::uint8_t {
    Int32 = 1,    // i32
    Int64 = 2,    // i64
    Float = 3,    // f32
    Double = 4,   // f64
    Utf8 = 5,     // str
    String = 6,   // str (runtime string constant)
    Class = 7,    // str (internal name, e.g. "java/lang/String")
    NameType = 8, // str = name, str2 = descriptor
    FieldRef = 9, // str = class, str2 = name, str3 = descriptor
    MethodRef = 10, // str = class, str2 = name, str3 = descriptor
    InterfaceMethodRef = 11,
    MethodType = 12,  // str = descriptor
    MethodHandle = 13, // str = kind, str2 = ref
    InvokeDynamic = 14, // str = name, str2 = descriptor (v0: structural only)
    SwitchTable = 15,   // ints (pairs: [match, target]* for lookup; low..high targets for table)
  };

  Kind kind = Kind::Int32;
  std::int32_t i32 = 0;
  std::int64_t i64 = 0;
  float f32 = 0.0F;
  double f64 = 0.0;
  std::string str;  // primary string payload (see Kind)
  std::string str2; // secondary payload (see Kind)
  std::string str3; // tertiary payload (see Kind)
  std::vector<std::int32_t> ints; // SwitchTable payload

  [[nodiscard]] const char* kindName() const noexcept;
};

// Exception handler entry: catches exceptions raised for instructions in
// [start, end) whose type is assignable to catchType (any if catchType < 0).
// Indices are instruction indices into the same code array.
struct ExceptionHandler {
  std::uint32_t start = 0;  // first covered instruction (inclusive)
  std::uint32_t end = 0;    // one past last covered instruction
  std::uint32_t handler = 0; // handler entry instruction
  std::int32_t catchType = -1; // cp index of Class, or -1 = catch-all
};

// Method flags (subset of JVM access flags relevant to compilation).
namespace method_flags {
inline constexpr std::uint16_t Public = 0x0001;
inline constexpr std::uint16_t Private = 0x0002;
inline constexpr std::uint16_t Protected = 0x0004;
inline constexpr std::uint16_t Static = 0x0008;
inline constexpr std::uint16_t Final = 0x0010;
inline constexpr std::uint16_t Synchronized = 0x0020;
inline constexpr std::uint16_t Native = 0x0100;
inline constexpr std::uint16_t Abstract = 0x0400;
inline constexpr std::uint16_t Varargs = 0x0080;
} // namespace method_flags

// One verified-able RBC method. Self-contained: owns its constant pool so
// methods serialize independently (golden tests, AOT artifacts).
struct Method {
  std::string name = "main";            // simple name
  std::string descriptor = "()V";       // JVM-style descriptor, "(II)I"
  std::uint16_t flags = 0;              // method_flags::*
  std::uint32_t numRegs = 0;            // working registers r0..rN-1
  std::uint32_t numLocals = 0;          // local slots l0..lM-1 (params first)
  std::vector<Ins> code;                // instruction array; pc = index
  std::vector<Const> cp;                // constant pool
  std::vector<ExceptionHandler> handlers; // exception table

  [[nodiscard]] bool isStatic() const noexcept {
    return (flags & method_flags::Static) != 0;
  }
  [[nodiscard]] bool isSynchronized() const noexcept {
    return (flags & method_flags::Synchronized) != 0;
  }
  [[nodiscard]] bool isAbstract() const noexcept {
    return (flags & method_flags::Abstract) != 0;
  }
};

// A compilation unit: one class of methods sharing a program identity.
// (The real class model arrives with the loader; v0 keeps programs small and
// self-contained for the interpreter and golden tests.)
struct Program {
  std::string className = "Main"; // internal name, "a/b/C"
  std::vector<Method> methods;

  [[nodiscard]] const Method* find(std::string_view name,
                                   std::string_view descriptor) const noexcept;
};

// --- descriptor helpers (shared by verifier, text format, frontend) -------

// JVM method descriptor parameter types -> RBC types, appended to `out`.
// Returns false on malformed descriptors (never throws; input is untrusted).
[[nodiscard]] bool parseParams(std::string_view descriptor,
                               std::vector<RType>& out) noexcept;

// Return type of a JVM method descriptor, or Bottom on malformed input.
[[nodiscard]] RType parseReturn(std::string_view descriptor) noexcept;

// Number of parameter slots (RBC: one slot per param, longs/doubles included).
[[nodiscard]] std::uint32_t paramCount(std::string_view descriptor) noexcept;

// Human-readable type of one descriptor type char sequence ("I" -> "int").
[[nodiscard]] std::string describeType(std::string_view descriptorType);

} // namespace b2::rbc
