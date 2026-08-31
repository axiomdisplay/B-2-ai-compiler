// B-2 IR - the NodeInfo registry: one row per NodeKind (Rule 130).
//
// WHY THIS FILE EXISTS:
// Rule 130 demands every node kind carry a specification; the charter makes
// it machine-checkable: this table is the normative input signature / effect
// classification that the verifier enforces and docs/ir_spec.md documents in
// prose. A test asserts the table covers every kind exactly once with unique
// names, so adding a NodeKind without a row (and without a doc entry) fails
// the suite - the "documentation lint" enforcement path.
//
// Signature conventions:
//   - roles[] is the fixed prefix; numFixed counts it; variadic kinds may
//     append inputs of variadicRole; hasFrameState adds a mandatory trailing
//     FrameState input (checked AFTER variadic inputs).
//   - effect is the PRIMARY reorder-algebra class (Rule 121). PEA and
//     inlining refine memory effects (WriteShared -> WriteLocal on
//     proven-non-escaping objects); that refinement is a pass-level proof,
//     not a registry change.

#include "b2/ir/Node.h"

#include <array>

namespace b2::ir {

namespace {

using EK = EffectKind;
using IR = InputRole;
using NC = NodeClass;

constexpr NodeInfo makeRow(const char* name, NC cls, std::uint8_t numFixed,
                           bool variadic, InputRole variadicRole,
                           bool hasFrameState, EffectKind effect,
                           std::initializer_list<InputRole> roles) {
  NodeInfo row{};
  row.name = name;
  row.cls = cls;
  row.numFixed = numFixed;
  row.variadic = variadic;
  row.variadicRole = variadicRole;
  row.hasFrameState = hasFrameState;
  row.effect = effect;
  std::uint8_t i = 0;
  for (InputRole r : roles) {
    if (i < 6) {
      row.roles[i] = r;
    }
    ++i;
  }
  return row;
}

// The registry, indexed by NodeKind. Order MUST match the enum; the
// completeness test walks [0, _Count) and pins this.
constexpr std::array<NodeInfo, static_cast<std::size_t>(NodeKind::_Count)>
    kRegistry = {
  // --- control ------------------------------------------------------------
  makeRow("Start", NC::Control, 0, false, IR::None, false, EK::Pure, {}),
  makeRow("End", NC::Control, 0, true, IR::Ctrl, false, EK::Pure, {}),
  makeRow("Region", NC::Control, 0, true, IR::Ctrl, false, EK::Pure, {}),
  makeRow("If", NC::Control, 2, false, IR::None, false, EK::Pure,
          {IR::Ctrl, IR::Data}),
  makeRow("IfTrue", NC::Control, 1, false, IR::None, false, EK::Pure,
          {IR::Parent}),
  makeRow("IfFalse", NC::Control, 1, false, IR::None, false, EK::Pure,
          {IR::Parent}),
  makeRow("Switch", NC::Control, 2, false, IR::None, false, EK::Pure,
          {IR::Ctrl, IR::Data}),
  makeRow("SwitchCase", NC::Control, 1, false, IR::None, false, EK::Pure,
          {IR::Parent}),
  makeRow("SwitchDefault", NC::Control, 1, false, IR::None, false, EK::Pure,
          {IR::Parent}),
  makeRow("LoopBegin", NC::Control, 0, true, IR::Ctrl, false, EK::Pure, {}),
  makeRow("LoopEnd", NC::Control, 1, false, IR::None, false, EK::Pure,
          {IR::Ctrl}),
  makeRow("LoopExit", NC::Control, 1, false, IR::None, true, EK::Pure,
          {IR::Ctrl}),
  makeRow("Return", NC::Control, 1, true, IR::Data, false, EK::Pure,
          {IR::Ctrl}),
  makeRow("Unwind", NC::Control, 2, false, IR::None, false, EK::ExceptionThrow,
          {IR::Ctrl, IR::Data}),
  makeRow("Deopt", NC::Control, 1, false, IR::None, true, EK::Pure,
          {IR::Ctrl}),

  // --- memory ---------------------------------------------------------------
  makeRow("LoadField", NC::Memory, 3, false, IR::None, false, EK::ReadShared,
          {IR::Ctrl, IR::Mem, IR::Data}),
  makeRow("StoreField", NC::Memory, 4, false, IR::None, false, EK::WriteShared,
          {IR::Ctrl, IR::Mem, IR::Data, IR::Data}),
  makeRow("LoadStatic", NC::Memory, 2, false, IR::None, false, EK::ReadShared,
          {IR::Ctrl, IR::Mem}),
  makeRow("StoreStatic", NC::Memory, 3, false, IR::None, false,
          EK::WriteShared, {IR::Ctrl, IR::Mem, IR::Data}),
  makeRow("LoadElem", NC::Memory, 4, false, IR::None, false, EK::ReadShared,
          {IR::Ctrl, IR::Mem, IR::Data, IR::Data}),
  makeRow("StoreElem", NC::Memory, 5, false, IR::None, false, EK::WriteShared,
          {IR::Ctrl, IR::Mem, IR::Data, IR::Data, IR::Data}),
  makeRow("ArrayLength", NC::Value, 1, false, IR::None, false, EK::ReadShared,
          {IR::Data}),
  makeRow("MemBar", NC::Memory, 2, false, IR::None, false, EK::VolatileWrite,
          {IR::Ctrl, IR::Mem}),
  makeRow("MonitorEnter", NC::Memory, 3, false, IR::None, false,
          EK::MonitorEnter, {IR::Ctrl, IR::Mem, IR::Data}),
  makeRow("MonitorExit", NC::Memory, 3, false, IR::None, false,
          EK::MonitorExit, {IR::Ctrl, IR::Mem, IR::Data}),
  makeRow("New", NC::Memory, 1, false, IR::None, false, EK::Allocation,
          {IR::Ctrl}),
  makeRow("NewArray", NC::Memory, 2, false, IR::None, false, EK::Allocation,
          {IR::Ctrl, IR::Data}),
  makeRow("NewRefArray", NC::Memory, 2, false, IR::None, false, EK::Allocation,
          {IR::Ctrl, IR::Data}),
  makeRow("NewMultiArray", NC::Memory, 1, true, IR::Data, false,
          EK::Allocation, {IR::Ctrl}),
  makeRow("ClassInit", NC::Memory, 2, false, IR::None, false, EK::CallOpaque,
          {IR::Ctrl, IR::Mem}),

  // --- calls ------------------------------------------------------------------
  makeRow("CallStatic", NC::Call, 2, true, IR::Data, true, EK::CallOpaque,
          {IR::Ctrl, IR::Mem}),
  makeRow("CallVirtual", NC::Call, 2, true, IR::Data, true, EK::CallOpaque,
           {IR::Ctrl, IR::Mem}),
  makeRow("CallInterface", NC::Call, 2, true, IR::Data, true, EK::CallOpaque,
          {IR::Ctrl, IR::Mem}),
  makeRow("CallDynamic", NC::Call, 2, true, IR::Data, true, EK::CallOpaque,
          {IR::Ctrl, IR::Mem}),
  makeRow("CallExcept", NC::Call, 1, false, IR::None, false, EK::Pure,
          {IR::Parent}),
  makeRow("LoadException", NC::Call, 1, false, IR::None, false, EK::Pure,
          {IR::Ctrl}),

  // --- object / type ops --------------------------------------------------------
  makeRow("CheckCast", NC::TypeOp, 2, false, IR::None, false,
          EK::ExceptionThrow, {IR::Ctrl, IR::Data}),
  makeRow("InstanceOf", NC::TypeOp, 1, false, IR::None, false, EK::ReadShared,
          {IR::Data}),

  // --- guards -------------------------------------------------------------------
  makeRow("Guard", NC::Guard, 2, false, IR::None, true, EK::Pure,
          {IR::Ctrl, IR::Data}),

  // --- constants / parameters ------------------------------------------------
  makeRow("ConstantI", NC::Value, 0, false, IR::None, false, EK::Pure, {}),
  makeRow("ConstantL", NC::Value, 0, false, IR::None, false, EK::Pure, {}),
  makeRow("ConstantF", NC::Value, 0, false, IR::None, false, EK::Pure, {}),
  makeRow("ConstantD", NC::Value, 0, false, IR::None, false, EK::Pure, {}),
  makeRow("ConstantNull", NC::Value, 0, false, IR::None, false, EK::Pure, {}),
  makeRow("ConstantSym", NC::Value, 0, false, IR::None, false, EK::Pure, {}),
  makeRow("Parameter", NC::Value, 0, false, IR::None, false, EK::Pure, {}),

  // --- int arithmetic ---------------------------------------------------------
  makeRow("AddI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("SubI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("MulI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("DivI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("RemI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("NegI", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("ShlI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("ShrI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("UShrI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("AndI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("OrI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("XorI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),

  // --- long arithmetic (shift counts stay Int, per JVM) -------------------------
  makeRow("AddL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("SubL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("MulL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("DivL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("RemL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("NegL", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("ShlL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("ShrL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("UShrL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("AndL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("OrL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("XorL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),

  // --- float arithmetic ---------------------------------------------------------
  makeRow("AddF", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("SubF", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("MulF", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("DivF", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("RemF", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("NegF", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),

  // --- double arithmetic ---------------------------------------------------------
  makeRow("AddD", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("SubD", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("MulD", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("DivD", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("RemD", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("NegD", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),

  // --- comparisons -----------------------------------------------------------------
  makeRow("CmpI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("CmpL", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("CmpFl", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("CmpFg", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("CmpDl", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("CmpDg", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),

  // --- conversions (Rule 33: explicit only) --------------------------------------
  makeRow("I2L", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("I2F", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("I2D", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("L2I", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("L2F", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("L2D", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("F2I", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("F2L", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("F2D", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("D2I", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("D2L", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("D2F", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("I2B", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("I2C", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("I2S", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("SignExtend", NC::Value, 1, false, IR::None, false, EK::Pure,
          {IR::Data}),
  makeRow("ZeroExtend", NC::Value, 1, false, IR::None, false, EK::Pure,
          {IR::Data}),
  makeRow("Truncate", NC::Value, 1, false, IR::None, false, EK::Pure,
          {IR::Data}),
  makeRow("BitCast", NC::Value, 1, false, IR::None, false, EK::Pure,
          {IR::Data}),
  makeRow("CompressedRefEncode", NC::Value, 1, false, IR::None, false,
          EK::Pure, {IR::Data}),
  makeRow("CompressedRefDecode", NC::Value, 1, false, IR::None, false,
          EK::Pure, {IR::Data}),
  makeRow("BoxPrimitive", NC::Value, 1, false, IR::None, false, EK::Pure,
          {IR::Data}),
  makeRow("UnboxPrimitive", NC::Value, 1, false, IR::None, false, EK::Pure,
          {IR::Data}),

  // --- merges -------------------------------------------------------------------
  makeRow("Phi", NC::Value, 1, true, IR::Data, false, EK::Pure, {IR::Ctrl}),

  // --- vector / SIMD (SWLP, Part XVIII) -----------------------------------------
  makeRow("VectorBroadcast", NC::Value, 1, false, IR::None, false, EK::Pure,
          {IR::Data}),
  makeRow("VectorPack", NC::Value, 0, true, IR::Data, false, EK::Pure, {}),
  makeRow("VectorExtract", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("VectorInsert", NC::Value, 3, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data, IR::Data}),
  makeRow("VectorOp", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("VectorMaskOp", NC::Value, 3, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data, IR::Data}),
  makeRow("VectorLoad", NC::Memory, 4, false, IR::None, false, EK::ReadShared,
          {IR::Ctrl, IR::Mem, IR::Data, IR::Data}),
  makeRow("VectorStore", NC::Memory, 5, false, IR::None, false,
          EK::WriteShared,
          {IR::Ctrl, IR::Mem, IR::Data, IR::Data, IR::Data}),
  makeRow("VectorReduce", NC::Value, 1, false, IR::None, false, EK::Pure,
          {IR::Data}),

  // --- PEA virtual objects / materialization (Part XVIII) ---------------------
  makeRow("VirtualObjectState", NC::Value, 0, true, IR::Data, false, EK::Pure,
          {}),
  makeRow("Materialize", NC::Memory, 3, false, IR::None, false, EK::Allocation,
          {IR::Ctrl, IR::Mem, IR::Data}),

  // --- tagged values (NaN boxing, Part XVIII) -----------------------------------
  makeRow("TagValue", NC::Value, 1, false, IR::None, false, EK::Pure,
          {IR::Data}),
  makeRow("UntagValue", NC::Value, 1, false, IR::None, false, EK::Pure,
          {IR::Data}),
  makeRow("RepTransitionGuard", NC::Guard, 2, false, IR::None, true, EK::Pure,
          {IR::Ctrl, IR::Data}),

  // --- state --------------------------------------------------------------------
  makeRow("FrameState", NC::State, 0, true, IR::Data, false, EK::Pure, {}),

  // --- v2 additions (MSG-20260831-007): graph-builder vocabulary --------------
  // Appended after v1's last row so kind VALUES are unchanged (Rule 31).
  // Undef is the uninitialized-slot placeholder: FrameState / Phi inputs only
  // (the verifier rejects every typed-operand use; its Bottom type is
  // assignable to nothing).
  makeRow("Undef", NC::Value, 0, false, IR::None, false, EK::Pure, {}),
  makeRow("Not", NC::Value, 1, false, IR::None, false, EK::Pure, {IR::Data}),
  makeRow("IsNull", NC::Value, 1, false, IR::None, false, EK::Pure,
          {IR::Data}),
  makeRow("RefEq", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("EqI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("NeI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("LtI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("LeI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("GtI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
  makeRow("GeI", NC::Value, 2, false, IR::None, false, EK::Pure,
          {IR::Data, IR::Data}),
};

const NodeInfo kBadNodeInfo = [] {
  NodeInfo row{};
  row.name = "bad<kind>";
  row.cls = NodeClass::Value;
  row.numFixed = 0;
  row.variadic = false;
  row.variadicRole = InputRole::None;
  row.hasFrameState = false;
  row.effect = EffectKind::Pure;
  return row;
}();

} // namespace

const NodeInfo& info(NodeKind kind) noexcept {
  const std::uint16_t k = static_cast<std::uint16_t>(kind);
  if (k < static_cast<std::uint16_t>(NodeKind::_Count)) {
    return kRegistry[k];
  }
  return kBadNodeInfo;
}

const char* nodeKindName(NodeKind kind) noexcept { return info(kind).name; }

std::uint16_t registeredNodeKinds() noexcept {
  return static_cast<std::uint16_t>(kRegistry.size());
}

} // namespace b2::ir
