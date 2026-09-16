// TurboScript — v0.3 builtin layer (bytecode_spec.md Section 11):
// Object.prototype / Function.prototype / Array.prototype wiring, the
// Array.prototype method set, and the Object / Array global namespaces.
// Semantics follow ECMA-262 within the v0.3 value model (holes, exotic
// array length, no primitive wrappers); divergences are registered in
// bytecode_spec.md Section 11 and interp_contract.md (Rule 143 register).
#include "ts_interpreter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace ts {

// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------
namespace {

// thisVal must be an array; TypeError otherwise (v0.3: no generic
// array-likes — registered divergence).
JsResult<Object*> requireArray(Isolate& iso, Value thisVal, const char* who) {
  if (thisVal.kind() == ValueKind::Object) {
    Object* obj = static_cast<Object*>(thisVal.asPtr());
    if (obj->isArray) return obj;
  }
  return std::unexpected(iso.typeError(
      std::string("Array.prototype.") + who + " called on a non-array"));
}

// ToIntegerOrInfinity over the hook-aware ToNumber (user code may run).
[[nodiscard]] JsResult<double> toIntegerOrInfinity(Isolate& iso,
                                                   const Value& v) {
  JsResult<Value> n = iso.toNumberValue(v);
  if (!n) return std::unexpected(n.error());
  double d = n->asDouble();
  if (std::isnan(d)) return 0.0;
  return std::trunc(d);
}

// Append `v` (which may be a Hole = absent slot) at a fresh dense index of
// `arr`, preserving the elements-kind invariant (holes never widen kinds;
// widening is the caller's duty via Isolate::widenElementsFor).
void appendElement(Object* arr, uint32_t idx, const Value& v) {
  if (idx >= kMaxDenseElements) {
    arr->ensureSparse()[idx] = v;
    arr->elementsKind = ElementsKind::HoleyTagged;
    if (arr->length <= idx) arr->length = idx + 1;
    return;
  }
  if (idx >= arr->elements.size()) {
    arr->elements.resize(static_cast<size_t>(idx) + 1, Value::hole());
  }
  arr->elements[idx] = v;
  if (v.isHole()) {
    arr->elementsKind = holeyOf(arr->elementsKind);
  }
  if (arr->length <= idx) arr->length = idx + 1;
}

// Fresh empty array with the standard prototype.
Object* newArrayObject(Isolate& iso) {
  Object* arr = iso.heap().makeObject();
  arr->isArray = true;
  arr->proto = Value::raw(ValueKind::Object, iso.arrayPrototype());
  return arr;
}

// ToString wrapper returning the u16 text.
[[nodiscard]] JsResult<std::u16string> toStr(Isolate& iso, const Value& v) {
  JsResult<Value> s = iso.toStringValue(v);
  if (!s) return std::unexpected(s.error());
  return s->asString()->flat();
}

}  // namespace

// ---------------------------------------------------------------------------
// Object.prototype natives
// ---------------------------------------------------------------------------
namespace {

JsResult<Value> builtinObjectToString(Isolate& iso, Value thisVal,
                                      const Value*, uint32_t) {
  const char* tag = "Object";
  if (thisVal.kind() == ValueKind::Object &&
      static_cast<Object*>(thisVal.asPtr())->isArray) {
    tag = "Array";
  }
  std::u16string out = u"[object ";
  for (const char* c = tag; *c != '\0'; c++) {
    out.push_back(static_cast<char16_t>(*c));
  }
  out += u"]";
  return Value::string(iso.heap().makeString(std::move(out)));
}

JsResult<Value> builtinObjectValueOf(Isolate&, Value thisVal, const Value*,
                                     uint32_t) {
  return thisVal;
}

JsResult<Value> builtinHasOwnProperty(Isolate& iso, Value thisVal,
                                      const Value* args, uint32_t argc) {
  if (argc < 1 || !thisVal.isObjectLike()) return Value::boolean(false);
  JsResult<SymbolId> key = iso.toPropertyKey(args[0]);
  if (!key) return std::unexpected(key.error());
  Object* obj = objectOfValue(thisVal);
  if (ownDataSlotAttrs(obj, *key, nullptr) >= 0) return Value::boolean(true);
  if (obj->isArray) {
    if (*key == iso.lengthSymbol()) return Value::boolean(true);
    uint32_t idx = 0;
    if (arrayIndexFromKey(iso.symbols().text(*key), &idx)) {
      return Value::boolean(iso.arrayHasOwnElement(obj, idx));
    }
  }
  return Value::boolean(false);
}

// ---------------------------------------------------------------------------
// Array.prototype natives
// ---------------------------------------------------------------------------
JsResult<Value> builtinArrayPush(Isolate& iso, Value thisVal,
                                 const Value* args, uint32_t argc) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "push");
  if (!arrR) return std::unexpected(arrR.error());
  Object* arr = *arrR;
  for (uint32_t i = 0; i < argc; i++) {
    uint32_t len = arr->length;
    JsResult<bool> r = iso.setArrayElement(arr, len, args[i]);
    if (!r) return std::unexpected(r.error());
  }
  return iso.arrayLengthValue(arr);
}

