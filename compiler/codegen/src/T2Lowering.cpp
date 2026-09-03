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

// --- block scheduling ---------------------------------------------------------
// WHY: sea-of-nodes must be linearized to basic blocks for emission. Each
// control node starts a block; data nodes are scheduled to the block where
// they're consumed. Loop backedge values (AddI feeding a Phi's backedge input)
// must go in the loop body, not the header — linear ID order gets this wrong.

struct Block {
  ir::NodeId leader = ir::kInvalidNodeId;  // control node
  std::vector<ir::NodeId> dataNodes;        // pure-data nodes scheduled here
  std::vector<ir::NodeId> fixedNodes;       // ctrl-dependent nodes (incl. leader)
  std::vector<ir::NodeId> successors;       // control successor block leaders
};

// Does this node kind have a Ctrl input in slot 0? (Check Node.h signatures.)
// WHY: only nodes with Ctrl inputs should be assigned to blocks via the
// ctrl-chain fixpoint. Data nodes (AddI, IsNull, etc.) have DATA in slot 0
// and must be assigned via the use-site fixpoint instead.
bool hasCtrlInput(K k) noexcept {
  switch (k) {
    case K::LoadField: case K::StoreField: case K::LoadStatic: case K::StoreStatic:
    case K::LoadElem: case K::StoreElem:
    case K::MemBar: case K::MonitorEnter: case K::MonitorExit:
    case K::New: case K::NewArray: case K::NewRefArray: case K::NewMultiArray:
    case K::ClassInit:
    case K::CallStatic: case K::CallVirtual: case K::CallInterface: case K::CallDynamic:
    case K::CheckCast:
    case K::Guard: case K::RepTransitionGuard:
    case K::Materialize:
    case K::LoopExit: case K::LoopEnd: case K::Return: case K::Unwind: case K::Deopt:
    case K::End: case K::If: case K::Switch:
      return true;
    default: return false; // ArrayLength, InstanceOf, arithmetic, etc.
  }
}

// Is this a control node that starts a block?
bool isBlockLeader(K k) noexcept {
  switch (k) {
    case K::Start: case K::Region: case K::LoopBegin:
    case K::IfTrue: case K::IfFalse:
    case K::LoopExit: case K::LoopEnd:
    case K::Return: case K::Unwind: case K::Deopt: case K::End:
    case K::SwitchCase: case K::SwitchDefault:
      return true;
    default: return false;
  }
}

// Find the block leader that controls a given node (its ctrl input's block).
// For fixed nodes, input[0] is ctrl. For projections (IfTrue etc.), input[0]
// is the parent If/Switch.
ir::NodeId ctrlOf(const ir::Graph& g, ir::NodeId n) {
  if (n >= g.nodeCount()) return ir::kInvalidNodeId;
  const ir::Node& nd = g.node(n);
  // If/LoopEnd are control nodes themselves but have a ctrl input too.
  if (nd.numInputs >= 1) return g.input(n, 0);
  return ir::kInvalidNodeId;
}

