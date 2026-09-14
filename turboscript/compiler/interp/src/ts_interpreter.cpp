// TurboScript — Isolate: heap/symbols/shapes setup, ECMAScript conversions
// with user-code hooks, property operations, call machinery, feedback
// recording, error factories. Fidelity per Rule 67/72 (oracle-verified).
#include "ts_interpreter.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ts {

// Convert UTF-16 to UTF-8 (exported; used by driver output too).
std::string utf16ToUtf8(const std::u16string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t c = static_cast<uint16_t>(s[i]);
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) {
      uint32_t lo = static_cast<uint16_t>(s[i + 1]);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        i++;
      }
    }
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (c >> 12)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (c >> 18)));
      out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

// BigInt vs non-integral double: x<y iff compare(x, floor(y)) <= 0.
[[nodiscard]] Relational bigintVsDouble(const BigInt& big, double d) {
  if (std::isnan(d)) return Relational::Unordered;
  if (std::isinf(d)) return d > 0 ? Relational::Less : Relational::Greater;
  if (pure::doubleIsIntegral(d)) {
    BigInt other;
    (void)BigInt::fromDouble(d, &other);  // infallible: d integral (checked)
    int c = BigInt::compare(big, other);
    if (c < 0) return Relational::Less;
    if (c > 0) return Relational::Greater;
    return Relational::Equal;
  }
  double fl = std::floor(d);
  BigInt floorBig;
  (void)BigInt::fromDouble(fl, &floorBig);  // infallible: fl integral
  int c = BigInt::compare(big, floorBig);
  // y is non-integral: x < y iff x <= floor(y).
  return c <= 0 ? Relational::Less : Relational::Greater;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

Isolate::Isolate() : shapes_(heap_) {
  global_ = heap_.makeObject();
  sym_prototype_ = symbols_.intern(u"prototype");
  sym_constructor_ = symbols_.intern(u"constructor");
  sym_name_ = symbols_.intern(u"name");
  sym_message_ = symbols_.intern(u"message");
  sym_valueOf_ = symbols_.intern(u"valueOf");
  sym_toString_ = symbols_.intern(u"toString");
  sym_length_ = symbols_.intern(u"length");
  // v0.3 builtin layer: standard prototypes + Object/Array namespaces
  // (ts_builtins.cpp). Runs before any module load; driver natives
  // (print) register on top afterwards.
  installStandardPrototypes();
  global_->proto = Value::raw(ValueKind::Object, objectPrototype_);
  opcodeCounts_.assign(kOpcodeSpace, 0);
}

void Isolate::registerNative(const char* name, NativeFn fn) {
  // v0.3: delegates to the shared definition path (the closure object gets
  // a .name property and Function.prototype as its [[Prototype]]).
  defineNativeOn(global_, name, fn);
}

TsResult<bool> Isolate::loadModule(const Module& module,
                                   const SymbolTable& sourceSymbols) {
  module_ = &module;
  // Resolve symbol ids WITHOUT mutating the module (load is idempotent):
  // the assembler interned names against its own table; re-intern the same
  // texts here so runtime comparisons use canonical ids (Rule 16).
  fnNames_.assign(module.functions.size(), kInvalidSymbol);
  for (const auto& fn : module.functions) {
    fnNames_[fn->index] = symbols_.intern(sourceSymbols.text(fn->name));
  }
  globalNames_.clear();
  globalNames_.reserve(module.globalNames.size());
  for (SymbolId id : module.globalNames) {
    globalNames_.push_back(symbols_.intern(sourceSymbols.text(id)));
  }
  constants_.clear();
  constants_.reserve(module.constants.size());
  for (const Constant& c : module.constants) {
    switch (c.kind) {
      case ValueKind::Smi:
        constants_.push_back(Value::smi(c.smi));
        break;
      case ValueKind::HeapNumber:
        constants_.push_back(normalizeNumber(c.num));
        break;
      case ValueKind::String:
        constants_.push_back(
            Value::string(heap_.makeString(c.text)));
        break;
      case ValueKind::BigInt:
        constants_.push_back(
            Value::raw(ValueKind::BigInt, heap_.makeBigInt(c.bigintValue)));
        break;
      default:
        constants_.push_back(Value::undefined());
        break;
    }
  }
  feedback_.assign(module.functions.size(), {});
  for (const auto& fn : module.functions) {
    feedback_[fn->index].resize(fn->feedbackLayout.size());
    // Slot kinds come from the verifier-computed layout (Section 8); without
    // this, --dump-feedback mislabels every slot and prints the wrong
    // per-kind counters (fixed in v0.2; Rule 150).
    for (size_t i = 0; i < fn->feedbackLayout.size(); i++) {
      feedback_[fn->index][i].kind = fn->feedbackLayout[i];
    }
  }
  return true;
}

JsResult<Value> Isolate::run() {
  if (module_ == nullptr) {
    return std::unexpected(typeError("internal: no module loaded"));
  }
  if (module_->entryIndex >= module_->functions.size()) {
    return std::unexpected(typeError("internal: entry function missing"));
  }
  Closure* entry = makeClosureFor(module_->entryIndex, nullptr);
  if (entry == nullptr) {
    return std::unexpected(typeError("internal: cannot create entry closure"));
  }
  return callClosure(entry, Value::undefined(), nullptr, 0);
}

Closure* Isolate::defineNativeOn(Object* holder, const char* name,
                                 NativeFn fn) {
  natives_.push_back(NativeEntry{name, fn});
  SymbolId sym = symbols_.intern(
      std::u16string(name, name + std::strlen(name)));
  Closure* cl = heap_.makeClosure();
  cl->funcIndex = kNativeFuncIndexBase + static_cast<uint32_t>(natives_.size()) - 1;
  cl->context = nullptr;
  cl->asObject = heap_.makeObject();
  cl->asObject->proto = Value::raw(ValueKind::Object, functionPrototype_);
  std::u16string wide(name, name + std::strlen(name));
  (void)defineProperty(cl->asObject, sym_name_,
                       Value::string(heap_.makeString(wide)),
                       kDefaultDataAttrs);
  (void)defineProperty(holder, sym, Value::raw(ValueKind::Closure, cl),
                       kDefaultDataAttrs);
  return cl;
}

Object* Isolate::newPlainObject() {
  Object* obj = heap_.makeObject();
  obj->proto = Value::raw(ValueKind::Object, objectPrototype_);
  return obj;
}

Closure* Isolate::makeClosureFor(uint32_t funcIndex, Context* context) {
  if (module_ == nullptr || funcIndex >= module_->functions.size()) {
    return nullptr;
  }
  Closure* cl = heap_.makeClosure();
  cl->funcIndex = funcIndex;
  cl->context = context;
  cl->asObject = newPlainObject();
  // v0.3: function objects chain to Function.prototype; each closure's
  // fresh .prototype instance chains to Object.prototype (ECMAScript
  // semantics); prototype.constructor points back at the closure.
  cl->asObject->proto = Value::raw(ValueKind::Object, functionPrototype_);
  Object* protoObj = newPlainObject();
  Value closureVal = Value::raw(ValueKind::Closure, cl);
  (void)defineProperty(protoObj, sym_constructor_, closureVal,
                       kDefaultDataAttrs);
  Value protoVal = Value::raw(ValueKind::Object, protoObj);
  (void)defineProperty(cl->asObject, sym_prototype_, protoVal,
                       kDefaultDataAttrs);
  return cl;
}

// ---------------------------------------------------------------------------
// Call machinery
// ---------------------------------------------------------------------------
JsResult<Value> Isolate::callValue(const Value& callee, Value thisVal,
                                   const Value* args, uint32_t argc) {
  if (callee.isProxy()) {
    // Proxy [[Call]] (v0.3).
    return proxyApply(callee, thisVal, args, argc);
  }
  if (callee.kind == ValueKind::Closure) {
    const Closure* cl = static_cast<const Closure*>(callee.ptr);
    if (cl->funcIndex >= kNativeFuncIndexBase) {
      uint32_t idx = cl->funcIndex - kNativeFuncIndexBase;
      if (idx < natives_.size()) {
        return natives_[idx].fn(*this, thisVal, args, argc);
      }
      return std::unexpected(typeError("internal: native not registered"));
    }
    return callClosure(cl, thisVal, args, argc);
  }
  return std::unexpected(typeError(
      pure::kindName(callee) + " is not a function"));
}

JsResult<Value> Isolate::callClosure(const Closure* closure, Value thisVal,
                                     const Value* args, uint32_t argc) {
  (void)thisVal;  // strict-mode v0.1: `this` reachable via CallMethod only.
  if (module_ == nullptr ||
      closure->funcIndex >= module_->functions.size()) {
    return std::unexpected(typeError("internal: function index out of range"));
  }
  if (callDepth_ >= kMaxCallDepth) {
    return std::unexpected(
        rangeError("Maximum call stack size exceeded"));  // Rule 90
  }
  const Function* fn = module_->functions[closure->funcIndex].get();
  Frame frame;
  frame.fn = fn;
  frame.context = closure->context;
  frame.regs.assign(fn->registerCount, Value::undefined());
  uint32_t bound = argc < fn->paramCount ? argc : fn->paramCount;
  for (uint32_t i = 0; i < bound; i++) frame.regs[i] = args[i];

  frameStack_.push_back(&frame);
  callDepth_++;
  JsResult<Value> result = runFrame(frame);
  callDepth_--;
  frameStack_.pop_back();
  return result;
}

std::vector<std::string> Isolate::currentTrace() const {
  std::vector<std::string> lines;
  for (const Frame* f : frameStack_) {
    SymbolId nameSym = f->fn->index < fnNames_.size()
                           ? fnNames_[f->fn->index]
                           : kInvalidSymbol;
    lines.push_back("    at " +
                    (nameSym != kInvalidSymbol && nameSym < symbols_.size()
                         ? utf16ToUtf8(symbols_.text(nameSym))
                         : "<anon>") +
                    " (pc " + std::to_string(f->pc) + ")");
  }
  return lines;
}

JsException Isolate::makeError(const char* name, const std::string& message) {
  Object* err = newPlainObject();  // v0.3: chains to Object.prototype
  std::u16string wideName(name, name + std::strlen(name));
  std::u16string wideMsg(message.begin(), message.end());
  (void)defineProperty(err, sym_name_,
                       Value::string(heap_.makeString(wideName)),
                       kDefaultDataAttrs);
  (void)defineProperty(err, sym_message_,
                       Value::string(heap_.makeString(wideMsg)),
                       kDefaultDataAttrs);
  stackTraces_[err] = currentTrace();  // Rule 75 diagnostics
  return JsException{Value::raw(ValueKind::Object, err)};
}

JsException Isolate::typeError(const std::string& m) {
  return makeError("TypeError", m);
}
JsException Isolate::rangeError(const std::string& m) {
  return makeError("RangeError", m);
}
JsException Isolate::referenceError(const std::string& m) {
  return makeError("ReferenceError", m);
}
JsException Isolate::syntaxError(const std::string& m) {
  return makeError("SyntaxError", m);
}

// ---------------------------------------------------------------------------
// Property operations. v0.2: array-aware. Exotic behavior (elements, length)
// is consulted at EVERY node of the prototype chain (an array can inherit
// from an object and vice versa), while named properties use the ordinary
// shape machinery. One semantic source feeds GetProperty/GetProperty-keyed
// opcodes alike (Rule 96).
// ---------------------------------------------------------------------------
Object* Isolate::protoOfObject(Object* obj) const {
  return obj->proto.isObjectLike() ? objectOfValue(obj->proto) : nullptr;
}

bool Isolate::arrayHasOwnElement(const Object* arr, uint32_t idx) const {
  if (idx < arr->elements.size()) return !arr->elements[idx].isHole();
  return arr->sparse.find(idx) != arr->sparse.end();
}

Value Isolate::ownElementValue(const Object* arr, uint32_t idx) const {
  if (idx < arr->elements.size()) {
    Value v = arr->elements[idx];
    if (!v.isHole()) return v;
    return Value::hole();  // dense hole: present-in-range but empty
  }
  auto it = arr->sparse.find(idx);
  if (it != arr->sparse.end()) return it->second;
  return Value::hole();
}

void Isolate::widenElementsFor(Object* arr, const Value& val) const {
  const ElementsKind k = arr->elementsKind;
  ElementsKind target = k;
  switch (k) {
    case ElementsKind::PackedSmi:
    case ElementsKind::HoleySmi:
      if (!val.isSmi()) {
        target = val.isNumber() ? static_cast<ElementsKind>(static_cast<uint8_t>(k) + 2)
                                : static_cast<ElementsKind>(static_cast<uint8_t>(k) + 4);
      }
      break;
    case ElementsKind::PackedDouble:
    case ElementsKind::HoleyDouble:
      if (!val.isNumber()) {
        target = static_cast<ElementsKind>(static_cast<uint8_t>(k) + 2);
      }
      break;
    case ElementsKind::PackedTagged:
    case ElementsKind::HoleyTagged:
      break;
  }
  arr->elementsKind = target;
}

Value Isolate::arrayLengthValue(const Object* arr) const {
  if (arr->length <= static_cast<uint32_t>(kSmiMax)) {
    return Value::smi(static_cast<int32_t>(arr->length));
  }
  return Value::heapNumber(static_cast<double>(arr->length));
}

SymbolId Isolate::internIndexKey(uint32_t idx) {
  char buf[16];
  int n = std::snprintf(buf, sizeof buf, "%u", idx);
  std::u16string text(buf, buf + n);
  return symbols_.intern(text);
}

JsResult<bool> Isolate::arraySetLength(Object* arr, const Value& newLen) {
  // ArraySetLength (ECMA-262): ToUint32(newLen) must equal ToNumber(newLen).
  JsResult<Value> numR = toNumberValue(newLen);  // may run user hooks
  if (!numR) return std::unexpected(numR.error());
  double lenNum = numR->asDouble();
  uint32_t uintLen = pure::toUint32(lenNum);
  if (static_cast<double>(uintLen) != lenNum) {
    // Strict-mode receiver semantics (v0.1+): throw, never return false.
    return std::unexpected(rangeError(
        "Invalid array length (expected: ToUint32(length) == ToNumber(length), "
        "got " + pure::doubleToString(lenNum) + ")"));
  }
  if (uintLen >= arr->length) {
    arr->length = uintLen;  // growth: holes are implied, no storage change
    return true;
  }
  // Shrink: hole-ify dense slots in [newLen, oldDenseEnd), drop sparse >=.
  bool madeHole = false;
  for (size_t i = arr->elements.size(); i-- > uintLen;) {
    if (!arr->elements[i].isHole()) {
      arr->elements[i] = Value::hole();
      madeHole = true;
    }
  }
  for (auto it = arr->sparse.begin(); it != arr->sparse.end();) {
    if (it->first >= uintLen) {
      it = arr->sparse.erase(it);
    } else {
      ++it;
    }
  }
  if (madeHole) {
    arr->elementsKind = holeyOf(arr->elementsKind);
  }
  arr->length = uintLen;
  return true;
}

JsResult<bool> Isolate::setArrayElement(Object* arr, uint32_t idx,
                                        const Value& val) {
  // Array length is writable in v0.2 (no freeze/ seal opcodes exist yet),
  // so every element store succeeds and then updates length (ECMA-262:
  // "if idx >= length, set length to idx+1" — uint32-safe: idx <= 2^32-2).
  if (idx < arr->elements.size()) {
    widenElementsFor(arr, val);
    arr->elements[idx] = val;
    if (arr->length <= idx) arr->length = idx + 1;
    return true;
  }
  if (idx < kMaxDenseElements) {
    size_t oldSize = arr->elements.size();
    if (idx >= oldSize) {
      arr->elements.resize(static_cast<size_t>(idx) + 1, Value::hole());
      arr->elementsKind = holeyOf(arr->elementsKind);  // gap slots are holes
    }
    widenElementsFor(arr, val);
    arr->elements[idx] = val;
    if (arr->length <= idx) arr->length = idx + 1;
    return true;
  }
  // Sparse territory: no dense allocation (a store at 2^32-2 must not
  // reserve 4G slots); values are tagged here.
  arr->sparse[idx] = val;
  arr->elementsKind = ElementsKind::HoleyTagged;
  if (arr->length <= idx) arr->length = idx + 1;
  return true;
}

JsResult<Value> Isolate::getAlongChain(Object* start, const Value& recv,
                                       SymbolId key, bool isIndex,
                                       uint32_t idx) {
  for (Object* cur = start; cur != nullptr; cur = protoOfObject(cur)) {
    if (cur->isArray && isIndex) {
      Value v = ownElementValue(cur, idx);
      if (!v.isHole()) return v;
      continue;  // hole/absent on this array node: keep walking
    }
    if (cur->isArray && key == sym_length_) {
      return arrayLengthValue(cur);
    }
    LookupResult own = lookupOwnProperty(cur, key);
    if (!own.found) continue;
    if (own.accessor != nullptr) {
      if (own.accessor->getter.isUndefined()) return Value::undefined();
      return callValue(own.accessor->getter, recv, nullptr, 0);
    }
    return *own.dataSlot;
  }
  return Value::undefined();
}

JsResult<bool> Isolate::setAlongChain(Object* start, const Value& recv,
                                      SymbolId key, const Value& val) {
  for (Object* cur = start; cur != nullptr; cur = protoOfObject(cur)) {
    LookupResult own = lookupOwnProperty(cur, key);
    if (!own.found) continue;
    if (own.accessor != nullptr) {
      if (own.accessor->setter.isUndefined()) {
        return std::unexpected(typeError(
            "Cannot set property '" + utf16ToUtf8(symbols_.text(key)) +
            "': no setter"));
      }
      // Setter invoked with the ORIGINAL receiver as `this`.
      JsResult<Value> r = callValue(own.accessor->setter, recv, &val, 1);
      if (!r) return std::unexpected(r.error());
      return true;
    }
    // Data property found on the chain: v0.3 honors the writable attribute
    // (descriptor-defined read-only data props are real now); default attrs
    // are all writable, so ordinary stores are unaffected.
    if (cur == start) {
      PropertyAttrs attrs;
      if (ownDataSlotAttrs(cur, key, &attrs) >= 0 &&
          !attrs.has(PropAttr::Writable)) {
        return std::unexpected(typeError(
            "Cannot assign to read only property '" +
            utf16ToUtf8(symbols_.text(key)) + "'"));
      }
      *own.dataSlot = val;
    } else {
      return defineProperty(start, key, val, kDefaultDataAttrs);
    }
    return true;
  }
  // Nothing on the chain: define a fresh own property. Arrays route exotic
  // keys through the element/length machinery before ever reaching here.
  return defineProperty(start, key, val, kDefaultDataAttrs);
}

bool Isolate::hasAlongChain(Object* start, SymbolId key, bool isIndex,
                            uint32_t idx) {
  for (Object* cur = start; cur != nullptr; cur = protoOfObject(cur)) {
    if (cur->isArray) {
      if (isIndex && arrayHasOwnElement(cur, idx)) return true;
      if (key == sym_length_) return true;  // own exotic length
    }
    if (lookupOwnProperty(cur, key).found) return true;
  }
  return false;
}

JsResult<bool> Isolate::defineProperty(Object* obj, SymbolId key,
                                       const Value& val,
                                       PropertyAttrs attrs) {
  // Defensive routing: arrays never define "length" or index keys as plain
  // named properties (they are exotic). Ordinary receivers are unaffected.
  if (obj->isArray) {
    uint32_t idx = 0;
    if (key == sym_length_) return arraySetLength(obj, val);
    if (arrayIndexFromKey(symbols_.text(key), &idx)) {
      return setArrayElement(obj, idx, val);
    }
  }
  // Fresh objects carry shape == nullptr; the root shape is the implicit
  // origin of every transition chain (interp_contract.md 3).
  Shape* from = obj->shape != nullptr ? obj->shape : shapes_.root();
  obj->shape = shapes_.transition(from, key, attrs);
  obj->slots.push_back(val);
  return true;
}

JsResult<Value> Isolate::getProperty(const Value& recv, SymbolId key) {
  if (recv.isProxy()) {
    // Proxy [[Get]]: route through the trap protocol with the key VALUE.
    return proxyGet(recv, keyToValue(key));
  }
  if (!recv.isObjectLike()) {
    // v0.2: no primitive wrappers (bytecode_spec.md Section 11).
    return Value::undefined();
  }
  Object* obj = objectOfValue(recv);
  uint32_t idx = 0;
  const bool isIndex =
      !isUserSymbolId(key) && arrayIndexFromKey(symbols_.text(key), &idx);
  return getAlongChain(obj, recv, key, isIndex, idx);
}

JsResult<bool> Isolate::setProperty(const Value& recv, SymbolId key,
                                    const Value& val) {
  if (recv.isProxy()) {
    return proxySet(recv, keyToValue(key), val);
  }
  if (!recv.isObjectLike()) {
    // Strict-mode store on a primitive receiver.
    return std::unexpected(typeError(
        "Cannot create property '" + utf16ToUtf8(keyText(key)) +
        "' on " + pure::kindName(recv)));
  }
  Object* obj = objectOfValue(recv);
  // Exotic array keys are handled BEFORE any chain walk (the array's own
  // length/elements are not shape slots).
  if (obj->isArray) {
    if (key == sym_length_) return arraySetLength(obj, val);
    uint32_t idx = 0;
    if (!isUserSymbolId(key) && arrayIndexFromKey(symbols_.text(key), &idx)) {
      return setArrayElement(obj, idx, val);
    }
  }
  return setAlongChain(obj, recv, key, val);
}

// ---------------------------------------------------------------------------
// Element access keyed by VALUE (v0.3, benchmarks_v0.2.md Section 5 #1).
// The v0.2 GetElement/SetElement path ran toPropertyKey on EVERY access,
// materializing + interning a fresh decimal string per element read (900k
// allocations for array_loop) and then re-parsing that text back into an
// index. The fast path below performs the identical semantic walk for
// canonical Smi keys with ZERO interning on element hits: per chain node it
// consults element storage first (array nodes) and named slots second, with
// the named key's symbol resolved lazily and cached (cachedIndexSymbol).
// Rule 96: one semantic source — the slow path (below) is unchanged and any
// key/receiver combination not matching the fast-path guard routes to it.
// ---------------------------------------------------------------------------
SymbolId Isolate::cachedIndexSymbol(uint32_t idx) {
  if (idx < kIndexSymbolCacheMax) {
    if (idx >= indexSymbolCache_.size()) {
      indexSymbolCache_.resize(static_cast<size_t>(idx) + 1, kInvalidSymbol);
    }
    SymbolId& cached = indexSymbolCache_[idx];
    if (cached == kInvalidSymbol) cached = internIndexKey(idx);
    return cached;
  }
  return internIndexKey(idx);
}

JsResult<Value> Isolate::getElementValue(const Value& recv, const Value& key) {
  if (key.isSmi() && key.i32 >= 0 && recv.isObjectLike()) {
    const uint32_t idx = static_cast<uint32_t>(key.i32);
    SymbolId keySym = kInvalidSymbol;  // resolved lazily, only on named probe
    for (Object* cur = objectOfValue(recv); cur != nullptr;
         cur = protoOfObject(cur)) {
      if (cur->isArray) {
        Value v = ownElementValue(cur, idx);
        if (!v.isHole()) return v;
        // Numeric keys never reach the exotic "length" property.
      }
      if (keySym == kInvalidSymbol) keySym = cachedIndexSymbol(idx);
      LookupResult own = lookupOwnProperty(cur, keySym);
      if (!own.found) continue;
      if (own.accessor != nullptr) {
        if (own.accessor->getter.isUndefined()) return Value::undefined();
        return callValue(own.accessor->getter, recv, nullptr, 0);
      }
      return *own.dataSlot;
    }
    return Value::undefined();
  }
  // Slow path: full ToPropertyKey routing (objects with hooks, doubles,
  // strings, BigInt keys, ...). Identical to the v0.2 semantics.
  JsResult<SymbolId> k = toPropertyKey(key);
  if (!k) return std::unexpected(k.error());
  return getProperty(recv, *k);
}

JsResult<bool> Isolate::setElementValue(const Value& recv, const Value& key,
                                        const Value& val) {
  if (key.isSmi() && key.i32 >= 0 && recv.isObjectLike()) {
    const uint32_t idx = static_cast<uint32_t>(key.i32);
    Object* obj = objectOfValue(recv);
    // Own array receiver: mirrors setProperty's array pre-routing — the
    // element/length machinery runs before any chain walk.
    if (obj->isArray) return setArrayElement(obj, idx, val);
    // Non-array receiver: identical walk to setAlongChain with a lazily
    // interned numeric key (a numeric-key accessor on the chain must run).
    SymbolId keySym = kInvalidSymbol;
    for (Object* cur = obj; cur != nullptr; cur = protoOfObject(cur)) {
      if (keySym == kInvalidSymbol) keySym = cachedIndexSymbol(idx);
      LookupResult own = lookupOwnProperty(cur, keySym);
      if (!own.found) continue;
      if (own.accessor != nullptr) {
        if (own.accessor->setter.isUndefined()) {
          return std::unexpected(typeError(
              "Cannot set property '" + utf16ToUtf8(symbols_.text(keySym)) +
              "': no setter"));
        }
        JsResult<Value> r = callValue(own.accessor->setter, recv, &val, 1);
        if (!r) return std::unexpected(r.error());
        return true;
      }
      if (cur == obj) {
        *own.dataSlot = val;
      } else {
        return defineProperty(obj, keySym, val, kDefaultDataAttrs);
      }
      return true;
    }
    if (keySym == kInvalidSymbol) keySym = cachedIndexSymbol(idx);
    return defineProperty(obj, keySym, val, kDefaultDataAttrs);
  }
  JsResult<SymbolId> k = toPropertyKey(key);
  if (!k) return std::unexpected(k.error());
  return setProperty(recv, *k, val);
}

JsResult<bool> Isolate::deletePropertyImpl(const Value& recv, SymbolId key) {
  if (recv.isProxy()) {
    return proxyDelete(recv, keyToValue(key));
  }
  if (!recv.isObjectLike()) return true;  // ToObject(prim) has no such own.
  Object* obj = objectOfValue(recv);
  // Array exotic behavior: "length" is non-configurable (delete -> false);
  // element deletion hole-ifies the slot (packed -> holey) and never
  // changes length.
  if (obj->isArray) {
    if (key == sym_length_) return false;
    uint32_t idx = 0;
    if (!isUserSymbolId(key) && arrayIndexFromKey(symbols_.text(key), &idx)) {
      if (idx < obj->elements.size()) {
        if (obj->elements[idx].isHole()) return true;
        obj->elements[idx] = Value::hole();
        obj->elementsKind = holeyOf(obj->elementsKind);
        return true;
      }
      obj->sparse.erase(idx);
      return true;
    }
  }
  int32_t slot = obj->findOwnSlot(key);
  if (slot < 0) return true;
  Value& v = obj->slots[static_cast<size_t>(slot)];
  if (v.isHole()) return true;
  // Invariant: v0.2 property attrs are always configurable
  // (kDefaultDataAttrs) except the array "length" intercepted above, so the
  // delete always succeeds. Frozen/sealed semantics arrive with
  // define-property descriptor opcodes.
  v = Value::hole();
  return true;
}

JsResult<bool> Isolate::hasPropertyImpl(const Value& recv, SymbolId key) {
  if (recv.isProxy()) {
    return proxyHas(recv, keyToValue(key));
  }
  if (!recv.isObjectLike()) return false;
  Object* obj = objectOfValue(recv);
  uint32_t idx = 0;
  const bool isIndex =
      !isUserSymbolId(key) && arrayIndexFromKey(symbols_.text(key), &idx);
  return hasAlongChain(obj, key, isIndex, idx);
}

// ---------------------------------------------------------------------------
// Conversions with hooks (oracle-verified semantics)
// ---------------------------------------------------------------------------
JsResult<Value> Isolate::toPrimitive(const Value& v, bool hintString) {
  if (!v.isObjectLike() && !v.isProxy()) return v;  // primitives: identity.
  if (v.isProxy()) {
    // v0.3: @@toPrimitive is deferred (no well-known symbols yet); the
    // toString/valueOf lookups run THROUGH the proxy's get trap.
    SymbolId first = hintString ? sym_toString_ : sym_valueOf_;
    SymbolId second = hintString ? sym_valueOf_ : sym_toString_;
    for (SymbolId method : {first, second}) {
      JsResult<Value> m = proxyGet(v, Value::string(
          heap_.makeString(symbols_.text(method))));
      if (!m) return std::unexpected(m.error());
      if (!m->isClosure()) continue;
      JsResult<Value> r = callValue(*m, v, nullptr, 0);
      if (!r) return std::unexpected(r.error());
      if (!r->isObjectLike() && !r->isProxy()) return *r;
    }
    return std::unexpected(typeError(
        "Cannot convert object to primitive value"));
  }
  // Well-known @@toPrimitive is deferred (no user Symbols in v0.1).
  SymbolId first = hintString ? sym_toString_ : sym_valueOf_;
  SymbolId second = hintString ? sym_valueOf_ : sym_toString_;
  for (SymbolId method : {first, second}) {
    JsResult<Value> m = getProperty(v, method);
    if (!m) return std::unexpected(m.error());
    if (!m->isClosure()) continue;
    JsResult<Value> r = callValue(*m, v, nullptr, 0);
    if (!r) return std::unexpected(r.error());
    if (!r->isObjectLike()) return *r;
  }
  return std::unexpected(typeError(
      "Cannot convert object to primitive value"));
}

JsResult<Value> Isolate::toNumberValue(const Value& v) {
  switch (v.kind) {
    case ValueKind::Undefined:
      return Value::heapNumber(std::numeric_limits<double>::quiet_NaN());
    case ValueKind::Null:
      return Value::smi(0);
    case ValueKind::Boolean:
      return Value::smi(v.b ? 1 : 0);
    case ValueKind::Smi:
    case ValueKind::HeapNumber:
      return v;
    case ValueKind::String:
      return normalizeNumber(pure::stringToNumber(v.str->data));
    case ValueKind::BigInt:
      return std::unexpected(
          typeError("Cannot convert a BigInt value to a number"));
    default: {
      JsResult<Value> prim = toPrimitive(v, false);
      if (!prim) return std::unexpected(prim.error());
      if (prim->kind == ValueKind::String) {
        return normalizeNumber(pure::stringToNumber(prim->str->data));
      }
      return toNumberValue(*prim);
    }
  }
}

JsResult<Value> Isolate::toNumericValue(const Value& v) {
  if (v.isBigInt()) return v;
  return toNumberValue(v);
}

JsResult<Value> Isolate::toStringValue(const Value& v) {
  switch (v.kind) {
    case ValueKind::Undefined:
      return Value::string(heap_.makeString(u"undefined"));
    case ValueKind::Null:
      return Value::string(heap_.makeString(u"null"));
    case ValueKind::Boolean:
      return Value::string(heap_.makeString(v.b ? u"true" : u"false"));
    case ValueKind::Smi:
    case ValueKind::HeapNumber: {
      std::string ascii = pure::doubleToString(v.asDouble());
      return Value::string(
          heap_.makeString(std::u16string(ascii.begin(), ascii.end())));
    }
    case ValueKind::String:
      return v;
    case ValueKind::BigInt: {
      std::u16string s = v.asBigInt()->toString();
      return Value::string(heap_.makeString(s));
    }
    case ValueKind::Symbol: {
      // ToString(Symbol) = "Symbol(desc)" (desc may be empty).
      std::u16string s = u"Symbol(";
      s += v.asSymbol()->desc;
      s += u")";
      return Value::string(heap_.makeString(std::move(s)));
    }
    default: {
      // ToString(Object) = ToString(ToPrimitive(value, hint string)).
      JsResult<Value> prim = toPrimitive(v, true);
      if (!prim) return std::unexpected(prim.error());
      if (prim->isObjectLike()) {
        // ToPrimitive throws on failure, so a non-primitive here would be
        // an internal invariant break.
        return std::unexpected(
            typeError("Cannot convert object to primitive value"));
      }
      return toStringValue(*prim);
    }
  }
}

JsResult<Value> Isolate::toBigIntValue(const Value& v) {
  switch (v.kind) {
    case ValueKind::BigInt:
      return v;
    case ValueKind::Boolean:
      return Value::raw(ValueKind::BigInt,
                        heap_.makeBigInt(BigInt::fromInt64(v.b ? 1 : 0)));
    case ValueKind::Smi:
    case ValueKind::HeapNumber: {
      double d = v.asDouble();
      BigInt big;
      if (!BigInt::fromDouble(d, &big)) {
        return std::unexpected(rangeError(
            "The number " + pure::doubleToString(d) +
            " cannot be converted to a BigInt because it is not an integer"));
      }
      return Value::raw(ValueKind::BigInt, heap_.makeBigInt(std::move(big)));
    }
    case ValueKind::String: {
      BigInt big;
      if (!BigInt::fromString(v.str->data, &big)) {
        return std::unexpected(
            syntaxError("Cannot convert '" +
                        utf16ToUtf8(v.str->data) + "' to a BigInt"));
      }
      return Value::raw(ValueKind::BigInt, heap_.makeBigInt(std::move(big)));
    }
    case ValueKind::Undefined:
    case ValueKind::Null:
      return std::unexpected(typeError(
          "Cannot convert undefined or null to a BigInt"));
    default: {
      JsResult<Value> prim = toPrimitive(v, false);
      if (!prim) return std::unexpected(prim.error());
      return toBigIntValue(*prim);
    }
  }
}

JsResult<SymbolId> Isolate::toPropertyKey(const Value& v) {
  // Oracle-verified: property keys use ToPrimitive with hint string.
  if (v.isString()) {
    // v0.3: const-pool strings carry their interned id after first use;
    // repeated string-keyed access skips the intern hash entirely.
    if (v.str->cachedSymbol != kInvalidSymbol) return v.str->cachedSymbol;
    SymbolId s = symbols_.intern(v.str->data);
    v.str->cachedSymbol = s;
    return s;
  }
  if (v.isSymbol()) {
    // v0.3: user symbols key properties directly (unique id range).
    return v.symbolKeyId();
  }
  if (v.isObjectLike()) {
    JsResult<Value> prim = toPrimitive(v, true);
    if (!prim) return std::unexpected(prim.error());
    return toPropertyKey(*prim);
  }
  if (v.isSmi() || v.isHeapNumber()) {
    JsResult<Value> s = toStringValue(v);
    if (!s) return std::unexpected(s.error());
    return symbols_.intern(s->str->data);
  }
  if (v.isBigInt()) {
    JsResult<Value> s = toStringValue(v);
    if (!s) return std::unexpected(s.error());
    return symbols_.intern(s->str->data);
  }
  // Boolean / undefined / null keys are their ToString forms.
  JsResult<Value> s = toStringValue(v);
  if (!s) return std::unexpected(s.error());
  return symbols_.intern(s->str->data);
}

// ---------------------------------------------------------------------------
// Arithmetic / comparison (oracle-verified edge cases)
// ---------------------------------------------------------------------------
JsResult<Value> Isolate::addValues(const Value& l, const Value& r,
                                   FeedbackSlot* fs) {
  recordBinarySite(fs, l, r);
  // BigInt + BigInt only when both are BigInt (mixing -> TypeError, Rule 72).
  if (l.isBigInt() && r.isBigInt()) {
    BigInt sum = BigInt::add(*l.asBigInt(), *r.asBigInt());
    return Value::raw(ValueKind::BigInt, heap_.makeBigInt(std::move(sum)));
  }
  if (l.isBigInt() || r.isBigInt()) {
    return std::unexpected(typeError(
        "Cannot mix BigInt and other types, use explicit conversions"));
  }
  JsResult<Value> lp = toPrimitive(l, false);  // hint: default
  if (!lp) return std::unexpected(lp.error());
  JsResult<Value> rp = toPrimitive(r, false);
  if (!rp) return std::unexpected(rp.error());
  if (lp->isString() || rp->isString()) {
    JsResult<Value> ls = toStringValue(*lp);
    if (!ls) return std::unexpected(ls.error());
    JsResult<Value> rs = toStringValue(*rp);
    if (!rs) return std::unexpected(rs.error());
    std::u16string out = ls->str->data + rs->str->data;
    return Value::string(heap_.makeString(std::move(out)));
  }
  JsResult<Value> ln = toNumericValue(*lp);
  if (!ln) return std::unexpected(ln.error());
  JsResult<Value> rn = toNumericValue(*rp);
  if (!rn) return std::unexpected(rn.error());
  if (ln->isBigInt() != rn->isBigInt()) {
    return std::unexpected(typeError(
        "Cannot mix BigInt and other types, use explicit conversions"));
  }
  if (ln->isBigInt()) {
    BigInt sum = BigInt::add(*ln->asBigInt(), *rn->asBigInt());
    return Value::raw(ValueKind::BigInt, heap_.makeBigInt(std::move(sum)));
  }
  return normalizeNumber(ln->asDouble() + rn->asDouble());
}

JsResult<Value> Isolate::arithValues(const Value& l, const Value& r,
                                     Opcode op) {
  if (l.isBigInt() || r.isBigInt()) {
    return bigintArith(l, r, op);
  }
  JsResult<Value> lp = toPrimitive(l, false);
  if (!lp) return std::unexpected(lp.error());
  JsResult<Value> rp = toPrimitive(r, false);
  if (!rp) return std::unexpected(rp.error());
  JsResult<Value> ln = toNumberValue(*lp);
  if (!ln) return std::unexpected(ln.error());
  JsResult<Value> rn = toNumberValue(*rp);
  if (!rn) return std::unexpected(rn.error());
  double x = ln->asDouble();
  double y = rn->asDouble();
  double out = 0.0;
  switch (op) {
    case Opcode::kSub: out = x - y; break;
    case Opcode::kMul: out = x * y; break;
    case Opcode::kDiv: out = x / y; break;
    case Opcode::kMod: out = std::fmod(x, y); break;  // sign follows dividend
    case Opcode::kPow:
      // Oracle-verified: 1**NaN = NaN, (±1)**Inf = NaN, NaN**0 = 1.
      if (std::isnan(y)) {
        out = std::numeric_limits<double>::quiet_NaN();
      } else if ((x == 1.0 || x == -1.0) && std::isinf(y)) {
        out = std::numeric_limits<double>::quiet_NaN();
      } else {
        out = std::pow(x, y);
      }
      break;
    default:
      return std::unexpected(typeError("internal: bad arith opcode"));
  }
  return normalizeNumber(out);
}

JsResult<Value> Isolate::bitwiseValues(const Value& l, const Value& r,
                                       Opcode op) {
  // JS bitwise ops operate on ToInt32/ToUint32 (Rule 72); ToPrimitive and
  // ToNumber run first so "3" | 0 == 3 and BigInt -> TypeError.
  JsResult<Value> lp = toPrimitive(l, false);
  if (!lp) return std::unexpected(lp.error());
  JsResult<Value> rp = toPrimitive(r, false);
  if (!rp) return std::unexpected(rp.error());
  JsResult<Value> ln = toNumberValue(*lp);
  if (!ln) return std::unexpected(ln.error());
  JsResult<Value> rn = toNumberValue(*rp);
  if (!rn) return std::unexpected(rn.error());
  const int32_t x = pure::toInt32(ln->asDouble());
  const int32_t y = pure::toInt32(rn->asDouble());
  const uint32_t shift = static_cast<uint32_t>(y) & 31;
  switch (op) {
    case Opcode::kBitAnd:
      return normalizeNumber(static_cast<double>(x & y));
    case Opcode::kBitOr:
      return normalizeNumber(static_cast<double>(x | y));
    case Opcode::kBitXor:
      return normalizeNumber(static_cast<double>(x ^ y));
    case Opcode::kShl:
      // Result is a signed int32; wrap-around is the JS semantics.
      return normalizeNumber(static_cast<double>(
          static_cast<int32_t>(static_cast<uint32_t>(x) << shift)));
    case Opcode::kShr:  // sign-propagating
      return normalizeNumber(static_cast<double>(x >> shift));
    case Opcode::kUShr:
      // >>> is unsigned: result is the uint32 value as a NUMBER (0..2^32-1),
      // never reinterpreted as int32 (Rule 72).
      return normalizeNumber(
          static_cast<double>(static_cast<uint32_t>(x) >> shift));
    default:
      return std::unexpected(typeError("internal: bad bitwise opcode"));
  }
}

JsResult<Value> Isolate::bigintArith(const Value& l, const Value& r,
                                     Opcode op) {
  if (!l.isBigInt() || !r.isBigInt()) {
    return std::unexpected(typeError(
        "Cannot mix BigInt and other types, use explicit conversions"));
  }
  const BigInt& a = *l.asBigInt();
  const BigInt& b = *r.asBigInt();
  BigInt out;
  switch (op) {
    case Opcode::kBigIntAdd:
    case Opcode::kSub:
      out = BigInt::add(a, b);
      break;
    case Opcode::kBigIntSub:
      out = BigInt::sub(a, b);
      break;
    case Opcode::kBigIntMul:
      out = BigInt::mul(a, b);
      break;
    case Opcode::kBigIntDiv: {
      if (b.isZero()) {
        return std::unexpected(rangeError("Division by zero"));
      }
      BigInt rem;
      (void)BigInt::divMod(a, b, &out, &rem);  // b nonzero (checked above)
      break;
    }
    case Opcode::kBigIntMod: {
      if (b.isZero()) {
        return std::unexpected(rangeError("Division by zero"));
      }
      BigInt q;
      (void)BigInt::divMod(a, b, &q, &out);  // b nonzero (checked above)
      break;
    }
    case Opcode::kBigIntNeg:
      out = BigInt::negate(a);
      break;
    default:
      return std::unexpected(typeError("internal: bad bigint opcode"));
  }
  return Value::raw(ValueKind::BigInt, heap_.makeBigInt(std::move(out)));
}

JsResult<Relational> Isolate::relationalCompare(const Value& l,
                                                const Value& r) {
  JsResult<Value> lp = toPrimitive(l, false);
  if (!lp) return std::unexpected(lp.error());
  JsResult<Value> rp = toPrimitive(r, false);
  if (!rp) return std::unexpected(rp.error());
  if (lp->isString() && rp->isString()) {
    int c = pure::compareStrings(*lp->str, *rp->str);
    if (c < 0) return Relational::Less;
    if (c > 0) return Relational::Greater;
    return Relational::Equal;
  }
  JsResult<Value> ln = toNumericValue(*lp);
  if (!ln) return std::unexpected(ln.error());
  JsResult<Value> rn = toNumericValue(*rp);
  if (!rn) return std::unexpected(rn.error());
  if (ln->isBigInt() && rn->isBigInt()) {
    int c = BigInt::compare(*ln->asBigInt(), *rn->asBigInt());
    if (c < 0) return Relational::Less;
    if (c > 0) return Relational::Greater;
    return Relational::Equal;
  }
  if (ln->isBigInt()) {
    Relational rel = bigintVsDouble(*ln->asBigInt(), rn->asDouble());
    return rel;
  }
  if (rn->isBigInt()) {
    Relational rel = bigintVsDouble(*rn->asBigInt(), ln->asDouble());
    if (rel == Relational::Less) return Relational::Greater;
    if (rel == Relational::Greater) return Relational::Less;
    return rel;
  }
  double x = ln->asDouble();
  double y = rn->asDouble();
  if (std::isnan(x) || std::isnan(y)) return Relational::Unordered;
  if (x < y) return Relational::Less;
  if (x > y) return Relational::Greater;
  return Relational::Equal;
}

JsResult<bool> Isolate::abstractEquals(const Value& l, const Value& r) {
  // Same-type (or both-number) branch.
  bool sameClass = l.kind == r.kind || (l.isNumber() && r.isNumber());
  if (sameClass) {
    if (l.isNumber()) return l.asDouble() == r.asDouble();
    if (l.kind == ValueKind::String) return pure::strictEquals(l, r);
    if (l.isBigInt()) return BigInt::compare(*l.asBigInt(), *r.asBigInt()) == 0;
    return pure::strictEquals(l, r);
  }
  // null == undefined; both-vs-anything-else false.
  if (l.isNullOrUndefined() && r.isNullOrUndefined()) return true;
  if (l.isNullOrUndefined() || r.isNullOrUndefined()) return false;

  // Number vs String.
  if (l.isNumber() && r.isString()) {
    return l.asDouble() == pure::stringToNumber(r.str->data);
  }
  if (l.isString() && r.isNumber()) {
    return pure::stringToNumber(l.str->data) == r.asDouble();
  }
  // BigInt vs String (invalid syntax -> false, never throws).
  if (l.isBigInt() && r.isString()) {
    BigInt big;
    if (!BigInt::fromString(r.str->data, &big)) return false;
    return BigInt::compare(*l.asBigInt(), big) == 0;
  }
  if (l.isString() && r.isBigInt()) {
    BigInt big;
    if (!BigInt::fromString(l.str->data, &big)) return false;
    return BigInt::compare(big, *r.asBigInt()) == 0;
  }
  // Boolean coerces to number first.
  if (l.isBoolean()) {
    Value num = Value::smi(l.b ? 1 : 0);
    return abstractEquals(num, r);
  }
  if (r.isBoolean()) {
    Value num = Value::smi(r.b ? 1 : 0);
    return abstractEquals(l, num);
  }
  // BigInt vs Number: exact mathematical comparison.
  if (l.isBigInt() && r.isNumber()) {
    double d = r.asDouble();
    if (std::isnan(d) || std::isinf(d) || !pure::doubleIsIntegral(d)) {
      return false;
    }
    BigInt big;
    (void)BigInt::fromDouble(d, &big);  // infallible: d integral (checked)
    return BigInt::compare(*l.asBigInt(), big) == 0;
  }
  if (l.isNumber() && r.isBigInt()) {
    double d = l.asDouble();
    if (std::isnan(d) || std::isinf(d) || !pure::doubleIsIntegral(d)) {
      return false;
    }
    BigInt big;
    (void)BigInt::fromDouble(d, &big);  // infallible: d integral (checked)
    return BigInt::compare(big, *r.asBigInt()) == 0;
  }
  // Object vs primitive: ToPrimitive(object, default), then retry.
  // (v0.3: proxies participate as objects.)
  if ((l.isObjectLike() || l.isProxy()) && !r.isObjectLike() && !r.isProxy()) {
    JsResult<Value> prim = toPrimitive(l, false);
    if (!prim) return std::unexpected(prim.error());
    return abstractEquals(*prim, r);
  }
  if ((r.isObjectLike() || r.isProxy()) && !l.isObjectLike() && !l.isProxy()) {
    JsResult<Value> prim = toPrimitive(r, false);
    if (!prim) return std::unexpected(prim.error());
    return abstractEquals(l, *prim);
  }
  return false;
}

JsResult<bool> Isolate::instanceofImpl(const Value& obj, const Value& ctor) {
  if (!ctor.isClosure() && !ctor.isProxy()) {
    return std::unexpected(typeError(
        "Right-hand side of 'instanceof' is not callable"));
  }
  // v0.3: proxy constructors resolve .prototype through the get trap.
  JsResult<Value> protoVal;
  if (ctor.isProxy()) {
    protoVal = proxyGet(ctor, Value::string(
        heap_.makeString(symbols_.text(sym_prototype_))));
  } else {
    protoVal = getProperty(ctor, sym_prototype_);
  }
  if (!protoVal) return std::unexpected(protoVal.error());
  if (!protoVal->isObjectLike()) {
    return std::unexpected(typeError(
        "Function has non-object prototype in instanceof check"));
  }
  Object* protoObj = objectOfValue(*protoVal);
  if (!obj.isObjectLike() && !obj.isProxy()) return false;
  Value curVal = obj;
  while (true) {
    if (curVal.isProxy()) {
      // [[GetPrototypeOf]] on the proxy node.
      JsResult<Value> p = proxyGetPrototype(curVal);
      if (!p) return std::unexpected(p.error());
      if (p->isNull()) return false;
      if (p->isObjectLike() && objectOfValue(*p) == protoObj) return true;
      if (!p->isObjectLike()) return false;
      curVal = *p;
      continue;
    }
    Object* cur = objectOfValue(curVal);
    while (cur != nullptr) {
      if (cur == protoObj) return true;
      cur = protoOfObject(cur);
    }
    return false;
  }
}

JsResult<Value> Isolate::constructImpl(const Value& ctor, const Value* args,
                                       uint32_t argc) {
  if (ctor.isProxy()) {
    // Proxy [[Construct]] (v0.3).
    return proxyConstruct(ctor, args, argc);
  }
  if (!ctor.isClosure()) {
    return std::unexpected(
        typeError("Class constructor is not callable"));
  }
  JsResult<Value> protoVal = getProperty(ctor, sym_prototype_);
  if (!protoVal) return std::unexpected(protoVal.error());
  Object* obj = newPlainObject();
  if (protoVal->isObjectLike()) {
    obj->proto = *protoVal;
  } else {
    // ES: non-object constructor .prototype falls back to Object.prototype.
    obj->proto = Value::raw(ValueKind::Object, objectPrototype_);
  }
  JsResult<Value> result =
      callValue(ctor, Value::raw(ValueKind::Object, obj), args, argc);
  if (!result) return std::unexpected(result.error());
  if (result->isObjectLike()) return *result;
  return Value::raw(ValueKind::Object, obj);
}

// ---------------------------------------------------------------------------
// Property descriptors (v0.3: Object builtins; reused by Proxy traps)
// ---------------------------------------------------------------------------
JsResult<bool> Isolate::definePropertyDescriptor(Object* obj, SymbolId key,
                                                 const PropertyDescriptor& d) {
  LookupResult existing = lookupOwnProperty(obj, key);
  if (!obj->extensible && !existing.found) {
    return std::unexpected(typeError(
        "Cannot define property '" + utf16ToUtf8(symbols_.text(key)) +
        "': object is not extensible"));
  }
  // Array exotica first (length / element keys never reach the shape tree).
  if (obj->isArray) {
    if (key == sym_length_) {
      if (d.isAccessor()) {
        return std::unexpected(
            typeError("Array length must be a data property"));
      }
      return arraySetLength(obj, d.hasValue ? d.value : Value::undefined());
    }
    uint32_t idx = 0;
    if (arrayIndexFromKey(symbols_.text(key), &idx)) {
      if (d.isAccessor()) {
        return std::unexpected(typeError(
            "Cannot define an accessor property for an array index"));
      }
      if (!obj->extensible && !arrayHasOwnElement(obj, idx)) {
        return std::unexpected(typeError(
            "Cannot add element '" + utf16ToUtf8(symbols_.text(key)) +
            "': array is not extensible"));
      }
      if (d.hasValue) return setArrayElement(obj, idx, d.value);
      return true;
    }
  }
  // Named property. Attribute mapping per ES defaults (absent -> false).
  PropertyAttrs attrs;
  if (d.enumerable) attrs.add(PropAttr::Enumerable);
  if (d.configurable) attrs.add(PropAttr::Configurable);
  if (d.isAccessor()) {
    attrs.add(PropAttr::IsAccessor);
    AccessorPair* pair = heap_.makeAccessor();
    pair->getter = d.getter;
    pair->setter = d.setter;
    // Replace-in-place when an accessor already occupies the slot with the
    // same enumerability/configurability (no shape change needed).
    if (existing.found && existing.accessor != nullptr) {
      PropertyAttrs oldAttrs;
      int32_t slot = ownDataSlotAttrs(obj, key, &oldAttrs);
      (void)slot;
      // Accessor slots cannot be probed via ownDataSlotAttrs (it rejects
      // accessors); the pair is overwritten through the existing slot.
      if (existing.dataSlot == nullptr && existing.accessor != nullptr) {
        Shape* s = obj->shape;
        while (s != nullptr && s->key != key) s = s->parent;
        if (s != nullptr &&
            (s->attrs.raw() & ~static_cast<uint8_t>(PropAttr::IsAccessor)) ==
                (attrs.raw() & ~static_cast<uint8_t>(PropAttr::IsAccessor))) {
          *existing.accessor = *pair;
          return true;
        }
      }
    }
    Shape* from = obj->shape != nullptr ? obj->shape : shapes_.root();
    obj->shape = shapes_.transition(from, key, attrs);
    obj->slots.push_back(Value::raw(ValueKind::Accessor, pair));
    return true;
  }
  // Data property.
  if (d.writable) attrs.add(PropAttr::Writable);
  if (existing.found && existing.dataSlot != nullptr) {
    PropertyAttrs oldAttrs;
    int32_t slot = ownDataSlotAttrs(obj, key, &oldAttrs);
    if (slot >= 0 && oldAttrs.raw() == attrs.raw()) {
      if (!attrs.has(PropAttr::Writable) && d.hasValue &&
          !pure::sameValue(*existing.dataSlot, d.value)) {
        return std::unexpected(typeError(
            "Cannot redefine non-writable property '" +
            utf16ToUtf8(symbols_.text(key)) + "' with a different value"));
      }
      if (attrs.has(PropAttr::Writable)) {
        *existing.dataSlot = d.hasValue ? d.value : Value::undefined();
        return true;
      }
    }
  }
  if (!attrs.has(PropAttr::Writable)) {
    // v0.3 store model: non-writable data properties are supported only via
    // the setAlongChain TypeError path; defining them fresh is allowed and
    // stores through ICs are guarded by installPropertyIc's writable check.
  }
  Shape* from = obj->shape != nullptr ? obj->shape : shapes_.root();
  obj->shape = shapes_.transition(from, key, attrs);
  obj->slots.push_back(d.hasValue ? d.value : Value::undefined());
  return true;
}

JsResult<Value> Isolate::getOwnPropertyDescriptorValue(Object* obj,
                                                       SymbolId key) {
  if (obj->isArray) {
    if (key == sym_length_) {
      PropertyDescriptor d;
      d.hasValue = d.hasWritable = d.hasEnumerable = d.hasConfigurable = true;
      d.value = arrayLengthValue(obj);
      d.writable = true;
      d.enumerable = false;
      d.configurable = false;  // exotic length (never deletable)
      return Value::raw(ValueKind::Object, makeDescriptorObject(*this, d));
    }
    uint32_t idx = 0;
    if (arrayIndexFromKey(symbols_.text(key), &idx)) {
      Value v = ownElementValue(obj, idx);
      if (v.isHole()) return Value::undefined();
      PropertyDescriptor d;
      d.hasValue = d.hasWritable = d.hasEnumerable = d.hasConfigurable = true;
      d.value = v;
      d.writable = d.enumerable = d.configurable = true;
      return Value::raw(ValueKind::Object, makeDescriptorObject(*this, d));
    }
  }
  LookupResult own = lookupOwnProperty(obj, key);
  if (!own.found) return Value::undefined();
  Shape* s = obj->shape;
  while (s != nullptr && s->key != key) s = s->parent;
  PropertyAttrs a = s != nullptr ? s->attrs : PropertyAttrs();
  PropertyDescriptor d;
  d.hasEnumerable = d.hasConfigurable = true;
  d.enumerable = a.has(PropAttr::Enumerable);
  d.configurable = a.has(PropAttr::Configurable);
  if (own.accessor != nullptr) {
    d.hasGet = d.hasSet = true;
    d.getter = own.accessor->getter;
    d.setter = own.accessor->setter;
  } else {
    d.hasValue = d.hasWritable = true;
    d.value = *own.dataSlot;
    d.writable = a.has(PropAttr::Writable);
  }
  return Value::raw(ValueKind::Object, makeDescriptorObject(*this, d));
}

// ---------------------------------------------------------------------------
// Feedback recording (saturating — Rule 114). v0.3: recording operates on
// resolved FeedbackSlot pointers provided by the dispatch handlers (no
// per-op frameStack/vector re-derivation); a site is skipped entirely when
// the slot pointer is null (no feedback vector, or --no-record measurement
// mode, benchmarks_v0.2.md Section 5 #2).
// ---------------------------------------------------------------------------
void Isolate::recordPropertySite(FeedbackSlot* s, const Value& recv) {
  if (s == nullptr) return;
  uint32_t shapeId = 0;  // 0 reserved for non-object receivers
  if (recv.isObjectLike()) {
    Shape* sh = objectOfValue(recv)->shape;
    // Fresh objects have no shape yet (nullptr) until the first store;
    // they share the reserved 0 bucket with primitives (deterministic).
    shapeId = sh != nullptr ? sh->id : 0;
  }
  s->propertyHits++;
  for (uint32_t i = 0; i < s->distinctShapes; i++) {
    if (s->shapeIds[i] == shapeId) {
      if (s->shapeCounts[i] < UINT32_MAX) s->shapeCounts[i]++;
      return;
    }
  }
  if (s->distinctShapes < kMegamorphicThreshold) {
    s->shapeIds[s->distinctShapes] = shapeId;
    s->shapeCounts[s->distinctShapes] = 1;
    s->distinctShapes++;
  } else {
    s->distinctShapes = kMegamorphicThreshold;  // stays megamorphic
  }
}

void Isolate::recordElementSite(FeedbackSlot* s, const Value& recv) {
  if (s == nullptr) return;
  // Element-kind identity: 0 = non-array receiver; 1..6 = ElementsKind+1 of
  // an array receiver (deterministic; saturating buckets as Property sites).
  uint32_t kindId = 0;
  if (recv.isObjectLike()) {
    Object* obj = objectOfValue(recv);
    if (obj->isArray) {
      kindId = static_cast<uint32_t>(obj->elementsKind) + 1;
    }
  }
  s->propertyHits++;
  for (uint32_t i = 0; i < s->distinctShapes; i++) {
    if (s->shapeIds[i] == kindId) {
      if (s->shapeCounts[i] < UINT32_MAX) s->shapeCounts[i]++;
      return;
    }
  }
  if (s->distinctShapes < kMegamorphicThreshold) {
    s->shapeIds[s->distinctShapes] = kindId;
    s->shapeCounts[s->distinctShapes] = 1;
    s->distinctShapes++;
  } else {
    s->distinctShapes = kMegamorphicThreshold;  // stays megamorphic
  }
}

void Isolate::recordBinarySite(FeedbackSlot* s, const Value& l, const Value& r) {
  if (s == nullptr) return;
  auto classify = [](const Value& v) -> uint32_t {
    switch (v.kind) {
      case ValueKind::Smi: return static_cast<uint32_t>(ValueClass::Smi);
      case ValueKind::HeapNumber:
        return static_cast<uint32_t>(ValueClass::HeapNumber);
      case ValueKind::String:
        return static_cast<uint32_t>(ValueClass::String);
      case ValueKind::BigInt:
        return static_cast<uint32_t>(ValueClass::BigInt);
      case ValueKind::Object:
      case ValueKind::Closure:
        return static_cast<uint32_t>(ValueClass::Object);
      default:
        return static_cast<uint32_t>(ValueClass::Other);
    }
  };
  if (s->classCounts[0] < UINT32_MAX) {
    s->classCounts[classify(l)]++;
    s->classCounts[classify(r)]++;
  }
}

void Isolate::recordBranchSite(FeedbackSlot* s, bool taken) {
  if (s == nullptr) return;
  if (taken) {
    if (s->takenCount < UINT32_MAX) s->takenCount++;
  } else {
    if (s->notTakenCount < UINT32_MAX) s->notTakenCount++;
  }
}

void Isolate::recordCallSite(FeedbackSlot* s, const Value& callee) {
  if (s == nullptr) return;
  if (s->callCount < UINT32_MAX) s->callCount++;
  int32_t funcIdx = -1;
  if (callee.isClosure()) {
    const Closure* cl = static_cast<const Closure*>(callee.ptr);
    if (cl->funcIndex < kNativeFuncIndexBase) funcIdx = static_cast<int32_t>(cl->funcIndex);
  }
  if (funcIdx < 0) {
    if (s->unknownCallees < UINT32_MAX) s->unknownCallees++;
    return;
  }
  for (uint32_t i = 0; i < kCallProfileRing; i++) {
    if (s->callees[i] == funcIdx) {
      if (s->calleeCounts[i] < UINT32_MAX) s->calleeCounts[i]++;
      return;
    }
  }
  for (uint32_t i = 0; i < kCallProfileRing; i++) {
    if (s->callees[i] == -1) {
      s->callees[i] = funcIdx;
      s->calleeCounts[i] = 1;
      return;
    }
  }
  // Ring full: fold into unknown (least-significant signal).
  if (s->unknownCallees < UINT32_MAX) s->unknownCallees++;
}

void Isolate::installPropertyIc(FeedbackSlot* s, Object* obj, SymbolId key) {
  if (s == nullptr || obj == nullptr || obj->shape == nullptr) return;
  PropertyAttrs attrs;
  int32_t slot = ownDataSlotAttrs(obj, key, &attrs);
  if (slot < 0) return;  // accessor, deleted, or absent: no IC
  if (!attrs.has(PropAttr::Writable)) return;  // read-only: never IC-store
  s->icShape = obj->shape->id;
  s->icSlot = static_cast<uint32_t>(slot);
  s->icAttrs = attrs.raw();
}

bool Isolate::chainHasAccessor(Object* obj, SymbolId key) const {
  for (Object* p = protoOfObject(obj); p != nullptr; p = protoOfObject(p)) {
    LookupResult r = lookupOwnProperty(p, key);
    if (r.found && r.accessor != nullptr) return true;
  }
  return false;
}

// Own string/symbol keys as VALUES, ES order (indices ascending, then named
// insertion order); user symbols come back as Symbol values. Used by the
// Object builtins and the Proxy ownKeys forwarding/invariants.
std::vector<Value> Isolate::ownKeysValues(Object* obj, bool includeLength) {
  std::vector<Value> out;
  auto pushKey = [&](SymbolId id) {
    if (isUserSymbolId(id)) {
      size_t idx = static_cast<size_t>(id - kUserSymbolBase);
      if (idx < userSymbols_.size()) {
        out.push_back(Value::raw(ValueKind::Symbol, userSymbols_[idx]));
        return;
      }
    }
    out.push_back(Value::string(heap_.makeString(symbols_.text(id))));
  };
  if (obj->isArray) {
    const uint32_t len = obj->length;
    for (uint32_t i = 0; i < len && i < obj->elements.size(); i++) {
      if (!obj->elements[i].isHole()) pushKey(internIndexKey(i));
    }
    for (const auto& kv : obj->sparse) {
      if (kv.first < len) pushKey(internIndexKey(kv.first));
    }
    if (includeLength) pushKey(sym_length_);
  }
  std::vector<SymbolId> named;
  for (Shape* s = obj->shape; s != nullptr && s->key != kInvalidSymbol;
       s = s->parent) {
    if (!obj->slots[s->slot].isHole()) named.push_back(s->key);
  }
  for (size_t i = named.size(); i-- > 0;) {
    bool dup = false;
    for (const Value& already : out) {
      if (already.isString() && !isUserSymbolId(named[i]) &&
          already.str->data == symbols_.text(named[i])) {
        dup = true;
        break;
      }
      if (already.isSymbol() && isUserSymbolId(named[i]) &&
          already.asSymbol()->uniqueId + kUserSymbolBase == named[i]) {
        dup = true;
        break;
      }
    }
    if (!dup) pushKey(named[i]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
JsResult<std::u16string> Isolate::displayString(const Value& v) {
  JsResult<Value> s = toStringValue(v);
  if (!s) return std::unexpected(s.error());
  return s->str->data;
}

std::u16string Isolate::formatUncaught(const Value& thrown) const {
  if (thrown.isObjectLike()) {
    Object* obj = objectOfValue(thrown);
    LookupResult nameR = lookupProperty(obj, sym_name_);
    LookupResult msgR = lookupProperty(obj, sym_message_);
    std::u16string out;
    bool hasName = false;
    if (nameR.found && nameR.dataSlot != nullptr &&
        nameR.dataSlot->isString()) {
      out += nameR.dataSlot->str->data;
      hasName = true;
    }
    if (msgR.found && msgR.dataSlot != nullptr && msgR.dataSlot->isString()) {
      if (hasName) out += u": ";
      out += msgR.dataSlot->str->data;
    }
    if (hasName || (msgR.found && msgR.dataSlot != nullptr)) return out;
  }
  // Non-error values: ToString (best effort; internal failures degrade).
  if (thrown.isString()) return thrown.str->data;
  switch (thrown.kind) {
    case ValueKind::Undefined: return u"undefined";
    case ValueKind::Null: return u"null";
    case ValueKind::Boolean: return thrown.b ? u"true" : u"false";
    case ValueKind::Smi: {
      std::string a = std::to_string(thrown.i32);
      return std::u16string(a.begin(), a.end());
    }
    case ValueKind::HeapNumber: {
      std::string a = pure::doubleToString(thrown.num);
      return std::u16string(a.begin(), a.end());
    }
    case ValueKind::BigInt: return thrown.asBigInt()->toString();
    default: return u"[object Object]";
  }
}

std::string joinTrace(const std::vector<std::string>& frames) {
  std::string out;
  for (const std::string& f : frames) {
    out += f;
    out += "\n";
  }
  return out;
}

}  // namespace ts