JsResult<Value> builtinArrayPop(Isolate& iso, Value thisVal, const Value*,
                                uint32_t) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "pop");
  if (!arrR) return std::unexpected(arrR.error());
  Object* arr = *arrR;
  if (arr->length == 0) return Value::undefined();
  const uint32_t idx = arr->length - 1;
  Value v = iso.ownElementValue(arr, idx);
  if (idx < arr->elements.size()) {
    arr->elements[idx] = Value::hole();
    arr->elementsKind = holeyOf(arr->elementsKind);
  } else {
    arr->ensureSparse().erase(idx);
  }
  arr->length = idx;
  return v.isHole() ? Value::undefined() : v;
}

JsResult<Value> builtinArrayShift(Isolate& iso, Value thisVal, const Value*,
                                  uint32_t) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "shift");
  if (!arrR) return std::unexpected(arrR.error());
  Object* arr = *arrR;
  if (arr->length == 0) return Value::undefined();
  Value first = iso.ownElementValue(arr, 0);
  if (!arr->elements.empty()) {
    arr->elements.erase(arr->elements.begin());
  }
  if (!arr->sparseMap().empty()) {
    std::map<uint32_t, Value> shifted;
    for (const auto& kv : arr->sparseMap()) {
      if (kv.first > 0 && kv.first < arr->length) shifted[kv.first - 1] = kv.second;
    }
    arr->ensureSparse() = std::move(shifted);
  }
  arr->length -= 1;
  return first.isHole() ? Value::undefined() : first;
}

JsResult<Value> builtinArrayUnshift(Isolate& iso, Value thisVal,
                                    const Value* args, uint32_t argc) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "unshift");
  if (!arrR) return std::unexpected(arrR.error());
  Object* arr = *arrR;
  // Widen first (kind invariant), then shift dense storage right.
  for (uint32_t i = 0; i < argc; i++) iso.widenElementsFor(arr, args[i]);
  if (!arr->elements.empty() && argc > 0) {
    arr->elements.insert(arr->elements.begin(), argc, Value::hole());
    for (uint32_t i = 0; i < argc; i++) {
      arr->elements[i] = argc > 0 ? args[i] : Value::undefined();
    }
  } else if (argc > 0) {
    arr->elements.resize(argc, Value::hole());
    for (uint32_t i = 0; i < argc; i++) arr->elements[i] = args[i];
  }
  if (!arr->sparseMap().empty() && argc > 0) {
    std::map<uint32_t, Value> shifted;
    for (const auto& kv : arr->sparseMap()) shifted[kv.first + argc] = kv.second;
    arr->ensureSparse() = std::move(shifted);
  }
  arr->length += argc;
  return iso.arrayLengthValue(arr);
}

JsResult<Value> builtinArrayJoin(Isolate& iso, Value thisVal,
                                 const Value* args, uint32_t argc) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "join");
  if (!arrR) return std::unexpected(arrR.error());
  Object* arr = *arrR;
  std::u16string sep = u",";
  if (argc >= 1 && !args[0].isUndefined()) {
    JsResult<std::u16string> s = toStr(iso, args[0]);
    if (!s) return std::unexpected(s.error());
    sep = *s;
  }
  std::u16string out;
  const uint32_t len = arr->length;  // ES: original length
  for (uint32_t i = 0; i < len; i++) {
    if (i > 0) out += sep;
    Value v = iso.ownElementValue(arr, i);
    if (v.isHole() || v.isNullOrUndefined()) continue;
    JsResult<std::u16string> s = toStr(iso, v);
    if (!s) return std::unexpected(s.error());
    out += *s;
  }
  return Value::string(iso.heap().makeString(std::move(out)));
}

