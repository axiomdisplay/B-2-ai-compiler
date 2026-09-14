// TurboScript — tagged value model, String/BigInt heap objects, and the
// pure (non-invoking) halves of the ECMAScript conversion algorithms.
// Rule 8: no RTTI — the tag is the type. Rule 72: numeric semantics exact.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ts_core.h"

namespace ts {

class Isolate;  // conversions that may invoke user code live there.

// ---------------------------------------------------------------------------
// BigInt — sign + 32-bit limbs, little-endian magnitude. Zero has no limbs
// and negative == false (canonical).
// ---------------------------------------------------------------------------
struct BigInt {
  bool negative = false;
  std::vector<uint32_t> limbs;  // little-endian, no trailing zero limbs.

  [[nodiscard]] bool isZero() const { return limbs.empty(); }
  [[nodiscard]] int sign() const {  // -1, 0, +1
    if (limbs.empty()) return 0;
    return negative ? -1 : 1;
  }
  [[nodiscard]] static int compareMag(const BigInt& a, const BigInt& b);
  [[nodiscard]] static int compare(const BigInt& a, const BigInt& b);

  // All results are canonical (zero strips sign). Division truncates toward
  // zero; remainder carries the dividend sign (JS semantics).
  [[nodiscard]] static BigInt add(const BigInt& a, const BigInt& b);
  [[nodiscard]] static BigInt sub(const BigInt& a, const BigInt& b);
  [[nodiscard]] static BigInt mul(const BigInt& a, const BigInt& b);
  [[nodiscard]] static bool divMod(const BigInt& a, const BigInt& b,
                                   BigInt* quotient, BigInt* remainder);
  [[nodiscard]] static BigInt negate(const BigInt& a);
  [[nodiscard]] static BigInt fromInt64(int64_t v);
  // Exact conversion of an integral double (|v| <= 2^1024); returns false if
  // v is not integral or is inf/nan.
  [[nodiscard]] static bool fromDouble(double v, BigInt* out);
  [[nodiscard]] double toDouble(bool* ok) const;
  // StringToBigInt: base-10, optional sign; returns false on invalid syntax.
  [[nodiscard]] static bool fromString(std::u16string_view s, BigInt* out);
  [[nodiscard]] std::u16string toString() const;
};

// ---------------------------------------------------------------------------
// String — UTF-16 code units (Rule: StringLength counts UTF-16 code units).
// ---------------------------------------------------------------------------
struct StringObj {
  std::u16string data;
};

// ---------------------------------------------------------------------------
// Value — 16-byte tagged slot.
// ---------------------------------------------------------------------------
struct Value {
  ValueKind kind = ValueKind::Undefined;
  union {
    bool b;
    int32_t i32;
    double num;
    const StringObj* str;
    void* ptr;  // Object | BigInt | Context | Closure | Accessor (heap-owned)
  };

  static Value undefined() { return Value{}; }
  static Value null() {
    Value v;
    v.kind = ValueKind::Null;
    return v;
  }
  static Value boolean(bool x) {
    Value v;
    v.kind = ValueKind::Boolean;
    v.b = x;
    return v;
  }
  static Value smi(int32_t x) {
    Value v;
    v.kind = ValueKind::Smi;
    v.i32 = x;
    return v;
  }
  static Value heapNumber(double x) {
    Value v;
    v.kind = ValueKind::HeapNumber;
    v.num = x;
    return v;
  }
  static Value string(const StringObj* s) {
    Value v;
    v.kind = ValueKind::String;
    v.str = s;
    return v;
  }
  static Value hole() {
    Value v;
    v.kind = ValueKind::Hole;
    return v;
  }
  static Value raw(ValueKind k, void* p) {
    Value v;
    v.kind = k;
    v.ptr = p;
    return v;
  }

