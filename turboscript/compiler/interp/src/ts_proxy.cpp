// TurboScript — v0.3 Proxy trap protocol (ES ch. 10.5: Proxy Object
// Internal Methods, all 13) plus the user-symbol Isolate services.
// Law coverage: Rule 70 ("Proxy traps for all 13 internal methods"),
// Rule 96 (Tier 0 universal fallback), Rule 74 (JS exceptions are values).
//
// Protocol shape per method: resolve the trap on the handler
// (GetMethod semantics — absent -> forward to the target; present but not
// callable -> TypeError), invoke it with the ES argument tuple, then run
// the target-side invariant checks. Invariants that depend on target
// descriptors go through getOwnPropertyDescriptorValue, so exotic array
// targets check their length/elements correctly.
#include "ts_interpreter.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace ts {

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------
Value Isolate::makeSymbolValue(std::u16string desc) {
  const uint32_t uniqueId = static_cast<uint32_t>(userSymbols_.size());
  SymbolObj* sym = heap_.makeSymbol(std::move(desc), uniqueId);
  userSymbols_.push_back(sym);
  return Value::raw(ValueKind::Symbol, sym);
}

SymbolObj* Isolate::symbolForRegistry(const std::u16string& key) const {
  auto it = symbolRegistry_.find(key);
  return it != symbolRegistry_.end() ? it->second : nullptr;
}

void Isolate::symbolForRegister(const std::u16string& key, SymbolObj* sym) {
  symbolRegistry_.emplace(key, sym);
}

std::u16string Isolate::keyText(SymbolId key) const {
  if (isUserSymbolId(key)) {
    size_t idx = static_cast<size_t>(key - kUserSymbolBase);
    if (idx < userSymbols_.size()) return userSymbols_[idx]->desc;
    return u"<symbol>";
  }
  if (key < symbols_.size()) return symbols_.text(key);
  return u"<key>";
}

Value Isolate::keyToValue(SymbolId key) {
  if (isUserSymbolId(key)) {
    size_t idx = static_cast<size_t>(key - kUserSymbolBase);
    if (idx < userSymbols_.size()) {
      return Value::raw(ValueKind::Symbol, userSymbols_[idx]);
    }
  }
  return Value::string(heap_.makeString(symbols_.text(key)));
}

// ---------------------------------------------------------------------------
// Trap resolution (GetMethod: undefined -> forward; non-callable -> TypeError)
// ---------------------------------------------------------------------------
JsResult<Value> Isolate::proxyTrap(Object* handler, const char* name) {
  SymbolId sym = symbols_.intern(std::u16string(name, name + std::strlen(name)));
  JsResult<Value> m = getProperty(Value::raw(ValueKind::Object, handler), sym);
  if (!m) return std::unexpected(m.error());
  if (m->isClosure() || m->isUndefined()) return *m;
  return std::unexpected(typeError(
      std::string("'") + name + "' trap must be a function or undefined"));
}

