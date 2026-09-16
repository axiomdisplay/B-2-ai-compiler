// TurboScript Tier 0 — object.cpp
#include "object.h"
#include "runtime.h"
#include <cstdio>

namespace ts {

// inline Value predicate definitions are in value.h (needs HeapObj kind byte)

// ---------------------------------------------------------------- ShapeArena
Shape* ShapeArena::addTransition(Shape* from, String* key) {
    Shape* s = new Shape();
    s->parent = from;
    s->key = key;
    s->slot = from->propCount;
    s->propCount = from->propCount + 1;
    if (from->nTransitions == from->capTransitions) {
        uint32_t nc = from->capTransitions ? from->capTransitions * 2 : 4;
        from->transitions = (Shape**)std::realloc(from->transitions, nc * sizeof(Shape*));
        from->capTransitions = nc;
    }
    from->transitions[from->nTransitions++] = s;
    return s;
}

Shape* ShapeArena::transition(Shape* from, String* key) {
    for (uint32_t i = 0; i < from->nTransitions; i++)
        if (from->transitions[i]->key == key) return from->transitions[i]; // interned: ptr eq
    return addTransition(from, key);
}

ShapeArena::~ShapeArena() {
    // shapes intentionally leak (permanent) — registered gap: shape GC
}

// ---------------------------------------------------------------- Dict
void Dict::init(uint32_t cap) {
    cap_ = cap ? cap : 8;
    keys_ = (String**)std::calloc(cap_, sizeof(String*));
    vals_ = (Value*)std::calloc(cap_, sizeof(Value));
    used_ = (uint8_t*)std::calloc(cap_, 1);
    count_ = 0;
}
void Dict::grow() {
    String** ok = keys_; Value* ov = vals_; uint8_t* ou = used_; uint32_t oc = cap_;
    cap_ *= 2;
    keys_ = (String**)std::calloc(cap_, sizeof(String*));
    vals_ = (Value*)std::calloc(cap_, sizeof(Value));
    used_ = (uint8_t*)std::calloc(cap_, 1);
    count_ = 0;
    for (uint32_t i = 0; i < oc; i++)
        if (ou && ou[i] == 1) set(ok[i], ov[i]);
    std::free(ok); std::free(ov); std::free(ou);
}
Value* Dict::find(String* key) {
    if (!keys_) return nullptr;
    uint32_t h = stringHash(key) & (cap_ - 1);
    for (uint32_t i = 0; i < cap_; i++) {
        uint32_t j = (h + i) & (cap_ - 1);
        if (used_[j] == 0) return nullptr;
        if (used_[j] == 1 && keys_[j] == key) return &vals_[j];
    }
    return nullptr;
}
void Dict::set(String* key, Value v) {
    if (!keys_) init(8);
    if ((count_ + 1) * 3 >= cap_ * 2) grow();
    uint32_t h = stringHash(key) & (cap_ - 1);
    for (uint32_t i = 0; i < cap_; i++) {
        uint32_t j = (h + i) & (cap_ - 1);
        if (used_[j] == 0) { used_[j] = 1; keys_[j] = key; vals_[j] = v; count_++; return; }
        if (used_[j] == 1 && keys_[j] == key) { vals_[j] = v; return; }
    }
}
bool Dict::remove(String* key) {
    if (!keys_) return false;
    uint32_t h = stringHash(key) & (cap_ - 1);
    for (uint32_t i = 0; i < cap_; i++) {
        uint32_t j = (h + i) & (cap_ - 1);
        if (used_[j] == 0) return false;
        if (used_[j] == 1 && keys_[j] == key) { used_[j] = 2; count_--; return true; }
    }
    return false;
}

// ---------------------------------------------------------------- Object
Object* Object::create(Runtime* rt, Object* proto, bool isArray) {
    // NOTE: no GC collection here — GC triggers only at interpreter safepoints
    // (loop backedges, calls) so C++ locals holding fresh Values stay safe.
    Object* o = (Object*)rt->heap.allocRaw(sizeof(Object), ObjKind::Object);
    o->shape = &rt->shapes.root;
    o->proto = proto;
    o->isArray = isArray;
    o->slotCap = Object::kInlineSlots;
    o->slots = o->inlineSlots; // grow to heap slots only beyond the inline window
    return o;
}

void Object::destroyInternal() {
    if (slots && slots != inlineSlots) std::free(slots);
    std::free(elements);
    if (dict) { dict->destroy(); delete dict; dict = nullptr; }
}

uint32_t Object::ownSlotIndex(String* key, bool* found) {
    if (dictMode) { *found = false; return 0; }
    for (Shape* s = shape; s && s->key; s = s->parent) {
        if (s->key == key) { *found = true; return s->slot; }
        if (s->key->len == key->len && stringEquals(s->key, key)) { *found = true; return s->slot; }
    }
    *found = false;
    return 0;
}

Value* Object::findOwnSlot(String* key) {
    if (dictMode) return dict ? dict->find(key) : nullptr;
    bool found; uint32_t i = ownSlotIndex(key, &found);
    return found ? &slots[i] : nullptr;
}

// ---------------------------------------------------------------- Context
Context* Context::create(Runtime* rt, Context* parent, uint32_t nCells) {
    Context* c = (Context*)rt->heap.allocRaw(sizeof(Context), ObjKind::Context);
    c->parent = parent;
    c->nCells = nCells;
    c->cells = (Value*)std::malloc((nCells ? nCells : 1) * sizeof(Value));
    for (uint32_t i = 0; i < nCells; i++) c->cells[i] = Value::tdz(); // TDZ default; param/var stores overwrite
    return c;
}

// ---------------------------------------------------------------- JSFunction
JSFunction* JSFunction::createInterpreted(Runtime* rt, BytecodeFunction* bf, Context* ctx) {
    JSFunction* f = (JSFunction*)rt->heap.allocRaw(sizeof(JSFunction), ObjKind::Function);
    f->shape = &rt->shapes.root;
    f->proto = rt->functionProto;
    f->slotCap = Object::kInlineSlots;
    f->slots = f->inlineSlots;
    f->fn.bf = bf;
    f->fn.ctx = ctx;
    f->fn.isCtor = true; // interpreted functions are constructors (ES, non-arrow)
    return f;
}

JSFunction* JSFunction::createNative(Runtime* rt, Object* proto,
        Value (*native)(Runtime*, Value, Value*, uint32_t), const char* name) {
    JSFunction* f = (JSFunction*)rt->heap.allocRaw(sizeof(JSFunction), ObjKind::Function);
    f->shape = &rt->shapes.root;
    f->proto = proto;
    f->slotCap = Object::kInlineSlots;
    f->slots = f->inlineSlots;
    f->fn.native = native;
    f->fn.isCtor = false;
    (void)name; // .name registered gap (Tier 0 MVP)
    return f;
}

// ---------------------------------------------------------------- generic property access
static bool lookupSpecial(Runtime* rt, Value recv, String* key, Value* out, Object** holderOut) {
    // Functions: "prototype"
    if (recv.isFunction()) {
        JSFunction* f = (JSFunction*)recv.asObj();
        if (key == rt->intern("prototype")) {
            if (!f->fn.hasCtorProto) {
                f->fn.ctorProto = Object::create(rt, rt->objectProto, false);
                f->fn.hasCtorProto = true;
            }
            *out = Value::fromObj(f->fn.ctorProto);
            if (holderOut) *holderOut = f;
            return true;
        }
    }
    // Arrays: "length"
    if (recv.isObject() && recv.asObj()->isArray && key == rt->intern("length")) {
        *out = Value::fromSmi((int32_t)recv.asObj()->elemLen);
        if (holderOut) *holderOut = recv.asObj();
        return true;
    }
    // String primitives: "length"
    if (recv.isString() && key == rt->intern("length")) {
        *out = Value::fromSmi((int32_t)recv.asStringUnchecked()->len);
        return true;
    }
    return false;
}

static bool lookupOnProto(Runtime* rt, Value recv, Object* start, String* key, Value* out) {
    for (Object* o = start; o; o = o->proto) {
        if (!o->dictMode) {
            bool found; uint32_t idx = o->ownSlotIndex(key, &found);
            if (found) { *out = o->slots[idx]; return true; }
        } else {
            if (o->dict) { if (Value* v = o->dict->find(key)) { *out = *v; return true; } }
        }
        // specials live on the holder itself
        Value sv;
        if (lookupSpecial(rt, Value::fromObj(o), key, &sv, nullptr)) { *out = sv; return true; }
    }
    return false;
}

bool getProperty(Runtime* rt, Value recv, String* key, Value* out) {
    if (recv.isObject()) {
        Object* o = recv.asObj();
        // special-cases bound to this receiver
        Value sv;
        if (lookupSpecial(rt, recv, key, &sv, nullptr)) { *out = sv; return true; }
        if (!o->dictMode) {
            bool found; uint32_t idx = o->ownSlotIndex(key, &found);
            if (found) { *out = o->slots[idx]; return true; }
        } else {
            if (Value* v = o->dict->find(key)) { *out = *v; return true; }
        }
        return lookupOnProto(rt, recv, o->proto, key, out);
    }
    if (recv.isString()) {
        String* s = recv.asStringUnchecked();
        if (key == rt->intern("length")) { *out = Value::fromSmi((int32_t)s->len); return true; }
        // methods on String.prototype
        return lookupOnProto(rt, recv, rt->stringProto, key, out);
    }
    if (recv.isNumber() && !recv.isSmi())
        return lookupOnProto(rt, recv, rt->numberProto, key, out);
    if (recv.isBool())
        return lookupOnProto(rt, recv, rt->numberProto, key, out); // registered gap: Boolean.prototype
    if (recv.isUndef() || recv.isNull()) {
        if (getenv("TS_DEBUG_THROW")) std::fprintf(stderr, "[dbg] getProperty undef/null\n");
        rt->throwTypeError("Cannot read properties of null/undefined");
    }
    *out = Value::undef();
    return false;
}

bool hasProperty(Runtime* rt, Value recv, String* key) {
    Value tmp;
    if (recv.isObject()) {
        Object* o = recv.asObj();
        Value sv;
        if (lookupSpecial(rt, recv, key, &sv, nullptr)) return true;
        if (!o->dictMode) { bool f; o->ownSlotIndex(key, &f); if (f) return true; }
        else if (o->dict && o->dict->find(key)) return true;
        return lookupOnProto(rt, recv, o->proto, key, &tmp);
    }
    if (recv.isString()) {
        if (key == rt->intern("length")) return true;
        return lookupOnProto(rt, recv, rt->stringProto, key, &tmp);
    }
    return false;
}

bool setProperty(Runtime* rt, Value recv, String* key, Value v) {
    if (!recv.isObject()) {
        // Assigning to primitives is silently ignored in sloppy mode (ES §7.3.13 note)
        return true;
    }
    Object* o = recv.asObj();
    // Array "length" write
    if (o->isArray && key == rt->intern("length")) {
        double n = rt->toNumberValue(v).asNumber();
        if (n < 0 || n != (uint32_t)n) rt->throwRangeError("Invalid array length");
        uint32_t nl = (uint32_t)n;
        if (nl < o->elemLen) o->elemLen = nl;   // truncate (elements stay allocated)
        else {
            if (nl > o->elemCap) { // grow
                uint32_t nc = o->elemCap ? o->elemCap : 4;
                while (nc < nl) nc *= 2;
                o->elements = (Value*)std::realloc(o->elements, (size_t)nc * sizeof(Value));
                o->elemCap = nc;
            }
            for (uint32_t i = o->elemLen; i < nl; i++) o->elements[i] = Value::undef();
            o->elemLen = nl;
        }
        return true;
    }
    if (o->dictMode) { o->dict->set(key, v); return true; }
    bool found; uint32_t idx = o->ownSlotIndex(key, &found);
    if (found) { o->slots[idx] = v; return true; }
    // proto chain setter? (accessors not supported — registered gap) shadow-add:
    // walk proto chain: if found on proto (data prop) => own add (sloppy semantics:
    // writing a data prop found on proto creates own property). Read-only proto
    // props would silently fail in sloppy mode — registered gap.
    Value tmp;
    if (lookupOnProto(rt, recv, o->proto, key, &tmp)) {
        // found on prototype: still add own property (data prop case)
    }
    // add own property via shape transition
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
    o->shape = rt->shapes.transition(o->shape, key);
    o->slots[o->shape->slot] = v;
    return true;
}

bool deleteProperty(Runtime* rt, Object* o, String* key) {
    // Snapshot into dict, then delete (safe, generic).
    if (!o->dictMode) {
        Dict* d = new Dict();
        d->init(8);
        if (o->shape) {
            for (Shape* s2 = o->shape; s2 && s2->key; s2 = s2->parent)
                d->set(s2->key, o->slots[s2->slot]);
        }
        o->dictMode = true;
        o->dict = d;
    }
    return o->dict->remove(key);
}

// ---------------------------------------------------------------- elements
static bool toArrayIndex(Value key, uint32_t* idx) {
    if (key.isSmi()) {
        int32_t i = key.asSmi();
        if (i >= 0) { *idx = (uint32_t)i; return true; }
        return false;
    }
    if (key.isDouble()) {
        double d = key.asDouble();
        if (d >= 0 && d == (uint64_t)d && d < 4294967294.0) { *idx = (uint32_t)d; return true; }
    }
    return false;
}

bool getElement(Runtime* rt, Value recv, Value key, Value* out) {
    if (recv.isObject()) {
        Object* o = recv.asObj();
        uint32_t idx;
        if (toArrayIndex(key, &idx)) {
            if (idx < o->elemLen) { *out = o->elements[idx]; return true; }
            Value sk = rt->toPropertyValue(key);
            if (sk.isString()) return getProperty(rt, recv, sk.asStringUnchecked(), out);
            return false;
        }
        if (key.isString()) {
            // canonical numeric strings address elements (for-in compatibility)
            if (strToIndex(key.asStringUnchecked(), &idx) && idx < o->elemLen) {
                *out = o->elements[idx];
                return true;
            }
            return getProperty(rt, recv, key.asStringUnchecked(), out);
        }
        Value pk = rt->toPropertyValue(key);
        return pk.isString() ? getProperty(rt, recv, pk.asStringUnchecked(), out) : false;
    }
    if (recv.isString()) {
        String* s = recv.asStringUnchecked();
        uint32_t idx;
        if (toArrayIndex(key, &idx)) {
            if (idx < s->len) {
                char16_t c = s->data()[idx];
                *out = Value::fromObj(stringFromUTF16(rt, &c, 1));
                return true;
            }
            return false;
        }
        if (key.isString()) return getProperty(rt, recv, key.asStringUnchecked(), out);
    }
    if (recv.isUndef() || recv.isNull()) rt->throwTypeError("Cannot read properties of null/undefined");
    return false;
}

bool setElement(Runtime* rt, Value recv, Value key, Value v) {
    if (!recv.isObject()) return true; // primitive write ignored (sloppy)
    Object* o = recv.asObj();
    uint32_t idx;
    if (toArrayIndex(key, &idx)) {
        if (idx < o->elemLen) { o->elements[idx] = v; return true; }
        if (idx < 4096 || idx < o->elemLen * 2 + 16) {
            uint32_t nc = o->elemCap ? o->elemCap : 4;
            while (nc <= idx) nc *= 2;
            o->elements = (Value*)std::realloc(o->elements, (size_t)nc * sizeof(Value));
            o->elemCap = nc;
            for (uint32_t i = o->elemLen; i <= idx; i++) o->elements[i] = Value::undef();
            o->elemLen = idx + 1;
            o->elements[idx] = v;
            return true;
        }
        // sparse: fall through to named property "12345"
        Value sk = rt->toPropertyValue(key);
        return setProperty(rt, recv, sk.asStringUnchecked(), v);
    }
    if (key.isString()) {
        // canonical numeric strings address elements (for-in compatibility)
        if (strToIndex(key.asStringUnchecked(), &idx)) {
            Value sk = Value::fromSmi((int32_t)idx);
            return setElement(rt, recv, sk, v);
        }
        return setProperty(rt, recv, key.asStringUnchecked(), v);
    }
    Value pk = rt->toPropertyValue(key);
    return pk.isString() ? setProperty(rt, recv, pk.asStringUnchecked(), v) : true;
}

// ---------------------------------------------------------------- equality
bool strictEquals(Value a, Value b) {
    if (a.isNumber() && b.isNumber()) {
        // Numbers compare by value: unifies smi/double, handles NaN and ±0 === 0.
        double da = a.asNumber(), db = b.asNumber();
        if (std::isnan(da) || std::isnan(db)) return false;
        return da == db;
    }
    if (a.isString() && b.isString())
        return stringEquals(a.asStringUnchecked(), b.asStringUnchecked());
    return a.bits == b.bits;
}

bool sameValue(Value a, Value b) {
    if (a.isNumber() && b.isNumber()) {
        double da = a.asNumber(), db = b.asNumber();
        if (std::isnan(da) && std::isnan(db)) return true;
        if (da == 0 && db == 0) return std::signbit(da) == std::signbit(db); // -0 vs 0
        return da == db;
    }
    if (a.isString() && b.isString())
        return stringEquals(a.asStringUnchecked(), b.asStringUnchecked());
    return a.bits == b.bits;
}

bool sameValueZero(Value a, Value b) {
    if (a.isNumber() && b.isNumber()) {
        double da = a.asNumber(), db = b.asNumber();
        if (std::isnan(da) && std::isnan(db)) return true;
        return da == db; // ±0 equal
    }
    if (a.isString() && b.isString())
        return stringEquals(a.asStringUnchecked(), b.asStringUnchecked());
    return a.bits == b.bits;
}

bool abstractEquals(Runtime* rt, Value a, Value b) {
    for (;;) {
        if (a.isNumber() && b.isNumber()) {
            if (a.isSmi() && b.isSmi()) return a.asSmi() == b.asSmi();
            return a.asNumber() == b.asNumber();
        }
        if (a.isString() && b.isString()) return stringEquals(a.asStringUnchecked(), b.asStringUnchecked());
        if (a.isBool()) { a = Value::fromSmi(a.isTrue() ? 1 : 0); continue; }
        if (b.isBool()) { b = Value::fromSmi(b.isTrue() ? 1 : 0); continue; }
        if (a.isUndef() && b.isNull()) return true;
        if (a.isNull() && b.isUndef()) return true;
        if (a.isNumber() && b.isString()) { b = rt->toNumberValue(b); continue; }
        if (a.isString() && b.isNumber()) { a = rt->toNumberValue(a); continue; }
        if (a.isObject()) { a = rt->toPrimitiveValue(a, "default"); continue; }
        if (b.isObject()) { b = rt->toPrimitiveValue(b, "default"); continue; }
        return false;
    }
}

Value lessThanValue(Runtime* rt, Value a, Value b, bool orEqual) {
    // Returns true/false Value; undefined when comparison involves NaN.
    auto numCmp = [&](double x, double y) -> Value {
        if (std::isnan(x) || std::isnan(y)) return Value::undef();
        if (orEqual) return Value::boolean(x <= y);
        return Value::boolean(x < y);
    };
    if (a.isString() && b.isString()) {
        int c = stringCompare(a.asStringUnchecked(), b.asStringUnchecked());
        return Value::boolean(orEqual ? c <= 0 : c < 0);
    }
    if (a.isNumber() && b.isNumber()) {
        if (a.isSmi() && b.isSmi() && !orEqual) return Value::boolean(a.asSmi() < b.asSmi());
        if (a.isSmi() && b.isSmi() && orEqual) return Value::boolean(a.asSmi() <= b.asSmi());
        return numCmp(a.asNumber(), b.asNumber());
    }
    Value pa = rt->toPrimitiveValue(a, "number");
    Value pb = rt->toPrimitiveValue(b, "number");
    if (pa.isString() && pb.isString()) {
        int c = stringCompare(pa.asStringUnchecked(), pb.asStringUnchecked());
        return Value::boolean(orEqual ? c <= 0 : c < 0);
    }
    return numCmp(rt->toDoubleForArith(pa), rt->toDoubleForArith(pb));
}

} // namespace ts