JsResult<Value> builtinArrayIndexOf(Isolate& iso, Value thisVal,
                                    const Value* args, uint32_t argc) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "indexOf");
  if (!arrR) return std::unexpected(arrR.error());
  Object* arr = *arrR;
  const uint32_t len = arr->length;
  uint32_t from = 0;
  if (argc >= 2) {
    JsResult<double> d = toIntegerOrInfinity(iso, args[1]);
    if (!d) return std::unexpected(d.error());
    if (*d < 0) {
      int64_t back = static_cast<int64_t>(len) + static_cast<int64_t>(*d);
      from = back < 0 ? 0 : static_cast<uint32_t>(back);
    } else {
      from = *d >= static_cast<double>(len) ? len : static_cast<uint32_t>(*d);
    }
  }
  for (uint32_t i = from; i < len; i++) {
    Value v = iso.ownElementValue(arr, i);
    if (!v.isHole() && pure::strictEquals(v, args[0])) {
      return Value::smi(static_cast<int32_t>(i));
    }
  }
  return Value::smi(-1);
}

JsResult<Value> builtinArrayIncludes(Isolate& iso, Value thisVal,
                                     const Value* args, uint32_t argc) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "includes");
  if (!arrR) return std::unexpected(arrR.error());
  Object* arr = *arrR;
  const uint32_t len = arr->length;
  uint32_t from = 0;
  if (argc >= 2) {
    JsResult<double> d = toIntegerOrInfinity(iso, args[1]);
    if (!d) return std::unexpected(d.error());
    if (*d < 0) {
      int64_t back = static_cast<int64_t>(len) + static_cast<int64_t>(*d);
      from = back < 0 ? 0 : static_cast<uint32_t>(back);
    } else {
      from = *d >= static_cast<double>(len) ? len : static_cast<uint32_t>(*d);
    }
  }
  for (uint32_t i = from; i < len; i++) {
    Value v = iso.ownElementValue(arr, i);
    Value cur = v.isHole() ? Value::undefined() : v;  // holes read as undefined
    if (pure::sameValueZero(cur, args[0])) return Value::boolean(true);
  }
  return Value::boolean(false);
}

JsResult<Value> builtinArraySlice(Isolate& iso, Value thisVal,
                                  const Value* args, uint32_t argc) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "slice");
  if (!arrR) return std::unexpected(arrR.error());
  Object* arr = *arrR;
  const uint32_t len = arr->length;
  double relStart = 0.0, relEnd = static_cast<double>(len);
  if (argc >= 1) {
    JsResult<double> d = toIntegerOrInfinity(iso, args[0]);
    if (!d) return std::unexpected(d.error());
    relStart = *d;
  }
  if (argc >= 2) {
    JsResult<double> d = toIntegerOrInfinity(iso, args[1]);
    if (!d) return std::unexpected(d.error());
    relEnd = *d;
  }
  auto clamp = [](double rel, double deflt, uint32_t l) -> uint32_t {
    double r = rel == rel && rel != std::numeric_limits<double>::infinity()
                   ? (std::isinf(rel) ? (rel < 0 ? -static_cast<double>(l) : static_cast<double>(l))
                                      : (rel < 0 ? rel + static_cast<double>(l) : rel))
                   : deflt;
    if (r < 0) r = 0;
    if (r > static_cast<double>(l)) r = static_cast<double>(l);
    return static_cast<uint32_t>(r);
  };
  const uint32_t k = clamp(relStart, 0, len);
  const uint32_t finalIdx = clamp(relEnd, static_cast<double>(len), len);
  Object* res = newArrayObject(iso);
  uint32_t count = 0;
  for (uint32_t i = k; i < finalIdx; i++) {
    Value v = iso.ownElementValue(arr, i);  // absent stays absent
    appendElement(res, count, v);
    if (!v.isHole()) iso.widenElementsFor(res, v);
    count++;
  }
  return Value::raw(ValueKind::Object, res);
}

JsResult<Value> builtinArrayConcat(Isolate& iso, Value thisVal,
                                   const Value* args, uint32_t argc) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "concat");
  if (!arrR) return std::unexpected(arrR.error());
  Object* res = newArrayObject(iso);
  uint32_t count = 0;
  auto spread = [&](Object* src) -> JsResult<bool> {
    const uint32_t n = src->length;  // original length (ES)
    for (uint32_t i = 0; i < n; i++) {
      Value v = iso.ownElementValue(src, i);
      appendElement(res, count, v);
      if (!v.isHole()) iso.widenElementsFor(res, v);
      count++;
    }
    return true;
  };
  JsResult<bool> self = spread(*arrR);
  if (!self) return std::unexpected(self.error());
  for (uint32_t a = 0; a < argc; a++) {
    if (args[a].kind() == ValueKind::Object &&
        static_cast<Object*>(args[a].asPtr())->isArray) {
      JsResult<bool> r = spread(static_cast<Object*>(args[a].asPtr()));
      if (!r) return std::unexpected(r.error());
    } else {
      appendElement(res, count, args[a]);
      iso.widenElementsFor(res, args[a]);
      count++;
    }
  }
  return Value::raw(ValueKind::Object, res);
}