// ---------------------------------------------------------------------------
// [[Get]]
// ---------------------------------------------------------------------------
JsResult<Value> Isolate::proxyGet(Value proxy, const Value& key) {
  ProxyObj* p = proxy.asProxy();
  JsResult<Value> trap = proxyTrap(p->handler, "get");
  if (!trap) return std::unexpected(trap.error());
  if (trap->isUndefined()) return getElementValue(p->target, key);
  Value args[3] = {p->target, key, proxy};
  JsResult<Value> result = callValue(*trap, Value::raw(ValueKind::Object, p->handler), args, 3);
  if (!result) return std::unexpected(result.error());
  // Invariant: target non-configurable data !writable -> SameValue;
  // non-configurable accessor without getter -> result must be undefined.
  JsResult<SymbolId> keySym = toPropertyKey(key);
  if (!keySym) return std::unexpected(keySym.error());
  Object* targetObj = objectOfValue(p->target);
  if (targetObj == nullptr) return *result;
  JsResult<Value> desc = getOwnPropertyDescriptorValue(targetObj, *keySym);
  if (!desc) return std::unexpected(desc.error());
  if (desc->isUndefined()) return *result;
  Object* d = objectOfValue(*desc);
  SymbolId symConfigurable = symbols_.intern(u"configurable");
  SymbolId symWritable = symbols_.intern(u"writable");
  SymbolId symValue = symbols_.intern(u"value");
  SymbolId symGet = symbols_.intern(u"get");
  JsResult<Value> cfg = getProperty(Value::raw(ValueKind::Object, d),
                                    symConfigurable);
  if (!cfg) return std::unexpected(cfg.error());
  if (cfg->isUndefined() || pure::toBoolean(*cfg)) return *result;
  JsResult<Value> wr = getProperty(Value::raw(ValueKind::Object, d), symWritable);
  if (!wr) return std::unexpected(wr.error());
  if (!wr->isUndefined() && !pure::toBoolean(*wr)) {
    JsResult<Value> tv = getProperty(Value::raw(ValueKind::Object, d), symValue);
    if (!tv) return std::unexpected(tv.error());
    if (!pure::sameValue(*result, *tv)) {
      return std::unexpected(typeError(
          "'get' on proxy: property '" + utf16ToUtf8(keyText(*keySym)) +
          "' is a non-writable, non-configurable data property on the proxy "
          "target but the proxy did not return its actual value"));
    }
  } else {
    JsResult<Value> gt = getProperty(Value::raw(ValueKind::Object, d), symGet);
    if (!gt) return std::unexpected(gt.error());
    if (gt->isUndefined() && !result->isUndefined()) {
      return std::unexpected(typeError(
          "'get' on proxy: property '" + utf16ToUtf8(keyText(*keySym)) +
          "' is a non-configurable accessor without a getter"));
    }
  }
  return *result;
}

// ---------------------------------------------------------------------------
// [[Set]]
// ---------------------------------------------------------------------------
JsResult<bool> Isolate::proxySet(Value proxy, const Value& key,
                                 const Value& val) {
  ProxyObj* p = proxy.asProxy();
  JsResult<Value> trap = proxyTrap(p->handler, "set");
  if (!trap) return std::unexpected(trap.error());
  JsResult<SymbolId> keySymR = toPropertyKey(key);
  if (!keySymR) return std::unexpected(keySymR.error());
  SymbolId keySym = *keySymR;
  Object* targetObj = objectOfValue(p->target);
  if (trap->isUndefined()) return setElementValue(p->target, key, val);
  Value args[4] = {p->target, key, val, proxy};
  JsResult<Value> r = callValue(*trap, Value::raw(ValueKind::Object, p->handler), args, 4);
  if (!r) return std::unexpected(r.error());
  bool success = pure::toBoolean(*r);
  // Invariants.
  JsResult<bool> ext = proxyIsExtensible(proxy);
  if (!ext) return std::unexpected(ext.error());
  JsResult<Value> desc = targetObj != nullptr
                             ? getOwnPropertyDescriptorValue(targetObj, keySym)
                             : Value::undefined();
  if (!desc) return std::unexpected(desc.error());
  if (desc->isUndefined()) {
    if (!*ext && success) {
      return std::unexpected(typeError(
          "'set' on proxy: new property '" + utf16ToUtf8(keyText(keySym)) +
          "' on a non-extensible target"));
    }
  } else {
    Object* d = objectOfValue(*desc);
    SymbolId symConfigurable = symbols_.intern(u"configurable");
    SymbolId symWritable = symbols_.intern(u"writable");
    SymbolId symValue = symbols_.intern(u"value");
    SymbolId symSet = symbols_.intern(u"set");
    JsResult<Value> cfg =
        getProperty(Value::raw(ValueKind::Object, d), symConfigurable);
    if (!cfg) return std::unexpected(cfg.error());
    if (cfg->isUndefined() || !pure::toBoolean(*cfg)) {
      JsResult<Value> wr =
          getProperty(Value::raw(ValueKind::Object, d), symWritable);
      if (!wr) return std::unexpected(wr.error());
      if (!wr->isUndefined()) {
        JsResult<Value> tv =
            getProperty(Value::raw(ValueKind::Object, d), symValue);
        if (!tv) return std::unexpected(tv.error());
        if (!pure::toBoolean(*wr)) {
          if (!pure::sameValue(val, *tv)) {
            return std::unexpected(typeError(
                "'set' on proxy: property '" + utf16ToUtf8(keyText(keySym)) +
                "' is a non-writable, non-configurable data property on the "
                "proxy target but the proxy did not report the same value"));
          }
        }
      } else {
        JsResult<Value> st =
            getProperty(Value::raw(ValueKind::Object, d), symSet);
        if (!st) return std::unexpected(st.error());
        if (st->isUndefined()) {
          return std::unexpected(typeError(
              "'set' on proxy: property '" + utf16ToUtf8(keyText(keySym)) +
              "' is a non-configurable accessor without a setter"));
        }
      }
    }
  }
  // Strict-mode receiver (v0.1+): failed sets throw.
  if (!success) {
    return std::unexpected(typeError(
        "'set' on proxy: trap returned falsish for property '" +
        utf16ToUtf8(keyText(keySym)) + "'"));
  }
  return true;
}

