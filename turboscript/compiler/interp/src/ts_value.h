// TurboScript — tagged value model, String/BigInt heap objects, and the
// pure (non-invoking) halves of the ECMAScript conversion algorithms.
// Rule 8: no RTTI — the tag is the type. Rule 72: numeric semantics exact.
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ts_core.h"

namespace ts {

class Isolate;  // conversions that may invoke user code live there.
struct ProxyObj;  // defined in ts_object.h (heap proxy object, v0.3).

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
// cachedSymbol (v0.3): the interned property-key SymbolId of this exact
// string, after its first use as a key (kInvalidSymbol until then). Const-
// pool strings are shared, so one intern per literal serves every site.
// ---------------------------------------------------------------------------
struct StringObj {
  std::u16string data;
  mutable SymbolId cachedSymbol = kInvalidSymbol;
};

// ---------------------------------------------------------------------------
// Symbol — v0.3 user symbol. Identity is the SymbolObj pointer; property-
// key capability comes from a stable SymbolId in the user-symbol range
// (kUserSymbolBase + uniqueId).
// ---------------------------------------------------------------------------
struct SymbolObj {
  std::u16string desc;
  uint32_t uniqueId = 0;
};

// ---------------------------------------------------------------------------
// Value — 64-bit NaN-boxed slot (v0.5; benchmarks_v0.4.md Section 5 #1).
//
// Encoding (normative; interp_contract.md Section 2):
//   double : any bit pattern whose top 13 bits are not 0x1FFF. NaN doubles
//            are canonicalized to +qNaN at boxing (a NaN's payload/sign is
//            unobservable through values — no TypedArray layer exists to
//            smuggle payloads), which is what keeps the tag space free.
//   tagged : 0x1FFF (bits 63..51) | kind nibble (bits 50..47) | payload
//            (bits 46..0, 47 bits).
//     The tag prefix forces sign=1, exponent=all-ones, mantissa[51]=1 — a
//     pattern only negative NaNs can have (mantissa != 0 is implied), so
//     boxing canonicalization is what makes it collision-free. The boundary
//     case that decides the 13-bit (not 12-bit) tag: -Infinity has mantissa
//     0 and would collide with any tag that does not include mantissa[51].
//     - Undefined/Null/Hole: payload 0 (single-compare constants).
//     - Boolean: payload 0/1.   - Smi: low 32 bits (kSmiMin/kSmiMax).
//     - pointer kinds: full 47-bit heap pointer (deque-backed stable
//       addresses; Linux user VA < 2^47 fits with no base+offset arithmetic).
//
// WHY: every register read/write, slot load, and Mov moves 8 bytes instead
// of 16 — the dominant structural term in benchmarks_v0.4.md Section 3. The
// accessor API is unchanged from the 16-byte form, so semantic helpers
// (Rule 96: one semantic source) are ported, not rewritten.
// ---------------------------------------------------------------------------
struct Value {
  // Canonical +qNaN: positive NaNs (sign 0) are outside the tag space, so
  // canonicalization may always pick this value.
  static constexpr uint64_t kQNaNBits = 0x7FF8000000000000ULL;
  // Tag space: top 13 bits 0x1FFF (see encoding comment above).
  static constexpr uint64_t kTaggedTop = 0xFFF8000000000000ULL;  // 0x1FFF<<51
  static constexpr uint64_t kKindBase = 0x1FFF0;   // (kTaggedTop >> 47)
  static constexpr uint64_t kPayloadMask = 0x00007FFFFFFFFFFFULL;  // 47 bits
  static constexpr uint64_t kMantissaMask = 0x000FFFFFFFFFFFFFULL;  // 52 bits
  static constexpr uint64_t kUndefBits = kTaggedTop;  // kind Undefined = 0

  uint64_t bits_ = kUndefBits;

