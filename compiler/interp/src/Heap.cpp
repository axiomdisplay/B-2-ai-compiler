// B-2 Interpreter - the v0 reference object model storage (Task BE-2).
//
// WHY THIS FILE EXISTS:
// The frozen contract is include/b2/interp/Heap.h: T0's dispatch core
// (Interp.cpp) needs Java-honest object semantics (stable identity, interned
// string literals, reentrant monitors, per-type array zero-fill) behind a
// narrow service seam, and the real runtime/ (class loading, GC, JNI, real
// monitors) does not exist yet. This reference implementation backs that seam
// with a plain object vector. When runtime/ lands it replaces this file
// behind the same header and the dispatch core does not change (charter:
// runtime services "used, never modified").
//
// KEY INVARIANTS (from the frozen header; restated where the code leans on
// them):
// - Ids are stable indices (Rule 15): objs_[i] has id i+1; ObjRef{0} is the
//   invalid/null sentinel. Storage moves on vector growth, ids never do, so
//   ObjRefs survive reallocation (deopt frames copy them freely).
// - One uniform Value-slot array per object: instances store fields in slot
//   order, arrays store elements; 16 bytes per slot. Correctness first (the
//   real heap uses typed storage + GC maps, docs/stencils.md SS8.2).
// - Hot paths (loadField/storeField/loadElem/storeElem/monitorenter/
//   monitorexit) are ALLOCATION-FREE in the steady state. The only
//   allocating paths are object creation and the lazy slot growth below
//   (once per (object, slot) pair, first touch of a field resolved after the
//   object was allocated).
// - No C++ exceptions cross this API (Rule 6): failure is a Bottom Value,
//   false, or a MonFail code; the CALLER turns those into Java traps
//   (Rule 74: exceptions are values).
//
// FROZEN-HEADER GAP (worked around, reported to the integrator):
// Heap has no member to remember the java/lang/String ClassId, yet
// isString()/stringOf() are Heap methods. Every string the v0 runtime
// creates is interned (Runtime routes ALL string allocation through
// internString with one class id), so any single interned entry anchors the
// string class and membership becomes a class-id compare. Strings allocated
// via allocString() before the first intern are not recognized - the Runtime
// never does that. v1 fix: pass stringCls to the Heap constructor, or move
// isString to Runtime (which owns the registry).
//
// CROSS-REFERENCES:
// - docs/rbc_spec.md SS3.13 (arrays), SS3.14 (objects/monitors)
// - docs/laws.md Rules 15, 74; Value.h deopt-state contract

#include "b2/interp/Heap.h"

#include <utility>