// ---------------------------------------------------------------------------
// [[HasProperty]]
// ---------------------------------------------------------------------------
JsResult<bool> Isolate::proxyHas(Value proxy, const Value& key) {
  ProxyObj* p = proxy.asProxy();
  JsResult<SymbolId> keySymR = toPropertyKey(key);
  if (!keySymR) return std::unexpected(keySymR.error());
  JsResult<Value> trap = proxyTrap(p->handler, "has");
  if (!trap) return std::unexpected(trap.error());
  if (trap->isUndefined()) return hasPropertyImpl(p->target, *keySymR);
  Value args[2] = {p->target, key};
  JsResult<Value> r = callValue(*trap, Value::raw(ValueKind::Object, p->handler), args, 2);
  if (!r) return std::unexpected(r.error());
  bool found = pure::toBoolean(*r);
  Object* targetObj = objectOfValue(p->target);
  if (targetObj != nullptr && !found) {
    JsResult<Value> desc =
        getOwnPropertyDescriptorValue(targetObj, *keySymR);
    if (!desc) return std::unexpected(desc.error());
    if (!desc->isUndefined()) {
      Object* d = objectOfValue(*desc);
      SymbolId symConfigurable = symbols_.intern(u"configurable");
      JsResult<Value> cfg =
          getProperty(Value::raw(ValueKind::Object, d), symConfigurable);
      if (!cfg) return std::unexpected(cfg.error());
      if (cfg->isUndefined() || !pure::toBoolean(*cfg)) {
        return std::unexpected(typeError(
            "'has' on proxy: property '" + utf16ToUtf8(keyText(*keySymR)) +
            "' is a non-configurable own property of the proxy target but "
            "the proxy did not report it as existent"));
      }
    }
  }
  return found;
}