  static constexpr Value undefined() { return Value{}; }
  static constexpr Value null() {
    Value v;
    v.bits_ = kTaggedTop | (uint64_t(ValueKind::Null) << 47);
    return v;
  }
  static constexpr Value boolean(bool x) {
    Value v;
    v.bits_ = kTaggedTop | (uint64_t(ValueKind::Boolean) << 47) | (x ? 1 : 0);
    return v;
  }
  static constexpr Value smi(int32_t x) {
    Value v;
    v.bits_ = kTaggedTop | (uint64_t(ValueKind::Smi) << 47) |
              uint64_t(static_cast<uint32_t>(x));
    return v;
  }
  // NaN-canonicalizing (the ONLY sanctioned double-boxing path; every fast
  // lane boxes results through this or normalizeNumber/tsSmiOrNumber). The
  // mantissa test must cover the full 52-bit mantissa: a negative NaN with
  // only high mantissa bits set would otherwise survive into the tag space.
  static Value heapNumber(double x) {
    Value v;
    uint64_t b = std::bit_cast<uint64_t>(x);
    if ((b & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
        (b & kMantissaMask) != 0) {
      b = kQNaNBits;  // any NaN -> +qNaN (sign/payload unobservable, Rule 72)
    }
    v.bits_ = b;
    return v;
  }
  static constexpr Value string(const StringObj* s) {
    Value v;
    v.bits_ = kTaggedTop | (uint64_t(ValueKind::String) << 47) |
              reinterpret_cast<uint64_t>(s);
    return v;
  }
  static constexpr Value hole() {
    Value v;
    v.bits_ = kTaggedTop | (uint64_t(ValueKind::Hole) << 47);
    return v;
  }
  static constexpr Value raw(ValueKind k, void* p) {
    Value v;
    v.bits_ = kTaggedTop | (uint64_t(k) << 47) |
              reinterpret_cast<uint64_t>(p);
    return v;
  }

  // --- queries (single shift+compare on the hot kinds; Rule 8: no RTTI) ---
  [[nodiscard]] constexpr bool isTagged() const {
    return (bits_ >> 51) == (kTaggedTop >> 51);
  }
  [[nodiscard]] constexpr bool isDouble() const { return !isTagged(); }
  [[nodiscard]] constexpr bool isUndefined() const { return bits_ == kUndefBits; }
  [[nodiscard]] constexpr bool isNull() const {
    return (bits_ >> 47) == kKindBase + uint64_t(ValueKind::Null);
  }
  [[nodiscard]] constexpr bool isNullOrUndefined() const {
    return bits_ == kUndefBits || isNull();
  }
  [[nodiscard]] constexpr bool isBoolean() const { return kind() == ValueKind::Boolean; }
  [[nodiscard]] constexpr bool isSmi() const {
    return (bits_ >> 47) == kKindBase + uint64_t(ValueKind::Smi);
  }
  [[nodiscard]] constexpr bool isHeapNumber() const { return isDouble(); }
  [[nodiscard]] constexpr bool isNumber() const { return isSmi() || isDouble(); }
  [[nodiscard]] constexpr bool isString() const { return kind() == ValueKind::String; }
  [[nodiscard]] constexpr bool isBigInt() const { return kind() == ValueKind::BigInt; }
  [[nodiscard]] constexpr bool isObjectLike() const {  // JS-visible object
    const uint64_t k = bits_ >> 47;
    return k == kKindBase + uint64_t(ValueKind::Object) ||
           k == kKindBase + uint64_t(ValueKind::Closure);
  }
  [[nodiscard]] constexpr bool isClosure() const { return kind() == ValueKind::Closure; }
  [[nodiscard]] constexpr bool isContext() const { return kind() == ValueKind::Context; }
  [[nodiscard]] constexpr bool isHole() const { return kind() == ValueKind::Hole; }
  [[nodiscard]] constexpr bool isSymbol() const { return kind() == ValueKind::Symbol; }
  [[nodiscard]] constexpr bool isProxy() const { return kind() == ValueKind::Proxy; }

  // Kind decode (doubles decode to HeapNumber; the HeapNumber member of the
  // enum is the phantom kind of the untagged space).
  [[nodiscard]] constexpr ValueKind kind() const {
    return isTagged() ? static_cast<ValueKind>((bits_ >> 47) & 0xF)
                      : ValueKind::HeapNumber;
  }

  // --- payload accessors (precondition: matching query returned true) ---
  [[nodiscard]] constexpr bool asBool() const { return (bits_ & 1) != 0; }
  [[nodiscard]] constexpr int32_t asSmi() const { return static_cast<int32_t>(bits_); }
  [[nodiscard]] constexpr double asDouble() const {
    return isSmi() ? static_cast<double>(static_cast<int32_t>(bits_))
                   : std::bit_cast<double>(bits_);
  }
  [[nodiscard]] void* asPtr() const {
    return reinterpret_cast<void*>(bits_ & kPayloadMask);
  }
  [[nodiscard]] const StringObj* asString() const {
    return static_cast<const StringObj*>(asPtr());
  }
  [[nodiscard]] StringObj* asString() {
    return static_cast<StringObj*>(asPtr());
  }
  [[nodiscard]] const SymbolObj* asSymbol() const {
    return static_cast<const SymbolObj*>(asPtr());
  }
  [[nodiscard]] SymbolObj* asSymbol() {
    return static_cast<SymbolObj*>(asPtr());
  }
  [[nodiscard]] ProxyObj* asProxy() const;
  [[nodiscard]] SymbolId symbolKeyId() const {
    return static_cast<SymbolId>(kUserSymbolBase + asSymbol()->uniqueId);
  }
  [[nodiscard]] const BigInt* asBigInt() const {
    return static_cast<const BigInt*>(asPtr());
  }
  [[nodiscard]] BigInt* asBigInt() {
    return static_cast<BigInt*>(asPtr());
  }
};

static_assert(sizeof(Value) == 8, "T0 slot values are exactly 8 bytes (v0.5)");
static_assert(std::is_trivially_copyable_v<Value>,
              "frame reconstruction is a memcpy (deopt contract)");

// --- encoding invariants: proved here, not assumed (Rule 105 discipline) ---
namespace value_enc {
constexpr Value mk(uint64_t b) {
  Value v;
  v.bits_ = b;
  return v;
}
// Tag-space patterns are negative NaNs (sign 1, exp all-ones, mantissa[51]
// set) — disjoint from every non-NaN double, including +/-Infinity (-Inf has
// mantissa 0, so its top 13 bits are 0x1FFE, not 0x1FFF).
static_assert(Value::kTaggedTop == 0x1FFFULL << 51, "tag = top 13 bits 0x1FFF");
static_assert(Value::kTaggedTop >> 51 == 0x1FFF, "tag width");
static_assert(mk(Value::kTaggedTop).isTagged(), "tag pattern is tagged");
static_assert(mk(0xFFF0000000000000ULL).isDouble(), "-Inf stays a double");
static_assert(mk(0x7FF0000000000000ULL).isDouble(), "+Inf stays a double");
static_assert(mk(Value::kQNaNBits).isDouble(), "+qNaN stays a double");
static_assert(mk(0x8000000000000000ULL).isDouble(), "-0.0 stays a double");
// Every kind-nibble/payload-0 tagged pattern is a canonicalizable NaN.
constexpr bool allKindPatternsAreNaNs() {
  for (uint64_t k = 0; k < 16; ++k) {
    uint64_t b = Value::kTaggedTop | (k << 47);
    bool isNaN = (b & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
                 (b & Value::kMantissaMask) != 0;
    if (!isNaN) return false;
  }
  return true;
}
static_assert(allKindPatternsAreNaNs(), "tag space = NaN patterns only");
// Round trips.
static_assert(Value::smi(-1).asSmi() == -1, "smi sign extension");
static_assert(Value::smi(kSmiMin).asSmi() == kSmiMin, "smi min");
static_assert(Value::smi(kSmiMax).asSmi() == kSmiMax, "smi max");
static_assert(Value::null().isNull(), "null tag");
static_assert(Value::hole().isHole(), "hole tag");
static_assert(Value::undefined().isUndefined(), "undefined tag");
static_assert(!Value::boolean(false).asBool(), "bool payload");
static_assert(Value::boolean(true).asBool(), "bool payload");
static_assert(mk(Value::kUndefBits).kind() == ValueKind::Undefined, "kind 0");
}  // namespace value_enc

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