// Build blocks + schedule nodes. Returns blocks in RPO (entry first).
std::vector<Block> buildBlocks(LowerState& s) {
  const ir::Graph& g = s.g;
  // 1. Identify block leaders.
  std::vector<ir::NodeId> leaders;
  for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
    if (!g.node(n).isDead() && isBlockLeader(g.node(n).kind)) leaders.push_back(n);
  }
  // Map: leader node → block index.
  std::unordered_map<ir::NodeId, std::uint32_t> blockIdx;
  for (std::uint32_t i = 0; i < leaders.size(); ++i) blockIdx[leaders[i]] = i;
  // Build Block objects.
  std::vector<Block> blocks(leaders.size());
  for (std::uint32_t i = 0; i < leaders.size(); ++i) blocks[i].leader = leaders[i];

  // 2. Assign each node to a block.
  // Fixed nodes (ctrl-dependent): block of their ctrl input.
  // Phi: block of its region (input[0]).
  // Pure data: block where consumed (fixpoint: first user's block).
  // Map: nodeId → block index (for leaders, it's their own block).
  std::vector<std::uint32_t> nodeBlock(g.nodeCount(), UINT32_MAX);
  for (std::uint32_t i = 0; i < leaders.size(); ++i) nodeBlock[leaders[i]] = i;
  // Fixed nodes: find their ctrl input's block.
  // A node is "fixed" if it has a ctrl input that is a control node.
  for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
    const ir::Node& nd = g.node(n);
    if (nd.isDead() || isBlockLeader(nd.kind)) continue;
    if (nd.kind == K::Phi) {
      // Phi belongs to its region's block.
      if (nd.numInputs >= 1) {
        ir::NodeId region = g.input(n, 0);
        auto it = blockIdx.find(region);
        if (it != blockIdx.end()) nodeBlock[n] = it->second;
      }
      continue;
    }
    // Constants/params/undef: always in entry block (0) so they're available
    // before any phi moves reference them.
    if (nd.kind == K::ConstantI || nd.kind == K::ConstantL ||
        nd.kind == K::ConstantF || nd.kind == K::ConstantD ||
        nd.kind == K::ConstantNull || nd.kind == K::ConstantSym ||
        nd.kind == K::Parameter || nd.kind == K::Undef) {
      nodeBlock[n] = 0;
      continue;
    }
    // Check if this node has a ctrl input (input[0] is a control node).
    if (nd.numInputs >= 1) {
      ir::NodeId c = g.input(n, 0);
      if (c < g.nodeCount() && !g.node(c).isDead() &&
          isBlockLeader(g.node(c).kind)) {
        auto it = blockIdx.find(c);
        if (it != blockIdx.end()) nodeBlock[n] = it->second;
      }
    }
  }
  // Pure data nodes: fixpoint — assign to the block of their first user.
  // Iterate until stable.
  bool changed = true;
  int iterations = 0;
  while (changed && iterations < 10) {
    changed = false; ++iterations;
    for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
      if (n >= g.nodeCount()) break;
      const ir::Node& nd = g.node(n);
      if (nd.isDead()) continue;
      if (nodeBlock[n] != UINT32_MAX) continue;  // already assigned
      if (isBlockLeader(nd.kind)) continue;
      if (nd.kind == K::Phi) continue;
      // Find the earliest-block user.
      std::uint32_t bestBlock = UINT32_MAX;
      for (ir::NodeId u = 0; u < g.nodeCount(); ++u) {
        if (u >= g.nodeCount()) break;
        const ir::Node& un = g.node(u);
        if (un.isDead()) continue;
        for (std::uint16_t i = 0; i < un.numInputs; ++i) {
          if (g.input(u, i) != n) continue;
          // u uses n. If u is a Phi, n is consumed at u's predecessor block.
          if (un.kind == K::Phi && un.numInputs >= 1) {
            ir::NodeId region = g.input(u, 0);
            // Find which predecessor position this is.
            for (std::uint16_t p = 1; p < un.numInputs; ++p) {
              if (g.input(u, p) == n) {
                // Predecessor p-1. Find the block leader for this predecessor.
                // For LoopBegin: pred 0 = entry, pred 1 = backedge (LoopEnd's block).
                // For Region: pred p = the p-th control predecessor's block.
                ir::NodeId predCtrl = (p < region) ? g.input(region, p) : ir::kInvalidNodeId;
                // Actually, the predecessor's block leader is the control node
                // that feeds region's input[p]. For a LoopBegin, input[1] is the
                // backedge control (LoopEnd or IfTrue/IfFalse before LoopEnd).
                if (region < g.nodeCount() && !g.node(region).isDead()) {
                  const ir::Node& rn = g.node(region);
                  if (p < rn.numInputs) {
                    ir::NodeId predNode = g.input(region, p);
                    // Walk to find the block leader: if predNode is not a leader,
                    // walk its ctrl chain until we find one.
                    while (predNode < g.nodeCount() && !g.node(predNode).isDead() &&
                           !isBlockLeader(g.node(predNode).kind) &&
                           g.node(predNode).numInputs >= 1) {
                      predNode = g.input(predNode, 0);
                    }
                    auto it = blockIdx.find(predNode);
                    if (it != blockIdx.end() && it->second < bestBlock) {
                      bestBlock = it->second;
                    }
                  }
                }
                break;
              }
            }
          } else if (nodeBlock[u] != UINT32_MAX) {
            if (nodeBlock[u] < bestBlock) bestBlock = nodeBlock[u];
          }
        }
      }
      if (bestBlock != UINT32_MAX) {
        nodeBlock[n] = bestBlock;
        changed = true;
      }
    }
  }
  // Any unassigned pure data nodes → entry block (block 0, the Start).
  for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
    if (n >= g.nodeCount()) break;
    const ir::Node& nd = g.node(n);
    if (nd.isDead() || isBlockLeader(nd.kind) || nd.kind == K::Phi) continue;
    if (nodeBlock[n] == UINT32_MAX) nodeBlock[n] = 0;
  }
  // 3. Populate blocks with their nodes.
  for (ir::NodeId n = 0; n < g.nodeCount(); ++n) {
    const ir::Node& nd = g.node(n);
    if (nd.isDead()) continue;
    std::uint32_t b = nodeBlock[n];
    if (b == UINT32_MAX) continue;
    if (isBlockLeader(nd.kind)) continue;  // leader is already the block
    if (nd.kind == K::Phi) {
      blocks[b].fixedNodes.push_back(n);  // phis emitted at block top
    } else if (hasCtrlInput(nd.kind)) {
      blocks[b].fixedNodes.push_back(n);  // ctrl-dependent (incl. chains)
    } else {
      blocks[b].dataNodes.push_back(n);   // pure data
    }
  }
  // 4. Build successor edges.
  // Start → first control user.
  // Region/LoopBegin → the control node that uses it as input[0].
  // If → IfTrue + IfFalse.
  // IfTrue/IfFalse → the control node that uses it as input[0].
  // LoopEnd → LoopBegin (backedge).
  // LoopExit → the control node that uses it as input[0].
  // Return/Unwind/Deopt/End → no successors.
  for (std::uint32_t bi = 0; bi < blocks.size(); ++bi) {
    ir::NodeId leader = blocks[bi].leader;
    const ir::Node& ln = g.node(leader);
    if (ln.kind == K::Return || ln.kind == K::Unwind ||
        ln.kind == K::Deopt || ln.kind == K::End) continue;
    if (ln.kind == K::LoopEnd) {
      // Backedge → LoopBegin (input[0] chain to find the LoopBegin).
      // LoopEnd's ctrl input is the IfTrue/IfFalse that feeds it; the
      // LoopBegin is found by scanning for a LoopBegin whose input[1]
      // chain reaches this LoopEnd.
      for (ir::NodeId i = 0; i < g.nodeCount(); ++i) {
        const ir::Node& lb = g.node(i);
        if (lb.isDead() || lb.kind != K::LoopBegin || lb.numInputs < 2) continue;
        ir::NodeId be = g.input(i, 1);
        if (be == leader) { blocks[bi].successors.push_back(i); break; }
        if (be < g.nodeCount() && !g.node(be).isDead()) {
          const ir::Node& beNode = g.node(be);
          if ((beNode.kind == K::IfTrue || beNode.kind == K::IfFalse) &&
              beNode.numInputs >= 1 && g.input(be, 0) == leader) {
            blocks[bi].successors.push_back(i); break;
          }
        }
      }
      continue;
    }
    if (ln.kind == K::If) {
      // Successors: IfTrue + IfFalse projections.
      for (ir::NodeId i = 0; i < g.nodeCount(); ++i) {
        const ir::Node& pn = g.node(i);
        if (pn.isDead()) continue;
        if ((pn.kind == K::IfTrue || pn.kind == K::IfFalse) &&
            pn.numInputs >= 1 && g.input(i, 0) == leader) {
          blocks[bi].successors.push_back(i);
        }
      }
      continue;
    }
    // Region/LoopBegin/LoopExit/IfTrue/IfFalse/Start: find the control node
    // that uses this node as input[0].
    for (ir::NodeId i = 0; i < g.nodeCount(); ++i) {
      const ir::Node& un = g.node(i);
      if (un.isDead() || !isBlockLeader(un.kind)) continue;
      if (i == leader) continue;
      if (un.numInputs >= 1 && g.input(i, 0) == leader) {
        blocks[bi].successors.push_back(i);
        break;  // first control successor
      }
      // Also check non-leader fixed nodes whose ctrl input is this leader.
      // The first such fixed node's block IS this leader's block, so the
      // successor is the next control node after the fixed nodes.
    }
    // If no direct control successor found, look for a fixed node in this
    // block whose ctrl-dependent chain leads to the next block.
    if (blocks[bi].successors.empty()) {
      for (ir::NodeId i = 0; i < g.nodeCount(); ++i) {
        const ir::Node& un = g.node(i);
        if (un.isDead() || isBlockLeader(un.kind)) continue;
        if (un.numInputs >= 1 && g.input(i, 0) == leader) {
          // This fixed node is in this block. Find what controls the NEXT
          // node that uses this fixed node's output... actually, just find
          // the next control node that is reachable.
          // For simplicity, look for a control node whose input chain
          // includes this fixed node.
          // Actually, the successor is the control node that uses the
          // fixed node's MEMORY or CONTROL output. For v1, scan for any
          // control leader whose input[0] is a fixed node in this block.
          for (ir::NodeId j = 0; j < g.nodeCount(); ++j) {
            if (j == leader) continue;
            const ir::Node& cn = g.node(j);
            if (cn.isDead() || !isBlockLeader(cn.kind)) continue;
            // WHY: skip exception-path nodes (Unwind/Deopt) — they're not
            // normal control successors. Their ctrl chain reaches through
            // CallExcept, which makes the scan find them before the real
            // successor (Return). The real successor is the non-exception
            // block leader whose ctrl chain reaches this block's fixed nodes.
            if (cn.kind == K::Unwind || cn.kind == K::Deopt) continue;
            if (cn.numInputs >= 1) {
              ir::NodeId c0 = g.input(j, 0);
              // Walk c0's ctrl chain to see if it reaches `leader` or a
              // node in this block.
              while (c0 < g.nodeCount() && !g.node(c0).isDead() &&
                     !isBlockLeader(g.node(c0).kind)) {
                if (c0 == i) { blocks[bi].successors.push_back(j); goto found; }
                if (g.node(c0).numInputs >= 1) c0 = g.input(c0, 0);
                else break;
              }
            }
          }
        found:;
        }
      }
    }
  }
  // 5. RPO order (entry-first). For now, just use the leader order (which
  // is node-ID order, roughly RPO since the builder creates nodes in RPO).
  // This is correct for acyclic regions; loops get a backedge jump.
  return blocks;
}