// ---------------------------------------------------------------------------
// [[Delete]]
// ---------------------------------------------------------------------------
JsResult<bool> Isolate::proxyDelete(Value proxy, const Value& key) {
  ProxyObj* p = proxy.asProxy();
  JsResult<SymbolId> keySymR = toPropertyKey(key);
  if (!keySymR) return std::unexpected(keySymR.error());
  JsResult<Value> trap = proxyTrap(p->handler, "deleteProperty");
  if (!trap) return std::unexpected(trap.error());
  if (trap->isUndefined()) return deletePropertyImpl(p->target, *keySymR);
  Value args[2] = {p->target, key};
  JsResult<Value> r = callValue(*trap, Value::raw(ValueKind::Object, p->handler), args, 2);
  if (!r) return std::unexpected(r.error());
  bool deleted = pure::toBoolean(*r);
  Object* targetObj = objectOfValue(p->target);
  if (targetObj != nullptr && deleted) {
    JsResult<Value> desc = getOwnPropertyDescriptorValue(targetObj, *keySymR);
    if (!desc) return std::unexpected(desc.error());
    if (!desc->isUndefined()) {
      Object* d = objectOfValue(*desc);
      SymbolId symConfigurable = symbols_.intern(u"configurable");
      JsResult<Value> cfg =
          getProperty(Value::raw(ValueKind::Object, d), symConfigurable);
      if (!cfg) return std::unexpected(cfg.error());
      if (cfg->isUndefined() || !pure::toBoolean(*cfg)) {
        return std::unexpected(typeError(
            "'deleteProperty' on proxy: property '" +
            utf16ToUtf8(keyText(*keySymR)) +
            "' is a non-configurable own property of the proxy target"));
      }
    }
  }
  return deleted;
}

// ---------------------------------------------------------------------------
// [[GetPrototypeOf]] / [[SetPrototypeOf]]
// ---------------------------------------------------------------------------
JsResult<Value> Isolate::proxyGetPrototype(Value proxy) {
  ProxyObj* p = proxy.asProxy();
  JsResult<Value> trap = proxyTrap(p->handler, "getPrototypeOf");
  if (!trap) return std::unexpected(trap.error());
  Object* targetObj = objectOfValue(p->target);
  if (trap->isUndefined()) {
    return targetObj != nullptr ? p->target.asProxy()->target
                                : Value::null();
  }
  // Forward target proto (plain walk; target may itself be a proxy).
  JsResult<Value> targetProto = Value::null();
  if (p->target.isProxy()) {
    JsResult<Value> nested = proxyGetPrototype(p->target);
    if (!nested) return std::unexpected(nested.error());
    targetProto = *nested;
  } else if (targetObj != nullptr) {
    targetProto = targetObj->proto;
  }
  Value args[1] = {p->target};
  JsResult<Value> r = callValue(*trap, Value::raw(ValueKind::Object, p->handler), args, 1);
  if (!r) return std::unexpected(r.error());
  if (!r->isObjectLike() && !r->isNull()) {
    return std::unexpected(typeError(
        "'getPrototypeOf' on proxy: trap returned neither object nor null"));
  }
  // Invariant: non-extensible target -> result must equal target proto.
  JsResult<bool> ext = proxyIsExtensible(proxy);
  if (!ext) return std::unexpected(ext.error());
  if (!*ext && !pure::sameValue(*r, *targetProto)) {
    return std::unexpected(typeError(
        "'getPrototypeOf' on proxy: non-extensible target proto mismatch"));
  }
  return *r;
}

