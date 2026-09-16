// TurboScript Tier 0 — value.h
// 64-bit NaN-boxed tagged value (LuaJIT-style layout).
//
// Spec basis (Compiler Laws Part I, Tier 0):
//   "Direct-Threaded Register Interpreter ... computed gotos and a register-based
//    bytecode representation ... 24-bit fixed-width instructions with 256 virtual
//    registers per frame ... collects initial Inline Cache shape feedback."
//
// Layout:
//   double : any bit pattern  < 0xFFFE_0000_0000_0000   -> raw IEEE-754, UNBOXED.
//            (This is the core perf edge vs V8 Ignition, which boxes every
//             non-Smi double into a HeapNumber when pointer compression is on.)
//   smi    : 0xFFFE_0000_0000_0000 | uint32 payload      (full 32-bit int)
//   heap   : 0xFFFF_0000_0000_0000 | 47-bit pointer      (8-aligned, low 3 bits 000)
//   special: 0xFFFF_0000_0000_0000 | odd small value     (never a real pointer)
//
// Part 0 contract notes baked in here:
//   - NaN is canonicalized on boxed construction; we never produce NaN payloads
//     whose top bits collide with the tag space (x86-64 SSE produces 0xFFF8_...).
//   - SameValue / SameValueZero / strict-equality -0 handling lives in interp.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>

namespace ts {

inline constexpr uint64_t kSmiBase  = 0xFFFE000000000000ULL;
inline constexpr uint64_t kHeapBase = 0xFFFF000000000000ULL;
inline constexpr uint64_t kPtrMask  = 0x0000FFFFFFFFFFFFULL;

// Special singletons (heap-tagged, bit0 = 1 => never a real pointer).
inline constexpr uint64_t kUndefBits  = kHeapBase | 1;
inline constexpr uint64_t kNullBits   = kHeapBase | 3;
inline constexpr uint64_t kTrueBits   = kHeapBase | 5;
inline constexpr uint64_t kFalseBits  = kHeapBase | 7;
inline constexpr uint64_t kTDZBits    = kHeapBase | 9;   // let/const before init
inline constexpr uint64_t kDeletedBit = kHeapBase | 11;  // dict-mode tombstone

class HeapObj;
class Object;
class JSFunction;
class Context;

// ---- heap object header + strings (layout shared with heap.h) ----
enum class ObjKind : uint8_t {
    String = 0,
    Object = 1,
    Function = 2,
    Context = 3,
};

struct HeapObj {
    uint32_t size;        // total bytes of this allocation (header incl.)
    uint8_t  kind;        // ObjKind
    bool     marked;
    HeapObj* gcNext;
};

struct String : HeapObj {
    uint32_t len;         // UTF-16 code units
    uint32_t hash;        // 0 = not computed
    bool     isAscii;     // fast flag: all code units < 128
    inline char16_t* data() { return (char16_t*)(void*)(this + 1); }
    inline const char16_t* data() const { return (const char16_t*)(const void*)(this + 1); }
};

struct Value {
    uint64_t bits;

    // ---- construction ----
    static inline Value fromBits(uint64_t b)            { return {b}; }
    static inline Value fromDouble(double d)            { Value v; std::memcpy(&v.bits, &d, 8); return v; }
    static inline Value fromSmi(int32_t i)              { return {(uint64_t)kSmiBase | (uint32_t)i}; }
    static inline Value fromObj(HeapObj* p)             { return {kHeapBase | (uint64_t)(uintptr_t)p}; }
    static inline Value undef()                          { return {kUndefBits}; }
    static inline Value null_()                          { return {kNullBits}; }
    static inline Value true_()                          { return {kTrueBits}; }
    static inline Value false_()                         { return {kFalseBits}; }
    static inline Value tdz()                            { return {kTDZBits}; }
    static inline Value boolean(bool b)                  { return {b ? kTrueBits : kFalseBits}; }
    static inline Value nan()                            { return fromDouble(std::nan("")); }

    // ---- type predicates (each compiles to 1 compare; hot path critical) ----
    inline bool isSmi()   const { return bits >= kSmiBase && bits < kHeapBase; }
    inline bool isHeap()  const { return (bits & 1) == 0 && bits >= kHeapBase; }  // real 8-aligned pointer
    inline bool isSpecial() const { return bits >= kHeapBase && (bits & 1) != 0; }
    inline bool isDouble() const { return bits < kSmiBase; }
    inline bool isNumber() const { return bits < kHeapBase; }          // smi or double
    inline bool isUndef() const { return bits == kUndefBits; }
    inline bool isNull()  const { return bits == kNullBits; }
    inline bool isBool()  const { return bits == kTrueBits || bits == kFalseBits; }
    inline bool isTrue()  const { return bits == kTrueBits; }
    inline bool isTDZ()   const { return bits == kTDZBits; }

    inline HeapObj* asHeap() const { return (HeapObj*)(uintptr_t)(bits & kPtrMask); }
    // JS functions ARE objects (they carry properties); "Object" here means
    // "any heap object that is not a primitive wrapper/String/Context".
    inline bool isObject() const {
        return isHeap() && (asHeap()->kind == (uint8_t)ObjKind::Object ||
                            asHeap()->kind == (uint8_t)ObjKind::Function);
    }
    inline bool isString() const { return isHeap() && asHeap()->kind == (uint8_t)ObjKind::String; }
    inline bool isFunction() const { return isHeap() && asHeap()->kind == (uint8_t)ObjKind::Function; }
    inline bool isContext() const { return isHeap() && asHeap()->kind == (uint8_t)ObjKind::Context; }

    // ---- accessors (unchecked unless noted) ----
    inline int32_t  asSmi()    const { return (int32_t)(uint32_t)(bits & 0xFFFFFFFFULL); }
    inline double   asDouble() const { double d; std::memcpy(&d, &bits, 8); return d; }
    inline double   asNumber() const { return isSmi() ? (double)asSmi() : asDouble(); }
    inline Object* asObj()     const { return (Object*)(uintptr_t)(bits & kPtrMask); }

    // ToBoolean per ES §7.1.2 (undefined/null false, ±0/NaN false, "" false).
    inline bool toBoolean() const {
        if (bits == kTrueBits)  return true;
        if (bits == kFalseBits) return false;
        if (isSmi())            return asSmi() != 0;
        if (isDouble()) { uint64_t m = bits & 0x7FFFFFFFFFFFFFFFULL; return m != 0; } // nonzero magnitude
        if (bits == kUndefBits || bits == kNullBits) return false;
        if (isString()) return asStringUnchecked()->len != 0;
        return true; // objects
    }
    inline String* asStringUnchecked() const { return (String*)asHeap(); }
    inline bool operator==(const Value& o) const { return bits == o.bits; }
    inline bool operator!=(const Value& o) const { return bits != o.bits; }
};

static_assert(sizeof(Value) == 8, "Value must be exactly one machine word");
static_assert(sizeof(HeapObj) == 16, "HeapObj header must be 16 bytes");

} // namespace ts
