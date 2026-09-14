// TurboScript — core limits and result types.
// Laws: Rule 6 (std::expected, no C++ exceptions), Rule 23 (no magic
// constants), Rule 32 (bitmask flags), Rule 48 ([[nodiscard]]).
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace ts {

// ---------------------------------------------------------------------------
// Limits (Rule 23: every threshold is a named constant).
// ---------------------------------------------------------------------------
constexpr uint32_t kRegisterBits = 8;
constexpr uint32_t kMaxRegisters = 256;          // Part I Tier 0: 256 vregs.
constexpr uint32_t kReservedRegNewTarget = 254;  // reserved (future new.target)
constexpr uint32_t kReservedRegSpare = 255;      // reserved
constexpr uint32_t kMaxUsableRegister = 253;     // r0..r253 usable
constexpr uint32_t kMaxParams = 254;
constexpr uint32_t kMaxContextCells = 255;       // NewContext cellCount8
constexpr uint32_t kMaxCallArgs = 255;           // argc8
constexpr uint32_t kMaxConstantPool = 1u << 24;  // imm24
constexpr uint32_t kMaxFunctionTable = 1u << 24;
constexpr uint32_t kMaxGlobalNames = 1u << 24;
constexpr uint32_t kOpcodeSpace = 256;
constexpr uint32_t kMaxCallDepth = 2048;         // Rule 90 recursion limit.
constexpr uint32_t kMaxStringCodeUnits = 1u << 26;  // 64M UTF-16 units.
constexpr uint32_t kMaxBigIntBits = 1u << 20;    // 1Mbit BigInt magnitude.
constexpr uint32_t kMaxModuleCodeWords = 1u << 26;
constexpr uint32_t kMaxHandlerTable = 1u << 16;
constexpr uint32_t kMegamorphicThreshold = 4;    // >4 distinct shapes => mega.
constexpr uint32_t kCallProfileRing = 4;
constexpr int32_t  kSmiMin = INT32_MIN;
constexpr int32_t  kSmiMax = INT32_MAX;

// ---------------------------------------------------------------------------
// Value kinds (Rule 8: no RTTI; the tag is the type).
// ---------------------------------------------------------------------------
enum class ValueKind : uint8_t {
  Undefined = 0,
  Null,
  Boolean,
  Smi,
  HeapNumber,
  String,
  Object,
  BigInt,
  Hole,        // internal: deleted property slot; must never escape to JS.
  Context,     // internal: closure context cell holder.
  Closure,     // internal: function object (also an Object kind to JS).
  Accessor,    // internal: accessor pair stored in a property slot.
};

// ---------------------------------------------------------------------------
// Property attributes (Rule 32: bitmask, never raw ints).
// ---------------------------------------------------------------------------
enum class PropAttr : uint8_t {
  Writable     = 1 << 0,
  Enumerable   = 1 << 1,
  Configurable = 1 << 2,
  IsAccessor   = 1 << 3,
};

template <typename E>
class Flags {
 public:
  constexpr Flags() : bits_(0) {}
  constexpr Flags(E e) : bits_(static_cast<uint8_t>(e)) {}
  constexpr explicit Flags(uint8_t raw) : bits_(raw) {}
  [[nodiscard]] constexpr bool has(E e) const {
    return (bits_ & static_cast<uint8_t>(e)) != 0;
  }
  constexpr void add(E e) { bits_ |= static_cast<uint8_t>(e); }
  constexpr void remove(E e) { bits_ &= static_cast<uint8_t>(~e); }
  [[nodiscard]] constexpr uint8_t raw() const { return bits_; }
  constexpr Flags operator|(Flags other) const {
    return Flags(static_cast<uint8_t>(bits_ | other.bits_));
  }
  constexpr bool operator==(Flags other) const { return bits_ == other.bits_; }

 private:
  uint8_t bits_;
};

// ---------------------------------------------------------------------------
// Errors.
//
// Two failure worlds, never mixed (Rule 6 / Rule 74):
//   * TsDiagnostic — compile-side failure (assembler, verifier, bad usage).
//     Has location + message + expected/actual + hint (Rule 47).
//   * JsException  — a JavaScript exception in flight; carries the thrown
//     value. It is a *value*, not a native exception.
// ---------------------------------------------------------------------------
struct TsDiagnostic {
  std::string file;
  uint32_t line = 0;
  std::string message;
  std::string expected;  // optional, Rule 47
  std::string actual;    // optional, Rule 47
  std::string hint;      // optional, Rule 47
};

// JsException and JsResult are defined in ts_value.h (they carry a Value,
// which is declared there; kept out of this header to break the cycle).

template <typename T>
using TsResult = std::expected<T, TsDiagnostic>;  // Rule 6 / Rule 48.

[[nodiscard]] inline TsDiagnostic diag(std::string file, uint32_t line,
                                       std::string message) {
  TsDiagnostic d;
  d.file = std::move(file);
  d.line = line;
  d.message = std::move(message);
  return d;
}

}  // namespace ts