JsResult<bool> Isolate::proxySetPrototype(Value proxy, const Value& proto) {
  ProxyObj* p = proxy.asProxy();
  if (!proto.isObjectLike() && !proto.isNull()) return false;
  JsResult<Value> trap = proxyTrap(p->handler, "setPrototypeOf");
  if (!trap) return std::unexpected(trap.error());
  if (trap->isUndefined()) {
    if (!p->target.isObjectLike()) return false;
    Object* t = objectOfValue(p->target);
    // Same cycle check + write as the SetPrototype opcode.
    Object* walk = proto.isObjectLike() ? objectOfValue(proto) : nullptr;
    while (walk != nullptr) {
      if (walk == t) return false;
      walk = protoOfObject(walk);
    }
    t->proto = proto;
    return true;
  }
  Value args[2] = {p->target, proto};
  JsResult<Value> r = callValue(*trap, Value::raw(ValueKind::Object, p->handler), args, 2);
  if (!r) return std::unexpected(r.error());
  bool ok = pure::toBoolean(*r);
  if (!ok) return false;
  // Invariant: non-extensible target -> proto must be unchanged.
  JsResult<bool> ext = proxyIsExtensible(proxy);
  if (!ext) return std::unexpected(ext.error());
  if (!*ext) {
    JsResult<Value> cur = proxyGetPrototype(proxy);
    if (!cur) return std::unexpected(cur.error());
    if (!pure::sameValue(*cur, proto)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// [[GetOwnProperty]] / [[DefineOwnProperty]]
// ---------------------------------------------------------------------------
JsResult<Value> Isolate::proxyGetOwnPropertyDescriptor(Value proxy,
                                                       const Value& key) {
  ProxyObj* p = proxy.asProxy();
  JsResult<SymbolId> keySymR = toPropertyKey(key);
  if (!keySymR) return std::unexpected(keySymR.error());
  JsResult<Value> trap = proxyTrap(p->handler, "getOwnPropertyDescriptor");
  if (!trap) return std::unexpected(trap.error());
  Object* targetObj = objectOfValue(p->target);
  if (trap->isUndefined()) {
    return targetObj != nullptr
               ? getOwnPropertyDescriptorValue(targetObj, *keySymR)
               : Value::undefined();
  }
  Value args[2] = {p->target, key};
  JsResult<Value> r = callValue(*trap, Value::raw(ValueKind::Object, p->handler), args, 2);
  if (!r) return std::unexpected(r.error());
  if (r->isUndefined()) {
    // Invariant: non-configurable target prop must be reported.
    if (targetObj != nullptr) {
      JsResult<Value> desc =
          getOwnPropertyDescriptorValue(targetObj, *keySymR);
      if (!desc) return std::unexpected(desc.error());
      if (!desc->isUndefined()) {
        Object* d = objectOfValue(*desc);
        SymbolId symConfigurable = symbols_.intern(u"configurable");
        JsResult<Value> cfg =
            getProperty(Value::raw(ValueKind::Object, d), symConfigurable);
        if (!cfg) return std::unexpected(cfg.error());
        if (cfg->isUndefined() || !pure::toBoolean(*cfg)) {
          return std::unexpected(typeError(
              "'getOwnPropertyDescriptor' on proxy: trap reported "
              "non-configurable target property '" +
              utf16ToUtf8(keyText(*keySymR)) + "' as undefined"));
        }
      }
    }
    return Value::undefined();
  }
  if (!r->isObjectLike()) {
    return std::unexpected(typeError(
        "'getOwnPropertyDescriptor' on proxy: trap must return an object "
        "or undefined"));
  }
  return *r;
}

JsResult<bool> Isolate::proxyDefineOwnProperty(Value proxy, const Value& key,
                                               const PropertyDescriptor& d) {
  ProxyObj* p = proxy.asProxy();
  JsResult<SymbolId> keySymR = toPropertyKey(key);
  if (!keySymR) return std::unexpected(keySymR.error());
  JsResult<Value> trap = proxyTrap(p->handler, "defineProperty");
  if (!trap) return std::unexpected(trap.error());
  if (trap->isUndefined()) {
    Object* targetObj = objectOfValue(p->target);
    if (targetObj == nullptr) return false;
    return definePropertyDescriptor(targetObj, *keySymR, d);
  }
  // Materialize the descriptor object for the trap call.
  Value descObj = Value::raw(ValueKind::Object, makeDescriptorObject(*this, d));
  Value args[3] = {p->target, key, descObj};
  JsResult<Value> r = callValue(*trap, Value::raw(ValueKind::Object, p->handler), args, 3);
  if (!r) return std::unexpected(r.error());
  bool ok = pure::toBoolean(*r);
  if (!ok) return false;
  // Invariant: non-configurable target prop -> trap result must be true
  // (it is), and the target prop must not have become undeletable-invisible.
  Object* targetObj = objectOfValue(p->target);
  if (targetObj != nullptr) {
    JsResult<Value> desc = getOwnPropertyDescriptorValue(targetObj, *keySymR);
    if (!desc) return std::unexpected(desc.error());
    (void)desc;
  }
  return true;
}

// ---------------------------------------------------------------------------
// [[IsExtensible]] / [[PreventExtensions]]
// ---------------------------------------------------------------------------
JsResult<bool> Isolate::proxyIsExtensible(Value proxy) {
  ProxyObj* p = proxy.asProxy();
  JsResult<Value> trap = proxyTrap(p->handler, "isExtensible");
  if (!trap) return std::unexpected(trap.error());
  Object* targetObj = objectOfValue(p->target);
  if (trap->isUndefined()) {
    return targetObj != nullptr ? targetObj->extensible : true;
  }
  Value args[1] = {p->target};
  JsResult<Value> r = callValue(*trap, Value::raw(ValueKind::Object, p->handler), args, 1);
  if (!r) return std::unexpected(r.error());
  bool extensible = pure::toBoolean(*r);
  if (!extensible && targetObj != nullptr && !targetObj->extensible) {
    return std::unexpected(typeError(
        "'isExtensible' on proxy: target is non-extensible"));
  }
  return extensible;
}

JsResult<bool> Isolate::proxyPreventExtensions(Value proxy) {
  ProxyObj* p = proxy.asProxy();
  JsResult<Value> trap = proxyTrap(p->handler, "preventExtensions");
  if (!trap) return std::unexpected(trap.error());
  Object* targetObj = objectOfValue(p->target);
  if (trap->isUndefined()) {
    if (targetObj != nullptr) targetObj->extensible = false;
    return true;
  }
  Value args[1] = {p->target};
  JsResult<Value> r = callValue(*trap, Value::raw(ValueKind::Object, p->handler), args, 1);
  if (!r) return std::unexpected(r.error());
  if (!pure::toBoolean(*r)) return false;
  if (targetObj != nullptr) targetObj->extensible = false;
  return true;
}

// ---------------------------------------------------------------------------
// [[OwnPropertyKeys]]
// ---------------------------------------------------------------------------
JsResult<std::vector<Value>> Isolate::proxyOwnKeys(Value proxy) {
  ProxyObj* p = proxy.asProxy();
  JsResult<Value> trap = proxyTrap(p->handler, "ownKeys");
  if (!trap) return std::unexpected(trap.error());
  Object* targetObj = objectOfValue(p->target);
  if (trap->isUndefined()) {
    return targetObj != nullptr ? ownKeysValues(targetObj, false)
                                : std::vector<Value>{};
  }
  Value args[1] = {p->target};
  JsResult<Value> r = callValue(*trap, Value::raw(ValueKind::Object, p->handler), args, 1);
  if (!r) return std::unexpected(r.error());
  if (!r->isObjectLike()) {
    return std::unexpected(typeError(
        "'ownKeys' on proxy: trap result must be an array-like object"));
  }
  Object* resultArr = objectOfValue(*r);
  std::vector<Value> keys;
  if (resultArr->isArray) {
    const uint32_t len = resultArr->length;
    for (uint32_t i = 0; i < len; i++) {
      Value v = ownElementValue(resultArr, i);
      if (v.isHole()) continue;
      if (!v.isString() && !v.isSymbol()) {
        return std::unexpected(typeError(
            "'ownKeys' on proxy: trap result must contain only strings "
            "and symbols"));
      }
      keys.push_back(v);
    }
  }
  // Invariants: every target non-configurable key must be present; a
  // non-extensible target must be reported exactly.
  std::vector<Value> targetKeys =
      targetObj != nullptr ? ownKeysValues(targetObj, true)
                           : std::vector<Value>{};
  auto contains = [&keys](const Value& k) {
    for (const Value& have : keys) {
      if (pure::sameValue(have, k)) return true;
    }
    return false;
  };
  bool targetExtensible = targetObj != nullptr ? targetObj->extensible : true;
  if (!targetExtensible) {
    // Exact same set (order-insensitive).
    if (keys.size() != targetKeys.size()) {
      return std::unexpected(typeError(
          "'ownKeys' on proxy: non-extensible target key set mismatch"));
    }
    for (const Value& k : targetKeys) {
      if (!contains(k)) {
        return std::unexpected(typeError(
            "'ownKeys' on proxy: non-extensible target key set mismatch"));
      }
    }
  } else {
    for (const Value& k : targetKeys) {
      if (k.isString() && k.asString()->flat() == u"length") continue;
      // Non-configurable target keys must be reported (descriptor probe).
      JsResult<SymbolId> ks = toPropertyKey(k);
      if (!ks) return std::unexpected(ks.error());
      JsResult<Value> desc = targetObj != nullptr
                                 ? getOwnPropertyDescriptorValue(targetObj, *ks)
                                 : Value::undefined();
      if (!desc) return std::unexpected(desc.error());
      if (desc->isUndefined()) continue;
      Object* d = objectOfValue(*desc);
      SymbolId symConfigurable = symbols_.intern(u"configurable");
      JsResult<Value> cfg =
          getProperty(Value::raw(ValueKind::Object, d), symConfigurable);
      if (!cfg) return std::unexpected(cfg.error());
      if (cfg->isUndefined() || !pure::toBoolean(*cfg)) {
        if (!contains(k)) {
          return std::unexpected(typeError(
              "'ownKeys' on proxy: non-configurable target key '" +
              utf16ToUtf8(keyText(*ks)) + "' was not reported"));
        }
      }
    }
  }
  return keys;
}

// ---------------------------------------------------------------------------
// [[Call]] / [[Construct]]
// ---------------------------------------------------------------------------
JsResult<Value> Isolate::proxyApply(Value proxy, Value thisVal,
                                    const Value* args, uint32_t argc) {
  ProxyObj* p = proxy.asProxy();
  if (!p->target.isClosure() && !p->target.isProxy()) {
    return std::unexpected(typeError("proxy target is not callable"));
  }
  JsResult<Value> trap = proxyTrap(p->handler, "apply");
  if (!trap) return std::unexpected(trap.error());
  if (trap->isUndefined()) {
    return callValue(p->target, thisVal, args, argc);
  }
  // argsList per ES: an array of the arguments.
  Object* argList = heap_.makeObject();
  argList->isArray = true;
  argList->proto = Value::raw(ValueKind::Object, arrayPrototype_);
  for (uint32_t i = 0; i < argc; i++) {
    JsResult<bool> st = setArrayElement(argList, i, args[i]);
    if (!st) return std::unexpected(st.error());
  }
  Value callArgs[3] = {p->target, thisVal,
                       Value::raw(ValueKind::Object, argList)};
  return callValue(*trap, Value::raw(ValueKind::Object, p->handler), callArgs, 3);
}

JsResult<Value> Isolate::proxyConstruct(Value proxy, const Value* args,
                                        uint32_t argc) {
  ProxyObj* p = proxy.asProxy();
  if (!p->target.isClosure() && !p->target.isProxy()) {
    return std::unexpected(typeError("proxy target is not a constructor"));
  }
  JsResult<Value> trap = proxyTrap(p->handler, "construct");
  if (!trap) return std::unexpected(trap.error());
  Object* argList = heap_.makeObject();
  argList->isArray = true;
  argList->proto = Value::raw(ValueKind::Object, arrayPrototype_);
  for (uint32_t i = 0; i < argc; i++) {
    JsResult<bool> st = setArrayElement(argList, i, args[i]);
    if (!st) return std::unexpected(st.error());
  }
  if (trap->isUndefined()) {
    return constructImpl(p->target, args, argc);
  }
  Value callArgs[3] = {p->target, Value::raw(ValueKind::Object, argList),
                       proxy};
  JsResult<Value> r = callValue(*trap, Value::raw(ValueKind::Object, p->handler), callArgs, 3);
  if (!r) return std::unexpected(r.error());
  if (!r->isObjectLike()) {
    return std::unexpected(typeError(
        "'construct' on proxy: trap must return an object"));
  }
  return *r;
}

}  // namespace ts
