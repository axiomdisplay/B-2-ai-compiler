// TurboScript — heap object model: interned symbols, shapes (hidden classes
// with transition tree), property storage, closures, contexts, accessors,
// and the arena heap. Rules 7 (arena ownership), 16 (interned symbols),
// 32 (bitmask attrs), 71 (versioned shape identity), 72 (identity semantics).
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "ts_core.h"
#include "ts_value.h"

namespace ts {

using SymbolId = uint32_t;
constexpr SymbolId kInvalidSymbol = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// SymbolTable — Rule 16: every identifier/property key is interned once.
// ---------------------------------------------------------------------------
class SymbolTable {
 public:
  [[nodiscard]] SymbolId intern(std::u16string_view text) {
    auto it = map_.find(std::u16string(text));
    if (it != map_.end()) return it->second;
    SymbolId id = static_cast<SymbolId>(texts_.size());
    texts_.push_back(std::u16string(text));
    map_.emplace(std::u16string(text), id);
    return id;
  }
  [[nodiscard]] const std::u16string& text(SymbolId id) const {
    return texts_[id];
  }
  [[nodiscard]] uint32_t size() const {
    return static_cast<uint32_t>(texts_.size());
  }

 private:
  std::vector<std::u16string> texts_;
  std::unordered_map<std::u16string, SymbolId> map_;
};

// ---------------------------------------------------------------------------
// Property attributes (Rule 32 bitmask).
// ---------------------------------------------------------------------------
using PropertyAttrs = Flags<PropAttr>;

constexpr PropertyAttrs kDefaultDataAttrs =
    PropertyAttrs(PropAttr::Writable) | PropertyAttrs(PropAttr::Enumerable) |
    PropertyAttrs(PropAttr::Configurable);

// ---------------------------------------------------------------------------
// Shape — hidden class: a node in the per-isolate transition tree.
// Shapes are immutable once published (Rule 71); transitions add nodes.
// ---------------------------------------------------------------------------
struct Shape {
  Shape* parent = nullptr;
  SymbolId key = kInvalidSymbol;  // key added by this link (root: invalid)
  PropertyAttrs attrs{};          // attrs of that property
  uint32_t slot = 0;              // slot index of `key` in instances
  uint32_t slotCount = 0;         // total slots for instances of this shape
  uint32_t id = 0;                // stable per-isolate id (IC feedback)
};

// ---------------------------------------------------------------------------
// AccessorPair — stored in a slot when attrs.has(IsAccessor).
// ---------------------------------------------------------------------------
struct AccessorPair {
  Value getter = Value::undefined();
  Value setter = Value::undefined();
};

// ---------------------------------------------------------------------------
// Context — closure capture environment. Cells start as Hole (TDZ-style).
// ---------------------------------------------------------------------------
struct Context {
  Context* parent = nullptr;
  std::vector<Value> cells;
};

struct Object;

// ---------------------------------------------------------------------------
// Closure — a function value. A closure IS an object for property purposes:
// its `asObject` carries .prototype (fresh object per closure) etc.
// ---------------------------------------------------------------------------
struct Closure {
  uint32_t funcIndex = 0;
  Context* context = nullptr;
  Object* asObject = nullptr;  // function-object property storage (never null)
};

// ---------------------------------------------------------------------------
// Elements kinds (v0.2 arrays). Order is load-bearing: packed->holey is
// `kind | 1`; smi->double is `+2` from the smi kinds; ->tagged is `+2` from
// the double kinds and `+4` from the smi kinds (widenFor in
// ts_interpreter.cpp asserts this layout).
// ---------------------------------------------------------------------------
enum class ElementsKind : uint8_t {
  PackedSmi = 0,
  HoleySmi = 1,
  PackedDouble = 2,
  HoleyDouble = 3,
  PackedTagged = 4,
  HoleyTagged = 5,
};

[[nodiscard]] constexpr ElementsKind holeyOf(ElementsKind k) {
  return static_cast<ElementsKind>(static_cast<uint8_t>(k) | 1);
}

// ---------------------------------------------------------------------------
// Object — shape-based property storage + prototype. v0.2: an Object may be
// an exotic Array (isArray): then `length` is the array-length property
// (writable, non-enumerable, non-configurable) and `elements`/`sparse` hold
// indexed storage per `elementsKind`. Non-array objects leave these unused.
// Design note (interp_contract.md 3): Tier 0 keeps ONE tagged element
// backend; kinds are enforced as invariants on every store and transitioned
// eagerly (smi -> double -> tagged, packed -> holey). Unboxed backends are a
// stencil-layer (Tier 1) concern, not a semantic one.
// ---------------------------------------------------------------------------
struct Object {
  Shape* shape = nullptr;       // root shape => empty instance
  Value proto = Value::null();  // object value (Object or Closure) or Null
  std::vector<Value> slots;     // size == shape->slotCount (Hole = deleted)

