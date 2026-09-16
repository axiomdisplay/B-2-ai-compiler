// TurboScript Tier 0 — object.h
// Object model: transition-tree shapes (hidden classes) + dense property slots
// + packed tagged elements + dictionary fallback + lexical Context cells.
//
// Perf rationale (vs Ignition):
//  - Own-property hit = shape pointer compare + slot index load. Same as V8.
//  - IC state is per bytecode site: {expected shape, slot, proto holder}.
//  - Keys are interned Strings, so common lookups are a pointer compare.
#pragma once
#include "heap.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace ts {

class BytecodeFunction;
class Runtime;
struct IC;

// ------------------------------------------------------------- Shapes
class Shape {
public:
    Shape* parent = nullptr;      // shape before this property was added
    String* key = nullptr;        // property added by this shape (null => root)
    uint32_t slot = 0;            // slot index of `key` in owner's props array
    uint32_t propCount = 0;       // total props when this shape is current
    Shape** transitions = nullptr; // children
    uint32_t nTransitions = 0;
    uint32_t capTransitions = 0;
};

// Transition tree root manager (shapes are permanent, never collected —
// registered gap: V8 shape GC; acceptable for Tier 0 MVP scope).
class ShapeArena {
public:
    Shape root;
    Shape* transition(Shape* from, String* key);
    ~ShapeArena();
private:
    Shape* addTransition(Shape* from, String* key);
};

// ------------------------------------------------------------- Dictionary fallback
struct DictEntry { String* key; Value val; };
class Dict {   // open addressing, tombstones on delete
public:
    void init(uint32_t cap = 8);
    Value* find(String* key);
    void   set(String* key, Value v);
    bool   remove(String* key);
    uint32_t count() const { return count_; }
    template <typename F> void forEach(F f) const {
        for (uint32_t i = 0; i < cap_; i++)
            if (used_[i] == 1) f(keys_[i], vals_[i]);
    }
    void destroy() { std::free(keys_); std::free(vals_); std::free(used_); keys_ = nullptr; }
private:
    String** keys_ = nullptr; Value* vals_ = nullptr; uint8_t* used_ = nullptr; // 0 empty 1 full 2 tombstone
    uint32_t cap_ = 0, count_ = 0;
    void grow();
};

// ------------------------------------------------------------- Object
// Layout is perf-critical: every heap object pays for these bytes.
//   - inline slots for small objects (<= kInlineSlots props): no second malloc
//   - Dict only as a lazy pointer (delete/dict-mode is rare)
//   - function fields live only in JSFunction (derived), not every object
class Object : public HeapObj {
public:
    static constexpr uint32_t kInlineSlots = 3;

    static Object* create(Runtime* rt, Object* proto, bool isArray);

    Shape*   shape = nullptr;    // null => dict mode
    Object*  proto = nullptr;
    Value*   slots = nullptr;    // dense property slots, [0 .. shape->propCount)
    Value*   elements = nullptr; // packed integer-indexed
    uint32_t slotCap = 0;
    uint32_t elemCap = 0;
    uint32_t elemLen = 0;        // array length (JSArray)
    bool     isArray = false;
    bool     dictMode = false;
    Dict*    dict = nullptr;     // lazily allocated on delete/dict conversion
    Value    inlineSlots[kInlineSlots];

    void destroyInternal();
    Value* findOwnSlot(String* key);            // pointer-compare fast, value-compare fallback
    uint32_t ownSlotIndex(String* key, bool* found);
};

// ------------------------------------------------------------- Context
class Context : public HeapObj {
public:
    static Context* create(Runtime* rt, Context* parent, uint32_t nCells);
    Context* parent = nullptr;
    uint32_t nCells = 0;
    Value* cells = nullptr; // trailing? separate malloc — freed by destroyInternal
    void destroyInternal() { std::free(cells); cells = nullptr; }
};

// ------------------------------------------------------------- JSFunction
class JSFunction : public Object {
public:
    // Function-only header (allocated as part of the JSFunction block, which is
    // larger than a plain Object — see the create functions below).
    struct {
        BytecodeFunction* bf = nullptr;
        Context* ctx = nullptr;
        Value (*native)(Runtime*, Value, Value*, uint32_t) = nullptr;
        Object* ctorProto = nullptr;
        bool hasCtorProto = false;
        bool isCtor = false;
    } fn;

    static JSFunction* createInterpreted(Runtime* rt, BytecodeFunction* bf, Context* ctx);
    static JSFunction* createNative(Runtime* rt, Object* proto,
                                    Value (*native)(Runtime*, Value, Value*, uint32_t),
                                    const char* name);
};

// Property lookup helpers used by runtime slow paths.
// Returns true and writes `out` on success.
bool getProperty(Runtime* rt, Value recv, String* key, Value* out);
bool setProperty(Runtime* rt, Value recv, String* key, Value v);   // returns false => TypeError in strict
bool hasProperty(Runtime* rt, Value recv, String* key);
bool deleteProperty(Runtime* rt, Object* o, String* key);
bool getElement(Runtime* rt, Value recv, Value key, Value* out);
bool setElement(Runtime* rt, Value recv, Value key, Value v);

} // namespace ts
