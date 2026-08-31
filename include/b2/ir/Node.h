#pragma once
// B-2 IR - node kinds, node record, and the node-info registry.
//
// WHY THIS FILE EXISTS:
// Rule 8 (no RTTI): every dispatch on a node is a switch over NodeKind.
// Rule 15: nodes are referenced by NodeId, never pointers. Rule 32: node
// boolean state is a typed bitmask. Rule 33: NO implicit conversions - every
// widening/narrowing/boxing/untagging is an explicit node kind. Part XVIII:
// the taxonomy must cover SWLP vector nodes, PEA materialization, and NaN
// boxing tagged-value nodes. Rule 130 + the IR team charter: every kind has
// a registry row (the machine-checkable node specification) and a
// docs/ir_spec.md entry.
//
// The taxonomy is intentionally Graal/C2-shaped: typed arithmetic kinds
// (AddI/AddL/AddF/AddD) rather than one generic Add with a type operand, so
// the graph is self-describing, pattern matching is a kind compare, and the
// verifier's operand-type check is a table lookup.

#include <cstdint>

#include "b2/ir/Effect.h"
#include "b2/ir/Types.h"

namespace b2::ir {

enum class NodeKind : std::uint16_t {
  // --- control ------------------------------------------------------------
  Start = 0,      // method entry; origin of control AND initial memory state
  End,            // terminal sink for paths that neither return nor throw
  Region,         // control merge; N control predecessors (variadic)
  If,             // [ctrl, cond(int)] -> IfTrue/IfFalse projections
  IfTrue,         // projection of If (cond != 0)
  IfFalse,        // projection of If (cond == 0)
  Switch,         // [ctrl, sel(int)] -> SwitchCase*/SwitchDefault projections
  SwitchCase,     // projection of Switch; payload = case ordinal
  SwitchDefault,  // projection of Switch (fall-through)
  LoopBegin,      // loop header: entry + backedge control predecessors (>= 2)
  LoopEnd,        // loop backedge anchor; [ctrl]
  LoopExit,       // loop exit; [ctrl, framestate] - safepoint/deopt point
  Return,         // [ctrl, value?] - value present iff method non-void
  Unwind,         // [ctrl, exception(ref)] - exceptional return to caller
  Deopt,          // [ctrl, framestate] - unconditional deopt; payload = DeoptId

  // --- memory ---------------------------------------------------------------
  LoadField,      // [ctrl, mem, obj] -> field value; payload = FieldId,
                  //   payload2 = result IRType (the graph is self-describing;
                  //   field metadata tables are frontend-side and opaque)
  StoreField,     // [ctrl, mem, obj, value]; payload = FieldId
  LoadStatic,     // [ctrl, mem] -> field value; payload = FieldId,
                  //   payload2 = result IRType
  StoreStatic,    // [ctrl, mem, value]; payload = FieldId
  LoadElem,       // [ctrl, mem, array, index] -> element; payload = elem IRType
  StoreElem,      // [ctrl, mem, array, index, value]; payload = elem IRType
  ArrayLength,    // [array] -> int (null-check is a separate Guard)
  MemBar,         // [ctrl, mem]; payload = MemBarKind
  MonitorEnter,   // [ctrl, mem, obj]
  MonitorExit,    // [ctrl, mem, obj]
  New,            // [ctrl] -> ref; payload = TypeId (class)
  NewArray,       // [ctrl, length] -> ref; payload = elem IRType (primitive)
  NewRefArray,    // [ctrl, length] -> ref; payload = TypeId (component)
  NewMultiArray,  // [ctrl, dims...] -> ref; payload = TypeId
  ClassInit,      // [ctrl, mem]; payload = TypeId - runs <clinit>