// Shared iteration core for forEach/map/filter/reduce: callback receives
// (value, index, array); holes are skipped. The callback may mutate the
// array — element reads and length stay live per ES Get/Has checks.
[[nodiscard]] JsResult<Value> arrayIterate(Isolate& iso, Value thisVal,
                                           const Value* args, uint32_t argc,
                                           const char* who,
                                           bool* skippedAll) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, who);
  if (!arrR) return std::unexpected(arrR.error());
  if (argc < 1 || !args[0].isClosure()) {
    return std::unexpected(iso.typeError(
        std::string("Array.prototype.") + who +
        ": callback must be a function"));
  }
  Value cb = args[0];
  Value thisArg = argc >= 2 ? args[1] : Value::undefined();
  (void)skippedAll;
  Object* arr = *arrR;
  const uint32_t len = arr->length;  // ES: original length drives the loop
  for (uint32_t i = 0; i < len; i++) {
    Value v = iso.ownElementValue(arr, i);
    if (v.isHole()) continue;  // HasProperty check fails for holes
    Value callArgs[3] = {v, Value::smi(static_cast<int32_t>(i)), thisVal};
    JsResult<Value> r = iso.callValue(cb, thisArg, callArgs, 3);
    if (!r) return std::unexpected(r.error());
  }
  return Value::undefined();
}

JsResult<Value> builtinArrayForEach(Isolate& iso, Value thisVal,
                                    const Value* args, uint32_t argc) {
  return arrayIterate(iso, thisVal, args, argc, "forEach", nullptr);
}

JsResult<Value> builtinArrayMap(Isolate& iso, Value thisVal, const Value* args,
                                uint32_t argc) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "map");
  if (!arrR) return std::unexpected(arrR.error());
  if (argc < 1 || !args[0].isClosure()) {
    return std::unexpected(
        iso.typeError("Array.prototype.map: callback must be a function"));
  }
  Value cb = args[0];
  Value thisArg = argc >= 2 ? args[1] : Value::undefined();
  Object* arr = *arrR;
  const uint32_t len = arr->length;
  Object* res = newArrayObject(iso);
  for (uint32_t i = 0; i < len; i++) {
    Value v = iso.ownElementValue(arr, i);
    if (v.isHole()) {
      appendElement(res, i, Value::hole());  // map preserves holes
      continue;
    }
    Value callArgs[3] = {v, Value::smi(static_cast<int32_t>(i)), thisVal};
    JsResult<Value> r = iso.callValue(cb, thisArg, callArgs, 3);
    if (!r) return std::unexpected(r.error());
    appendElement(res, i, *r);
    iso.widenElementsFor(res, *r);
  }
  return Value::raw(ValueKind::Object, res);
}

JsResult<Value> builtinArrayFilter(Isolate& iso, Value thisVal,
                                   const Value* args, uint32_t argc) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "filter");
  if (!arrR) return std::unexpected(arrR.error());
  if (argc < 1 || !args[0].isClosure()) {
    return std::unexpected(
        iso.typeError("Array.prototype.filter: callback must be a function"));
  }
  Value cb = args[0];
  Value thisArg = argc >= 2 ? args[1] : Value::undefined();
  Object* arr = *arrR;
  const uint32_t len = arr->length;
  Object* res = newArrayObject(iso);
  uint32_t count = 0;
  for (uint32_t i = 0; i < len; i++) {
    Value v = iso.ownElementValue(arr, i);
    if (v.isHole()) continue;
    Value callArgs[3] = {v, Value::smi(static_cast<int32_t>(i)), thisVal};
    JsResult<Value> r = iso.callValue(cb, thisArg, callArgs, 3);
    if (!r) return std::unexpected(r.error());
    if (pure::toBoolean(*r)) {
      appendElement(res, count, v);
      iso.widenElementsFor(res, v);
      count++;
    }
  }
  return Value::raw(ValueKind::Object, res);
}

