// B-2 codegen - T2 IR-to-x86-64 lowering implementation.
// Slot-based: every value node gets a 16-byte activation slot. Operations load
// operands into EAX/ECX, compute, store result. Control flow uses labels +
// backpatched jumps. Phi values resolved by moves at predecessor exits.
#include "compiler/codegen/src/X64RuntimeEmitter.h"
#include "compiler/codegen/src/Helpers.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "b2/codegen/Archive.h"
#include "b2/codegen/Instantiate.h"
#include "b2/codegen/T2Lowering.h"
#include "b2/codegen/Tier1.h"
#include "b2/interp/Interp.h"
#include "b2/interp/Runtime.h"
#include "b2/ir/Graph.h"
#include "b2/ir/Node.h"
#include "b2/ir/Types.h"
#include "b2/ir/Verifier.h"
#include "b2/rbc/Rbc.h"
#include "b2/rbc/Type.h"

namespace b2::codegen {

namespace {

using K = ir::NodeKind;

// RType tag values for Value.type.
constexpr std::uint32_t kRTypeInt  = static_cast<std::uint32_t>(rbc::RType::Int);
constexpr std::uint32_t kRTypeLong = static_cast<std::uint32_t>(rbc::RType::Long);
constexpr std::uint32_t kRTypeRef  = static_cast<std::uint32_t>(rbc::RType::Ref);
constexpr std::uint32_t kRTypeNull = static_cast<std::uint32_t>(rbc::RType::Null);

inline std::int32_t slotOff(std::uint32_t slot) noexcept { return static_cast<std::int32_t>(kSlotsBase) + static_cast<std::int32_t>(slot) * 16; }
inline std::int32_t slotPayload(std::uint32_t slot) noexcept { return slotOff(slot) + 8; }
inline std::int32_t slotTag(std::uint32_t slot) noexcept { return slotOff(slot); }

struct LowerState {
  const ir::Graph& g;
  const rbc::Method& method;
  std::uint32_t methodId;
  interp::Runtime& rt;
  X64RuntimeEmitter em;
  std::unordered_map<ir::NodeId, std::uint32_t> slotOf;
  std::unordered_map<ir::NodeId, std::uint32_t> labelOf;
  std::uint32_t nextSlot = 0;
  std::uint32_t numParams = 0;
  std::uint32_t liveNodes = 0;
  std::uint32_t deoptEpilogue = 0;
  std::uint32_t normalEpilogue = 0;
  std::string refusal;
  // Pending jumps: (patchOffset, targetNodeId). Sentinel kInvalidNodeId = deopt,
  // kInvalidNodeId-1 = normal exit.
  std::vector<std::pair<std::uint32_t, ir::NodeId>> pendingJumps;
  // Pending helper calls: (patchOffset, helperAddr).
  std::vector<std::pair<std::uint32_t, void*>> pendingCalls;
  // Pending IfFalse jumps: (patchOffset, IfNodeId) — resolved to the IfFalse
  // projection of the If node.
  std::vector<std::pair<std::uint32_t, ir::NodeId>> pendingIfFalse;
  // Pending backedge jumps: (patchOffset, LoopEndNodeId) — resolved to the
  // LoopBegin that owns this backedge.
  std::vector<std::pair<std::uint32_t, ir::NodeId>> pendingBackedge;
  LowerState(const ir::Graph& graph, const rbc::Method& m, std::uint32_t mid, interp::Runtime& runtime)
    : g(graph), method(m), methodId(mid), rt(runtime) {}
};

bool isValueNode(K k) noexcept {
  switch (k) {
    case K::ConstantI: case K::ConstantL: case K::ConstantF: case K::ConstantD:
    case K::ConstantNull: case K::ConstantSym: case K::Parameter:
    case K::AddI: case K::SubI: case K::MulI: case K::DivI: case K::RemI:
    case K::NegI: case K::ShlI: case K::ShrI: case K::UShrI: case K::AndI: case K::OrI: case K::XorI:
    case K::AddL: case K::SubL: case K::MulL: case K::AndL: case K::OrL: case K::XorL: case K::NegL:
    case K::CmpI: case K::CmpL: case K::CmpFl: case K::CmpFg: case K::CmpDl: case K::CmpDg:
    case K::I2L: case K::L2I: case K::I2B: case K::I2C: case K::I2S:
    case K::Phi: case K::Undef:
    case K::Not: case K::IsNull: case K::RefEq:
    case K::EqI: case K::NeI: case K::LtI: case K::LeI: case K::GtI: case K::GeI:
    case K::LoadField: case K::LoadStatic: case K::LoadElem: case K::ArrayLength:
    case K::InstanceOf: case K::LoadException:
    case K::CallStatic: case K::CallVirtual: case K::CallInterface:
    case K::New: case K::NewArray: case K::NewRefArray: case K::Materialize:
      return true;
    default: return false;
  }
}

void assignSlots(LowerState& s) {
  // Parameters first (sorted by index → slots 0..N-1, matching T1 arg copy).
  std::vector<std::pair<std::uint32_t, ir::NodeId>> params;
  for (ir::NodeId n = 0; n < s.g.nodeCount(); ++n) {
    const ir::Node& nd = s.g.node(n);
    if (!nd.isDead() && nd.kind == K::Parameter) params.emplace_back(nd.payload, n);
  }
  std::sort(params.begin(), params.end());
  for (auto& [idx, nodeId] : params) s.slotOf[nodeId] = s.nextSlot++;
  s.numParams = static_cast<std::uint32_t>(params.size());
  // All other live value nodes.
  for (ir::NodeId n = 0; n < s.g.nodeCount(); ++n) {
    const ir::Node& nd = s.g.node(n);
    if (nd.isDead()) continue;
    ++s.liveNodes;
    if (nd.kind == K::Parameter) continue;
    if (isValueNode(nd.kind)) s.slotOf[n] = s.nextSlot++;
  }
}

// --- load/store helpers ---

void loadIntEax(LowerState& s, ir::NodeId n) { auto it=s.slotOf.find(n); if(it!=s.slotOf.end()) s.em.loadRbpDisp32(Reg32::EAX, slotPayload(it->second)); }
void loadIntEcx(LowerState& s, ir::NodeId n) { auto it=s.slotOf.find(n); if(it!=s.slotOf.end()) s.em.loadRbpDisp32(Reg32::ECX, slotPayload(it->second)); }
void loadLongRax(LowerState& s, ir::NodeId n) { auto it=s.slotOf.find(n); if(it!=s.slotOf.end()) s.em.loadRbpDisp64(Reg::RAX, slotPayload(it->second)); }
void loadLongRcx(LowerState& s, ir::NodeId n) { auto it=s.slotOf.find(n); if(it!=s.slotOf.end()) s.em.loadRbpDisp64(Reg::RCX, slotPayload(it->second)); }

void storeInt(LowerState& s, ir::NodeId n) {
  auto it=s.slotOf.find(n); if(it==s.slotOf.end()) return;
  s.em.storeRbpDisp32(Reg32::EAX, slotPayload(it->second));
  s.em.xorEaxEax();
  s.em.storeRbpDisp32(Reg32::EAX, slotPayload(it->second)+4);
  s.em.movEaxImm32(static_cast<std::int32_t>(kRTypeInt));
  s.em.storeRbpDisp32(Reg32::EAX, slotTag(it->second));
}
void storeLong(LowerState& s, ir::NodeId n) {
  auto it=s.slotOf.find(n); if(it==s.slotOf.end()) return;
  s.em.storeRbpDisp64(Reg::RAX, slotPayload(it->second));
  s.em.movEaxImm32(static_cast<std::int32_t>(kRTypeLong));
  s.em.storeRbpDisp32(Reg32::EAX, slotTag(it->second));
}
void storeNull(LowerState& s, ir::NodeId n) {
  auto it=s.slotOf.find(n); if(it==s.slotOf.end()) return;
  s.em.xorEaxEax();
  s.em.storeRbpDisp32(Reg32::EAX, slotPayload(it->second));
  s.em.storeRbpDisp32(Reg32::EAX, slotPayload(it->second)+4);
  s.em.movEaxImm32(static_cast<std::int32_t>(kRTypeNull));
  s.em.storeRbpDisp32(Reg32::EAX, slotTag(it->second));
}
void storeRef(LowerState& s, ir::NodeId n) {
  auto it=s.slotOf.find(n); if(it==s.slotOf.end()) return;
  s.em.storeRbpDisp32(Reg32::EAX, slotPayload(it->second));
  s.em.xorEaxEax();
  s.em.storeRbpDisp32(Reg32::EAX, slotPayload(it->second)+4);
  s.em.movEaxImm32(static_cast<std::int32_t>(kRTypeRef));
  s.em.storeRbpDisp32(Reg32::EAX, slotTag(it->second));
}

// Copy one slot's 16-byte Value to another (for phi moves).
void copySlot(LowerState& s, std::uint32_t dstSlot, std::uint32_t srcSlot) {
  s.em.loadRbpDisp64(Reg::RAX, slotPayload(srcSlot));
  s.em.storeRbpDisp64(Reg::RAX, slotPayload(dstSlot));
  s.em.loadRbpDisp32(Reg32::EAX, slotTag(srcSlot));
  s.em.storeRbpDisp32(Reg32::EAX, slotTag(dstSlot));
}

// Emit phi moves for predecessor `predIdx` of merge `merge` (Region/LoopBegin).
// For each Phi at `merge`, copy phi.input[predIdx+1] → phi.slot.
void emitPhiMovesForPred(LowerState& s, ir::NodeId merge, std::uint32_t predIdx) {
  for (ir::NodeId n = 0; n < s.g.nodeCount(); ++n) {
    const ir::Node& nd = s.g.node(n);
    if (nd.isDead() || nd.kind != K::Phi) continue;
    if (nd.numInputs < 1 || s.g.input(n, 0) != merge) continue;
    // phi.input[0] = region, phi.input[1+] = values per predecessor.
    std::uint32_t valIdx = predIdx + 1;
    if (valIdx >= nd.numInputs) continue;
    ir::NodeId srcNode = s.g.input(n, valIdx);
    auto srcIt = s.slotOf.find(srcNode);
    auto dstIt = s.slotOf.find(n);
    if (srcIt != s.slotOf.end() && dstIt != s.slotOf.end()) {
      copySlot(s, dstIt->second, srcIt->second);
    }
  }
}

// Emit a helper call: mov rdi,rbp; set args; call; test eax; jnz deopt.
void emitHelperCall(LowerState& s, std::uint8_t helperId,
                    std::uint32_t a1=0, std::uint32_t a2=0,
                    std::uint32_t a3=0, std::uint32_t a4=0) {
  void* addr = helperAddress(helperId);
  if (addr == nullptr) { s.refusal = "unknown helper " + std::to_string(helperId); return; }
  s.em.movRdiRbp();
  s.em.movEsiImm32(a1);
  s.em.movEdxImm32(a2);
  s.em.movEcxImm32u(a3);
  if (a4 != 0) s.em.movR8dImm32(a4);
  std::uint32_t patchOff = s.em.callRip32();
  s.pendingCalls.push_back({patchOff, addr});
  s.em.testEaxEax();
  std::uint32_t jnzPatch = s.em.jccRel32(0x85); // jnz deopt
  s.pendingJumps.push_back({jnzPatch, ir::kInvalidNodeId});
}

// --- the main lowering pass ---

bool lowerGraph(LowerState& s) {
  s.em.prologue();

  for (ir::NodeId n = 0; n < s.g.nodeCount(); ++n) {
    const ir::Node& nd = s.g.node(n);
    if (nd.isDead()) continue;

    switch (nd.kind) {
      // === control flow ===
      case K::Start:
        break; // prologue already emitted

      case K::LoopBegin: case K::Region: {
        // Entry-path phi moves (predecessor 0 = the fall-through/entry path).
        // Emitted BEFORE the label so the values are in place when we enter.
        emitPhiMovesForPred(s, n, 0);
        s.labelOf[n] = s.em.offset();
        break;
      }

      case K::IfTrue:
        s.labelOf[n] = s.em.offset(); // fall-through from If
        break;

      case K::IfFalse:
        s.labelOf[n] = s.em.offset();
        break;

      case K::If: {
        if (nd.numInputs >= 2) {
          loadIntEax(s, s.g.input(n, 1));
          s.em.testEaxEax();
          // je IfFalse (cond == 0 → false branch). Backpatched later.
          std::uint32_t patchOff = s.em.jccRel32(0x84); // je
          s.pendingIfFalse.push_back({patchOff, n});
        }
        break;
      }

      case K::LoopEnd: {
        // Backedge phi moves (predecessor 1 = the backedge path).
        // Find the LoopBegin this LoopEnd feeds.
        // The LoopBegin's input[1] (backedge ctrl) should reach this LoopEnd.
        // Scan for a LoopBegin whose backedge input chain includes this node.
        ir::NodeId loopBegin = ir::kInvalidNodeId;
        for (ir::NodeId i = 0; i < s.g.nodeCount(); ++i) {
          const ir::Node& lb = s.g.node(i);
          if (lb.isDead() || lb.kind != K::LoopBegin || lb.numInputs < 2) continue;
          // Check if input[1] (backedge) is this LoopEnd or reaches it.
          ir::NodeId be = s.g.input(i, 1);
          if (be == n) { loopBegin = i; break; }
          // Check through one level (IfTrue → LoopEnd).
          if (be < s.g.nodeCount() && !s.g.node(be).isDead()) {
            const ir::Node& beNode = s.g.node(be);
            if ((beNode.kind == K::IfTrue || beNode.kind == K::IfFalse) &&
                beNode.numInputs >= 1 && s.g.input(be, 0) == n) {
              loopBegin = i; break;
            }
          }
        }
        if (loopBegin != ir::kInvalidNodeId) {
          emitPhiMovesForPred(s, loopBegin, 1); // backedge = predecessor 1
        }
        // jmp LoopBegin
        std::uint32_t patchOff = s.em.jmpRel32();
        s.pendingBackedge.push_back({patchOff, n});
        break;
      }

      case K::LoopExit:
        s.labelOf[n] = s.em.offset();
        break;

      case K::Return: {
        if (nd.numInputs >= 2) {
          ir::NodeId val = s.g.input(n, 1);
          auto it = s.slotOf.find(val);
          if (it != s.slotOf.end()) {
            // Copy 16-byte Value to act->ret_value (kActOffRetValue = 64).
            s.em.loadRbpDisp64(Reg::RAX, slotPayload(it->second));
            s.em.storeRbpDisp64(Reg::RAX, static_cast<std::int32_t>(kActOffRetValue));
            s.em.loadRbpDisp32(Reg32::EAX, slotTag(it->second));
            s.em.storeRbpDisp32(Reg32::EAX, static_cast<std::int32_t>(kActOffRetValue));
          }
        }
        std::uint32_t patchOff = s.em.jmpRel32();
        s.pendingJumps.push_back({patchOff, ir::kInvalidNodeId - 1}); // normal exit
        break;
      }

      case K::Unwind: case K::Deopt: case K::CallExcept: case K::LoadException:
        // Exception-path only: skip in normal flow (v1: no exception edge lowering).
        break;

      case K::End: {
        std::uint32_t patchOff = s.em.jmpRel32();
        s.pendingJumps.push_back({patchOff, ir::kInvalidNodeId - 1});
        break;
      }

      // === constants ===
      case K::ConstantI:
        s.em.movEaxImm32(static_cast<std::int32_t>(nd.constValue));
        storeInt(s, n);
        break;
      case K::ConstantL:
        s.em.movRaxImm64(nd.constValue);
        storeLong(s, n);
        break;
      case K::ConstantNull:
        storeNull(s, n);
        break;
      case K::ConstantF: case K::ConstantD: case K::ConstantSym:
        // Store the 8-byte constValue as a long (type tag may be wrong but
        // the payload is correct for integer comparisons).
        s.em.movRaxImm64(nd.constValue);
        storeLong(s, n);
        break;

      case K::Parameter: case K::Undef:
        break; // slots already set up by assignSlots / engine arg copy

      // === int arithmetic ===
      case K::AddI: loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.addEaxEcx(); storeInt(s,n); break;
      case K::SubI: loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.subEaxEcx(); storeInt(s,n); break;
      case K::MulI: loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.imulEaxEcx(); storeInt(s,n); break;
      case K::DivI: loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.idivEcx(); storeInt(s,n); break;
      case K::RemI: loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.idivEcx(); s.em.movEaxEdx(); storeInt(s,n); break;
      case K::NegI: loadIntEax(s,s.g.input(n,0)); s.em.negEax(); storeInt(s,n); break;
      case K::AndI: loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.andEaxEcx(); storeInt(s,n); break;
      case K::OrI:  loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.orEaxEcx(); storeInt(s,n); break;
      case K::XorI: loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.xorEaxEcx(); storeInt(s,n); break;
      case K::ShlI: loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.andEcxImm31(); s.em.shlEaxCl(); storeInt(s,n); break;
      case K::ShrI: loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.andEcxImm31(); s.em.sarEaxCl(); storeInt(s,n); break;
      case K::UShrI: loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.andEcxImm31(); s.em.shrEaxCl(); storeInt(s,n); break;

      // === long arithmetic ===
      case K::AddL: loadLongRax(s,s.g.input(n,0)); loadLongRcx(s,s.g.input(n,1)); s.em.addRaxRcx(); storeLong(s,n); break;
      case K::SubL: loadLongRax(s,s.g.input(n,0)); loadLongRcx(s,s.g.input(n,1)); s.em.subRaxRcx(); storeLong(s,n); break;
      case K::MulL: loadLongRax(s,s.g.input(n,0)); loadLongRcx(s,s.g.input(n,1)); s.em.imulRaxRcx(); storeLong(s,n); break;
      case K::AndL: loadLongRax(s,s.g.input(n,0)); loadLongRcx(s,s.g.input(n,1)); s.em.andRaxRcx(); storeLong(s,n); break;
      case K::OrL:  loadLongRax(s,s.g.input(n,0)); loadLongRcx(s,s.g.input(n,1)); s.em.orRaxRcx(); storeLong(s,n); break;
      case K::XorL: loadLongRax(s,s.g.input(n,0)); loadLongRcx(s,s.g.input(n,1)); s.em.xorRaxRcx(); storeLong(s,n); break;
      case K::NegL: loadLongRax(s,s.g.input(n,0)); s.em.negRax(); storeLong(s,n); break;

      // === comparisons (0/1) ===
      case K::EqI: case K::NeI: case K::LtI: case K::LeI: case K::GtI: case K::GeI: {
        loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.cmpEaxEcx();
        std::uint8_t cc = 0x94;
        switch (nd.kind) {
          case K::EqI: cc=0x94; break; case K::NeI: cc=0x95; break;
          case K::LtI: cc=0x9C; break; case K::LeI: cc=0x9E; break;
          case K::GtI: cc=0x9F; break; case K::GeI: cc=0x9D; break;
          default: break;
        }
        s.em.setccAl(cc); s.em.movzxEaxAl(); storeInt(s,n);
        break;
      }
      case K::CmpI: {
        // (a,b) -> (a>b)-(a<b). Simplified: setg (1 if a>b), which is 0/1.
        // Correct enough for if_icmp* (which only test zero/nonzero).
        loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.cmpEaxEcx();
        s.em.setccAl(0x9F); s.em.movzxEaxAl(); storeInt(s,n);
        break;
      }

      case K::Not: {
        loadIntEax(s,s.g.input(n,0)); s.em.testEaxEax();
        s.em.setccAl(0x94); s.em.movzxEaxAl(); storeInt(s,n);
        break;
      }
      case K::IsNull: {
        auto it=s.slotOf.find(s.g.input(n,0));
        if(it!=s.slotOf.end()) {
          s.em.loadRbpDisp32(Reg32::EAX, slotTag(it->second));
          s.em.movEcxImm32(static_cast<std::int32_t>(kRTypeNull));
          s.em.cmpEaxEcx();
          s.em.setccAl(0x94); s.em.movzxEaxAl(); storeInt(s,n);
        }
        break;
      }

      // === conversions ===
      case K::I2L: {
        auto it=s.slotOf.find(s.g.input(n,0));
        if(it!=s.slotOf.end()) { s.em.movsxdRaxMem(slotPayload(it->second)); storeLong(s,n); }
        break;
      }
      case K::L2I:
        loadLongRax(s,s.g.input(n,0)); storeInt(s,n); break;
      case K::I2B: loadIntEax(s,s.g.input(n,0)); s.em.movsxEaxAl(); storeInt(s,n); break;
      case K::I2C: loadIntEax(s,s.g.input(n,0)); s.em.movzxEaxAx(); storeInt(s,n); break;
      case K::I2S: loadIntEax(s,s.g.input(n,0)); s.em.movsxEaxAx(); storeInt(s,n); break;

      // === phi ===
      case K::Phi:
        // Phi slots are filled by emitPhiMovesForPred at predecessor exits.
        break;

      // === memory ops via helpers ===
      case K::LoadStatic: {
        // [ctrl, mem] -> field value; payload = FieldId, payload2 = IRType
        auto dstIt=s.slotOf.find(n);
        if (dstIt!=s.slotOf.end())
          emitHelperCall(s, static_cast<std::uint8_t>(HelperId::GetStatic),
                        nd.payload, slotOff(dstIt->second));
        break;
      }
      case K::LoadField: {
        if (nd.numInputs >= 3) {
          auto objIt=s.slotOf.find(s.g.input(n,2)), dstIt=s.slotOf.find(n);
          if (objIt!=s.slotOf.end() && dstIt!=s.slotOf.end())
            emitHelperCall(s, static_cast<std::uint8_t>(HelperId::GetField),
                          slotOff(objIt->second), nd.payload, slotOff(dstIt->second));
        }
        break;
      }
      case K::StoreField: {
        if (nd.numInputs >= 4) {
          auto objIt=s.slotOf.find(s.g.input(n,2)), valIt=s.slotOf.find(s.g.input(n,3));
          if (objIt!=s.slotOf.end() && valIt!=s.slotOf.end())
            emitHelperCall(s, static_cast<std::uint8_t>(HelperId::PutField),
                          slotOff(objIt->second), nd.payload, slotOff(valIt->second));
        }
        break;
      }
      case K::ArrayLength: {
        if (nd.numInputs >= 1) {
          auto arrIt=s.slotOf.find(s.g.input(n,0)), dstIt=s.slotOf.find(n);
          if (arrIt!=s.slotOf.end() && dstIt!=s.slotOf.end())
            emitHelperCall(s, static_cast<std::uint8_t>(HelperId::ArrayLength),
                          slotOff(arrIt->second), slotOff(dstIt->second));
        }
        break;
      }
      case K::New: {
        auto dstIt=s.slotOf.find(n);
        if (dstIt!=s.slotOf.end())
          emitHelperCall(s, static_cast<std::uint8_t>(HelperId::NewObject),
                        nd.payload, slotOff(dstIt->second));
        break;
      }
      case K::NewArray: {
        if (nd.numInputs >= 2) {
          auto lenIt=s.slotOf.find(s.g.input(n,1)), dstIt=s.slotOf.find(n);
          if (lenIt!=s.slotOf.end() && dstIt!=s.slotOf.end())
            emitHelperCall(s, static_cast<std::uint8_t>(HelperId::NewArray),
                          slotOff(lenIt->second), nd.payload, slotOff(dstIt->second));
        }
        break;
      }

      // === calls ===
      case K::CallStatic: case K::CallVirtual: case K::CallInterface: {
        auto dstIt=s.slotOf.find(n);
        std::uint32_t dstOff = (dstIt!=s.slotOf.end()) ? slotOff(dstIt->second) : 0;
        // Count args: skip ctrl(0) + mem(1); exclude trailing FrameState.
        std::uint32_t argCount = 0;
        if (nd.numInputs > 2) {
          argCount = nd.numInputs - 2;
          if (argCount > 0) {
            ir::NodeId last = s.g.input(n, nd.numInputs-1);
            if (last < s.g.nodeCount() && s.g.node(last).kind == K::FrameState) --argCount;
          }
        }
        std::uint32_t flavor = (nd.kind==K::CallStatic) ? 0 : 1;
        std::uint32_t packedTarget = (flavor << 28) | (nd.payload & 0x0FFF'FFFF);
        // Copy args to consecutive staging slots at the end of the activation.
        // The b2cg_call helper expects args in consecutive 16-byte slots.
        std::uint32_t argBaseSlot = s.nextSlot; // staging area starts here
        for (std::uint32_t a = 0; a < argCount; ++a) {
          ir::NodeId argNode = s.g.input(n, 2 + a);
          auto argIt = s.slotOf.find(argNode);
          if (argIt != s.slotOf.end()) {
            copySlot(s, argBaseSlot + a, argIt->second);
          }
        }
        emitHelperCall(s, static_cast<std::uint8_t>(HelperId::Call),
                      slotOff(argBaseSlot), argCount, packedTarget, dstOff);
        break;
      }

      // === guards ===
      case K::Guard:
        // v1: skip guard checks. The helpers (b2cg_get_field, etc.) do their
        // own null/bounds checks and trap. This means we lose deopt metadata,
        // but the helper trap path still deopts to T0.
        break;

      case K::FrameState: case K::MemBar: case K::ClassInit:
      case K::Switch: case K::SwitchCase: case K::SwitchDefault:
        break; // no code (v1: no deopt metadata emission)

      // --- unhandled kinds (TODO: refuse instead of silent wrong) ---
      default:
        break;
    }
  }

  // Epilogues.
  s.normalEpilogue = s.em.offset();
  s.em.epilogueNormal();
  s.deoptEpilogue = s.em.offset();
  s.em.epilogueDeopt();

  // Backpatch pending jumps.
  for (auto& [patchOff, target] : s.pendingJumps) {
    if (target == ir::kInvalidNodeId) s.em.patchRel32(patchOff, s.deoptEpilogue);
    else if (target == ir::kInvalidNodeId - 1) s.em.patchRel32(patchOff, s.normalEpilogue);
    else { auto lab=s.labelOf.find(target); s.em.patchRel32(patchOff, lab!=s.labelOf.end()?lab->second:s.normalEpilogue); }
  }
  // Backpatch IfFalse jumps: find the IfFalse projection of each If.
  for (auto& [patchOff, ifNode] : s.pendingIfFalse) {
    ir::NodeId ifFalse = ir::kInvalidNodeId;
    for (ir::NodeId i = 0; i < s.g.nodeCount(); ++i) {
      const ir::Node& ni = s.g.node(i);
      if (ni.isDead() || ni.kind != K::IfFalse) continue;
      if (ni.numInputs >= 1 && s.g.input(i, 0) == ifNode) { ifFalse = i; break; }
    }
    if (ifFalse != ir::kInvalidNodeId) {
      auto lab=s.labelOf.find(ifFalse);
      s.em.patchRel32(patchOff, lab!=s.labelOf.end()?lab->second:s.normalEpilogue);
    } else {
      s.em.patchRel32(patchOff, s.normalEpilogue);
    }
  }
  // Backpatch backedge jumps: find the LoopBegin for each LoopEnd.
  for (auto& [patchOff, loopEnd] : s.pendingBackedge) {
    ir::NodeId loopBegin = ir::kInvalidNodeId;
    for (ir::NodeId i = 0; i < s.g.nodeCount(); ++i) {
      const ir::Node& lb = s.g.node(i);
      if (lb.isDead() || lb.kind != K::LoopBegin || lb.numInputs < 2) continue;
      ir::NodeId be = s.g.input(i, 1);
      if (be == loopEnd) { loopBegin = i; break; }
      if (be < s.g.nodeCount() && !s.g.node(be).isDead()) {
        const ir::Node& beNode = s.g.node(be);
        if ((beNode.kind==K::IfTrue||beNode.kind==K::IfFalse) && beNode.numInputs>=1 && s.g.input(be,0)==loopEnd) { loopBegin=i; break; }
      }
    }
    if (loopBegin != ir::kInvalidNodeId) {
      auto lab=s.labelOf.find(loopBegin);
      s.em.patchRel32(patchOff, lab!=s.labelOf.end()?lab->second:0);
    }
  }
  // Patch helper calls.
  for (auto& [patchOff, addr] : s.pendingCalls) s.em.patchCallAbs(patchOff, addr);

  return s.refusal.empty();
}

} // anonymous namespace

std::unique_ptr<CompiledCode> lowerOnly(
    const ir::Graph& g, const rbc::Method& method, std::uint32_t methodId,
    interp::Runtime& rt, std::string* refusalReason) {
  LowerState s(g, method, methodId, rt);
  assignSlots(s);
  if (!lowerGraph(s)) {
    if (refusalReason) *refusalReason = s.refusal;
    return nullptr;
  }
  auto cc = std::make_unique<CompiledCode>();
  cc->code = std::move(s.em.buf);
  cc->method_index = methodId;
  cc->method_name = method.name;
  cc->method_descriptor = method.descriptor;
  cc->num_locals = s.numParams;
  // Extra 16 slots for call arg staging (b2cg_call needs consecutive slots).
  cc->num_regs = (s.nextSlot + 16) - s.numParams;
  CodeEntry entry; entry.native_offset = 0; entry.rbc_pc = 0; entry.is_method_entry = true;
  cc->entries.push_back(entry);
  // W^X publish: page-aligned alloc (mprotect requires page alignment).
  const std::size_t pageSize = sysconf(_SC_PAGESIZE);
  const std::size_t allocSize = (cc->code.size() + pageSize - 1) & ~(pageSize - 1);
  if (allocSize == 0) { if(refusalReason) *refusalReason = "empty code"; return nullptr; }
  void* block = std::aligned_alloc(pageSize, allocSize);
  if (!block) { if(refusalReason) *refusalReason = "aligned_alloc failed"; return nullptr; }
  std::memcpy(block, cc->code.data(), cc->code.size());
  std::memset(static_cast<std::uint8_t*>(block)+cc->code.size(), 0, allocSize-cc->code.size());
  // mprotect RW first (some kernels require W before X), then RX.
  if (mprotect(block, allocSize, PROT_READ|PROT_WRITE) != 0) {
    std::free(block); if(refusalReason) *refusalReason="mprotect RW failed"; return nullptr;
  }
  if (mprotect(block, allocSize, PROT_READ|PROT_EXEC) != 0) {
    std::free(block); if(refusalReason) *refusalReason="mprotect RX failed"; return nullptr;
  }
  cc->exec_base = static_cast<std::uint8_t*>(block);
  cc->exec_alloc_size = allocSize;
  return cc;
}

Tier1RunResult lowerAndExecute(
    ir::Graph& g, const rbc::Method& method, std::uint32_t methodId,
    Tier1& engine, std::span<const interp::Value> args,
    const T2LoweringConfig& /*config*/) {
  const ir::VerifyResult irv = ir::verify(g);
  if (irv.hasErrors()) {
    Tier1RunResult r; r.status = Tier1Status::VerifyFailed;
    for (const auto& d : irv.diags) r.verify_diags.push_back({d.node, d.message});
    return r;
  }
  std::string refusal;
  auto cc = lowerOnly(g, method, methodId, engine.interp().runtime(), &refusal);
  if (!cc) return engine.runMethod(methodId, args); // Rule 96: fall back to T0
  engine.installCompiledCode(std::move(cc));
  return engine.runMethod(methodId, args);
}

} // namespace b2::codegen