  // --- calls ------------------------------------------------------------------
  CallStatic,     // [ctrl, mem, args..., framestate]; payload = MethodId,
                  //   payload2 = result IRType (Bottom = void)
  CallVirtual,    // same shape; virtual dispatch
  CallInterface,  // same shape; interface dispatch
  CallDynamic,    // same shape; invokedynamic
  CallExcept,     // projection of a Call*; [call] -> exception(ref)
  LoadException,  // [ctrl] -> ref - catch-handler entry exception

  // --- object / type ops --------------------------------------------------------
  CheckCast,      // [ctrl, obj] -> obj; payload = TypeId; throws on failure
  InstanceOf,     // [obj] -> int; payload = TypeId

  // --- guards (Rule 32: GuardKind taxonomy) --------------------------------------
  Guard,          // [ctrl, cond(int), framestate]; payload = GuardKind,
                  //   payload2 = DeoptId; deopts when cond == 0

  // --- constants / parameters ------------------------------------------------
  ConstantI,      // constValue = int32 value -> int
  ConstantL,      // constValue = int64 value -> long
  ConstantF,      // constValue = float bits (low 32) -> float
  ConstantD,      // constValue = double bits -> double
  ConstantNull,   // -> null
  ConstantSym,    // payload = SymbolId -> ref (string/class/methodhandle)
  Parameter,      // payload = index, payload2 = IRType

  // --- int arithmetic ---------------------------------------------------------
  AddI, SubI, MulI, DivI, RemI, NegI,
  ShlI, ShrI, UShrI, AndI, OrI, XorI,

  // --- long arithmetic (shift counts stay Int, per JVM) -------------------------
  AddL, SubL, MulL, DivL, RemL, NegL,
  ShlL, ShrL, UShrL, AndL, OrL, XorL,

  // --- float arithmetic ---------------------------------------------------------
  AddF, SubF, MulF, DivF, RemF, NegF,

  // --- double arithmetic ---------------------------------------------------------
  AddD, SubD, MulD, DivD, RemD, NegD,

  // --- comparisons -----------------------------------------------------------------
  CmpI,   // (a, b) -> int : (a > b) - (a < b)
  CmpL,   // (a, b) -> int
  CmpFl,  // (a, b) -> int : NaN compares less
  CmpFg,  // (a, b) -> int : NaN compares greater
  CmpDl,  // (a, b) -> int : NaN compares less
  CmpDg,  // (a, b) -> int : NaN compares greater

  // --- conversions (Rule 33: explicit only) --------------------------------------
  I2L, I2F, I2D,
  L2I, L2F, L2D,
  F2I, F2L, F2D,
  D2I, D2L, D2F,
  I2B, I2C, I2S,
  SignExtend,             // (int -> long-family) explicit widening
  ZeroExtend,             // explicit unsigned widening
  Truncate,               // explicit narrowing
  BitCast,                // same-width reinterpretation
  CompressedRefEncode,    // ref -> int (compressed oop)
  CompressedRefDecode,    // int -> ref
  BoxPrimitive,           // primitive -> boxed ref
  UnboxPrimitive,         // boxed ref -> primitive; payload = target IRType

  // --- merges -------------------------------------------------------------------
  Phi,             // [region, v0..vN-1]; one value per region predecessor

  // --- vector / SIMD (SWLP, Part XVIII) -----------------------------------------
  VectorBroadcast, // [scalar] -> vector; payload = lane IRType, payload2 =
                   //   packed(lane, lanes)
  VectorPack,      // [s0..sN-1] -> vector; payload = lane IRType, lanes =
                   //   numInputs
  VectorExtract,   // [vector, index(int)] -> lane scalar
  VectorInsert,    // [vector, value, index(int)] -> vector
  VectorOp,        // [a, b] -> vector; payload = VectorOp, payload2 = packed
  VectorMaskOp,    // [mask(vec.mask), a, b] -> vector; payload = VectorOp,
                   //   payload2 = packed; mask lanes must match operand lanes
  VectorLoad,      // [ctrl, mem, array, index] -> vector; payload/payload2 as
                   //   above (consecutive elements)
  VectorStore,     // [ctrl, mem, array, index, vector]; payload/payload2
  VectorReduce,    // [vector] -> lane scalar; payload = VectorOp (incl. Sum),
                   //   payload2 = packed

