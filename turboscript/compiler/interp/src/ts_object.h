// TurboScript — heap object model: interned symbols, shapes (hidden classes
// with transition tree), property storage, closures, contexts, accessors,
// and the arena heap. Rules 7 (arena ownership), 16 (interned symbols),
// 32 (bitmask attrs), 71 (versioned shape identity), 72 (identity semantics).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "ts_core.h"
#include "ts_value.h"

namespace ts {

// SymbolId/kInvalidSymbol now live in ts_core.h (shared with ts_value.h).

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
// Proxy — v0.3 exotic object (ES ch. 10.5: Proxy Object Internal Methods).
// `target` is any value (object, closure, or another proxy — nested proxies
// recurse); `handler` must be a plain Object. Every one of the 13 internal
// methods routes through the trap protocol in ts_proxy.cpp.
// ---------------------------------------------------------------------------
struct ProxyObj {
  Value target;
  Object* handler = nullptr;
};

// Out-of-line member (ProxyObj lives here; Value lives in ts_value.h).
inline ProxyObj* Value::asProxy() const {
  return static_cast<ProxyObj*>(asPtr());
}

// ---------------------------------------------------------------------------
// Closure — a function value. A closure IS an object for property purposes:
// its `asObject` carries .prototype (fresh object per closure) etc.
// ---------------------------------------------------------------------------
struct Function;  // ts_module.h (resolved at closure creation, v0.5)

struct Closure {
  uint32_t funcIndex = 0;
  Context* context = nullptr;
  Object* asObject = nullptr;  // function-object property storage (never null)
  // v0.5 call path (benchmarks_v0.4.md register #2): the resolved Function
  // pointer, cached at creation. callClosure no longer walks the module
  // function table (unique_ptr indirection + bounds check) per call.
  const struct Function* fn = nullptr;
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
// v0.6 layout (benchmarks_v0.5.md register #1): the sparse map is OUT OF
// LINE (unique_ptr) — it exists only for arrays storing indices >=
// kMaxDenseElements, so plain objects (the dominant allocation in
// property-heavy kernels) no longer construct/carry a 48-byte std::map.
// ---------------------------------------------------------------------------
struct Object {
  Shape* shape = nullptr;       // root shape => empty instance
  Value proto = Value::null();  // object value (Object or Closure) or Null
  std::vector<Value> slots;     // size == shape->slotCount (Hole = deleted)
  bool extensible = true;       // [[Extensible]] (v0.3: Proxy/preventExtensions)

  // --- Array state (meaningful iff isArray) ---
  bool isArray = false;
  ElementsKind elementsKind = ElementsKind::PackedSmi;
  uint32_t length = 0;  // exotic "length" property value
  // Dense element storage; indices >= kMaxDenseElements live in `sparse`.
  // Hole values are real holes (prototype-chain lookups); size <=
  // kMaxDenseElements. Growth via length writes does NOT allocate here.
  std::vector<Value> elements;
  // Sparse element storage (ordered: deterministic, Rule 124). Allocated on
  // first sparse write; never allocated for dense-only arrays or plain objs.
  std::unique_ptr<std::map<uint32_t, Value>> sparse;

  // Sparse accessor for readers: an empty static map when unallocated
  // (single-threaded isolate, Rule 119; reference stability irrelevant for
  // read-only use).
  [[nodiscard]] const std::map<uint32_t, Value>& sparseMap() const;
  // Sparse accessor for writers: allocates the map on first use.
  [[nodiscard]] std::map<uint32_t, Value>& ensureSparse();

  // Own-slot lookup along the shape parent chain; -1 when absent.
  [[nodiscard]] int32_t findOwnSlot(SymbolId key) const;
};

// Object|Closure view over a value; nullptr when not an object.
[[nodiscard]] Object* objectOfValue(const Value& v);

// ---------------------------------------------------------------------------
// BumpArena — segment-chained bump allocator (v0.6, benchmarks_v0.5.md
// register #1). Same ownership contract as the deques it replaces: nothing
// is ever collected and element addresses are stable for the Isolate
// lifetime (bytecode_spec.md Section 11). Allocation is a pointer bump plus
// placement-new; segments are chained unique_ptrs so elements never move.
// ---------------------------------------------------------------------------
template <typename T>
class BumpArena {
 public:
  // Rule 23 (named constant): objects per segment. 256 x sizeof(Object)
  // ~= 20KB segments — large enough that segment churn is negligible,
  // small enough that memory floor is bounded.
  static constexpr size_t kPerSegment = 256;
  // Segment storage comes from default new[] (max_align_t-aligned).
  static_assert(alignof(T) <= alignof(std::max_align_t),
                "BumpArena segments assume max_align_t-aligned storage");

  BumpArena() { addSegment(); }
  ~BumpArena() {
    // Destroy constructed elements: every segment before the last is full,
    // the last holds liveInSegment_ constructed objects.
    for (size_t s = 0; s < segments_.size(); ++s) {
      const size_t count = s + 1 == segments_.size() ? liveInSegment_ : kPerSegment;
      T* base = reinterpret_cast<T*>(segments_[s].get());
      for (size_t i = count; i > 0; --i) base[i - 1].~T();
    }
  }
  BumpArena(const BumpArena&) = delete;
  BumpArena& operator=(const BumpArena&) = delete;

