#pragma once
// B-2 IR - the explicit effect model (Rule 121, docs/effect_system.md).
//
// WHY THIS FILE EXISTS:
// Rule 121 requires an explicit, checkable effect model; docs/special_passes.md
// section 2.1 fixes it as a 12-entry tagged enum plus a 12x12 = 144-entry
// reorder LOOKUP TABLE (not a logic, not a solver). Every memory-effecting
// NodeKind maps to exactly one primary EffectKind (see NodeInfo in Node.h);
// the table answers "may effect A be reordered before effect B?" with
// Allowed / Forbidden / RequiresCompensation. Compensation is emitted as
// first-class IR nodes (special_passes.md 2.2), and the effect-chain auditor
// plus the construction rules below are the enforcement (Rules 40, 121, 126).
//
// Table semantics: canReorder(a, b) answers "may an effect of class a,
// currently ordered AFTER an adjacent effect of class b, be moved BEFORE it?"
// Both effects are assumed to still execute. Location identity is NOT part
// of the class - the table assumes possible aliasing; RequiresCompensation
// is where alias proof, versioning, or compensation nodes plug in.
//
// CONSTRUCTION RULES (each table entry is produced by the first matching
// rule; "TL" = thread-local classes Pure/ReadLocal/WriteLocal):
//
//  R1  Pure commutes with everything.
//  R2  An opaque call (JNI/reflection/clinit) observes everything except
//      thread-local state, which no callee can read or write: (RL|WL, CO)
//      and (CO, RL|WL) are Allowed; every other pair against CallOpaque is
//      Forbidden.
//  R3  ExceptionThrow is a publication point: handlers, stack traces, and
//      deopt reconstruction observe prior WriteShared / VolatileWrite /
//      Monitor* / CallOpaque / WriteLocal / throw effects, so those pairs
//      are Forbidden. Pure reads (RL/RS/VR) and Allocations commute with
//      the throw (same value either side).
//  R4  TL-vs-TL: two reads commute; any pair involving a WriteLocal requires
//      compensation (same-location value/order sensitivity).
//  R5  TL-vs-shared/monitor/allocation: disjoint by classification
//      (monitors and volatiles synchronize shared state only; allocations
//      are GC-visible, not Java-visible; JLS 17.4 keeps thread-local storage
//      outside the synchronizes-with relation): Allowed.
//  R6  Synchronization-order participants (VolatileRead/Write,
//      MonitorEnter/Exit) are Forbidden among themselves (JLS 17.4.4:
//      sync order follows program order between them).
//  R7  A shared WRITE crossing a strong sync participant (VolatileWrite,
//      MonitorEnter/Exit) is Forbidden - it would break a
//      happens-before edge another thread can observe (JLS 17.4.5). A shared
//      write crossing an acquire read (VolatileRead), and any read-vs-read
//      shared pair, RequiresCompensation (same-variable value sensitivity).
//  R8  Plain shared reads commute; plain shared write-vs-read and
//      write-vs-write pairs RequireCompensation.
//  R9  Allocation commutes with every non-opaque, non-throw class
//      (identity/allocation order is not Java-observable; the GC does not
//      observe Java values).
//
// Anything not covered defaults to Forbidden (conservative).
//
// The table is generated from the rules at compile time and frozen as the
// law; a unit test asserts table == rules for all 144 entries so they can
// never drift, and spot-checks documented cells.

#include <cstdint>