namespace b2::interp {
namespace {

// The single v0 "thread" that owns monitors (single-threaded T0; the real
// concurrency contract arrives with the real runtime, Rule 118). A named
// constant because 0 is the "free" sentinel and a bare 1 reads as magic.
constexpr std::uint32_t kOwnerThread = 1;

// Zero value of an array element type. WHY pre-fill with the ELEMENT TYPE's
// zero (not Bottom): the deopt-state contract (Value.h: "locals and
// registers hold exactly the type the verifier proved") extends to array
// slots - an un-written element flowing into a register must already carry
// its verified type or frame reconstruction and state dumps go wrong. JLS
// 15.10.2 pins the same default values.
[[nodiscard]] constexpr Value zeroOf(rbc::RType elem) noexcept {
  switch (elem) {
  case rbc::RType::Int:
    return Value::intVal(0);
  case rbc::RType::Long:
    return Value::longVal(0);
  case rbc::RType::Float:
    return Value::floatVal(0.0F);
  case rbc::RType::Double:
    return Value::doubleVal(0.0);
  case rbc::RType::Ref:
  case rbc::RType::Null:
    return Value::nullVal();
  case rbc::RType::Bottom:
    break; // defensive: untyped arrays (never created by Runtime) stay Bottom
  }
  return Value::bottom();
}

} // namespace

// --- private accessors -------------------------------------------------------
//
// ids are 1-based into objs_ (0 = invalid); the r.id == 0 guard makes the
// subtraction below underflow-proof.
HeapObj* Heap::obj(ObjRef r) noexcept {
  if (r.id == 0 || r.id > objs_.size()) {
    return nullptr;
  }
  return &objs_[r.id - 1];
}

const HeapObj* Heap::obj(ObjRef r) const noexcept {
  if (r.id == 0 || r.id > objs_.size()) {
    return nullptr;
  }
  return &objs_[r.id - 1];
}

// --- allocation (never fails in v0; sizes were validated upstream) -----------

ObjRef Heap::allocInstance(ClassId cls, std::uint32_t slotHint) {
  // slotHint is the declaring class's CURRENT layout size (Runtime passes
  // classInfo(cls).fields.size()). Fields resolved after this allocation
  // grow the slots lazily on first access (loadField/storeField below) -
  // the v0 lazy layout; the real runtime computes layouts at class-load.
  HeapObj o;
  o.kind = HeapObj::Kind::Instance;
  o.cls = cls;
  o.slots.assign(slotHint, Value::bottom());
  objs_.push_back(std::move(o));
  return ObjRef{static_cast<std::uint32_t>(objs_.size())};
}

ObjRef Heap::allocArray(ClassId arrayCls, rbc::RType elem,
                        std::uint32_t length) {
  HeapObj o;
  o.kind = HeapObj::Kind::Array;
  o.cls = arrayCls;
  o.elem = elem;
  o.length = length;
  o.slots.assign(length, zeroOf(elem)); // per-type zero-fill, see zeroOf
  objs_.push_back(std::move(o));
  return ObjRef{static_cast<std::uint32_t>(objs_.size())};
}

ObjRef Heap::allocString(std::string_view utf8, ClassId stringCls) {
  // String payloads live out of band (Heap.h: ldc/println need no char[]
  // machinery). Instance slots start EMPTY: v0 resolves no fields on
  // java/lang/String; if that ever changes, the lazy growth in
  // loadField/storeField covers it.
  HeapObj o;
  o.kind = HeapObj::Kind::Instance;
  o.cls = stringCls;
  o.str.assign(utf8);
  objs_.push_back(std::move(o));
  return ObjRef{static_cast<std::uint32_t>(objs_.size())};
}

// --- object queries -----------------------------------------------------------

bool Heap::isInstance(ObjRef r) const noexcept {
  const HeapObj* o = obj(r);
  return o != nullptr && o->kind == HeapObj::Kind::Instance;
}

bool Heap::isArray(ObjRef r) const noexcept {
  const HeapObj* o = obj(r);
  return o != nullptr && o->kind == HeapObj::Kind::Array;
}

bool Heap::isString(ObjRef r) const noexcept {
  // FROZEN-HEADER GAP workaround (see the file header): the string class is
  // anchored by ANY interned entry - all of them share one class id because
  // Runtime passes the same stringCls to every internString call. O(1).
  const HeapObj* o = obj(r);
  if (o == nullptr || o->kind != HeapObj::Kind::Instance || interned_.empty()) {
    return false;
  }
  const HeapObj* anchor = obj(interned_.begin()->second);
  return anchor != nullptr && anchor->cls == o->cls;
}

ClassId Heap::classOf(ObjRef r) const noexcept {
  // NOTE (frozen-header overload bug, reported): ClassId{0} is BOTH Object's
  // id (Object is registered first in Runtime) and this function's
  // dead-reference result. Live objects always have real class ids, so only
  // the defensive path is ambiguous; Runtime::classNameOf probes liveness
  // via peek() to disambiguate.
  const HeapObj* o = obj(r);
  return o != nullptr ? o->cls : ClassId{};
}

std::uint32_t Heap::arrayLength(ObjRef r) const noexcept {
  const HeapObj* o = obj(r);
  return (o != nullptr && o->kind == HeapObj::Kind::Array) ? o->length : 0;
}

// --- instance fields ----------------------------------------------------------

Value Heap::loadField(ObjRef r, std::uint32_t slot) {
  HeapObj* o = obj(r);
  if (o == nullptr || o->kind != HeapObj::Kind::Instance) {
    // Dead id or kind misuse: the "nullptr-like" Bottom result (the frozen
    // header's nullopt-ish defensive convention). Verified streams never
    // get here (the dispatch core NPE-checks receivers first).
    return Value::bottom();
  }
  if (slot >= o->slots.size()) {
    // Lazy layout extended past allocation: grow ONCE, Bottom-filled. The
    // load then observes Bottom, the honest "field never written" value.
    // This is the only allocating path in loadField.
    o->slots.resize(slot + 1, Value::bottom());
  }
  return o->slots[slot];
}

bool Heap::storeField(ObjRef r, std::uint32_t slot, Value v) {
  HeapObj* o = obj(r);
  if (o == nullptr || o->kind != HeapObj::Kind::Instance) {
    return false;
  }
  if (slot >= o->slots.size()) {
    // Same lazy growth as loadField; the only allocating path in storeField.
    o->slots.resize(slot + 1, Value::bottom());
  }
  o->slots[slot] = v;
  return true;
}

// --- array elements -----------------------------------------------------------

Value Heap::loadElem(ObjRef r, std::uint32_t index) {
  HeapObj* o = obj(r);
  if (o == nullptr || o->kind != HeapObj::Kind::Array) {
    return Value::bottom(); // kind misuse defense (caller handles bounds)
  }
  // Bounds are the CALLER's job (it raises ArrayIndexOutOfBoundsException
  // with the JVM's message, rbc_spec.md SS3.13). This is the defensive
  // clamp: memory outside the element array is NEVER touched - an
  // out-of-range index reports Bottom instead of guessing a neighbor slot
  // (a wrong-slot read would silently corrupt observable state).
  if (index >= o->slots.size()) {
    return Value::bottom();
  }
  return o->slots[index];
}

bool Heap::storeElem(ObjRef r, std::uint32_t index, Value v) {
  HeapObj* o = obj(r);
  if (o == nullptr || o->kind != HeapObj::Kind::Array) {
    return false;
  }
  // Defensive clamp, same policy as loadElem: refuse, never write OOB and
  // never redirect to a clamped slot (silent wrong-slot stores are worse
  // than a refused store on a path the caller already trapped).
  if (index >= o->slots.size()) {
    return false;
  }
  o->slots[index] = v;
  return true;
}

// --- strings --------------------------------------------------------------------

std::string_view Heap::stringOf(ObjRef r) const noexcept {
  if (!isString(r)) {
    return {}; // non-strings and dead ids have no payload
  }
  return obj(r)->str; // isString() proved obj(r) non-null
}

// --- monitors (reentrant, single-threaded v0) -----------------------------------

Heap::MonFail Heap::monitorenter(ObjRef r) {
  HeapObj* o = obj(r);
  if (o == nullptr) {
    return MonFail::Null; // caller raises NullPointerException
  }
  if (o->mon.owner == 0) {
    o->mon.owner = kOwnerThread;
    o->mon.count = 1;
  } else if (o->mon.owner == kOwnerThread) {
    ++o->mon.count; // reentrant acquire (JLS 17.1, JVMS monitorenter)
  } else {
    // Unreachable in the single-threaded v0 world (no other thread id
    // exists). Reported as NotOwner so a corrupted monitor fails loudly
    // (caller raises IllegalMonitorStateException) instead of silently
    // incrementing a foreign owner's count.
    return MonFail::NotOwner;
  }
  return MonFail::None;
}

Heap::MonFail Heap::monitorexit(ObjRef r) {
  HeapObj* o = obj(r);
  if (o == nullptr) {
    return MonFail::Null;
  }
  if (o->mon.owner != kOwnerThread || o->mon.count == 0) {
    // Releasing a free or foreign monitor (JVMS monitorexit): the caller
    // raises IllegalMonitorStateException.
    return MonFail::NotOwner;
  }
  if (--o->mon.count == 0) {
    o->mon.owner = 0; // monitor fully released
  }
  return MonFail::None;
}

// --- interning (JVMS 5.1: equal string constants are one object) -----------------

ObjRef Heap::internString(std::string_view utf8, ClassId stringCls) {
  std::string key(utf8);
  if (auto it = interned_.find(key); it != interned_.end()) {
    return it->second; // identity reuse - if_acmpeq depends on it
  }
  const ObjRef r = allocString(utf8, stringCls);
  interned_.emplace(std::move(key), r);
  return r;
}

// --- diagnostics -----------------------------------------------------------------

const HeapObj* Heap::peek(ObjRef r) const noexcept {
  // TESTS and the frame dumper only (cold path); Runtime::classNameOf /
  // exceptionMessage borrow it for const liveness probes (documented there).
  return obj(r);
}

} // namespace b2::interp