JsResult<Value> builtinArrayReduce(Isolate& iso, Value thisVal,
                                   const Value* args, uint32_t argc) {
  JsResult<Object*> arrR = requireArray(iso, thisVal, "reduce");
  if (!arrR) return std::unexpected(arrR.error());
  if (argc < 1 || !args[0].isClosure()) {
    return std::unexpected(
        iso.typeError("Array.prototype.reduce: callback must be a function"));
  }
  Value cb = args[0];
  Object* arr = *arrR;
  const uint32_t len = arr->length;
  uint32_t k = 0;
  Value acc;
  if (argc >= 2) {
    acc = args[1];
  } else {
    while (k < len && iso.ownElementValue(arr, k).isHole()) k++;
    if (k == len) {
      return std::unexpected(iso.typeError(
          "Reduce of empty array with no initial value"));
    }
    acc = iso.ownElementValue(arr, k);
    k++;
  }
  for (; k < len; k++) {
    Value v = iso.ownElementValue(arr, k);
    if (v.isHole()) continue;
    Value callArgs[4] = {acc, v, Value::smi(static_cast<int32_t>(k)), thisVal};
    JsResult<Value> r = iso.callValue(cb, Value::undefined(), callArgs, 4);
    if (!r) return std::unexpected(r.error());
    acc = *r;
  }
  return acc;
}

// ---------------------------------------------------------------------------
// Object namespace natives
// ---------------------------------------------------------------------------
// Own enumerable string keys, ES order: array indices ascending first, then
// named keys in insertion order (shape chain walked oldest->newest). Holes
// and the exotic "length" are excluded (Rule 124: deterministic order).

JsResult<Value> builtinObjectKeys(Isolate& iso, Value thisVal,
                                  const Value* args, uint32_t argc) {
  (void)thisVal;
  Object* res = newArrayObject(iso);
  if (argc >= 1 && args[0].isProxy()) {
    JsResult<std::vector<Value>> keys = iso.proxyOwnKeys(args[0]);
    if (!keys) return std::unexpected(keys.error());
    uint32_t n = 0;
    for (const Value& k : *keys) {
      if (k.isString()) {  // Object.keys reports string keys only
        JsResult<bool> r = iso.setArrayElement(res, n, k);
        if (!r) return std::unexpected(r.error());
        n++;
      }
    }
    return Value::raw(ValueKind::Object, res);
  }
  std::vector<Value> keys = iso.ownKeysValues(objectOfValue(args[0]), false);
  uint32_t n = 0;
  for (const Value& k : keys) {
    if (k.isString()) {
      JsResult<bool> r = iso.setArrayElement(res, n, k);
      if (!r) return std::unexpected(r.error());
      n++;
    }
  }
  return Value::raw(ValueKind::Object, res);
}

JsResult<Value> builtinObjectGetPropertyNames(Isolate& iso, Value thisVal,
                                              const Value* args,
                                              uint32_t argc) {
  (void)thisVal;
  Object* res = newArrayObject(iso);
  if (argc >= 1 && args[0].isProxy()) {
    JsResult<std::vector<Value>> keys = iso.proxyOwnKeys(args[0]);
    if (!keys) return std::unexpected(keys.error());
    uint32_t n = 0;
    for (const Value& k : *keys) {
      if (k.isString()) {
        JsResult<bool> r = iso.setArrayElement(res, n, k);
        if (!r) return std::unexpected(r.error());
        n++;
      }
    }
    return Value::raw(ValueKind::Object, res);
  }
  std::vector<Value> keys = iso.ownKeysValues(objectOfValue(args[0]), true);
  uint32_t n = 0;
  for (const Value& k : keys) {
    if (k.isString()) {
      JsResult<bool> r = iso.setArrayElement(res, n, k);
      if (!r) return std::unexpected(r.error());
      n++;
    }
  }
  return Value::raw(ValueKind::Object, res);
}

JsResult<Value> builtinObjectGetOwnPropertyDescriptor(Isolate& iso,
                                                      Value thisVal,
                                                      const Value* args,
                                                      uint32_t argc) {
  (void)thisVal;
  if (argc < 2 || !args[0].isObjectLike()) return Value::undefined();
  if (args[0].isProxy()) {
    return iso.proxyGetOwnPropertyDescriptor(args[0], args[1]);
  }
  JsResult<SymbolId> key = iso.toPropertyKey(args[1]);
  if (!key) return std::unexpected(key.error());
  JsResult<Value> d = iso.getOwnPropertyDescriptorValue(objectOfValue(args[0]),
                                                        *key);
  if (!d) return std::unexpected(d.error());
  return *d;
}

