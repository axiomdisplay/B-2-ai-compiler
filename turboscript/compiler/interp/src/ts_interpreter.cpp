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
  opcodeCounts_.assign(kOpcodeSpace, 0);
}

void Isolate::registerNative(const char* name, NativeFn fn) {
  natives_.push_back(NativeEntry{name, fn});
  SymbolId sym = symbols_.intern(
      std::u16string(name, name + std::strlen(name)));
  Closure* cl = heap_.makeClosure();
  cl->funcIndex = kNativeFuncIndexBase + static_cast<uint32_t>(natives_.size()) - 1;
  cl->context = nullptr;
  cl->asObject = heap_.makeObject();
  // Give the native a name property for diagnostics.
  std::u16string wide(name, name + std::strlen(name));
  Value nameVal = Value::string(heap_.makeString(wide));
  (void)defineProperty(cl->asObject, sym_name_, nameVal, kDefaultDataAttrs);
  nativeClosures_.push_back(cl);
  (void)defineProperty(global_, sym, Value::raw(ValueKind::Closure, cl),
                       kDefaultDataAttrs);
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

Closure* Isolate::makeClosureFor(uint32_t funcIndex, Context* context) {
  if (module_ == nullptr || funcIndex >= module_->functions.size()) {
    return nullptr;
  }
  Closure* cl = heap_.makeClosure();
  cl->funcIndex = funcIndex;
  cl->context = context;
  cl->asObject = heap_.makeObject();
  // Each closure gets a fresh .prototype object (ECMAScript semantics);
  // prototype.constructor points back at the closure (v0.1: enumerable,
  // data; non-enumerable attrs arrive with define-property semantics).
  Object* protoObj = heap_.makeObject();
  protoObj->proto = Value::null();  // builtins deferred (bytecode_spec 11)
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
  Object* err = heap_.makeObject();
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
// Property operations
// ---------------------------------------------------------------------------
Object* Isolate::protoOfObject(Object* obj) const {
  return obj->proto.isObjectLike() ? objectOfValue(obj->proto) : nullptr;
}

JsResult<bool> Isolate::defineProperty(Object* obj, SymbolId key,
                                       const Value& val,
                                       PropertyAttrs attrs) {
  // Fresh objects carry shape == nullptr; the root shape is the implicit
  // origin of every transition chain (interp_contract.md 3).
  Shape* from = obj->shape != nullptr ? obj->shape : shapes_.root();
  obj->shape = shapes_.transition(from, key, attrs);
  obj->slots.push_back(val);
  return true;
}

JsResult<Value> Isolate::getProperty(const Value& recv, SymbolId key) {
  if (!recv.isObjectLike()) {
    // v0.1: no primitive wrappers (bytecode_spec.md Section 11).
    return Value::undefined();
  }
  Object* obj = objectOfValue(recv);
  LookupResult lr = lookupProperty(obj, key);
  if (!lr.found) return Value::undefined();
  if (lr.accessor != nullptr) {
    if (lr.accessor->getter.isUndefined()) return Value::undefined();
    return callValue(lr.accessor->getter, recv, nullptr, 0);
  }
  return *lr.dataSlot;
}

JsResult<bool> Isolate::setProperty(const Value& recv, SymbolId key,
                                    const Value& val) {
  if (!recv.isObjectLike()) {
    // Strict-mode store on a primitive receiver.
    return std::unexpected(typeError(
        "Cannot create property '" + utf16ToUtf8(symbols_.text(key)) +
        "' on " + pure::kindName(recv)));
  }
  Object* obj = objectOfValue(recv);
  LookupResult lr = lookupProperty(obj, key);
  if (lr.found) {
    if (lr.accessor != nullptr) {
      if (lr.accessor->setter.isUndefined()) {
        return std::unexpected(typeError(
            "Cannot set property '" + utf16ToUtf8(symbols_.text(key)) +
            "': no setter"));
      }
      // Setter invoked with the ORIGINAL receiver as `this`.
      JsResult<Value> r = callValue(lr.accessor->setter, recv, &val, 1);
      if (!r) return std::unexpected(r.error());
      return true;
    }
    // Own or inherited data property: v0.1 data attrs are always writable,
    // so the write succeeds (inherited data writes shadow on the receiver
    // per OrdinarySetWithOwnDescriptor).
    if (lr.holder == obj) {
      *lr.dataSlot = val;
      return true;
    }
    // Inherited data property: create own shadow property on the receiver.
    return defineProperty(obj, key, val, kDefaultDataAttrs);
  }
  return defineProperty(obj, key, val, kDefaultDataAttrs);
}

JsResult<bool> Isolate::deletePropertyImpl(const Value& recv, SymbolId key) {
  if (!recv.isObjectLike()) return true;  // ToObject(prim) has no such own.
  Object* obj = objectOfValue(recv);
  int32_t slot = obj->findOwnSlot(key);
  if (slot < 0) return true;
  Value& v = obj->slots[static_cast<size_t>(slot)];
  if (v.isHole()) return true;
  // Invariant: v0.1 property attrs are always configurable
  // (kDefaultDataAttrs), so the delete always succeeds. Non-configurable
  // properties arrive with define-property semantics in v0.2.
  v = Value::hole();
  return true;
}

JsResult<bool> Isolate::hasPropertyImpl(const Value& recv, SymbolId key) {
  if (!recv.isObjectLike()) return false;
  LookupResult lr = lookupProperty(objectOfValue(recv), key);
  return lr.found;
}

// ---------------------------------------------------------------------------
// Conversions with hooks (oracle-verified semantics)
// ---------------------------------------------------------------------------
JsResult<Value> Isolate::toPrimitive(const Value& v, bool hintString) {
  if (!v.isObjectLike()) return v;  // primitives: no hooks, identity.
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
    return symbols_.intern(v.str->data);
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
                                   uint32_t slot) {
  recordBinarySite(slot, l, r);
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
  if (l.isObjectLike() && !r.isObjectLike()) {
    JsResult<Value> prim = toPrimitive(l, false);
    if (!prim) return std::unexpected(prim.error());
    return abstractEquals(*prim, r);
  }
  if (r.isObjectLike() && !l.isObjectLike()) {
    JsResult<Value> prim = toPrimitive(r, false);
    if (!prim) return std::unexpected(prim.error());
    return abstractEquals(l, *prim);
  }
  return false;
}

JsResult<bool> Isolate::instanceofImpl(const Value& obj, const Value& ctor) {
  if (!ctor.isClosure()) {
    return std::unexpected(typeError(
        "Right-hand side of 'instanceof' is not callable"));
  }
  JsResult<Value> protoVal = getProperty(ctor, sym_prototype_);
  if (!protoVal) return std::unexpected(protoVal.error());
  if (!protoVal->isObjectLike()) {
    return std::unexpected(typeError(
        "Function has non-object prototype in instanceof check"));
  }
  Object* protoObj = objectOfValue(*protoVal);
  if (!obj.isObjectLike()) return false;
  Object* cur = objectOfValue(obj);
  while (cur != nullptr) {
    if (cur == protoObj) return true;
    cur = protoOfObject(cur);
  }
  return false;
}

JsResult<Value> Isolate::constructImpl(const Value& ctor, const Value* args,
                                       uint32_t argc) {
  if (!ctor.isClosure()) {
    return std::unexpected(
        typeError("Class constructor is not callable"));
  }
  JsResult<Value> protoVal = getProperty(ctor, sym_prototype_);
  if (!protoVal) return std::unexpected(protoVal.error());
  Object* obj = heap_.makeObject();
  if (protoVal->isObjectLike()) {
    obj->proto = *protoVal;
  } else {
    obj->proto = Value::null();  // builtins deferred (bytecode_spec 11)
  }
  JsResult<Value> result =
      callValue(ctor, Value::raw(ValueKind::Object, obj), args, argc);
  if (!result) return std::unexpected(result.error());
  if (result->isObjectLike()) return *result;
  return Value::raw(ValueKind::Object, obj);
}

// ---------------------------------------------------------------------------
// Feedback recording (always on; saturating — Rule 114)
// ---------------------------------------------------------------------------
void Isolate::recordPropertySite(uint32_t slot, const Value& recv) {
  if (slot == kNoFeedbackSlot) return;
  FeedbackSlot& s = feedback_[module_->functions.empty()
                                  ? 0
                                  : frameStack_.back()->fn->index][slot];
  uint32_t shapeId = 0;  // 0 reserved for non-object receivers
  if (recv.isObjectLike()) {
    Shape* sh = objectOfValue(recv)->shape;
    // Fresh objects have no shape yet (nullptr) until the first store;
    // they share the reserved 0 bucket with primitives (deterministic).
    shapeId = sh != nullptr ? sh->id : 0;
  }
  s.propertyHits++;
  for (uint32_t i = 0; i < s.distinctShapes; i++) {
    if (s.shapeIds[i] == shapeId) {
      if (s.shapeCounts[i] < UINT32_MAX) s.shapeCounts[i]++;
      return;
    }
  }
  if (s.distinctShapes < kMegamorphicThreshold) {
    s.shapeIds[s.distinctShapes] = shapeId;
    s.shapeCounts[s.distinctShapes] = 1;
    s.distinctShapes++;
  } else {
    s.distinctShapes = kMegamorphicThreshold;  // stays megamorphic
  }
}

void Isolate::recordBinarySite(uint32_t slot, const Value& l, const Value& r) {
  if (slot == kNoFeedbackSlot) return;
  FeedbackSlot& s = feedback_[frameStack_.back()->fn->index][slot];
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
  if (s.classCounts[0] < UINT32_MAX) {
    s.classCounts[classify(l)]++;
    s.classCounts[classify(r)]++;
  }
}

void Isolate::recordBranchSite(uint32_t slot, bool taken) {
  if (slot == kNoFeedbackSlot) return;
  FeedbackSlot& s = feedback_[frameStack_.back()->fn->index][slot];
  if (taken) {
    if (s.takenCount < UINT32_MAX) s.takenCount++;
  } else {
    if (s.notTakenCount < UINT32_MAX) s.notTakenCount++;
  }
}

void Isolate::recordCallSite(uint32_t slot, const Value& callee) {
  if (slot == kNoFeedbackSlot) return;
  FeedbackSlot& s = feedback_[frameStack_.back()->fn->index][slot];
  if (s.callCount < UINT32_MAX) s.callCount++;
  int32_t funcIdx = -1;
  if (callee.isClosure()) {
    const Closure* cl = static_cast<const Closure*>(callee.ptr);
    if (cl->funcIndex < kNativeFuncIndexBase) funcIdx = static_cast<int32_t>(cl->funcIndex);
  }
  if (funcIdx < 0) {
    if (s.unknownCallees < UINT32_MAX) s.unknownCallees++;
    return;
  }
  for (uint32_t i = 0; i < kCallProfileRing; i++) {
    if (s.callees[i] == funcIdx) {
      if (s.calleeCounts[i] < UINT32_MAX) s.calleeCounts[i]++;
      return;
    }
  }
  for (uint32_t i = 0; i < kCallProfileRing; i++) {
    if (s.callees[i] == -1) {
      s.callees[i] = funcIdx;
      s.calleeCounts[i] = 1;
      return;
    }
  }
  // Ring full: fold into unknown (least-significant signal).
  if (s.unknownCallees < UINT32_MAX) s.unknownCallees++;
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