  [[nodiscard]] bool isUndefined() const { return kind == ValueKind::Undefined; }
  [[nodiscard]] bool isNull() const { return kind == ValueKind::Null; }
  [[nodiscard]] bool isNullOrUndefined() const {
    return kind == ValueKind::Null || kind == ValueKind::Undefined;
  }
  [[nodiscard]] bool isBoolean() const { return kind == ValueKind::Boolean; }
  [[nodiscard]] bool isSmi() const { return kind == ValueKind::Smi; }
  [[nodiscard]] bool isHeapNumber() const { return kind == ValueKind::HeapNumber; }
  [[nodiscard]] bool isNumber() const { return isSmi() || isHeapNumber(); }
  [[nodiscard]] bool isString() const { return kind == ValueKind::String; }
  [[nodiscard]] bool isBigInt() const { return kind == ValueKind::BigInt; }
  [[nodiscard]] bool isObjectLike() const {  // JS-visible object
    return kind == ValueKind::Object || kind == ValueKind::Closure;
  }
  [[nodiscard]] bool isClosure() const { return kind == ValueKind::Closure; }
  [[nodiscard]] bool isContext() const { return kind == ValueKind::Context; }
  [[nodiscard]] bool isHole() const { return kind == ValueKind::Hole; }

  [[nodiscard]] double asDouble() const {
    return isSmi() ? static_cast<double>(i32) : num;
  }
  [[nodiscard]] const BigInt* asBigInt() const {
    return static_cast<const BigInt*>(ptr);
  }
  [[nodiscard]] BigInt* asBigInt() {
    return static_cast<BigInt*>(ptr);
  }
};

// Smi normalization law: integral, in range, and never negative zero
// (Rule 72: Object.is(-0, +0) must be false, so -0 stays a HeapNumber).
[[nodiscard]] inline Value normalizeNumber(double v) {
  if (v >= static_cast<double>(kSmiMin) && v <= static_cast<double>(kSmiMax)) {
    double truncated = static_cast<double>(static_cast<int32_t>(v));
    if (truncated == v && !(v == 0.0 && std::signbit(v))) {
      return Value::smi(static_cast<int32_t>(v));
    }
  }
  return Value::heapNumber(v);
}

// ---------------------------------------------------------------------------
// JsException — a JavaScript exception in flight (Rule 74: JS exceptions are
// values, never native exceptions). Carried by value; safe because every
// pointer payload targets the non-collecting Heap with stable addresses.
// ---------------------------------------------------------------------------
struct JsException {
  Value thrown;
};

template <typename T>
using JsResult = std::expected<T, JsException>;

// ---------------------------------------------------------------------------
// Pure conversion halves (no user code can run). The hook-aware halves
// (ToPrimitive, ToString of objects, ...) are Isolate methods.
// ---------------------------------------------------------------------------
namespace pure {

[[nodiscard]] bool toBoolean(const Value& v);           // ToBoolean
[[nodiscard]] int32_t toInt32(double v);                // modulo 2^32 signed
[[nodiscard]] uint32_t toUint32(double v);              // modulo 2^32 unsigned
[[nodiscard]] std::string kindName(const Value& v);     // typeof strings

// Number::toString per ECMA-262 (shortest round-trip, decimal/exponential
// selection verified against the reference oracle).
[[nodiscard]] std::string doubleToString(double v);

// StringToNumber: full grammar (whitespace trim, Infinity, 0x/0o/0b,
// decimal with exponent). nullopt-equivalent signaled via NaN.
[[nodiscard]] double stringToNumber(std::u16string_view s);

// Strict equality (Pure): numbers (+0 == -0), BigInt value equality,
// string code-unit equality, object identity.
[[nodiscard]] bool strictEquals(const Value& a, const Value& b);

// SameValue / SameValueZero (Pure).
[[nodiscard]] bool sameValue(const Value& a, const Value& b);
[[nodiscard]] bool sameValueZero(const Value& a, const Value& b);

// UTF-16 code-unit-wise string comparison.
[[nodiscard]] int compareStrings(const StringObj& a, const StringObj& b);

// Double -> BigInt exactness check used by relational/abstract equality.
[[nodiscard]] bool doubleIsIntegral(double v);

}  // namespace pure

}  // namespace ts