// Read a descriptor object's fields (missing fields take ES defaults:
// value undefined, writable/enumerable/configurable false). Used by
// Object.defineProperty for both plain objects and proxy targets.
[[nodiscard]] JsResult<bool> readDescriptor(Isolate& iso, const Value& descVal,
                                            PropertyDescriptor* d) {
  if (!descVal.isObjectLike()) {
    std::unexpected(iso.typeError("Property description must be an object"));
  }
  Object* desc = objectOfValue(descVal);
  auto get = [&](const char16_t* name) -> JsResult<Value> {
    SymbolId sym = iso.symbols().intern(name);
    JsResult<Value> v = iso.getProperty(descVal, sym);
    if (!v) return std::unexpected(v.error());
    LookupResult own = lookupOwnProperty(desc, sym);
    if (own.found) return *own.dataSlot;
    return Value::undefined();  // absent field (inherited treated as absent)
  };
  auto hasField = [&](const char16_t* name) -> bool {
    SymbolId sym = iso.symbols().intern(name);
    return lookupOwnProperty(desc, sym).found;
  };
  JsResult<Value> gv = get(u"value");
  if (!gv) return std::unexpected(gv.error());
  if (!gv->isUndefined() || hasField(u"value")) {
    d->hasValue = true;
    d->value = *gv;
  }
  auto getFlag = [&](const char16_t* name, bool* out) -> JsResult<bool> {
    SymbolId sym = iso.symbols().intern(name);
    JsResult<Value> v = iso.getProperty(descVal, sym);
    if (!v) return std::unexpected(v.error());
    LookupResult own = lookupOwnProperty(desc, sym);
    if (!own.found) return true;  // default false, field absent
    *out = pure::toBoolean(*own.dataSlot);
    return true;
  };
  JsResult<bool> rw = getFlag(u"writable", &d->writable);
  if (!rw) return std::unexpected(rw.error());
  d->hasWritable = true;
  JsResult<bool> re = getFlag(u"enumerable", &d->enumerable);
  if (!re) return std::unexpected(re.error());
  d->hasEnumerable = true;
  JsResult<bool> rc = getFlag(u"configurable", &d->configurable);
  if (!rc) return std::unexpected(rc.error());
  d->hasConfigurable = true;
  JsResult<Value> g = get(u"get");
  if (!g) return std::unexpected(g.error());
  if (g->isClosure()) {
    d->hasGet = true;
    d->getter = *g;
  }
  JsResult<Value> st = get(u"set");
  if (!st) return std::unexpected(st.error());
  if (st->isClosure()) {
    d->hasSet = true;
    d->setter = *st;
  }
  return true;
}

JsResult<Value> builtinObjectDefineProperty(Isolate& iso, Value thisVal,
                                             const Value* args,
                                             uint32_t argc) {
  (void)thisVal;
  if (argc < 3 || !args[0].isObjectLike()) {
    return std::unexpected(iso.typeError(
        "Object.defineProperty requires an object, a key and a descriptor"));
  }
  if (!args[2].isObjectLike()) {
    return std::unexpected(
        iso.typeError("Property description must be an object"));
  }
  PropertyDescriptor d;
  JsResult<bool> rd = readDescriptor(iso, args[2], &d);
  if (!rd) return std::unexpected(rd.error());
  if (args[0].isProxy()) {
    // Descriptor read HERE (off the proxy), then the trap runs.
    JsResult<bool> defined = iso.proxyDefineOwnProperty(args[0], args[1], d);
    if (!defined) return std::unexpected(defined.error());
    return args[0];
  }
  JsResult<SymbolId> key = iso.toPropertyKey(args[1]);
  if (!key) return std::unexpected(key.error());
  JsResult<bool> defined =
      iso.definePropertyDescriptor(objectOfValue(args[0]), *key, d);
  if (!defined) return std::unexpected(defined.error());
  return args[0];
}

JsResult<Value> builtinObjectIsExtensible(Isolate& iso, Value thisVal,
                                          const Value* args, uint32_t argc) {
  (void)thisVal;
  (void)argc;
  if (argc >= 1 && args[0].isProxy()) {
    JsResult<bool> ext = iso.proxyIsExtensible(args[0]);
    if (!ext) return std::unexpected(ext.error());
    return Value::boolean(*ext);
  }
  if (argc < 1 || !args[0].isObjectLike()) return Value::boolean(true);
  return Value::boolean(objectOfValue(args[0])->extensible);
}