  // --- PEA virtual objects / materialization (Part XVIII) ---------------------
  VirtualObjectState, // [field values...] (Array: length first); payload =
                     //   Instance: TypeId / Array: elem IRType; payload2 =
                     //   0 instance, 1 array. The NodeId of this node IS the
                     //   VirtualObjectId. Floating analysis state: fields are
                     //   EDGES so node replacement propagates automatically.
  Materialize,     // [ctrl, mem, vobjState] -> ref; the escape-point
                   //   allocation; effect = Allocation

  // --- tagged values (NaN boxing, Part XVIII) -----------------------------------
  TagValue,        // [value] -> tagged; payload = from ValueRep, payload2 = to
  UntagValue,      // [tagged] -> value; payload = from, payload2 = to
  RepTransitionGuard, // [ctrl, value, framestate]; payload/payload2 = reps;
                      //   deopts on unexpected tag; always speculative

  // --- state --------------------------------------------------------------------
  FrameState,      // [locals...] ; payload = FrameStateId (desc index);
                   //   the deopt reconstruction state (Rule 5)

  _Count
};

[[nodiscard]] constexpr std::uint16_t nodeKindCount() noexcept {
  return static_cast<std::uint16_t>(NodeKind::_Count);
}

// Guard taxonomy (Rule 32; payload of Guard).
enum class GuardKind : std::uint8_t {
  None = 0,
  NullCheck,       // receiver/field/array null guard
  BoundsCheck,     // array index in range
  ZeroCheck,       // integer/long divide-by-zero
  ClassCast,       // checkcast will succeed (speculative form)
  ArrayStore,      // array store type check will succeed
  TypeProfile,     // observed type distribution still holds
  UnstableIf,      // branch never taken per profile
  UnstableSwitch,  // case never taken per profile
  LoopLimit,       // loop trip count bound holds
  Deoptimize,      // unconditional deopt reason carried by a guard
};

[[nodiscard]] constexpr const char* guardKindName(GuardKind k) noexcept {
  switch (k) {
  case GuardKind::None:
    return "none";
  case GuardKind::NullCheck:
    return "nullcheck";
  case GuardKind::BoundsCheck:
    return "boundscheck";
  case GuardKind::ZeroCheck:
    return "zerocheck";
  case GuardKind::ClassCast:
    return "classcast";
  case GuardKind::ArrayStore:
    return "arraystore";
  case GuardKind::TypeProfile:
    return "typeprofile";
  case GuardKind::UnstableIf:
    return "unstableif";
  case GuardKind::UnstableSwitch:
    return "unstableswitch";
  case GuardKind::LoopLimit:
    return "looplimit";
  case GuardKind::Deoptimize:
    return "deoptimize";
  default:
    return "?";
  }
}

// Node flags (Rule 32: typed bitmask, never raw integers).
enum class NodeFlag : std::uint16_t {
  None = 0,
  Dead = 1u << 0,        // tombstone (replaced or removed)
  Replaced = 1u << 1,    // replaced by another node (see replacement log)
  Speculative = 1u << 2, // carries SpecMeta (Rule 122)
  Pinned = 1u << 3,      // fixed to its control position (memory/ctrl nodes)
  NeverNull = 1u << 4,   // provably non-null value (dataflow fact)
  OnExceptionPath = 1u << 5, // reachable only via an exception edge
};

// Type-safe Flags<E> wrapper (Rule 32).
template <typename E> class Flags {
public:
  constexpr Flags() noexcept = default;
  constexpr explicit Flags(std::uint16_t raw) noexcept : raw_(raw) {}

  constexpr void set(E f) noexcept { raw_ |= static_cast<std::uint16_t>(f); }
  constexpr void clear(E f) noexcept { raw_ &= ~static_cast<std::uint16_t>(f); }
  [[nodiscard]] constexpr bool has(E f) const noexcept {
    return (raw_ & static_cast<std::uint16_t>(f)) != 0;
  }
  [[nodiscard]] constexpr std::uint16_t raw() const noexcept { return raw_; }

private:
  std::uint16_t raw_ = 0;
};

using NodeFlags = Flags<NodeFlag>;

// Input roles: what an input slot MEANS for the node. The registry fixes the
// per-kind signature; the verifier checks that each slot's producer kind is
// compatible with its role.
enum class InputRole : std::uint8_t {
  None = 0,
  Ctrl,  // control predecessor (producer: control node / projection)
  Mem,   // memory state predecessor (producer: memory-state node)
  Data,  // data value (producer: value node of the checked IRType)
  FrameState, // deopt state (producer: FrameState node)
  Parent,    // projection source (producer: the If/Switch/Call node itself;
             // checked by kind in checkProjectionParent)
};

// Coarse node classification, derived from the registry.
enum class NodeClass : std::uint8_t {
  Control,   // Start/End/Region/If/Switch/Loop*/Return/Unwind/Deopt/projections
  Memory,    // loads/stores/barriers/monitors/allocs/classinit/materialize
  Call,      // call nodes + projections + LoadException
  TypeOp,    // CheckCast/InstanceOf
  Guard,     // Guard, RepTransitionGuard
  Value,     // pure data: arithmetic/compare/convert/phi/param/const/vector...
  State,     // FrameState
};

// Per-kind registry row (Rule 130's machine-checkable specification).
//   - fixed input roles (padded with None),
//   - whether extra inputs may be appended (and with which role),
//   - whether a mandatory trailing FrameState input exists,
//   - the primary effect class for the reorder algebra,
//   - the coarse node class.
struct NodeInfo {
  const char* name;    // canonical short name, e.g. "AddI"
  NodeClass cls;
  std::uint8_t numFixed;       // fixed-prefix input count
  bool variadic;               // may append more inputs of variadicRole
  InputRole variadicRole;      // role of appended inputs
  bool hasFrameState;          // mandatory trailing FrameState input
  InputRole roles[6];          // roles of the fixed prefix
  EffectKind effect;           // primary effect class
};

// Registry lookup. `kind` must be < _Count (callers check first); invalid
// codes get the static bad row (name "bad<kind>", like rbc::info()).
[[nodiscard]] const NodeInfo& info(NodeKind kind) noexcept;

[[nodiscard]] const char* nodeKindName(NodeKind kind) noexcept;

// Registry row counts for the doc/test cross-check (ir_spec.md keeps the
// same numbers; a test asserts they match nodeKindCount()).
[[nodiscard]] std::uint16_t registeredNodeKinds() noexcept;

// The one Node record. Fixed 40 bytes, trivially copyable, no pointers
// (Rules 8, 9, 15). Inputs live in the graph's edge pool at
// [edgeOffset, edgeOffset + numInputs).
struct Node {
  NodeKind kind = NodeKind::Start;
  NodeFlags flags;
  std::uint32_t edgeOffset = 0;  // into the graph edge pool
  std::uint16_t numInputs = 0;   // current input count (fixed + variadic + FS)
  std::uint16_t reserved = 0;    // padding; always 0 (serialization checks)
  std::uint32_t payload = 0;     // kind-specific primary slot
  std::uint32_t payload2 = 0;    // kind-specific secondary slot
  std::int64_t constValue = 0;   // constants: exact bits (see NodeInfo doc)
  std::uint32_t specMeta = 0;    // SpecMetaId + 1; 0 = none (Rule 122)
  std::uint32_t epoch = 0;       // epoch when Replaced/Dead was set (Rule 14)

  [[nodiscard]] bool isDead() const noexcept {
    return flags.has(NodeFlag::Dead);
  }
};

} // namespace b2::ir