namespace b2::ir {

// The 12 effect classes. Order is normative: row/column order of the reorder
// table and the serialization order of the model.
enum class EffectKind : std::uint8_t {
  Pure = 0,          // no observable effect
  ReadLocal,         // read of thread-local / stack state
  ReadShared,        // read of shared non-volatile memory
  WriteLocal,        // write of thread-local / stack state
  WriteShared,       // write of shared non-volatile memory
  VolatileRead,      // volatile / VarHandle acquire read
  VolatileWrite,     // volatile / VarHandle release write
  MonitorEnter,      // lock acquisition
  MonitorExit,       // lock release
  Allocation,        // object allocation (GC-visible, not Java-visible)
  CallOpaque,        // JNI / native / reflection / clinit - observes anything
  ExceptionThrow,    // Java exception publication point
  _Count
};

[[nodiscard]] constexpr std::uint8_t effectCount() noexcept {
  return static_cast<std::uint8_t>(EffectKind::_Count);
}

[[nodiscard]] constexpr const char* effectName(EffectKind e) noexcept {
  switch (e) {
  case EffectKind::Pure:
    return "pure";
  case EffectKind::ReadLocal:
    return "read.local";
  case EffectKind::ReadShared:
    return "read.shared";
  case EffectKind::WriteLocal:
    return "write.local";
  case EffectKind::WriteShared:
    return "write.shared";
  case EffectKind::VolatileRead:
    return "volatile.read";
  case EffectKind::VolatileWrite:
    return "volatile.write";
  case EffectKind::MonitorEnter:
    return "monitor.enter";
  case EffectKind::MonitorExit:
    return "monitor.exit";
  case EffectKind::Allocation:
    return "alloc";
  case EffectKind::CallOpaque:
    return "call.opaque";
  case EffectKind::ExceptionThrow:
    return "throw";
  default:
    return "?";
  }
}

enum class EffectOrderResult : std::uint8_t {
  Allowed = 0,
  Forbidden,
  RequiresCompensation,
};

[[nodiscard]] constexpr const char*
effectOrderName(EffectOrderResult r) noexcept {
  switch (r) {
  case EffectOrderResult::Allowed:
    return "allowed";
  case EffectOrderResult::Forbidden:
    return "forbidden";
  case EffectOrderResult::RequiresCompensation:
    return "compensate";
  default:
    return "?";
  }
}

// Classification helpers used by the rules and the effect-chain auditor.
[[nodiscard]] constexpr bool isReadEffect(EffectKind e) noexcept {
  return e == EffectKind::ReadLocal || e == EffectKind::ReadShared ||
         e == EffectKind::VolatileRead;
}

[[nodiscard]] constexpr bool isWriteEffect(EffectKind e) noexcept {
  return e == EffectKind::WriteLocal || e == EffectKind::WriteShared ||
         e == EffectKind::VolatileWrite;
}

// Thread-local-only classes (special_passes.md 2.3 Rule 4: only these may be
// SPECULATIVELY reordered; shared/volatile/monitor effects need proof).
[[nodiscard]] constexpr bool isThreadLocalEffect(EffectKind e) noexcept {
  return e == EffectKind::Pure || e == EffectKind::ReadLocal ||
         e == EffectKind::WriteLocal;
}

// Synchronization-order participants (rule R6, JLS 17.4.4).
[[nodiscard]] constexpr bool isSyncEffect(EffectKind e) noexcept {
  return e == EffectKind::VolatileRead || e == EffectKind::VolatileWrite ||
         e == EffectKind::MonitorEnter || e == EffectKind::MonitorExit;
}

// Strong sync participants that happen-before-cross with shared writes
// (rule R7): everything sync except the plain acquire read.
[[nodiscard]] constexpr bool isStrongSyncEffect(EffectKind e) noexcept {
  return e == EffectKind::VolatileWrite || e == EffectKind::MonitorEnter ||
         e == EffectKind::MonitorExit;
}

namespace detail {

// The construction rules R1-R9 for one (a, b) pair. First match wins.
constexpr EffectOrderResult ruleImpl(EffectKind a, EffectKind b) noexcept {
  using EK = EffectKind;
  using R = EffectOrderResult;

  // R1 - pure commutes with everything (both directions).
  if (a == EK::Pure || b == EK::Pure) {
    return R::Allowed;
  }

  // R2 - opaque calls vs everything.
  if (a == EK::CallOpaque || b == EK::CallOpaque) {
    const EK other = (a == EK::CallOpaque) ? b : a;
    if (other == EK::ReadLocal || other == EK::WriteLocal) {
      return R::Allowed; // callee cannot observe thread-local state
    }
    return R::Forbidden;
  }

  // R3 - exception publication points.
  if (a == EK::ExceptionThrow || b == EK::ExceptionThrow) {
    const EK other = (a == EK::ExceptionThrow) ? b : a;
    switch (other) {
    case EK::ReadLocal:
    case EK::ReadShared:
    case EK::VolatileRead:
    case EK::Allocation:
      return R::Allowed; // same value either side
    default:
      return R::Forbidden; // publication must observe prior effects
    }
  }

  const bool aTL = isThreadLocalEffect(a); // Pure handled by R1
  const bool bTL = isThreadLocalEffect(b);
  const bool aRL = (a == EK::ReadLocal);
  const bool bRL = (b == EK::ReadLocal);

  // R4 - thread-local vs thread-local.
  if (aTL && bTL) {
    if (aRL && bRL) {
      return R::Allowed; // two reads commute
    }
    return R::RequiresCompensation; // write involvement, location unknown
  }

  // R5 - thread-local vs shared/monitor/allocation: disjoint locations.
  if (aTL || bTL) {
    return R::Allowed;
  }

  // Remaining classes: ReadShared, WriteShared, sync participants, Allocation.
  const bool aSync = isSyncEffect(a);
  const bool bSync = isSyncEffect(b);

  // R6 - sync participants among themselves.
  if (aSync && bSync) {
    return R::Forbidden;
  }

  // R9 - allocation vs every remaining class.
  if (a == EK::Allocation || b == EK::Allocation) {
    return R::Allowed;
  }

  // Plain shared classes only now: ReadShared / WriteShared (+ sync on the
  // other side).
  const bool aWrite = (a == EK::WriteShared);
  const bool bWrite = (b == EK::WriteShared);

  // R8 - two plain shared reads commute.
  if (a == EK::ReadShared && b == EK::ReadShared) {
    return R::Allowed;
  }

  // R7 - shared write crossing a strong sync participant breaks an
  // observable happens-before edge; everything else in this region is a
  // same-variable-sensitive pair.
  if (aWrite && isStrongSyncEffect(b)) {
    return R::Forbidden;
  }
  if (isStrongSyncEffect(a) && bWrite) {
    return R::Forbidden;
  }

  // R8 tail + R7 tail: write-vs-read, read-vs-write, write-vs-write, and
  // shared-vs-acquire-read pairs.
  return R::RequiresCompensation;
}

// The literal 12x12 table, generated from the rules at compile time and
// frozen as the law: if a rule change is ever proposed, the diff of this
// table is the review artifact.
struct EffectOrderTable {
  static constexpr unsigned kN = 12;
  EffectOrderResult entries[kN][kN];