JsResult<Value> builtinObjectPreventExtensions(Isolate& iso, Value thisVal,
                                               const Value* args,
                                               uint32_t argc) {
  (void)thisVal;
  (void)argc;
  if (argc >= 1 && args[0].isProxy()) {
    JsResult<bool> ok = iso.proxyPreventExtensions(args[0]);
    if (!ok) return std::unexpected(ok.error());
    if (!*ok) {
      return std::unexpected(iso.typeError(
          "'preventExtensions' on proxy: trap returned falsish"));
    }
    return args[0];
  }
  if (argc >= 1 && args[0].isObjectLike()) {
    Object* obj = objectOfValue(args[0]);
    if (obj->isArray) {
      // ES: ArraySetIntegrityLevel on arrays redefines length non-writable;
      // v0.3 has no non-writable length, so arrays are rejected (matches
      // observable TypeError from real engines, registered divergence).
      return std::unexpected(iso.typeError(
          "Cannot prevent extensions on an array (length exotic support "
          "pending, bytecode_spec.md Section 11)"));
    }
    obj->extensible = false;
    return args[0];
  }
  return args[0];
}

// ---------------------------------------------------------------------------
// Array namespace natives
// ---------------------------------------------------------------------------
JsResult<Value> builtinArrayIsArray(Isolate& iso, Value thisVal,
                                    const Value* args, uint32_t argc) {
  (void)iso;
  (void)thisVal;
  bool isArray = argc >= 1 && args[0].kind() == ValueKind::Object &&
                 static_cast<Object*>(args[0].asPtr())->isArray;
  return Value::boolean(isArray);
}

// ---------------------------------------------------------------------------
// Symbol namespace natives (v0.3)
// ---------------------------------------------------------------------------
JsResult<Value> builtinSymbolCtor(Isolate& iso, Value thisVal,
                                  const Value* args, uint32_t argc) {
  (void)thisVal;
  std::u16string desc;
  if (argc >= 1 && !args[0].isUndefined()) {
    JsResult<Value> s = iso.toStringValue(args[0]);
    if (!s) return std::unexpected(s.error());
    desc = s->asString()->flat();
  }
  return iso.makeSymbolValue(std::move(desc));
}

JsResult<Value> builtinSymbolFor(Isolate& iso, Value thisVal,
                                 const Value* args, uint32_t argc) {
  (void)thisVal;
  if (argc < 1) return iso.makeSymbolValue(u"");
  JsResult<Value> s = iso.toStringValue(args[0]);
  if (!s) return std::unexpected(s.error());
  std::u16string key = s->asString()->flat();
  if (SymbolObj* existing = iso.symbolForRegistry(key)) {
    return Value::raw(ValueKind::Symbol, existing);
  }
  Value sym = iso.makeSymbolValue(key);
  iso.symbolForRegister(key, sym.asSymbol());
  return sym;
}

JsResult<Value> builtinSymbolKeyFor(Isolate& iso, Value thisVal,
                                    const Value* args, uint32_t argc) {
  (void)thisVal;
  if (argc < 1 || !args[0].isSymbol()) return Value::undefined();
  const std::u16string& desc = args[0].asSymbol()->desc;
  if (iso.symbolForRegistry(desc) == args[0].asSymbol()) {
    return Value::string(iso.heap().makeString(desc));
  }
  return Value::undefined();
}

// ---------------------------------------------------------------------------
// Proxy constructor native (v0.3)
// ---------------------------------------------------------------------------
JsResult<Value> builtinProxyCtor(Isolate& iso, Value thisVal,
                                 const Value* args, uint32_t argc) {
  (void)thisVal;
  if (argc < 2 || !(args[0].isObjectLike() || args[0].isProxy())) {
    return std::unexpected(
        iso.typeError("Cannot create proxy with a non-object target"));
  }
  if (!args[1].isObjectLike() || args[1].isProxy()) {
    return std::unexpected(
        iso.typeError("Cannot create proxy with a non-object handler"));
  }
  return Value::raw(ValueKind::Proxy,
                    iso.heap().makeProxy(args[0], objectOfValue(args[1])));
}

}  // namespace

Object* makeDescriptorObject(Isolate& iso, const PropertyDescriptor& d) {
  Object* desc = iso.newPlainObject();
  SymbolId symValue = iso.symbols().intern(u"value");
  SymbolId symWritable = iso.symbols().intern(u"writable");
  SymbolId symEnumerable = iso.symbols().intern(u"enumerable");
  SymbolId symConfigurable = iso.symbols().intern(u"configurable");
  SymbolId symGet = iso.symbols().intern(u"get");
  SymbolId symSet = iso.symbols().intern(u"set");
  auto put = [&](SymbolId k, Value v) {
    (void)iso.defineProperty(desc, k, v, kDefaultDataAttrs);
  };
  if (d.isAccessor()) {
    put(symGet, d.getter);
    put(symSet, d.setter);
  } else {
    put(symValue, d.value);
    put(symWritable, Value::boolean(d.writable));
  }
  put(symEnumerable, Value::boolean(d.enumerable));
  put(symConfigurable, Value::boolean(d.configurable));
  return desc;
}

