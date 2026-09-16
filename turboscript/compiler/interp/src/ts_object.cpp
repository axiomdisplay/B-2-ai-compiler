// TurboScript — object model implementation: shape transitions, property
// lookup, object views over values. Semantics per ECMA-262.
#include "ts_object.h"

namespace ts {

Object* objectOfValue(const Value& v) {
  if (v.kind() == ValueKind::Object) {
    return static_cast<Object*>(v.asPtr());
  }
  if (v.kind() == ValueKind::Closure) {
    return static_cast<Closure*>(v.asPtr())->asObject;
  }
  return nullptr;
}

const std::map<uint32_t, Value>& Object::sparseMap() const {
  static const std::map<uint32_t, Value> kEmpty;  // read-only shared empty
  return sparse != nullptr ? *sparse : kEmpty;
}

std::map<uint32_t, Value>& Object::ensureSparse() {
  if (sparse == nullptr) sparse = std::make_unique<std::map<uint32_t, Value>>();
  return *sparse;
}

int32_t Object::findOwnSlot(SymbolId key) const {
  Shape* s = shape;
  while (s != nullptr) {
    if (s->key == key) return static_cast<int32_t>(s->slot);
    s = s->parent;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Canonical array-index test. "0" is an index; "00", "+0", "-0" and
// "4294967295" (2^32-1) are not (ECMA-262 ArrayCreate / canonical numeric
// string). Inputs up to 10 digits; overflow rejected during accumulation.
// ---------------------------------------------------------------------------
bool arrayIndexFromKey(std::u16string_view key, uint32_t* out) {
  if (key.empty() || key.size() > 10) return false;
  if (key.size() > 1 && key[0] == u'0') return false;  // no leading zeros
  uint64_t v = 0;
  for (char16_t c : key) {
    if (c < u'0' || c > u'9') return false;
    v = v * 10 + static_cast<uint64_t>(c - u'0');
    if (v > 0xFFFFFFFFull) return false;
  }
  if (v == kMaxArrayLength) return false;  // 2^32-1 is not an array index
  *out = static_cast<uint32_t>(v);
  return true;
}

Shape* ShapeTree::transition(Shape* from, SymbolId key, PropertyAttrs attrs) {
  auto mapKey = std::make_tuple(from->id, key, attrs.raw());
  auto it = transitions_.find(mapKey);
  if (it != transitions_.end()) return it->second;

  Shape* next = &shapes_.emplace_back(Shape{});
  next->parent = from;
  next->key = key;
  next->attrs = attrs;
  next->slot = from->slotCount;
  next->slotCount = from->slotCount + 1;
  next->id = nextId_++;
  transitions_.emplace(std::move(mapKey), next);
  return next;
}

namespace {

// Proto of an object as the next Object in the chain (or nullptr).
Object* protoOf(Object* obj) {
  return obj->proto.isObjectLike() ? objectOfValue(obj->proto) : nullptr;
}

// Classify a found own slot: accessor or data, honoring Hole semantics.
// (Shared by lookupOwnProperty and lookupProperty.)
bool classifyOwn(Object* holder, SymbolId key, Value* slot,
                 LookupResult* out) {
  if (slot->isHole()) return false;  // deleted: invisible
  out->found = true;
  out->holder = holder;
  Shape* s = holder->shape;
  while (s != nullptr && s->key != key) s = s->parent;
  if (s != nullptr && s->attrs.has(PropAttr::IsAccessor)) {
    out->accessor = static_cast<AccessorPair*>(slot->asPtr());
  } else {
    out->dataSlot = slot;
  }
  return true;
}

}  // namespace

LookupResult lookupOwnProperty(Object* obj, SymbolId key) {
  LookupResult result;
  int32_t slot = obj->findOwnSlot(key);
  if (slot >= 0) {
    classifyOwn(obj, key, &obj->slots[static_cast<size_t>(slot)], &result);
  }
  return result;
}

int32_t ownDataSlotAttrs(Object* obj, SymbolId key, PropertyAttrs* attrsOut) {
  int32_t slot = obj->findOwnSlot(key);
  if (slot < 0) return -1;
  if (obj->slots[static_cast<size_t>(slot)].isHole()) return -1;  // deleted
  Shape* s = obj->shape;
  while (s != nullptr && s->key != key) s = s->parent;
  if (s == nullptr) return -1;  // invariant break: slot without shape node
  if (s->attrs.has(PropAttr::IsAccessor)) return -1;
  if (attrsOut != nullptr) *attrsOut = s->attrs;
  return slot;
}

LookupResult lookupProperty(Object* start, SymbolId key) {
  LookupResult result;
  Object* current = start;
  while (current != nullptr) {
    int32_t slot = current->findOwnSlot(key);
    if (slot >= 0) {
      if (classifyOwn(current, key, &current->slots[static_cast<size_t>(slot)],
                      &result)) {
        return result;
      }
      // Own slot exists but was deleted: continue up the chain.
    }
    current = protoOf(current);
  }
  return result;
}

}  // namespace ts
