#pragma once
// B-2 Interpreter - the T0 value representation.
//
// WHY THIS FILE EXISTS:
// T0 is the universal correctness fallback (Rule 96) and the deopt
// reconstruction target for every compiled tier (Rule 4). The value model is
// therefore a published contract, not an implementation detail: what a slot
// holds at an instruction boundary is exactly what deopt must reproduce and
// what the GC must see (Amendment B.1; docs/deopt_backend.md Part A SS1/SS3).
//
// DESIGN RULES (normative):
// - One value per slot, 16 bytes, tag + payload. The tag IS the verifier's
//   RType (docs/rbc_spec.md SS4): locals and registers hold exactly the type
//   the verifier proved at that program point. Runtime tag and verified type
//   can never disagree on a verified stream, which makes state dumps usable
//   as deopt fixtures (tests/interp/ golden state).
// - Null is its own tag. Invariant: type == Ref implies obj != 0; a null
//   reference is always Tag Null, never Ref-with-zero. One representation for
//   "null" keeps identity comparisons (if_acmp*) and NPE checks branch-free.
// - Rule 15 (stable indices): references are heap object ids (indices), never
//   pointers into containers that can reallocate.
// - NaN boxing (Part XVIII) is a FUTURE, flag-gated representation change that
//   must preserve this contract bit-for-bit at every observable boundary; it
//   is disabled by default and requires this team's approval per Part XVIII.
//   v0 uses the wide tagged form: correctness and observability first.

#include <cstdint>
#include <type_traits>

#include "b2/rbc/Type.h"

namespace b2::interp {

// Heap object reference. Ids start at 1; 0 is never a valid object (Null is
// represented by the Value tag, see above). Ids are stable for the lifetime
// of the owning Runtime (Rule 15 discipline).
struct ObjRef {
  std::uint32_t id = 0; // 0 = invalid/null sentinel for defensive checks

  [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
  [[nodiscard]] constexpr bool operator==(const ObjRef&) const noexcept = default;
};

// A T0 slot value: one local, one register, one array element, one field, or
// one method result. Trivially copyable, memset-able, memcpy-able - the frame
// is the deopt unit and reconstruction is a plain copy (rbc_spec.md SS1.4).
struct Value {
  rbc::RType type = rbc::RType::Bottom; // what the verifier proved this slot holds
  union {
    std::int32_t i;    // Int
    std::int64_t l;    // Long
    float f;           // Float
    double d;          // Double
    std::uint32_t obj; // Ref -> ObjRef::id (payload of ObjRef)
  } as{};

  constexpr Value() = default;

  // --- typed constructors (the only sanctioned ways to build a Value) ------
  [[nodiscard]] static constexpr Value intVal(std::int32_t v) noexcept {
    Value out; out.type = rbc::RType::Int; out.as.i = v; return out;
  }
  [[nodiscard]] static constexpr Value longVal(std::int64_t v) noexcept {
    Value out; out.type = rbc::RType::Long; out.as.l = v; return out;
  }
  [[nodiscard]] static constexpr Value floatVal(float v) noexcept {
    Value out; out.type = rbc::RType::Float; out.as.f = v; return out;
  }
  [[nodiscard]] static constexpr Value doubleVal(double v) noexcept {
    Value out; out.type = rbc::RType::Double; out.as.d = v; return out;
  }
  [[nodiscard]] static constexpr Value refVal(ObjRef r) noexcept {
    Value out; out.type = rbc::RType::Ref; out.as.obj = r.id; return out;
  }
  [[nodiscard]] static constexpr Value nullVal() noexcept {
    Value out; out.type = rbc::RType::Null; return out;
  }
  [[nodiscard]] static constexpr Value bottom() noexcept { return Value{}; }

  // --- queries (all branch-free tag tests; no RTTI, Rule 8) ----------------
  [[nodiscard]] constexpr bool isNull() const noexcept {
    return type == rbc::RType::Null; // invariant: null is always tagged Null
  }
  [[nodiscard]] constexpr bool isRef() const noexcept {
    return type == rbc::RType::Ref;
  }
  // Any reference-like value (Null or Ref): the operands getfield/arraylength/
  // monitorenter/call receivers accept.
  [[nodiscard]] constexpr bool isRefLike() const noexcept {
    return type == rbc::RType::Ref || type == rbc::RType::Null;
  }
  [[nodiscard]] constexpr ObjRef ref() const noexcept { return ObjRef{as.obj}; }

  // Reference identity (if_acmpeq / if_acmpne semantics, rbc_spec.md SS3.11):
  // null == null, same id == same object, null != any object.
  [[nodiscard]] static constexpr bool sameObject(const Value& a,
                                                 const Value& b) noexcept {
    if (a.isNull() || b.isNull()) return a.isNull() && b.isNull();
    return a.isRef() && b.isRef() && a.as.obj == b.as.obj;
  }
};

static_assert(sizeof(Value) == 16, "T0 slot values are exactly 16 bytes");
static_assert(std::is_trivially_copyable_v<Value>,
              "frame reconstruction is a memcpy (deopt contract)");

} // namespace b2::interp