  template <typename... Args>
  [[nodiscard]] T* construct(Args&&... args) {
    if (liveInSegment_ == kPerSegment) addSegment();
    T* p = reinterpret_cast<T*>(segments_.back().get() +
                                liveInSegment_ * sizeof(T));
    new (p) T(std::forward<Args>(args)...);
    ++liveInSegment_;
    return p;
  }

  [[nodiscard]] uint64_t constructed() const {
    return static_cast<uint64_t>(segments_.size() - 1) * kPerSegment +
           liveInSegment_;
  }

 private:
  void addSegment() {
    // std::byte arrays from default new[] carry max_align_t alignment
    // (>= alignof(Object)); pinned below (Rule 105 discipline).
    segments_.push_back(std::unique_ptr<std::byte[]>(
        new std::byte[sizeof(T) * kPerSegment]));
    liveInSegment_ = 0;
  }
  std::vector<std::unique_ptr<std::byte[]>> segments_;
  size_t liveInSegment_ = 0;
};

// ---------------------------------------------------------------------------
// Heap — arena ownership. v0.1 never collects (documented divergence,
// bytecode_spec.md Section 11); addresses stay valid for the Isolate
// lifetime. Typed deques give stable element addresses; v0.6: Objects come
// from a bump arena (BumpArena<Object>, same stability contract).
// ---------------------------------------------------------------------------
class Heap {
 public:
  [[nodiscard]] StringObj* makeString(std::u16string data) {
    strings_.emplace_back(std::move(data));
    return &strings_.back();
  }
  // v0.6 cons-string (benchmarks_v0.5.md register #2): a concatenation is
  // one header node referencing its operands; text materializes lazily via
  // StringObj::flat(). Results shorter than kMinConsLength build flat
  // eagerly. Overflow of kMaxStringCodeUnits returns nullptr — call sites
  // raise the RangeError (Rule 74: JS exceptions are values, and the Heap
  // cannot raise).
  [[nodiscard]] StringObj* makeCons(StringObj* left, StringObj* right) {
    const uint64_t combined =
        static_cast<uint64_t>(left->length) + static_cast<uint64_t>(right->length);
    if (combined > kMaxStringCodeUnits) return nullptr;
    if (combined < kMinConsLength) {
      std::u16string out;
      out.reserve(combined);
      out.append(left->flat());
      out.append(right->flat());
      return makeString(std::move(out));
    }
    strings_.emplace_back(StringObj::kCons, left, right,
                          static_cast<uint32_t>(combined));
    return &strings_.back();
  }
  [[nodiscard]] BigInt* makeBigInt(BigInt v) {
    bigints_.push_back(std::move(v));
    return &bigints_.back();
  }
  [[nodiscard]] Object* makeObject() { return objects_.construct(); }
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
  [[nodiscard]] SymbolObj* makeSymbol(std::u16string desc, uint32_t uniqueId) {
    symbols_.emplace_back();
    symbols_.back().desc = std::move(desc);
    symbols_.back().uniqueId = uniqueId;
    return &symbols_.back();
  }
  [[nodiscard]] ProxyObj* makeProxy(Value target, Object* handler) {
    proxies_.emplace_back();
    proxies_.back().target = target;
    proxies_.back().handler = handler;
    return &proxies_.back();
  }

  [[nodiscard]] uint64_t allocationCount() const {
    return strings_.size() + bigints_.size() + objects_.constructed() +
           contexts_.size() + closures_.size() + accessors_.size() +
           symbols_.size() + proxies_.size();
  }

 private:
  // Strings: deque (stable addresses; flat nodes carry their payload, cons
  // nodes are tiny headers). deque<StringObj> remains the owner of every
  // node, including cons operands (cons links are non-owning).
  std::deque<StringObj> strings_;
  std::deque<BigInt> bigints_;
  BumpArena<Object> objects_;
  std::deque<Context> contexts_;
  std::deque<Closure> closures_;
  std::deque<AccessorPair> accessors_;
  std::deque<SymbolObj> symbols_;
  std::deque<ProxyObj> proxies_;
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

// v0.5 slot growth (kMinSlotCapacity, ts_core.h): geometric reserve before a
// transition push. Pure storage strategy — slotCount is shape-driven and the
// IC/lookup paths read shape->slotCount, never capacity. Shared by
// Isolate::defineProperty AND the dispatch transition-IC lane (both push
// fresh slots; Rule 96: no semantic surface).
inline void reserveForSlotPush(std::vector<Value>& slots) {
  if (slots.capacity() == slots.size()) {
    slots.reserve(slots.size() < kMinSlotCapacity ? kMinSlotCapacity
                                                  : slots.size() * 2);
  }
}

// Find `key` starting at `start`, walking the prototype chain (named
// properties only; array elements/length are handled by the Isolate).
[[nodiscard]] LookupResult lookupProperty(Object* start, SymbolId key);

// Own-property lookup on a single object (named properties only; no chain
// walk, no array exotic behavior).
[[nodiscard]] LookupResult lookupOwnProperty(Object* obj, SymbolId key);

// IC helper (v0.3): slot index of an own, present, DATA property named
// `key`, with its attribute byte; -1 when absent, deleted, or an accessor.
[[nodiscard]] int32_t ownDataSlotAttrs(Object* obj, SymbolId key,
                                       PropertyAttrs* attrsOut);

// Canonical array-index test (ECMA-262: canonical numeric strings, values
// 0..2^32-2; "00" and "4294967295" are NOT array indices).
[[nodiscard]] bool arrayIndexFromKey(std::u16string_view key, uint32_t* out);

}  // namespace ts
