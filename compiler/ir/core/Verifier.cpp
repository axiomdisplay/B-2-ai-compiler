// B-2 IR - graph verifier (Rules 40, 126).
//
// WHY THIS FILE EXISTS:
// The verifier is the debug-build after-every-pass gate (Rule 40) and the
// replay load gate. It is deliberately TOTAL: any malformed graph yields a
// bounded diagnostic list (cap: named constant), never UB and never an
// unbounded walk - every id is range-checked before use and every chain walk
// is generation-stamped, so cycles terminate. Checks are ordered cheap-first
// (structure, roles, types) then semantic (memory chains, guard metadata,
// PEA closure, vector well-formedness).

#include "b2/ir/Verifier.h"

#include <vector>

#include "b2/ir/Printer.h"

namespace b2::ir {

namespace {

// Law 23: diagnostics cap is a named constant.
constexpr std::size_t kMaxDiags = 100;

struct Verifier {
  const Graph& g;
  std::vector<VerifyDiag> diags;

  explicit Verifier(const Graph& graph) : g(graph) {}

  void diag(NodeId n, const std::string& msg) {
    if (diags.size() < kMaxDiags) {
      VerifyDiag d;
      d.node = n;
      d.message = msg;
      diags.push_back(std::move(d));
    }
  }

  [[nodiscard]] bool validId(NodeId n) const {
    return n != kInvalidNodeId && n < g.nodeCount();
  }