  constexpr EffectOrderTable() : entries{} {
    for (unsigned a = 0; a < kN; ++a) {
      for (unsigned b = 0; b < kN; ++b) {
        entries[a][b] = ruleImpl(static_cast<EffectKind>(a),
                                 static_cast<EffectKind>(b));
      }
    }
  }
};

inline constexpr EffectOrderTable kEffectOrderTable{};

} // namespace detail

// The rules, exposed for the table-vs-rules consistency test and for
// documentation tooling. Same answer as canReorder by construction.
[[nodiscard]] constexpr EffectOrderResult reorderByRule(EffectKind a,
                                                        EffectKind b) noexcept {
  return detail::ruleImpl(a, b);
}

// The 144-entry lookup (special_passes.md 2.1): table[row(a)][col(b)].
[[nodiscard]] constexpr EffectOrderResult canReorder(EffectKind a,
                                                     EffectKind b) noexcept {
  const unsigned ra = static_cast<unsigned>(a);
  const unsigned rb = static_cast<unsigned>(b);
  if (ra >= detail::EffectOrderTable::kN ||
      rb >= detail::EffectOrderTable::kN) {
    return EffectOrderResult::Forbidden;
  }
  return detail::kEffectOrderTable.entries[ra][rb];
}

} // namespace b2::ir