// ---------------------------------------------------------------------------
// Prototype installation (called from the Isolate constructor)
// ---------------------------------------------------------------------------
void Isolate::installStandardPrototypes() {
  objectPrototype_ = heap_.makeObject();
  objectPrototype_->proto = Value::null();
  functionPrototype_ = heap_.makeObject();
  functionPrototype_->proto = Value::raw(ValueKind::Object, objectPrototype_);
  arrayPrototype_ = heap_.makeObject();
  arrayPrototype_->proto = Value::raw(ValueKind::Object, objectPrototype_);

  // Object.prototype (Rule 70: monkey-patchable builtins are ordinary
  // data properties — user code may shadow or replace them).
  defineNativeOn(objectPrototype_, "toString", builtinObjectToString);
  defineNativeOn(objectPrototype_, "valueOf", builtinObjectValueOf);
  defineNativeOn(objectPrototype_, "hasOwnProperty", builtinHasOwnProperty);
  defineNativeOn(arrayPrototype_, "push", builtinArrayPush);
  defineNativeOn(arrayPrototype_, "pop", builtinArrayPop);
  defineNativeOn(arrayPrototype_, "shift", builtinArrayShift);
  defineNativeOn(arrayPrototype_, "unshift", builtinArrayUnshift);
  defineNativeOn(arrayPrototype_, "join", builtinArrayJoin);
  defineNativeOn(arrayPrototype_, "indexOf", builtinArrayIndexOf);
  defineNativeOn(arrayPrototype_, "includes", builtinArrayIncludes);
  defineNativeOn(arrayPrototype_, "slice", builtinArraySlice);
  defineNativeOn(arrayPrototype_, "concat", builtinArrayConcat);
  defineNativeOn(arrayPrototype_, "forEach", builtinArrayForEach);
  defineNativeOn(arrayPrototype_, "map", builtinArrayMap);
  defineNativeOn(arrayPrototype_, "filter", builtinArrayFilter);
  defineNativeOn(arrayPrototype_, "reduce", builtinArrayReduce);

  // Global namespaces: Object and Array. v0.3: plain holder objects, not
  // callable (documented divergence — Object()/Array() construction is not
  // representable until the constructor-builtin layer lands).
  Object* objNs = heap_.makeObject();
  objNs->proto = Value::raw(ValueKind::Object, functionPrototype_);
  defineNativeOn(objNs, "keys", builtinObjectKeys);
  defineNativeOn(objNs, "getOwnPropertyNames", builtinObjectGetPropertyNames);
  defineNativeOn(objNs, "getOwnPropertyDescriptor",
                 builtinObjectGetOwnPropertyDescriptor);
  defineNativeOn(objNs, "defineProperty", builtinObjectDefineProperty);
  defineNativeOn(objNs, "isExtensible", builtinObjectIsExtensible);
  defineNativeOn(objNs, "preventExtensions", builtinObjectPreventExtensions);
  (void)defineProperty(objNs, symbols_.intern(u"prototype"),
                       Value::raw(ValueKind::Object, objectPrototype_),
                       kDefaultDataAttrs);
  (void)defineProperty(global_, symbols_.intern(u"Object"),
                       Value::raw(ValueKind::Object, objNs),
                       kDefaultDataAttrs);

  Object* arrNs = heap_.makeObject();
  arrNs->proto = Value::raw(ValueKind::Object, functionPrototype_);
  defineNativeOn(arrNs, "isArray", builtinArrayIsArray);
  (void)defineProperty(arrNs, symbols_.intern(u"prototype"),
                       Value::raw(ValueKind::Object, arrayPrototype_),
                       kDefaultDataAttrs);
  (void)defineProperty(global_, symbols_.intern(u"Array"),
                       Value::raw(ValueKind::Object, arrNs),
                       kDefaultDataAttrs);

  // Symbol: a callable namespace function (Symbol(desc)) with statics
  // .for / .keyFor attached to its function object (Rule 70: well-known
  // symbol machinery; @@iterator etc. arrive with the iterator protocol).
  Closure* symbolFn = defineNativeOn(global_, "Symbol", builtinSymbolCtor);
  defineNativeOn(symbolFn->asObject, "for", builtinSymbolFor);
  defineNativeOn(symbolFn->asObject, "keyFor", builtinSymbolKeyFor);

  // Proxy(target, handler): the 13-trap exotic object (Rule 70).
  defineNativeOn(global_, "Proxy", builtinProxyCtor);
}

}  // namespace ts