  // --- classification predicates ------------------------------------------
  [[nodiscard]] static bool producesControl(NodeKind k) noexcept {
    // v2 (MSG-20260831-007): control chains through EVERY fixed node - a
    // node that consumes a Ctrl input is pinned into the control flow and
    // produces control for the fixed nodes after it (Graal's fixed-node
    // discipline). Terminals (Return/Unwind/Deopt/End) end chains. v1's
    // sibling-control graphs remain legal: this only ADDS legal Ctrl-input
    // producers, it removes none - and without it, a branch or terminal
    // following a same-block store would be schedulable BEFORE the store.
    switch (k) {
    case NodeKind::Start:
    case NodeKind::Region:
    case NodeKind::IfTrue:
    case NodeKind::IfFalse:
    case NodeKind::SwitchCase:
    case NodeKind::SwitchDefault:
    case NodeKind::LoopBegin:
    case NodeKind::LoopEnd:
    case NodeKind::LoopExit:
    case NodeKind::CallStatic:
    case NodeKind::CallVirtual:
    case NodeKind::CallInterface:
    case NodeKind::CallDynamic:
    case NodeKind::CallExcept:
    case NodeKind::LoadException:
    // Fixed nodes (consume Ctrl, produce Ctrl):
    case NodeKind::If:
    case NodeKind::Switch:
    case NodeKind::LoadField:
    case NodeKind::StoreField:
    case NodeKind::LoadStatic:
    case NodeKind::StoreStatic:
    case NodeKind::LoadElem:
    case NodeKind::StoreElem:
    case NodeKind::MemBar:
    case NodeKind::MonitorEnter:
    case NodeKind::MonitorExit:
    case NodeKind::New:
    case NodeKind::NewArray:
    case NodeKind::NewRefArray:
    case NodeKind::NewMultiArray:
    case NodeKind::ClassInit:
    case NodeKind::CheckCast:
    case NodeKind::Guard:
    case NodeKind::RepTransitionGuard:
    case NodeKind::Materialize:
    case NodeKind::VectorLoad:
    case NodeKind::VectorStore:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] static bool producesMemoryState(NodeKind k) noexcept {
    switch (k) {
    case NodeKind::Start:
    case NodeKind::StoreField:
    case NodeKind::StoreStatic:
    case NodeKind::StoreElem:
    case NodeKind::MemBar:
    case NodeKind::MonitorEnter:
    case NodeKind::MonitorExit:
    case NodeKind::New:
    case NodeKind::NewArray:
    case NodeKind::NewRefArray:
    case NodeKind::NewMultiArray:
    case NodeKind::ClassInit:
    case NodeKind::CallStatic:
    case NodeKind::CallVirtual:
    case NodeKind::CallInterface:
    case NodeKind::CallDynamic:
    case NodeKind::Materialize:
    case NodeKind::VectorStore:
      return true;
    case NodeKind::Phi:
      // v2 (MSG-20260831-007): the memory-state MERGE (Graal's MemoryPhi
      // shape). Legal only when every value input is itself a memory-state
      // producer (transitively) - checked in checkMemoryPhiInputs / the
      // role pass, so a Phi used as a Mem input can never smuggle a pure
      // value into the memory chain.
      return true;
    default:
      return false;
    }
  }

  // v2: a Phi feeding a Mem slot (or appearing inside a memory chain) must
  // have every value input (slots 1..) be a memory-state producer, allowing
  // nested memory Phis. Returns the offending input id, or kInvalidNodeId.
  // Depth-bounded: malformed graphs can contain Phi cycles.
  [[nodiscard]] NodeId memoryPhiOffender(NodeId phi, std::uint32_t depth) {
    if (depth > g.nodeCount() + 1) {
      return phi; // pathological nesting depth: report the outer Phi
    }
    const Node& p = g.node(phi);
    for (std::uint16_t s = 1; s < p.numInputs; ++s) {
      const NodeId in = g.input(phi, s);
      if (in == phi) {
        continue; // self input: loop-invariant marker, not a chain link
      }
      const NodeId off = memoryChainOffender(in, depth + 1);
      if (off != kInvalidNodeId) {
        return off;
      }
    }
    return kInvalidNodeId;
  }

  // One link of the memory-chain legality check: node `cur` must be a
  // memory-state producer (Phi handled recursively). Returns the first
  // offending node id, or kInvalidNodeId when `cur` is fine.
  [[nodiscard]] NodeId memoryChainOffender(NodeId cur, std::uint32_t depth) {
    if (!validId(cur)) {
      return kInvalidNodeId; // dangling ids are reported structurally
    }
    if (g.node(cur).kind == NodeKind::Phi) {
      return memoryPhiOffender(cur, depth);
    }
    return producesMemoryState(g.node(cur).kind) ? kInvalidNodeId : cur;
  }

  [[nodiscard]] static bool isTerminal(NodeKind k) noexcept {
    switch (k) {
    case NodeKind::Return:
    case NodeKind::Unwind:
    case NodeKind::Deopt:
    case NodeKind::End:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] static bool isCallKind(NodeKind k) noexcept {
    return k == NodeKind::CallStatic || k == NodeKind::CallVirtual ||
           k == NodeKind::CallInterface || k == NodeKind::CallDynamic;
  }

  // --- operand type helpers --------------------------------------------------
  [[nodiscard]] IRType typeOf(NodeId n) { return resultTypeOf(g, n); }

  // Scalar carrier type for a vector lane type: i8/i16 lanes carry their
  // values in Int scalars (byte/char/short fold into Int per the RBC type
  // discipline); packing into narrow lanes is a vector-formation decision,
  // not an implicit value coercion (documented in ir_spec.md SS vectors).
  [[nodiscard]] static IRType laneCarrier(IRType lane) noexcept {
    switch (lane) {
    case IRType::Int8:
    case IRType::Int16:
    case IRType::Int:
      return IRType::Int;
    case IRType::Long:
      return IRType::Long;
    case IRType::Float:
      return IRType::Float;
    case IRType::Double:
      return IRType::Double;
    default:
      return IRType::Bottom;
    }
  }

  void checkOperandType(NodeId n, NodeId operand, IRType want,
                        const char* what) {
    if (!validId(operand)) {
      return; // structural check already reported the dangling id
    }
    const IRType got = typeOf(operand);
    if (!isAssignable(got, want)) {
      diag(n, std::string(what) + ": operand n" + std::to_string(operand) +
                  " has type " + typeName(got) + ", expected " +
                  typeName(want));
    }
  }

  // Expected operand type pair for binary arithmetic / comparisons.
  [[nodiscard]] static bool binaryOperandTypes(NodeKind k, IRType& a,
                                               IRType& b) noexcept {
    switch (k) {
    case NodeKind::AddI: case NodeKind::SubI: case NodeKind::MulI:
    case NodeKind::DivI: case NodeKind::RemI:
    case NodeKind::ShlI: case NodeKind::ShrI: case NodeKind::UShrI:
    case NodeKind::AndI: case NodeKind::OrI: case NodeKind::XorI:
    case NodeKind::CmpI:
    case NodeKind::EqI: case NodeKind::NeI:
    case NodeKind::LtI: case NodeKind::LeI:
    case NodeKind::GtI: case NodeKind::GeI:
      a = IRType::Int; b = IRType::Int; return true;
    case NodeKind::RefEq:
      // v2: reference identity test (if_acmp*); Null <: Ref on either side.
      a = IRType::Ref; b = IRType::Ref; return true;
    case NodeKind::AddL: case NodeKind::SubL: case NodeKind::MulL:
    case NodeKind::DivL: case NodeKind::RemL:
    case NodeKind::AndL: case NodeKind::OrL: case NodeKind::XorL:
    case NodeKind::CmpL:
      a = IRType::Long; b = IRType::Long; return true;
    case NodeKind::ShlL: case NodeKind::ShrL: case NodeKind::UShrL:
      a = IRType::Long; b = IRType::Int; return true; // shift count is Int
    case NodeKind::AddF: case NodeKind::SubF: case NodeKind::MulF:
    case NodeKind::DivF: case NodeKind::RemF:
    case NodeKind::CmpFl: case NodeKind::CmpFg:
      a = IRType::Float; b = IRType::Float; return true;
    case NodeKind::AddD: case NodeKind::SubD: case NodeKind::MulD:
    case NodeKind::DivD: case NodeKind::RemD:
    case NodeKind::CmpDl: case NodeKind::CmpDg:
      a = IRType::Double; b = IRType::Double; return true;
    default:
      return false;
    }
  }

  [[nodiscard]] static bool unaryOperandType(NodeKind k, IRType& a) noexcept {
    switch (k) {
    case NodeKind::NegI:
      a = IRType::Int; return true;
    case NodeKind::Not:
      // v2: logical complement of a boolean-test Int.
      a = IRType::Int; return true;
    case NodeKind::IsNull:
      // v2: null test; Null <: Ref accepted (ConstantNull feeds it).
      a = IRType::Ref; return true;
    case NodeKind::NegL:
      a = IRType::Long; return true;
    case NodeKind::NegF:
      a = IRType::Float; return true;
    case NodeKind::NegD:
      a = IRType::Double; return true;
    case NodeKind::I2L: case NodeKind::I2F: case NodeKind::I2D:
    case NodeKind::I2B: case NodeKind::I2C: case NodeKind::I2S:
    case NodeKind::SignExtend: case NodeKind::ZeroExtend:
      a = IRType::Int; return true;
    case NodeKind::L2I: case NodeKind::L2F: case NodeKind::L2D:
    case NodeKind::Truncate:
      a = IRType::Long; return true;
    case NodeKind::F2I: case NodeKind::F2L: case NodeKind::F2D:
      a = IRType::Float; return true;
    case NodeKind::D2I: case NodeKind::D2L: case NodeKind::D2F:
      a = IRType::Double; return true;
    case NodeKind::CompressedRefEncode:
    case NodeKind::CompressedRefDecode:
      return false; // checked inline (pair rule)
    default:
      return false;
    }
  }

  // --- pass 1: structure + roles + types --------------------------------------
  void checkNode(NodeId n) {
    const Node& nd = g.node(n);
    const NodeInfo& row = info(nd.kind);

    if (nd.reserved != 0) {
      diag(n, "reserved field must be 0 (corrupt node record)");
    }

    // Structural: ids alive, no self reference. EXEMPTION (v2,
    // MSG-20260831-007): a Phi value input may be the Phi itself - the
    // loop-invariant marker of the standard sea-of-nodes loop phi (the
    // value flows around the backedge unchanged); the graph builder relies
    // on it whenever a slot's backedge definition is the header phi.
    for (std::uint16_t s = 0; s < nd.numInputs; ++s) {
      const NodeId in = g.input(n, s);
      if (!validId(in)) {
        diag(n, "input slot " + std::to_string(s) + ": dangling NodeId " +
                    std::to_string(in));
      } else if (in == n && !(nd.kind == NodeKind::Phi && s >= 1)) {
        diag(n, "input slot " + std::to_string(s) + ": self reference");
      } else if (g.node(in).isDead()) {
        diag(n, "input slot " + std::to_string(s) + ": n" +
                    std::to_string(in) + " is dead");
      }
    }

    // Signature shape: fixed prefix + variadic + trailing FrameState.
    const std::uint16_t fsSlots = row.hasFrameState ? 1 : 0;
    const std::uint16_t minInputs = row.numFixed + fsSlots;
    if (nd.numInputs < minInputs) {
      diag(n, std::string(info(nd.kind).name) + ": expected at least " +
                  std::to_string(minInputs) + " inputs, found " +
                  std::to_string(nd.numInputs));
    }
    if (!row.variadic && nd.numInputs != minInputs) {
      diag(n, std::string(info(nd.kind).name) + ": expected exactly " +
                  std::to_string(minInputs) + " inputs, found " +
                  std::to_string(nd.numInputs) + " (kind is not variadic)");
    }
    if (row.hasFrameState && nd.numInputs >= minInputs) {
      const NodeId fs = g.input(n, nd.numInputs - 1);
      if (validId(fs) && g.node(fs).kind != NodeKind::FrameState) {
        diag(n, "last input must be a FrameState (Rule 5)");
      }
    }

    // Role conformance per slot.
    for (std::uint16_t s = 0; s < nd.numInputs; ++s) {
      const NodeId in = g.input(n, s);
      if (!validId(in)) {
        continue;
      }
      const Node& prod = g.node(in);
      InputRole role;
      if (s < row.numFixed) {
        role = row.roles[s];
      } else if (row.hasFrameState && s + 1 == nd.numInputs) {
        role = InputRole::FrameState;
      } else {
        role = row.variadicRole;
      }
      switch (role) {
      case InputRole::Ctrl:
        if (!producesControl(prod.kind)) {
          diag(n, "ctrl input slot " + std::to_string(s) + ": n" +
                      std::to_string(in) + " (" + info(prod.kind).name +
                      ") does not produce control");
        }
        break;
      case InputRole::Mem:
        if (!producesMemoryState(prod.kind)) {
          diag(n, "mem input slot " + std::to_string(s) + ": n" +
                      std::to_string(in) + " (" + info(prod.kind).name +
                      ") does not produce memory state");
        } else if (prod.kind == NodeKind::Phi) {
          const NodeId off = memoryPhiOffender(in, 0);
          if (off != kInvalidNodeId) {
            diag(n, "mem input slot " + std::to_string(s) +
                        ": Phi n" + std::to_string(in) +
                        " has value input n" + std::to_string(off) +
                        " (" + info(g.node(off).kind).name +
                        ") which does not produce memory state");
          }
        }
        break;
      case InputRole::FrameState:
        if (prod.kind != NodeKind::FrameState) {
          diag(n, "framestate input slot " + std::to_string(s) +
                      ": n" + std::to_string(in) + " is not a FrameState");
        }
        break;
      case InputRole::Parent:
        // Parent-kind rule lives in checkProjectionParent (below).
        break;
      case InputRole::Data:
      case InputRole::None:
        break;
      }

      // v2 (MSG-20260831-007): Undef is the uninitialized-slot placeholder.
      // It may feed a FrameState (a Bottom-tagged deopt slot) or a Phi (the
      // "Bottom on this path" marker); every other use would read an
      // uninitialized value, which verified RBC never does.
      if (prod.kind == NodeKind::Undef && nd.kind != NodeKind::FrameState &&
          !(nd.kind == NodeKind::Phi && s >= 1)) {
        diag(n, "Undef may only feed a FrameState or a Phi value input; n" +
                    std::to_string(n) + " (" + info(nd.kind).name +
                    ") reads it at slot " + std::to_string(s));
      }
    }

    // Terminals must have no users at all (they produce neither control nor
    // values).
    if (isTerminal(nd.kind) && !g.usesOf(n).empty()) {
      diag(n, "terminal node still has users");
    }

    checkProjectionParent(n);
    checkMergeShapes(n);
    checkOperandTypes(n);
    checkKindPayloads(n);
  }

  // Phi/Region/LoopBegin/Return structural shapes.
  void checkMergeShapes(NodeId n) {
    const Node& nd = g.node(n);
    switch (nd.kind) {
    case NodeKind::Phi: {
      if (nd.numInputs < 1) {
        break; // signature check already reported
      }
      const NodeId region = g.input(n, 0);
      if (!validId(region)) {
        break;
      }
      const Node& r = g.node(region);
      if (r.kind != NodeKind::Region && r.kind != NodeKind::LoopBegin) {
        diag(n, "Phi input 0 must be a Region or LoopBegin");
        break;
      }
      if (nd.numInputs != r.numInputs + 1) {
        diag(n, "Phi input count must be 1 + region predecessors (" +
                    std::to_string(r.numInputs) + "), found " +
                    std::to_string(nd.numInputs - 1));
      }
      break;
    }
    case NodeKind::Region:
      if (nd.numInputs < 2) {
        diag(n, "Region needs at least 2 predecessors at verification time");
      }
      break;
    case NodeKind::LoopBegin:
      if (nd.numInputs < 2) {
        diag(n, "LoopBegin needs an entry and at least one backedge");
      }
      break;
    case NodeKind::Return: {
      const std::uint16_t values = nd.numInputs > 0 ? nd.numInputs - 1 : 0;
      if (values > 1) {
        diag(n, "Return takes at most one value input");
      }
      break;
    }
    default:
      break;
    }
  }

  void checkProjectionParent(NodeId n) {
    const Node& nd = g.node(n);
    NodeKind wantParent = NodeKind::Start;
    bool needsParent = false;
    switch (nd.kind) {
    case NodeKind::IfTrue:
    case NodeKind::IfFalse:
      needsParent = true;
      wantParent = NodeKind::If;
      break;
    case NodeKind::SwitchCase:
    case NodeKind::SwitchDefault:
      needsParent = true;
      wantParent = NodeKind::Switch;
      break;
    case NodeKind::CallExcept:
      needsParent = true;
      break; // any Call*
    default:
      break;
    }
    if (!needsParent || nd.numInputs < 1) {
      return;
    }
    const NodeId p = g.input(n, 0);
    if (!validId(p)) {
      return;
    }
    const Node& parent = g.node(p);
    if (nd.kind == NodeKind::CallExcept) {
      if (!isCallKind(parent.kind)) {
        diag(n, "CallExcept input must be a Call node");
      }
    } else if (parent.kind != wantParent) {
      diag(n, std::string(info(nd.kind).name) + " input must be a " +
                  info(wantParent).name + " node");
    }
  }

  void checkOperandTypes(NodeId n) {
    const Node& nd = g.node(n);

    IRType a = IRType::Bottom, b = IRType::Bottom;
    if (binaryOperandTypes(nd.kind, a, b)) {
      if (nd.numInputs >= 2) {
        checkOperandType(n, g.input(n, 0), a, "binary operand a");
        checkOperandType(n, g.input(n, 1), b, "binary operand b");
      }
      return;
    }
    if (unaryOperandType(nd.kind, a)) {
      if (nd.numInputs >= 1) {
        checkOperandType(n, g.input(n, 0), a, "unary operand");
      }
      return;
    }

    switch (nd.kind) {
    case NodeKind::If:
      if (nd.numInputs >= 2) {
        checkOperandType(n, g.input(n, 1), IRType::Int, "If condition");
      }
      break;
    case NodeKind::Switch:
      if (nd.numInputs >= 2) {
        checkOperandType(n, g.input(n, 1), IRType::Int, "Switch selector");
      }
      break;
    case NodeKind::Guard:
      if (nd.numInputs >= 2) {
        checkOperandType(n, g.input(n, 1), IRType::Int, "Guard condition");
      }
      break;
    case NodeKind::Unwind:
      if (nd.numInputs >= 2) {
        checkOperandType(n, g.input(n, 1), IRType::Ref, "Unwind exception");
      }
      break;
    case NodeKind::LoadField:
      if (nd.numInputs >= 3) {
        checkOperandType(n, g.input(n, 2), IRType::Ref, "LoadField object");
      }
      break;
    case NodeKind::StoreField:
      if (nd.numInputs >= 4) {
        checkOperandType(n, g.input(n, 2), IRType::Ref, "StoreField object");
      }
      break;
    case NodeKind::LoadElem:
      if (nd.numInputs >= 4) {
        checkOperandType(n, g.input(n, 2), IRType::Ref, "LoadElem array");
        checkOperandType(n, g.input(n, 3), IRType::Int, "LoadElem index");
      }
      break;
    case NodeKind::StoreElem:
      if (nd.numInputs >= 5) {
        checkOperandType(n, g.input(n, 2), IRType::Ref, "StoreElem array");
        checkOperandType(n, g.input(n, 3), IRType::Int, "StoreElem index");
        checkOperandType(n, g.input(n, 4), static_cast<IRType>(nd.payload),
                         "StoreElem value");
      }
      break;
    case NodeKind::ArrayLength:
      if (nd.numInputs >= 1) {
        checkOperandType(n, g.input(n, 0), IRType::Ref, "ArrayLength array");
      }
      break;
    case NodeKind::MonitorEnter:
    case NodeKind::MonitorExit:
      if (nd.numInputs >= 3) {
        checkOperandType(n, g.input(n, 2), IRType::Ref, "monitor object");
      }
      break;
    case NodeKind::NewArray:
    case NodeKind::NewRefArray:
      if (nd.numInputs >= 2) {
        checkOperandType(n, g.input(n, 1), IRType::Int, "array length");
      }
      break;
    case NodeKind::NewMultiArray:
      for (std::uint16_t s = 1; s < nd.numInputs; ++s) {
        checkOperandType(n, g.input(n, s), IRType::Int, "array dimension");
      }
      break;
    case NodeKind::CheckCast:
      if (nd.numInputs >= 2) {
        checkOperandType(n, g.input(n, 1), IRType::Ref, "CheckCast object");
      }
      break;
    case NodeKind::InstanceOf:
      if (nd.numInputs >= 1) {
        checkOperandType(n, g.input(n, 0), IRType::Ref, "InstanceOf object");
      }
      break;
    case NodeKind::CompressedRefEncode:
      if (nd.numInputs >= 1) {
        checkOperandType(n, g.input(n, 0), IRType::Ref,
                         "CompressedRefEncode reference");
      }
      break;
    case NodeKind::CompressedRefDecode:
      if (nd.numInputs >= 1) {
        checkOperandType(n, g.input(n, 0), IRType::Int,
                         "CompressedRefDecode oop");
      }
      break;
    case NodeKind::UntagValue:
      if (nd.numInputs >= 1) {
        checkOperandType(n, g.input(n, 0), IRType::Tagged,
                         "UntagValue tagged value");
      }
      break;
    case NodeKind::RepTransitionGuard:
      if (nd.numInputs >= 2) {
        checkOperandType(n, g.input(n, 1), IRType::Tagged,
                         "RepTransitionGuard value");
      }
      break;
    case NodeKind::Materialize:
      if (nd.numInputs >= 3) {
        const NodeId v = g.input(n, 2);
        if (validId(v) && g.node(v).kind != NodeKind::VirtualObjectState) {
          diag(n, "Materialize input 2 must be a VirtualObjectState");
        }
      }
      break;
    default:
      break;
    }
  }

  void checkKindPayloads(NodeId n) {
    const Node& nd = g.node(n);
    switch (nd.kind) {
    case NodeKind::Parameter: {
      const IRType t = static_cast<IRType>(nd.payload2);
      if (!isScalarType(t)) {
        diag(n, "Parameter type must be a scalar IRType");
      }
      break;
    }
    case NodeKind::LoadField:
    case NodeKind::LoadStatic:
    case NodeKind::UnboxPrimitive: {
      const IRType t = static_cast<IRType>(nd.payload2);
      if (!isScalarType(t)) {
        diag(n, "load result type must be a scalar IRType");
      }
      break;
    }
    case NodeKind::LoadElem:
    case NodeKind::StoreElem:
    case NodeKind::NewArray: {
      const IRType t = static_cast<IRType>(nd.payload);
      if (!isScalarType(t)) {
        diag(n, "element type must be a scalar IRType");
      }
      break;
    }
    case NodeKind::MemBar:
      if (nd.payload > static_cast<std::uint32_t>(MemBarKind::Release)) {
        diag(n, "MemBar kind out of range");
      }
      break;
    case NodeKind::Guard:
      if (nd.payload > static_cast<std::uint32_t>(GuardKind::Deoptimize)) {
        diag(n, "Guard kind out of range");
      }
      break;
    case NodeKind::VirtualObjectState:
      if (nd.payload2 > 1) {
        diag(n, "VirtualObjectState kind must be 0 (instance) or 1 (array)");
      }
      break;
    default:
      break;
    }
  }

  // --- pass 2: memory chain continuity (Rules 40, 121) -------------------------
  void checkMemoryChains() {
    std::vector<std::uint32_t> onPath(g.nodeCount(), 0);
    std::vector<std::uint32_t> doneStamp(g.nodeCount(), 0);
    std::uint32_t generation = 0;
    for (NodeId n = 0; n < g.nodeCount(); ++n) {
      const Node& nd = g.node(n);
      if (nd.isDead() || nd.numInputs < 1) {
        continue;
      }
      const NodeInfo& row = info(nd.kind);
      // Find the Mem input slot (registry: Mem appears in the fixed prefix).
      std::uint16_t memSlot = 0xFFFF;
      for (std::uint16_t s = 0; s < row.numFixed && s < 6; ++s) {
        if (row.roles[s] == InputRole::Mem) {
          memSlot = s;
          break;
        }
      }
      if (memSlot == 0xFFFF || memSlot >= nd.numInputs) {
        continue;
      }
      // Walk back through memory-state producers; every link must produce
      // memory state and the chain must terminate at Start. v2: the chain
      // may pass through memory Phis (control merges), which fork the walk
      // over the Phi's value inputs. DFS coloring: onPath marks the current
      // branch (a revisit from the SAME branch is a cycle), done marks
      // nodes proven on any earlier branch (legal DAG sharing).
      ++generation;
      struct WalkFrame {
        NodeId n;
        std::uint16_t child; // next child index to visit
      };
      std::vector<WalkFrame> stack;
      if (validId(g.input(n, memSlot))) {
        stack.push_back({g.input(n, memSlot), 0});
        onPath[stack.back().n] = generation;
      }
      std::uint32_t steps = 0;
      // MSG-20260901-007: the walk is a DFS with an explicit stack — each
      // visited node costs TWO while-iterations (one when pushed/visited,
      // one when popped after its children are proven). A well-formed
      // acyclic chain of L memory producers therefore costs ~2L steps, so
      // a belt of `nodeCount + 1` falsely trips whenever `2L > nodeCount + 1`
      // (i.e. any chain longer than half the graph). Size the belt for the
      // DFS: `2 * nodeCount + 2` admits any well-formed chain up to the
      // full node count while still catching genuine cycles (a cycle
      // exceeds any linear bound).
      const std::uint32_t maxSteps = 2 * g.nodeCount() + 2;
      while (!stack.empty()) {
        if (steps++ > maxSteps) {
          diag(n, "memory chain walk exceeded graph size (cycle?)");
          break;
        }
        WalkFrame& top = stack.back();
        const Node& c = g.node(top.n);
        // Children of a memory Phi are its value inputs (slot 0 is the
        // region); every other memory producer continues at its own Mem
        // input; Start (and chain roots) have none.
        std::uint16_t numChildren = 0;
        bool isPhi = c.kind == NodeKind::Phi;
        if (isPhi) {
          numChildren = c.numInputs > 1
                            ? static_cast<std::uint16_t>(c.numInputs - 1)
                            : 0;
        } else {
          const NodeInfo& crow = info(c.kind);
          for (std::uint16_t s = 0; s < crow.numFixed && s < 6; ++s) {
            if (crow.roles[s] == InputRole::Mem) {
              if (s < c.numInputs) {
                numChildren = 1;
              }
              break;
            }
          }
        }
        if (!isPhi && !producesMemoryState(c.kind)) {
          diag(n, "memory chain passes through n" + std::to_string(top.n) +
                      " (" + info(c.kind).name +
                      ") which does not produce memory state");
          break;
        }
        if (top.child >= numChildren) {
          // Node fully proven on this branch.
          onPath[top.n] = 0;
          doneStamp[top.n] = generation;
          stack.pop_back();
          continue;
        }
        const std::uint16_t childIdx = top.child++;
        NodeId child{};
        if (isPhi) {
          // Self inputs are the loop-invariant marker: nothing to prove.
          if (g.input(top.n, static_cast<std::uint16_t>(childIdx + 1)) ==
              top.n) {
            continue;
          }
          child = g.input(top.n, static_cast<std::uint16_t>(childIdx + 1));
        } else {
          const NodeInfo& crow = info(c.kind);
          for (std::uint16_t s = 0; s < crow.numFixed && s < 6; ++s) {
            if (crow.roles[s] == InputRole::Mem) {
              child = g.input(top.n, s);
              break;
            }
          }
        }
        if (!validId(child)) {
          continue; // dangling ids are reported structurally
        }
        if (child == g.startNode()) {
          continue; // proven: terminates at Start
        }
        if (onPath[child] == generation) {
          const Node& cn = g.node(child);
          if (cn.kind == NodeKind::Phi && cn.numInputs >= 1) {
            const NodeId region = g.input(child, 0);
            if (validId(region) &&
                g.node(region).kind == NodeKind::LoopBegin) {
              // MSG-20260901-004: an on-path revisit of a LOOP-HEADER
              // Phi is a backedge closure. The walk forked over the
              // header phi's value inputs and followed the BACKEDGE
              // input through the body's memory producers back to the
              // header itself; the phi is being proven through its
              // other inputs (the entry side terminates at Start; the
              // loop-invariant side is the self-input skip above). In
              // reducible flow (the builder rejects irreducible input)
              // every LEGAL memory-chain cycle closes at a LoopBegin
              // header phi: forward merges (Region phis) have no
              // backedge, so a closure at one is a real cycle and
              // still reports below. A non-Phi revisit also stays a
              // cycle error; the maxSteps bound is the belt.
              continue;
            }
          }
          diag(n, "memory chain cycle detected at n" +
                      std::to_string(child));
          break;
        }
        if (doneStamp[child] == generation) {
          continue; // proven on an earlier branch (legal DAG sharing)
        }
        stack.push_back({child, 0});
        onPath[child] = generation;
      }
      for (const WalkFrame& f : stack) {
        onPath[f.n] = 0; // unwind partial state on early exit
      }
    }
  }

  // --- pass 3: guards, speculation, FrameStates (Rules 5, 42, 122, 126) ------
  void checkSpeculation(NodeId n) {
    const Node& nd = g.node(n);
    if (nd.flags.has(NodeFlag::Speculative)) {
      if (nd.specMeta == 0) {
        diag(n, "speculative node has no SpecMeta (Rule 122)");
      } else {
        const SpecMeta& sm = g.specMeta(nd.specMeta - 1);
        if (sm.confidence > 10000) {
          diag(n, "SpecMeta confidence exceeds 10000 basis points");
        }
        if (sm.source == SpecMeta::Source::PGO ||
            sm.source == SpecMeta::Source::Assumption) {
          if (sm.dependency == kInvalidDependency ||
              sm.dependency >= g.dependencyCount()) {
            diag(n, "PGO/assumption speculation without a valid dependency "
                    "(Rule 42)");
          }
        }
        if (sm.source != SpecMeta::Source::Static && sm.guard == kInvalidNodeId) {
          diag(n, "non-static speculation has no guard plan (Rule 122)");
        }
        if (sm.source == SpecMeta::Source::Static &&
            sm.guard != kInvalidNodeId) {
          // legal: a static proof may still carry a belt-and-suspenders guard
        }
      }
    }
    if (nd.kind == NodeKind::RepTransitionGuard && !nd.flags.has(NodeFlag::Speculative)) {
      diag(n, "RepTransitionGuard must be speculative (Part XVIII)");
    }
  }

  void checkFrameStates() {
    // MSG-20260901-002: a caller chain must resolve to a LIVE snapshot
    // node. The chain reference is side-table data (a desc id, not a
    // use-def edge), so use-def invariants cannot see a killed
    // chain-target FrameState; without this check a dead target would
    // pass verification while its input edges - junked by the kill - no
    // longer carry the caller-frame slot values the deoptimizer needs
    // (Rule 75: frames reconstructible on demand).
    std::vector<std::uint8_t> descHasLiveFs(g.frameStateCount(), 0);
    for (NodeId n = 0; n < g.nodeCount(); ++n) {
      const Node& nd = g.node(n);
      if (nd.kind == NodeKind::FrameState && !nd.isDead() &&
          nd.payload < g.frameStateCount()) {
        descHasLiveFs[nd.payload] = 1;
      }
    }
    for (NodeId n = 0; n < g.nodeCount(); ++n) {
      const Node& nd = g.node(n);
      if (nd.kind != NodeKind::FrameState || nd.isDead()) {
        continue;
      }
      if (nd.payload >= g.frameStateCount()) {
        diag(n, "FrameState descriptor id out of range");
        continue;
      }
      // Caller chain acyclicity.
      std::uint32_t steps = 0;
      const std::uint32_t maxSteps = g.frameStateCount() + 1;
      FrameStateId cur = nd.payload;
      while (cur != kInvalidFrameState) {
        if (steps++ > maxSteps) {
          diag(n, "FrameState caller chain cycle");
          break;
        }
        if (cur >= g.frameStateCount()) {
          diag(n, "FrameState caller id out of range");
          break;
        }
        if (descHasLiveFs[cur] == 0) {
          diag(n, "FrameState caller chain target " +
                      std::to_string(cur) +
                      " has no live snapshot node");
          break;
        }
        cur = g.frameState(cur).caller;
      }
      // Vobj entries are VirtualObjectState nodes.
      for (VirtualObjectId v : g.frameStateVobjs(nd.payload)) {
        if (!validId(v)) {
          diag(n, "FrameState vobj list contains a dangling id");
        } else if (g.node(v).kind != NodeKind::VirtualObjectState) {
          diag(n, "FrameState vobj entry is not a VirtualObjectState");
        } else if (g.node(v).isDead()) {
          diag(n, "FrameState vobj entry is dead");
        }
      }
    }
  }

  // --- pass 4: PEA materialization closure + acyclicity (Rule 126) -------------
  void checkMaterialization() {
    // Cycle check over the vobj state graph: edges v -> w when a field of v
    // is (the state node of) w.
    std::vector<std::uint8_t> color(g.nodeCount(), 0); // 0 white, 1 gray, 2 black
    for (NodeId n = 0; n < g.nodeCount(); ++n) {
      if (g.node(n).kind == NodeKind::VirtualObjectState && color[n] == 0) {
        hasVobjCycle(n, color);
      }
    }
    // Closure: every vobj referenced by a field of a vobj in a FrameState's
    // deopt list must be materialized at the SAME point (in the same list).
    // And a Materialize node's vobj must have no still-virtual fields (the
    // pass must have replaced them with materialized references first).
    for (NodeId n = 0; n < g.nodeCount(); ++n) {
      const Node& nd = g.node(n);
      if (nd.isDead()) {
        continue;
      }
      if (nd.kind == NodeKind::Materialize && nd.numInputs >= 3) {
        const NodeId v = g.input(n, 2);
        if (validId(v) && g.node(v).kind == NodeKind::VirtualObjectState) {
          const Node& vs = g.node(v);
          for (std::uint16_t s = 0; s < vs.numInputs; ++s) {
            const NodeId f = g.input(v, s);
            if (validId(f) &&
                g.node(f).kind == NodeKind::VirtualObjectState) {
              diag(n, "Materialize stores a still-virtual field: vobj n" +
                          std::to_string(v) + " field n" +
                          std::to_string(f) +
                          " must be materialized (or deopt-listed) first");
            }
          }
        }
      }
      if (nd.kind != NodeKind::FrameState || nd.isDead()) {
        continue;
      }
      if (nd.payload >= g.frameStateCount()) {
        continue;
      }
      const auto list = g.frameStateVobjs(nd.payload);
      for (VirtualObjectId v : list) {
        if (!validId(v) || g.node(v).kind != NodeKind::VirtualObjectState) {
          continue;
        }
        const Node& vs = g.node(v);
        for (std::uint16_t s = 0; s < vs.numInputs; ++s) {
          const NodeId f = g.input(v, s);
          if (!validId(f) || g.node(f).kind != NodeKind::VirtualObjectState) {
            continue;
          }
          bool found = false;
          for (VirtualObjectId w : list) {
            if (w == f) {
              found = true;
              break;
            }
          }
          if (!found) {
            diag(n, "materialization closure violated: vobj n" +
                        std::to_string(v) + " references vobj n" +
                        std::to_string(f) +
                        " which is not materialized at this deopt point");
          }
        }
      }
    }
  }

  void hasVobjCycle(NodeId n, std::vector<std::uint8_t>& color) {
    color[n] = 1;
    const Node& nd = g.node(n);
    for (std::uint16_t s = 0; s < nd.numInputs; ++s) {
      const NodeId f = g.input(n, s);
      if (!validId(f) || g.node(f).kind != NodeKind::VirtualObjectState) {
        continue;
      }
      if (color[f] == 1) {
        diag(n, "materialization graph cycle: vobj n" + std::to_string(f) +
                    " participates in a cycle");
      } else if (color[f] == 0) {
        hasVobjCycle(f, color);
      }
    }
    color[n] = 2;
  }

  // --- pass 5: vector well-formedness (Part XVIII SWLP) -------------------------
  void checkVector(NodeId n) {
    const Node& nd = g.node(n);
    switch (nd.kind) {
    case NodeKind::VectorPack: {
      const IRType lane = static_cast<IRType>(nd.payload);
      if (laneTypeOf(vectorOfLane(lane)) != lane ||
          vectorOfLane(lane) == IRType::Bottom) {
        diag(n, "VectorPack lane type must be a lane-legal IRType");
        break;
      }
      const std::uint32_t lanes = nd.numInputs;
      if (lanes < kMinVectorLanes || lanes > kMaxVectorLanes ||
          (lanes & (lanes - 1)) != 0) {
        diag(n, "VectorPack lane count must be a power of two in [2, 64]");
        break;
      }
      const IRType carrier = laneCarrier(lane);
      for (std::uint16_t s = 0; s < nd.numInputs; ++s) {
        checkOperandType(n, g.input(n, s), carrier, "VectorPack lane value");
      }
      break;
    }
    case NodeKind::VectorBroadcast: {
      const IRType lane = static_cast<IRType>(nd.payload);
      const std::uint32_t lanes = unpackVecLanes(nd.payload2);
      if (vectorOfLane(lane) == IRType::Bottom) {
        diag(n, "VectorBroadcast lane type must be lane-legal");
        break;
      }
      if (lanes < kMinVectorLanes || lanes > kMaxVectorLanes ||
          (lanes & (lanes - 1)) != 0) {
        diag(n, "VectorBroadcast lane count must be a power of two in [2, 64]");
        break;
      }
      checkOperandType(n, g.input(n, 0), laneCarrier(lane),
                       "VectorBroadcast scalar");
      break;
    }
    case NodeKind::VectorOp:
    case NodeKind::VectorMaskOp: {
      const IRType lane = unpackVecLane(nd.payload2);
      const std::uint32_t lanes = unpackVecLanes(nd.payload2);
      const IRType vt = vectorOfLane(lane);
      if (vt == IRType::Bottom) {
        diag(n, "vector op lane type must be lane-legal");
        break;
      }
      if (lanes < kMinVectorLanes || lanes > kMaxVectorLanes ||
          (lanes & (lanes - 1)) != 0) {
        diag(n, "vector op lane count must be a power of two in [2, 64]");
        break;
      }
      const VectorOp op = static_cast<VectorOp>(nd.payload);
      if (op >= VectorOp::_Count || !isLegalElementWiseOp(op, lane)) {
        diag(n, "vector op not legal for this lane type");
        break;
      }
      if (nd.kind == NodeKind::VectorOp) {
        if (nd.numInputs >= 2) {
          checkOperandType(n, g.input(n, 0), vt, "VectorOp operand a");
          checkOperandType(n, g.input(n, 1), vt, "VectorOp operand b");
        }
      } else {
        if (nd.numInputs >= 3) {
          checkOperandType(n, g.input(n, 0), IRType::VectorI8,
                           "VectorMaskOp mask");
          checkOperandType(n, g.input(n, 1), vt, "VectorMaskOp operand a");
          checkOperandType(n, g.input(n, 2), vt, "VectorMaskOp operand b");
        }
        // Mask lane agreement: the mask node must carry the same lane count.
        if (nd.numInputs >= 1) {
          const NodeId m = g.input(n, 0);
          if (validId(m) && g.node(m).kind == NodeKind::VectorBroadcast) {
            if (unpackVecLanes(g.node(m).payload2) != lanes) {
              diag(n, "mask lane count differs from operand lane count");
            }
          }
        }
      }
      break;
    }
    case NodeKind::VectorLoad:
    case NodeKind::VectorStore: {
      const IRType lane = unpackVecLane(nd.payload2);
      const std::uint32_t lanes = unpackVecLanes(nd.payload2);
      if (vectorOfLane(lane) == IRType::Bottom) {
        diag(n, "vector memory lane type must be lane-legal");
        break;
      }
      if (lanes < kMinVectorLanes || lanes > kMaxVectorLanes ||
          (lanes & (lanes - 1)) != 0) {
        diag(n, "vector memory lane count must be a power of two in [2, 64]");
        break;
      }
      if (nd.kind == NodeKind::VectorLoad && nd.numInputs >= 4) {
        checkOperandType(n, g.input(n, 2), IRType::Ref, "VectorLoad array");
        checkOperandType(n, g.input(n, 3), IRType::Int, "VectorLoad index");
      }
      if (nd.kind == NodeKind::VectorStore && nd.numInputs >= 5) {
        checkOperandType(n, g.input(n, 2), IRType::Ref, "VectorStore array");
        checkOperandType(n, g.input(n, 3), IRType::Int, "VectorStore index");
        checkOperandType(n, g.input(n, 4), vectorOfLane(lane),
                         "VectorStore vector");
      }
      break;
    }
    case NodeKind::VectorReduce: {
      const IRType lane = unpackVecLane(nd.payload2);
      if (vectorOfLane(lane) == IRType::Bottom) {
        diag(n, "VectorReduce lane type must be lane-legal");
        break;
      }
      const VectorOp op = static_cast<VectorOp>(nd.payload);
      if (op >= VectorOp::_Count || !isLegalReductionOp(op, lane)) {
        diag(n, "VectorReduce op not legal for this lane type");
      }
      if (nd.numInputs >= 1) {
        checkOperandType(n, g.input(n, 0), vectorOfLane(lane),
                         "VectorReduce operand");
      }
      break;
    }
    case NodeKind::VectorExtract:
      if (nd.numInputs >= 2) {
        const NodeId v = g.input(n, 0);
        if (validId(v)) {
          const IRType vt = typeOf(v);
          if (!isVectorType(vt)) {
            diag(n, "VectorExtract operand must be a vector");
          }
        }
        checkOperandType(n, g.input(n, 1), IRType::Int, "VectorExtract index");
      }
      break;
    case NodeKind::VectorInsert:
      if (nd.numInputs >= 3) {
        const NodeId v = g.input(n, 0);
        if (validId(v)) {
          const IRType vt = typeOf(v);
          if (!isVectorType(vt)) {
            diag(n, "VectorInsert operand must be a vector");
          } else {
            checkOperandType(n, g.input(n, 1), laneCarrier(laneTypeOf(vt)),
                             "VectorInsert value");
          }
        }
        checkOperandType(n, g.input(n, 2), IRType::Int, "VectorInsert index");
      }
      break;
    default:
      break;
    }
  }

  // --- pass 6: tagged transitions (Part XVIII NaN boxing) ----------------------
  void checkTagged(NodeId n) {
    const Node& nd = g.node(n);
    switch (nd.kind) {
    case NodeKind::TagValue:
    case NodeKind::UntagValue:
    case NodeKind::RepTransitionGuard: {
      const auto from = static_cast<ValueRep>(nd.payload);
      const auto to = static_cast<ValueRep>(nd.payload2);
      if (!isValidRep(from) || !isValidRep(to)) {
        diag(n, "tagged node has an out-of-range ValueRep payload");
        break;
      }
      if (nd.kind != NodeKind::RepTransitionGuard &&
          (from == ValueRep::Polymorphic || to == ValueRep::Polymorphic)) {
        diag(n, "unguarded Polymorphic rep transition: use "
                "RepTransitionGuard (Part XVIII)");
      }
      if (nd.kind == NodeKind::RepTransitionGuard && from == to) {
        diag(n, "RepTransitionGuard must actually change the representation");
      }
      break;
    }
    default:
      break;
    }
  }
};

} // namespace

VerifyResult verify(const Graph& g) noexcept {
  VerifyResult r;
  Verifier v(g);
  for (NodeId n = 0; n < g.nodeCount(); ++n) {
    v.checkNode(n);
  }
  v.checkMemoryChains();
  for (NodeId n = 0; n < g.nodeCount(); ++n) {
    v.checkSpeculation(n);
  }
  v.checkFrameStates();
  v.checkMaterialization();
  for (NodeId n = 0; n < g.nodeCount(); ++n) {
    v.checkVector(n);
    v.checkTagged(n);
  }
  r.ok = v.diags.empty();
  r.diags = std::move(v.diags);
  return r;
}

} // namespace b2::ir
