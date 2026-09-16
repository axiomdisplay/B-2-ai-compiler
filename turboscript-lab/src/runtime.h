// TurboScript Tier 0 — runtime.h
// Runtime = heap + shape arena + interned key table + global object +
// exception channel (longjmp-based, QuickJS-style) + GC root registry.
#pragma once
#include "value.h"
#include "heap.h"
#include "object.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <setjmp.h>

namespace ts {

class BytecodeFunction;
class String;
class Interp;

class Runtime {
public:
    Runtime();
    ~Runtime();

    Heap        heap;
    ShapeArena  shapes;
    Object*     globalObj = nullptr;
    Object*     objectProto = nullptr;   // Object.prototype
    Object*     functionProto = nullptr; // Function.prototype
    Object*     arrayProto = nullptr;    // Array.prototype
    Object*     stringProto = nullptr;   // String.prototype
    Object*     numberProto = nullptr;   // Number.prototype
    String*     globalThisName = nullptr;

    // interpreter back-reference (set by Interp ctor; used for throws + JS callbacks)
    Interp*     interp = nullptr;

    // ---- GC root marking hook (implemented by Interp) ----
    void (*gcMarkRoots)(void*) = nullptr;
    void* gcCtx = nullptr;
    void runGC();

    // interned property keys / strings
    String* intern(const char* ascii);
    String* internUTF16(const char16_t* s, uint32_t len);
    std::unordered_map<std::string, String*> internTable_;

    // ---- GC roots ----
    // The interpreter registers its value stack + frames here each collection.
    Value*   stackBase = nullptr;   // value stack (contiguous)
    size_t   stackLen = 0;          // words to mark
    Context* lastContext = nullptr; // context chain of the current frame

    void collectIfNeeded() { if (heap.gcRequested()) runGC(); }


    // ---- exceptions (longjmp channel; QuickJS-proven design) ----
    volatile Value pendingException = {kUndefBits};
    jmp_buf*   activeJmp = nullptr;   // set by TryFrame push
    // Throw: stores value, longjmps to innermost handler. Never returns.
    [[noreturn]] void throwValue(Value v);
    [[noreturn]] void throwTypeError(const char* msg);
    [[noreturn]] void throwReferenceError(const char* msg);
    [[noreturn]] void throwRangeError(const char* msg);
    Object* newErrorObject(const char* ctor, const char* msg);

    // ---- conversions (ES Part 0 contract helpers) ----
    Value toNumberValue(Value v);            // ToNumber, may run user code (valueOf)
    Value toPrimitiveValue(Value v, const char* hint); // hint: "number"|"string"|"default"
    String* toStringObj(Value v);            // ToString
    Value toNumericValue(Value v);           // ToNumeric (Number for MVP; BigInt registered gap)
    Value toPropertyValue(Value k);          // ToPropertyKey (string/symbol; MVP string+index)
    double toDoubleForArith(Value v);

    // ---- allocation helpers ----
    Object* newArrayObject(uint32_t initialCap);
    void defineProperty(Object* o, const char* key, Value v) {
        definePropertyStr(o, intern(key), v);
    }
    void definePropertyStr(Object* o, String* key, Value v);
    JSFunction* defineBuiltin(Object* o, const char* name,
                              Value (*fn)(Runtime*, Value, Value*, uint32_t));
};

void installBuiltins(Runtime* rt);
bool strToIndex(String* s, uint32_t* out);

// Strict-equality per ES §7.2.15 (handles -0 === 0 => true, NaN !== NaN).
bool strictEquals(Value a, Value b);
// SameValue per Object.is (NaN equal, -0 != 0).
bool sameValue(Value a, Value b);
// SameValueZero (NaN equal, -0 == 0) — Map/Set/Array.includes semantics.
bool sameValueZero(Value a, Value b);
// Abstract equality per ES §7.2.14.
bool abstractEquals(Runtime* rt, Value a, Value b);
// Abstract relational per ES §7.2.12 (returns undef for NaN comparisons).
Value lessThanValue(Runtime* rt, Value a, Value b, bool orEqual);

} // namespace ts