// --- emit a single node's code (the per-node switch) --------------------------
void emitNode(LowerState& s, ir::NodeId n) {
  const ir::Node& nd = s.g.node(n);
  if (nd.isDead()) return;
  using K = ir::NodeKind;
  switch (nd.kind) {
    // === constants ===
    case K::ConstantI: s.em.movEaxImm32(static_cast<std::int32_t>(nd.constValue)); storeInt(s,n); break;
    case K::ConstantL: s.em.movRaxImm64(nd.constValue); storeLong(s,n); break;
    case K::ConstantNull: storeNull(s,n); break;
    case K::ConstantF: case K::ConstantD: case K::ConstantSym:
      s.em.movRaxImm64(nd.constValue); storeLong(s,n); break;
    case K::Parameter: case K::Undef: break;
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
    // === comparisons ===
    case K::EqI: case K::NeI: case K::LtI: case K::LeI: case K::GtI: case K::GeI: {
      loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.cmpEaxEcx();
      std::uint8_t cc = 0x94;
      switch (nd.kind) { case K::EqI: cc=0x94; break; case K::NeI: cc=0x95; break;
        case K::LtI: cc=0x9C; break; case K::LeI: cc=0x9E; break;
        case K::GtI: cc=0x9F; break; case K::GeI: cc=0x9D; break; default: break; }
      s.em.setccAl(cc); s.em.movzxEaxAl(); storeInt(s,n); break;
    }
    case K::CmpI: { loadIntEax(s,s.g.input(n,0)); loadIntEcx(s,s.g.input(n,1)); s.em.cmpEaxEcx();
      s.em.setccAl(0x9F); s.em.movzxEaxAl(); storeInt(s,n); break; }
    case K::Not: { loadIntEax(s,s.g.input(n,0)); s.em.testEaxEax();
      s.em.setccAl(0x94); s.em.movzxEaxAl(); storeInt(s,n); break; }
    case K::IsNull: { auto it=s.slotOf.find(s.g.input(n,0));
      if(it!=s.slotOf.end()) { s.em.loadRbpDisp32(Reg32::EAX, slotTag(it->second));
        s.em.movEcxImm32(static_cast<std::int32_t>(kRTypeNull)); s.em.cmpEaxEcx();
        s.em.setccAl(0x94); s.em.movzxEaxAl(); storeInt(s,n); } break; }
    // === conversions ===
    case K::I2L: { auto it=s.slotOf.find(s.g.input(n,0));
      if(it!=s.slotOf.end()) { s.em.movsxdRaxMem(slotPayload(it->second)); storeLong(s,n); } break; }
    case K::L2I: loadLongRax(s,s.g.input(n,0)); storeInt(s,n); break;
    case K::I2B: loadIntEax(s,s.g.input(n,0)); s.em.movsxEaxAl(); storeInt(s,n); break;
    case K::I2C: loadIntEax(s,s.g.input(n,0)); s.em.movzxEaxAx(); storeInt(s,n); break;
    case K::I2S: loadIntEax(s,s.g.input(n,0)); s.em.movsxEaxAx(); storeInt(s,n); break;
    // === phi (no code; slot filled by predecessor moves) ===
    case K::Phi: break;
    // === memory ops via helpers ===
    case K::LoadStatic: {
      // WHY: the IR's payload is the RBC field CP index. The b2cg_get_static
      // helper expects either a statics-storage FieldId (< kStaticBuiltinBase)
      // or kStaticBuiltinBase | objRefId for System.out/err. Resolve through
      // the runtime: if the CP entry is a builtin (System.out/err), encode
      // the ObjRef id; otherwise pass the field id directly.
      auto dstIt=s.slotOf.find(n);
      if(dstIt==s.slotOf.end()) break;
      std::uint32_t fieldIdOrBuiltin = nd.payload;
      if (nd.payload < s.method.cp.size()) {
        const rbc::Const& fc = s.method.cp[nd.payload];
        auto obj = s.rt.builtinStatic(fc);
        if (obj.has_value()) {
          fieldIdOrBuiltin = kStaticBuiltinBase | obj->id;
        }
      }
      emitHelperCall(s, static_cast<std::uint8_t>(HelperId::GetStatic),
                    fieldIdOrBuiltin, slotOff(dstIt->second));
      break;
    }
    case K::LoadField: { if(nd.numInputs>=3){ auto o=s.slotOf.find(s.g.input(n,2)),d=s.slotOf.find(n);
      if(o!=s.slotOf.end()&&d!=s.slotOf.end()) emitHelperCall(s, static_cast<std::uint8_t>(HelperId::GetField), slotOff(o->second), nd.payload, slotOff(d->second)); } break; }
    case K::StoreField: { if(nd.numInputs>=4){ auto o=s.slotOf.find(s.g.input(n,2)),v=s.slotOf.find(s.g.input(n,3));
      if(o!=s.slotOf.end()&&v!=s.slotOf.end()) emitHelperCall(s, static_cast<std::uint8_t>(HelperId::PutField), slotOff(o->second), nd.payload, slotOff(v->second)); } break; }
    case K::ArrayLength: { if(nd.numInputs>=1){ auto a=s.slotOf.find(s.g.input(n,0)),d=s.slotOf.find(n);
      if(a!=s.slotOf.end()&&d!=s.slotOf.end()) emitHelperCall(s, static_cast<std::uint8_t>(HelperId::ArrayLength), slotOff(a->second), slotOff(d->second)); } break; }
    case K::New: { auto d=s.slotOf.find(n); if(d!=s.slotOf.end()) emitHelperCall(s, static_cast<std::uint8_t>(HelperId::NewObject), nd.payload, slotOff(d->second)); break; }
    case K::NewArray: { if(nd.numInputs>=2){ auto l=s.slotOf.find(s.g.input(n,1)),d=s.slotOf.find(n);
      if(l!=s.slotOf.end()&&d!=s.slotOf.end()) emitHelperCall(s, static_cast<std::uint8_t>(HelperId::NewArray), slotOff(l->second), nd.payload, slotOff(d->second)); } break; }
    // === calls ===
    case K::CallStatic: case K::CallVirtual: case K::CallInterface: {
      auto dstIt=s.slotOf.find(n); std::uint32_t dstOff=(dstIt!=s.slotOf.end())?slotOff(dstIt->second):0;
      std::uint32_t argCount=0;
      ir::NodeId fsNode=ir::kInvalidNodeId;
      if(nd.numInputs>2){ argCount=nd.numInputs-2;
        if(argCount>0){ fsNode=s.g.input(n,nd.numInputs-1);
          if(fsNode<s.g.nodeCount()&&s.g.node(fsNode).kind==K::FrameState) { --argCount; } else { fsNode=ir::kInvalidNodeId; } } }
      // WHY: b2cg_call for virtual/interface calls uses caller.cp[id] to
      // resolve the method name/desc. The IR's nd.payload is a resolved
      // MethodId (from the builder's resolver), NOT a CP index. For static
      // calls, id IS the program method index (matching the helper). For
      // virtual/interface, we must recover the CP index from the RBC: the
      // call's FrameState carries the RBC pc, and the invokevirtual
      // instruction at that pc has the CP index as ins.imm.
      std::uint32_t packedTarget=0;
      if (nd.kind == K::CallStatic) {
        packedTarget = (static_cast<std::uint32_t>(0) << 28) | (nd.payload & 0x0FFF'FFFF);
      } else {
        // Virtual/interface: find the CP index from the RBC instruction.
        std::uint32_t cpIndex = nd.payload; // fallback (wrong but won't crash)
        if (fsNode != ir::kInvalidNodeId && fsNode < s.g.nodeCount()) {
          const ir::Node& fsn = s.g.node(fsNode);
          if (fsn.kind == K::FrameState && fsn.payload < s.g.frameStateCount()) {
            std::uint32_t rbcPc = s.g.frameState(fsn.payload).pc;
            if (rbcPc < s.method.code.size()) {
              cpIndex = s.method.code[rbcPc].imm;
            }
          }
        }
        packedTarget = (static_cast<std::uint32_t>(1) << 28) | (cpIndex & 0x0FFF'FFFF);
      }
      std::uint32_t argBaseSlot=s.nextSlot;
      for(std::uint32_t a=0;a<argCount;++a){ ir::NodeId argNode=s.g.input(n,2+a);
        auto argIt=s.slotOf.find(argNode); if(argIt!=s.slotOf.end()) copySlot(s, argBaseSlot+a, argIt->second); }
      emitHelperCall(s, static_cast<std::uint8_t>(HelperId::Call), slotOff(argBaseSlot), argCount, packedTarget, dstOff);
      break;
    }
    case K::Guard: break; // helpers do their own checks
    case K::FrameState: case K::MemBar: case K::ClassInit: break;
    // === If (not a block leader; emitted as a fixed node in the predecessor block) ===
    case K::If: {
      if (nd.numInputs >= 2) {
        loadIntEax(s, s.g.input(n, 1));
        s.em.testEaxEax();
        std::uint32_t patchOff = s.em.jccRel32(0x84); // je IfFalse
        s.pendingIfFalse.push_back({patchOff, n});
      }
      break;
    }
    default: break;
  }
}

// --- emit a block terminator (the control node's branch/return) ---------------
void emitTerminator(LowerState& s, ir::NodeId n) {
  const ir::Node& nd = s.g.node(n);
  using K = ir::NodeKind;
  switch (nd.kind) {
    case K::Start: break; // no terminator
    case K::Region: case K::LoopBegin: break; // fall through to successor
    case K::IfTrue: case K::IfFalse: break; // fall through
    case K::LoopExit: break; // fall through
    case K::If: break; // handled by emitNode (If is a fixed node, not a leader)
    case K::LoopEnd: {
      // Backedge phi moves (predecessor 1) then jmp LoopBegin.
      ir::NodeId loopBegin = ir::kInvalidNodeId;
      for (ir::NodeId i = 0; i < s.g.nodeCount(); ++i) {
        const ir::Node& lb = s.g.node(i);
        if (lb.isDead() || lb.kind != K::LoopBegin || lb.numInputs < 2) continue;
        ir::NodeId be = s.g.input(i, 1);
        if (be == n) { loopBegin = i; break; }
        if (be < s.g.nodeCount() && !s.g.node(be).isDead()) {
          const ir::Node& beNode = s.g.node(be);
          if ((beNode.kind==K::IfTrue||beNode.kind==K::IfFalse) && beNode.numInputs>=1 && s.g.input(be,0)==n) { loopBegin=i; break; }
        }
      }
      if (loopBegin != ir::kInvalidNodeId) emitPhiMovesForPred(s, loopBegin, 1);
      std::uint32_t patchOff = s.em.jmpRel32();
      s.pendingBackedge.push_back({patchOff, n});
      break;
    }
    case K::Return: {
      if (nd.numInputs >= 2) {
        ir::NodeId val = s.g.input(n, 1);
        auto it = s.slotOf.find(val);
        if (it != s.slotOf.end()) {
          s.em.loadRbpDisp64(Reg::RAX, slotPayload(it->second));
          s.em.storeRbpDisp64(Reg::RAX, static_cast<std::int32_t>(kActOffRetValue));
          s.em.loadRbpDisp32(Reg32::EAX, slotTag(it->second));
          s.em.storeRbpDisp32(Reg32::EAX, static_cast<std::int32_t>(kActOffRetValue));
        }
      }
      std::uint32_t patchOff = s.em.jmpRel32();
      s.pendingJumps.push_back({patchOff, ir::kInvalidNodeId - 1});
      break;
    }
    case K::Unwind: case K::Deopt: {
      std::uint32_t patchOff = s.em.jmpRel32();
      s.pendingJumps.push_back({patchOff, ir::kInvalidNodeId});
      break;
    }
    case K::End: {
      std::uint32_t patchOff = s.em.jmpRel32();
      s.pendingJumps.push_back({patchOff, ir::kInvalidNodeId - 1});
      break;
    }
    default: break;
  }
}

// --- the main lowering pass (block-based) ---

bool lowerGraph(LowerState& s) {
  s.em.prologue();
  std::vector<Block> blocks = buildBlocks(s);
  // Map: leader → block index (for successor lookup).
  std::unordered_map<ir::NodeId, std::uint32_t> blockIdxMap;
  for (std::uint32_t i = 0; i < blocks.size(); ++i) blockIdxMap[blocks[i].leader] = i;

  for (std::uint32_t bi = 0; bi < blocks.size(); ++bi) {
    Block& blk = blocks[bi];
    ir::NodeId leader = blk.leader;
    if (leader == ir::kInvalidNodeId) continue;
    const ir::Node& ln = s.g.node(leader);
    // WHY: do NOT emit entry phi moves at LoopBegin — they would overwrite
    // the backedge phi moves on every iteration, resetting loop variables to
    // their initial values (infinite loop). Instead, entry phi moves are
    // emitted at the END of the predecessor block (see the fall-through logic
    // below). Only Region (non-loop merge) emits entry phi moves here.
    if (ln.kind == K::Region) {
      emitPhiMovesForPred(s, leader, 0);
    }
    s.labelOf[leader] = s.em.offset();
    // WHY: merge data+fixed and emit in NODE-ID order. The builder creates
    // nodes in topological order (inputs before users), so node-ID order
    // ensures a data node like IsNull (depends on LoadStatic) is emitted
    // AFTER LoadStatic, not before.
    {
      std::vector<ir::NodeId> allNodes = blk.dataNodes;
      for (ir::NodeId fn : blk.fixedNodes) allNodes.push_back(fn);
      std::sort(allNodes.begin(), allNodes.end());
      for (ir::NodeId n : allNodes) emitNode(s, n);
    }
    emitTerminator(s, leader);
    // Fall-through JMP: if this block has exactly one successor and it's not
    // the next block in emission order, emit a JMP to it. This handles
    // IfTrue/IfFalse projections (which need to jump to their merge) and
    // LoopExit (which needs to jump to the post-loop code if not adjacent).
    // If blocks: the conditional jump handles one branch; the fall-through is
    // the other — if the fall-through isn't next, emit a JMP.
    if (ln.kind != K::Return && ln.kind != K::Unwind &&
        ln.kind != K::Deopt && ln.kind != K::End &&
        ln.kind != K::LoopEnd && ln.kind != K::If) {
      if (!blk.successors.empty()) {
        ir::NodeId succ = blk.successors[0];
        std::uint32_t succBi = blockIdxMap.count(succ) ? blockIdxMap[succ] : UINT32_MAX;
        // WHY: if the successor is a LoopBegin, emit entry phi moves BEFORE
        // the jump/fall-through (the entry-path predecessor's values go into
        // the phi slots). This is the counterpart to the backedge phi moves
        // emitted at LoopEnd.
        if (succ < s.g.nodeCount() && s.g.node(succ).kind == K::LoopBegin) {
          emitPhiMovesForPred(s, succ, 0);
        }
        if (succBi != bi + 1 && succBi != UINT32_MAX) {
          std::uint32_t patchOff = s.em.jmpRel32();
          s.pendingJumps.push_back({patchOff, succ});
        }
      } else {
        // WHY: no successor found (common for LoopExit — its ctrl-dependent
        // fixed nodes aren't block leaders). Fall through to the next block
        // that ISN'T a LoopEnd (which would be a backedge into the loop).
        // Skip LoopEnd blocks to avoid re-entering the loop on exit.
        for (std::uint32_t next = bi + 1; next < blocks.size(); ++next) {
          ir::NodeId nextLeader = blocks[next].leader;
          if (nextLeader != ir::kInvalidNodeId &&
              s.g.node(nextLeader).kind != K::LoopEnd &&
              s.g.node(nextLeader).kind != K::Unwind &&
              s.g.node(nextLeader).kind != K::Deopt) {
            if (next != bi + 1) {
              std::uint32_t patchOff = s.em.jmpRel32();
              s.pendingJumps.push_back({patchOff, nextLeader});
            }
            break;
          }
        }
      }
    } else if (ln.kind == K::If) {
      // If block: fall-through goes to IfTrue (the next block if in order).
      // The conditional je goes to IfFalse. If IfTrue isn't the next block,
      // emit a JMP to it after the conditional jump.
      // Find which successor is IfTrue (fall-through) and which is IfFalse.
      ir::NodeId ifTrueSucc = ir::kInvalidNodeId, ifFalseSucc = ir::kInvalidNodeId;
      for (ir::NodeId sc : blk.successors) {
        if (sc < s.g.nodeCount() && s.g.node(sc).kind == K::IfTrue) ifTrueSucc = sc;
        else ifFalseSucc = sc;
      }
      // The pendingIfFalse entry already handles the je to IfFalse.
      // If IfTrue (fall-through) isn't the next block, emit a JMP to it.
      if (ifTrueSucc != ir::kInvalidNodeId) {
        std::uint32_t succBi = blockIdxMap.count(ifTrueSucc) ? blockIdxMap[ifTrueSucc] : UINT32_MAX;
        if (succBi != bi + 1 && succBi != UINT32_MAX) {
          std::uint32_t patchOff = s.em.jmpRel32();
          s.pendingJumps.push_back({patchOff, ifTrueSucc});
        }
      }
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
  for (auto& [patchOff, ifNode] : s.pendingIfFalse) {
    ir::NodeId ifFalse = ir::kInvalidNodeId;
    for (ir::NodeId i = 0; i < s.g.nodeCount(); ++i) {
      const ir::Node& ni = s.g.node(i);
      if (ni.isDead() || ni.kind != K::IfFalse) continue;
      if (ni.numInputs >= 1 && s.g.input(i, 0) == ifNode) { ifFalse = i; break; }
    }
    if (ifFalse != ir::kInvalidNodeId) {
      auto lab=s.labelOf.find(ifFalse); s.em.patchRel32(patchOff, lab!=s.labelOf.end()?lab->second:s.normalEpilogue);
    } else { s.em.patchRel32(patchOff, s.normalEpilogue); }
  }
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
      auto lab=s.labelOf.find(loopBegin); s.em.patchRel32(patchOff, lab!=s.labelOf.end()?lab->second:0);
    }
  }
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