  // --- Array state (meaningful iff isArray) ---
  bool isArray = false;
  ElementsKind elementsKind = ElementsKind::PackedSmi;
  uint32_t length = 0;  // exotic "length" property value
  // Dense element storage; indices >= kMaxDenseElements live in `sparse`.
  // Hole values are real holes (prototype-chain lookups); size <=
  // kMaxDenseElements. Growth via length writes does NOT allocate here.
  std::vector<Value> elements;
  std::map<uint32_t, Value> sparse;  // ordered: deterministic (Rule 124)

  // Own-slot lookup along the shape parent chain; -1 when absent.
  [[nodiscard]] int32_t findOwnSlot(SymbolId key) const;
};

// Object|Closure view over a value; nullptr when not an object.
[[nodiscard]] Object* objectOfValue(const Value& v);

// ---------------------------------------------------------------------------
// Heap — arena ownership. v0.1 never collects (documented divergence,
// bytecode_spec.md Section 11); addresses stay valid for the Isolate
// lifetime. Typed deques give stable element addresses.
// ---------------------------------------------------------------------------
class Heap {
 public:
  [[nodiscard]] StringObj* makeString(std::u16string data) {
    strings_.emplace_back(std::move(data));
    return &strings_.back();
  }
  [[nodiscard]] BigInt* makeBigInt(BigInt v) {
    bigints_.push_back(std::move(v));
    return &bigints_.back();
  }
  [[nodiscard]] Object* makeObject() {
    objects_.emplace_back();
    return &objects_.back();
  }
  [[nodiscard]] Context* makeContext(Context* parent, uint32_t cells) {
    contexts_.emplace_back();
    contexts_.back().parent = parent;
    contexts_.back().cells.assign(cells, Value::hole());
    return &contexts_.back();
  }
  [[nodiscard]] Closure* makeClosure() {
    closures_.emplace_back();
    return &closures_.back();
  }
  [[nodiscard]] AccessorPair* makeAccessor() {
    accessors_.emplace_back();
    return &accessors_.back();
  }

  [[nodiscard]] uint64_t allocationCount() const {
    return strings_.size() + bigints_.size() + objects_.size() +
           contexts_.size() + closures_.size() + accessors_.size();
  }

 private:
  std::deque<StringObj> strings_;
  std::deque<BigInt> bigints_;
  std::deque<Object> objects_;
  std::deque<Context> contexts_;
  std::deque<Closure> closures_;
  std::deque<AccessorPair> accessors_;
};

// ---------------------------------------------------------------------------
// ShapeTree — per-isolate transition tree.
// ---------------------------------------------------------------------------
class ShapeTree {
 public:
  explicit ShapeTree(Heap& heap) : heap_(heap) {
    shapes_.emplace_back(Shape{});
    root_ = &shapes_.back();
    root_->id = nextId_++;
  }

  [[nodiscard]] Shape* root() const { return root_; }

  // Follow/add the transition adding `key` with `attrs` to `from`.
  [[nodiscard]] Shape* transition(Shape* from, SymbolId key,
                                  PropertyAttrs attrs);

 private:
  struct ShapeKeyHash {
    size_t operator()(
        const std::tuple<uint32_t, SymbolId, uint8_t>& k) const noexcept {
      size_t h = std::get<0>(k);
      h = h * 1000003u ^ static_cast<size_t>(std::get<1>(k));
      h = h * 1000003u ^ static_cast<size_t>(std::get<2>(k));
      return h;
    }
  };

  Heap& heap_;  // reserved: future shape storage strategy
  std::deque<Shape> shapes_;
  Shape* root_ = nullptr;
  uint32_t nextId_ = 1;
  std::unordered_map<std::tuple<uint32_t, SymbolId, uint8_t>, Shape*,
                     ShapeKeyHash>
      transitions_;
};

// ---------------------------------------------------------------------------
// Property lookup across the prototype chain.
// ---------------------------------------------------------------------------
struct LookupResult {
  bool found = false;
  Value* dataSlot = nullptr;         // data property storage (data hit)
  AccessorPair* accessor = nullptr;  // accessor (accessor hit)
  Object* holder = nullptr;
};

// Find `key` starting at `start`, walking the prototype chain (named
// properties only; array elements/length are handled by the Isolate).
[[nodiscard]] LookupResult lookupProperty(Object* start, SymbolId key);

// Own-property lookup on a single object (named properties only; no chain
// walk, no array exotic behavior).
[[nodiscard]] LookupResult lookupOwnProperty(Object* obj, SymbolId key);

// Canonical array-index test (ECMA-262: canonical numeric strings, values
// 0..2^32-2; "00" and "4294967295" are NOT array indices).
[[nodiscard]] bool arrayIndexFromKey(std::u16string_view key, uint32_t* out);

}  // namespace ts
