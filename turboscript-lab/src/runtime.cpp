// TurboScript Tier 0 — runtime.cpp
#include "runtime.h"
#include "interp.h"
#include <cstdio>
#include <cstring>

namespace ts {

Runtime::Runtime() {
    heap.init();
    objectProto = Object::create(this, nullptr, false);
    functionProto = Object::create(this, objectProto, false);
    arrayProto = Object::create(this, objectProto, true);
    stringProto = Object::create(this, objectProto, false);
    numberProto = Object::create(this, objectProto, false);
    globalObj = Object::create(this, objectProto, false);
    globalThisName = intern("globalThis");
    defineProperty(globalObj, "globalThis", Value::fromObj((HeapObj*)globalObj));
    installBuiltins(this);
}

Runtime::~Runtime() {}

String* Runtime::intern(const char* ascii) {
    // Route through the UTF-16 path so every spelling of a name shares ONE
    // canonical String object (property keys rely on pointer identity).
    std::u16string s;
    while (*ascii) s.push_back((char16_t)(unsigned char)*ascii++);
    return internUTF16(s.data(), (uint32_t)s.size());
}

String* Runtime::internUTF16(const char16_t* s, uint32_t len) {
    std::string key((const char*)s, (size_t)len * 2);
    auto it = internTable_.find(key);
    if (it != internTable_.end()) return it->second;
    String* str = stringFromUTF16(this, s, len);
    internTable_[key] = str;
    return str;
}

void Runtime::runGC() {
    if (gcMarkRoots) gcMarkRoots(gcCtx);
    heap.clearGCRequest(); // re-armed by the next threshold crossing
    size_t freed = heap.collect();
    if (getenv("TS_DEBUG_GC"))
        std::fprintf(stderr, "[gc] freed=%.1fMB live=%.1fMB\n",
                     freed / 1048576.0, heap.bytesAllocated() / 1048576.0);
}

void Runtime::throwValue(Value v) {
    interp->throwValue(v);
}

Object* Runtime::newErrorObject(const char* ctor, const char* msg) {
    Object* o = Object::create(this, objectProto, false);
    defineProperty(o, "name", Value::fromObj((HeapObj*)intern(ctor)));
    defineProperty(o, "message", Value::fromObj((HeapObj*)intern(msg)));
    return o;
}

[[noreturn]] void Runtime::throwTypeError(const char* msg) {
    interp->throwValue(Value::fromObj((HeapObj*)newErrorObject("TypeError", msg)));
}
[[noreturn]] void Runtime::throwReferenceError(const char* msg) {
    interp->throwValue(Value::fromObj((HeapObj*)newErrorObject("ReferenceError", msg)));
}
[[noreturn]] void Runtime::throwRangeError(const char* msg) {
    interp->throwValue(Value::fromObj((HeapObj*)newErrorObject("RangeError", msg)));
}

Value makeError(Runtime* rt, const char* ctor, const char* msg) {
    return Value::fromObj((HeapObj*)rt->newErrorObject(ctor, msg));
}

// ------------------------------------------------------------- conversions
Value Runtime::toPrimitiveValue(Value v, const char* hint) {
    if (!v.isObject()) return v;
    Object* o = v.asObj();
    const char* order[2];
    if (hint[0] == 's') { order[0] = "toString"; order[1] = "valueOf"; }
    else { order[0] = "valueOf"; order[1] = "toString"; }
    for (int i = 0; i < 2; i++) {
        Value m;
        if (getProperty(this, v, intern(order[i]), &m) && m.isFunction()) {
            JSFunction* f = (JSFunction*)m.asObj();
            Value res = interp->callFunction(f, v, nullptr, 0);
            if (!res.isObject()) return res;
        }
    }
    throwTypeError("Cannot convert object to primitive value");
}

Value Runtime::toNumberValue(Value v) {
    if (v.isNumber()) return v;
    if (v.isBool()) return Value::fromSmi(v.isTrue() ? 1 : 0);
    if (v.isUndef()) return Value::nan();
    if (v.isNull()) return Value::fromSmi(0);
    if (v.isString()) return Value::fromDouble(stringToNumber(v.asStringUnchecked()));
    return toNumberValue(toPrimitiveValue(v, "number"));
}

Value Runtime::toNumericValue(Value v) {
    return toNumberValue(v); // BigInt: registered gap
}

double Runtime::toDoubleForArith(Value v) {
    return toNumberValue(v).asNumber();
}

String* Runtime::toStringObj(Value v) {
    if (v.isString()) return v.asStringUnchecked();
    if (v.isUndef()) return intern("undefined");
    if (v.isNull()) return intern("null");
    if (v.isTrue()) return intern("true");
    if (v.bits == kFalseBits) return intern("false");
    if (v.isNumber()) return numberToString(this, v.asNumber());
    return toStringObj(toPrimitiveValue(v, "string"));
}

Value Runtime::toPropertyValue(Value k) {
    if (k.isSmi()) return k;
    if (k.isNumber()) return Value::fromObj((HeapObj*)numberToString(this, k.asNumber()));
    if (k.isString()) return k;
    return Value::fromObj((HeapObj*)toStringObj(k));
}

void Runtime::definePropertyStr(Object* o, String* key, Value v) {
    if (o->dictMode) { o->dict->set(key, v); return; }
    bool found; uint32_t idx = o->ownSlotIndex(key, &found);
    if (found) { o->slots[idx] = v; return; }
    if (o->shape->propCount >= o->slotCap) {
        uint32_t nc = o->slotCap * 2;
        if (o->slots == o->inlineSlots) {
            Value* heapSlots = (Value*)std::malloc((size_t)nc * sizeof(Value));
            for (uint32_t i2 = 0; i2 < o->slotCap; i2++) heapSlots[i2] = o->inlineSlots[i2];
            o->slots = heapSlots;
        } else {
            o->slots = (Value*)std::realloc(o->slots, (size_t)nc * sizeof(Value));
        }
        o->slotCap = nc;
    }
    o->shape = shapes.transition(o->shape, key);
    o->slots[o->shape->slot] = v;
}

JSFunction* Runtime::defineBuiltin(Object* o, const char* name,
        Value (*fn)(Runtime*, Value, Value*, uint32_t)) {
    JSFunction* f = JSFunction::createNative(this, functionProto, fn, name);
    definePropertyStr(o, intern(name), Value::fromObj((HeapObj*)f));
    return f;
}

} // namespace ts
