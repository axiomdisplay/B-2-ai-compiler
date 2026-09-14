// TurboScript — object model implementation: shape transitions, property
// lookup, object views over values. Semantics per ECMA-262.
#include "ts_object.h"

namespace ts {

Object* objectOfValue(const Value& v) {
  if (v.kind == ValueKind::Object) {
    return static_cast<Object*>(v.ptr);
  }
  if (v.kind == ValueKind::Closure) {
    return static_cast<Closure*>(v.ptr)->asObject;
  }
  return nullptr;
}

int32_t Object::findOwnSlot(SymbolId key) const {
  Shape* s = shape;
  while (s != nullptr) {
    if (s->key == key) return static_cast<int32_t>(s->slot);
    s = s->parent;
  }
  return -1;
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
bool classifyOwn(Object* holder, SymbolId key, Value* slot,
                 LookupResult* out) {
  if (slot->isHole()) return false;  // deleted: invisible
  out->found = true;
  out->holder = holder;
  Shape* s = holder->shape;
  while (s != nullptr && s->key != key) s = s->parent;
  if (s != nullptr && s->attrs.has(PropAttr::IsAccessor)) {
    out->accessor = static_cast<AccessorPair*>(slot->ptr);
  } else {
    out->dataSlot = slot;
  }
  return true;
}

}  // namespace

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
