// B-2 Passes - the RBC-to-IR graph builder implementation.
//
// WHY THIS FILE EXISTS:
// The builder is the T2 pipeline's front door (Amendment B.1): it turns one
// verified rbc::Method into one b2::ir::Graph. The normative lowering
// contract - opcode table, FrameState policy, guard placement, exception
// policy, loop/merge phi rules - is docs/graph_builder.md; this file is its
// executable twin.
//
// ALGORITHM (why it is shaped this way):
//   Phase A - defensive structural scan (total input contract: bounded
//             diagnostics, never UB; scope refusals up front).
//   Phase B - blocks, CFG, RPO, dominators (Cooper-Harvey-Kennedy), natural
//             loops, and a Kahn materialization order over NON-back edges.
//             A cycle over non-back edges = irreducible control flow (or an
//             unorderable shape), which v1 refuses: javac-shaped bytecode
//             is always reducible.
//   Phase C - "bytecode type stabilization" (pass-suite item 2): the same
//             join abstract interpretation the RBC verifier runs, mirroring
//             its handler-seeding discipline. Needed for quickened ops
//             (which carry no descriptors - their dst types are the
//             pre-established register types) and as the builder's own
//             type-consistency defense at every FrameState.
//   Phase D - materialization in the Kahn order. Locals and registers are
//             SSA values (iload/istore/moves are slot aliases, no nodes);
//             phis at merges; LoopBegin/LoopEnd for backedges (loop phis
//             include the Graal-style SELF input when a value flows around
//             unchanged); fixed nodes chain control (guards gate the
//             null/bounds/zero traps; memory ops chain the memory state).
//   Phase E - backedge patches, then trivial-phi cleanup (a phi whose
//             non-self inputs are all one node is that node; replaceNode
//             rewires FrameStates automatically - Rule 14 doing real work).
//
// DETERMINISM (Rule 124): fixed successor orders, fixed block orders
// (ascending pc), Kahn tie-break by block index, memoized constants in
// first-encounter order, one deopt-id counter. Same input => same ids,
// same print, same serialization bytes.

#include "b2/passes/GraphBuilder.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <unordered_map>

#include "PassInternal.h"

#include "b2/ir/Node.h"
#include "b2/ir/Printer.h"
#include "b2/ir/Verifier.h"
#include "b2/rbc/Opcode.h"
#include "b2/rbc/Type.h"

namespace b2::passes {

// --- CounterResolver --------------------------------------------------------

namespace {

// Builder-allocated deopt ids start here so they can never collide with the
// frontend-lowered ids that ride in deopt_trap / guard_non_null streams
// (Law 23: named constant, documented in graph_builder.md section 4).
constexpr std::uint32_t kBuilderDeoptIdBase = 0x80000000u;

// Law 23 named bounds. The diag cap mirrors the verifiers' discipline; the
// slot cap guards FrameState input counts (uint16) with margin; the budget
// is the fail-closed dataflow operation ceiling.
constexpr std::size_t kMaxDiags = 100;
constexpr std::uint32_t kMaxSlots = 60000;
constexpr std::uint64_t kBuildBudget = 8'000'000;

[[nodiscard]] ir::IRType rtypeToIr(rbc::RType t) noexcept {
  switch (t) {
  case rbc::RType::Int:
    return ir::IRType::Int;
  case rbc::RType::Long:
    return ir::IRType::Long;
  case rbc::RType::Float:
    return ir::IRType::Float;
  case rbc::RType::Double:
    return ir::IRType::Double;
  case rbc::RType::Null:
    return ir::IRType::Null;
  case rbc::RType::Ref:
    return ir::IRType::Ref;
  case rbc::RType::Bottom:
    break;
  }
  return ir::IRType::Bottom;
}

// Field-descriptor type scan, mirroring the RBC verifier's scanField:
// "I"/"Z"/"B"/"C"/"S" -> Int (sub-integral folding), J/F/D, "L...;" and
// "[...": Ref. Malformed -> Bottom (defensive; verified streams never
// carry one, and the build refuses on the Bottom mismatch).
[[nodiscard]] rbc::RType fieldDescType(std::string_view s) noexcept {
  if (s.empty()) {
    return rbc::RType::Bottom;
  }
  switch (s[0]) {
  case 'I': case 'Z': case 'B': case 'C': case 'S':
    return rbc::RType::Int;
  case 'J':
    return rbc::RType::Long;
  case 'F':
    return rbc::RType::Float;
  case 'D':
    return rbc::RType::Double;
  case 'L': {
    const std::size_t semi = s.find(';');
    if (semi == std::string_view::npos || semi < 2) {
      return rbc::RType::Bottom;
    }
    return rbc::RType::Ref;
  }
  case '[': {
    // Arrays are references; a well-formed component must follow.
    std::size_t i = 1;
    while (i < s.size() && s[i] == '[') {
      ++i;
    }
    if (i >= s.size()) {
      return rbc::RType::Bottom;
    }
    if (s[i] == 'L') {
      const std::size_t semi = s.find(';', i);
      if (semi == std::string_view::npos) {
        return rbc::RType::Bottom;
      }
      return rbc::RType::Ref;
    }
    switch (s[i]) {
    case 'I': case 'Z': case 'B': case 'C': case 'S':
    case 'J': case 'F': case 'D':
      return rbc::RType::Ref;
    default:
      return rbc::RType::Bottom;
    }
  }
  default:
    return rbc::RType::Bottom;
  }
}

[[nodiscard]] bool isBranchSig(rbc::Sig s) noexcept {
  return s == rbc::Sig::Branch || s == rbc::Sig::RegBranch ||
         s == rbc::Sig::RegRegBranch || s == rbc::Sig::RegCpBranch;
}

[[nodiscard]] bool isReturnOp(rbc::Op op) noexcept {
  return op == rbc::Op::Return || op == rbc::Op::Ireturn ||
         op == rbc::Op::Lreturn || op == rbc::Op::Freturn ||
         op == rbc::Op::Dreturn || op == rbc::Op::Areturn;
}

[[nodiscard]] bool canFallThrough(rbc::Op op) noexcept {
  const rbc::OpInfo& oi = rbc::info(op);
  // Only conditional branches (and non-terminators) fall through; goto,
  // switches (default is an explicit payload target), returns, athrow and
  // deopt_trap do not.
  if (oi.sig == rbc::Sig::RegBranch || oi.sig == rbc::Sig::RegRegBranch) {
    return true;
  }
  return !isBranchSig(oi.sig) && !isReturnOp(op) && op != rbc::Op::Athrow &&
         op != rbc::Op::DeoptTrap;
}

// Calls (quick and un-quickened) - the exception-edge sources.
[[nodiscard]] bool isInvokeOp(rbc::Op op) noexcept {
  switch (op) {
  case rbc::Op::Invokevirtual:
  case rbc::Op::Invokespecial:
  case rbc::Op::Invokestatic:
  case rbc::Op::Invokeinterface:
  case rbc::Op::Invokedynamic:
  case rbc::Op::InvokevirtualQuick:
  case rbc::Op::InvokespecialQuick:
  case rbc::Op::InvokestaticQuick:
  case rbc::Op::InvokeinterfaceQuick:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool isQuickCall(rbc::Op op) noexcept {
  return op == rbc::Op::InvokevirtualQuick ||
         op == rbc::Op::InvokespecialQuick ||
         op == rbc::Op::InvokestaticQuick ||
         op == rbc::Op::InvokeinterfaceQuick;
}

[[nodiscard]] bool hasReceiver(rbc::Op op) noexcept {
  switch (op) {
  case rbc::Op::Invokevirtual:
  case rbc::Op::Invokespecial:
  case rbc::Op::Invokeinterface:
  case rbc::Op::InvokevirtualQuick:
  case rbc::Op::InvokespecialQuick:
  case rbc::Op::InvokeinterfaceQuick:
    return true;
  default:
    return false;
  }
}

// Element IRType surfaced by the byte/char/short/int load families (the
// RBC discipline: sub-integral scalars fold into Int; the memory-side
// narrowing happens in StoreElem, not as a value conversion).
[[nodiscard]] ir::IRType elemIrOfAtype(rbc::Atype a) noexcept {
  switch (a) {
  case rbc::Atype::Boolean:
  case rbc::Atype::Byte:
  case rbc::Atype::Char:
  case rbc::Atype::Short:
  case rbc::Atype::Int:
    return ir::IRType::Int;
  case rbc::Atype::Long:
    return ir::IRType::Long;
  case rbc::Atype::Float:
    return ir::IRType::Float;
  case rbc::Atype::Double:
    return ir::IRType::Double;
  }
  return ir::IRType::Int;
}

// One successor edge of a block terminator. `ctrl` is filled during
// materialization; the order of the edges list is the phi-input order at
// the target merge.
struct EdgeExit {
  std::uint32_t target = 0;
  ir::NodeId ctrl = ir::kInvalidNodeId;
};

struct BlockExit {
  bool materialized = false;
  ir::NodeId mem = ir::kInvalidNodeId;
  std::vector<ir::NodeId> slots;
  std::vector<EdgeExit> edges;
};

// Pending backedge patch (applied in Phase E).
struct BackedgePatch {
  std::uint32_t header = 0;   // LoopBegin block
  ir::NodeId loopEnd = ir::kInvalidNodeId;
  ir::NodeId mem = ir::kInvalidNodeId;
  std::vector<ir::NodeId> slots;
};

struct Loop {
  std::uint32_t header = 0;
  std::vector<std::uint32_t> blocks; // natural-loop body incl. header
};

} // namespace

// === PART 2: the Builder ===

namespace {

class Builder {
 public:
  Builder(const rbc::Method& m, SymbolResolver& res, ir::Graph& g,
          ir::MethodId mid, BuildResult& out,
          const detail::InlineSiteWiring* wiring = nullptr,
          detail::InlineBodyResult* body = nullptr)
      : m_(m), res_(res), g_(g), mid_(mid), out_(out), iw_(wiring),
        body_(body) {
    if (iw_ != nullptr) {
      // Inline mode: deopt ids continue the caller graph's allocation.
      nextDeoptId_ = iw_->deoptIdSeed;
    }
  }

  [[nodiscard]] BuildResult run();

 private:
  // --- diagnostics (bounded, ordered) --------------------------------------
  void fail(std::uint32_t pc, std::string_view msg) {
    if (out_.diags.size() < kMaxDiags) {
      BuildDiag d;
      d.pc = pc;
      d.message = msg;
      out_.diags.push_back(std::move(d));
    }
    failed_ = true;
  }
  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  void tick() {
    if (++ticks_ > kBuildBudget) {
      fail(0, "build budget exceeded (pathological input)");
    }
  }

  // --- slot addressing -----------------------------------------------------
  [[nodiscard]] std::uint32_t nSlots() const noexcept {
    return m_.numLocals + m_.numRegs;
  }
  [[nodiscard]] std::uint32_t localIdx(std::uint32_t slot) const noexcept {
    return slot;
  }
  [[nodiscard]] std::uint32_t regIdx(std::uint32_t reg) const noexcept {
    return m_.numLocals + reg;
  }

  [[nodiscard]] ir::NodeId defReg(std::uint32_t pc, std::uint32_t reg) {
    if (reg >= m_.numRegs) {
      fail(pc, "register r" + std::to_string(reg) + " out of range (numRegs " +
                   std::to_string(m_.numRegs) + ")");
      return undef();
    }
    return slots_[regIdx(reg)];
  }
  [[nodiscard]] ir::NodeId defLocal(std::uint32_t pc, std::uint32_t slot) {
    if (slot >= m_.numLocals) {
      fail(pc, "local l" + std::to_string(slot) +
                   " out of range (numLocals " + std::to_string(m_.numLocals) +
                   ")");
      return undef();
    }
    return slots_[localIdx(slot)];
  }
  void setReg(std::uint32_t pc, std::uint32_t reg, ir::NodeId def) {
    if (reg < m_.numRegs) {
      slots_[regIdx(reg)] = def;
      ++slotsVersion_;
    } else {
      fail(pc, "register r" + std::to_string(reg) + " out of range");
    }
  }
  void setLocal(std::uint32_t pc, std::uint32_t slot, ir::NodeId def) {
    if (slot < m_.numLocals) {
      slots_[localIdx(slot)] = def;
      ++slotsVersion_;
    } else {
      fail(pc, "local l" + std::to_string(slot) + " out of range");
    }
  }

  // --- memoized values (deterministic: first-encounter order) --------------
  [[nodiscard]] ir::NodeId undef() {
    if (undef_ == ir::kInvalidNodeId) {
      undef_ = g_.make(ir::NodeKind::Undef);
    }
    return undef_;
  }
  [[nodiscard]] ir::NodeId constInt(std::int32_t v) {
    const auto it = constI_.find(v);
    if (it != constI_.end()) {
      return it->second;
    }
    return constI_.emplace(v, g_.constantI(v)).first->second;
  }
  [[nodiscard]] ir::NodeId constLong(std::int64_t v) {
    const auto it = constL_.find(v);
    if (it != constL_.end()) {
      return it->second;
    }
    return constL_.emplace(v, g_.constantL(v)).first->second;
  }
  [[nodiscard]] ir::NodeId constFloat(float v) {
    const auto it = constF_.find(v);
    if (it != constF_.end()) {
      return it->second;
    }
    return constF_.emplace(v, g_.constantF(v)).first->second;
  }
  [[nodiscard]] ir::NodeId constDouble(double v) {
    const auto it = constD_.find(v);
    if (it != constD_.end()) {
      return it->second;
    }
    return constD_.emplace(v, g_.constantD(v)).first->second;
  }

  // --- phases ---------------------------------------------------------------
  [[nodiscard]] bool scanStructure();
  [[nodiscard]] bool computeBlocks();
  [[nodiscard]] bool computeOrder();
  [[nodiscard]] bool stabilizeTypes();
  [[nodiscard]] bool materialize();
  void patchBackedges();
  void removeTrivialPhis();

  // --- lowering helpers -----------------------------------------------------
  [[nodiscard]] ir::NodeId fsAt(std::uint32_t pc);
  [[nodiscard]] std::uint32_t nextDeoptId() noexcept { return nextDeoptId_++; }
  [[nodiscard]] bool coveredByHandler(std::uint32_t pc) const noexcept;
  [[nodiscard]] const rbc::Const* cpAt(std::uint32_t pc, std::uint32_t idx,
                                       rbc::Const::Kind want);
  [[nodiscard]] bool switchTargets(std::uint32_t pc, const rbc::Ins& ins,
                                   std::vector<std::uint32_t>& targets,
                                   std::uint32_t& defaultTarget);
  ir::NodeId nullGuard(std::uint32_t pc, ir::NodeId obj);
  ir::NodeId zeroGuard(std::uint32_t pc, ir::NodeId divisor,
                       bool isLong = false);
  ir::NodeId boundsGuard(std::uint32_t pc, ir::NodeId array,
                         ir::NodeId index);
  [[nodiscard]] ir::IRType fieldResultType(const rbc::Const& c);
  [[nodiscard]] bool lowerInstruction(std::uint32_t pc, const rbc::Ins& ins);
  [[nodiscard]] bool lowerCall(std::uint32_t pc, const rbc::Ins& ins);
  [[nodiscard]] bool lowerTerminator(std::uint32_t pc, const rbc::Ins& ins);
  void addEdge(std::uint32_t pc, std::uint32_t targetPc, ir::NodeId ctrl);
  void branchOut(std::uint32_t pc, ir::NodeId cond, std::uint32_t target);
  [[nodiscard]] ir::NodeId applyLoopExits(std::uint32_t pc,
                                          std::uint32_t target,
                                          ir::NodeId ctrl);
  void materializeEntry(std::uint32_t b);
  [[nodiscard]] bool materializeBlock(std::uint32_t b);

  // --- data ---------------------------------------------------------------
  const rbc::Method& m_;
  SymbolResolver& res_;
  ir::Graph& g_;
  const ir::MethodId mid_;
  BuildResult& out_;
  bool failed_ = false;
  std::uint64_t ticks_ = 0;

  // Inline mode (null = standalone build): the call-site wiring and the
  // exit collection target (docs/inlining.md section 4).
  const detail::InlineSiteWiring* iw_ = nullptr;
  detail::InlineBodyResult* body_ = nullptr;

  // Blocks / CFG (Phase B).
  std::vector<std::uint32_t> starts_;  // ascending block start pcs
  std::vector<std::uint32_t> blockOf_; // pc -> block
  std::uint32_t nb_ = 0;
  std::vector<std::vector<std::uint32_t>> succ_;   // edges, fixed order
  std::vector<std::vector<std::uint32_t>> fwdPreds_; // non-back preds
  std::vector<std::vector<std::uint32_t>> backPreds_;
  std::vector<char> reachable_;
  std::vector<std::uint32_t> idom_;
  std::vector<std::uint32_t> order_;    // Kahn materialization order
  std::vector<Loop> loops_;
  std::vector<std::uint32_t> loopOf_;   // block -> innermost loop or NONE
  std::vector<std::vector<char>> writes_; // per block: slot -> written?

  // Type stabilization (Phase C): state BEFORE pc, pc-major.
  std::vector<rbc::RType> types_;

  // Materialization (Phase D/E).
  std::vector<BlockExit> exit_;
  std::vector<BackedgePatch> patches_;
  ir::NodeId curCtrl_ = ir::kInvalidNodeId;
  ir::NodeId curMem_ = ir::kInvalidNodeId;
  std::vector<ir::NodeId> slots_;
  std::uint32_t curBlock_ = 0;

  // Header records for Phase E patching: the LoopBegin node per header
  // block, the memory phi (invalid if none), and per-slot loop phis that
  // still need backedge inputs (invalid when the slot has no phi).
  std::vector<ir::NodeId> loopBeginOf_;
  std::vector<ir::NodeId> memPhiOf_;
  std::vector<std::vector<ir::NodeId>> slotPhiOf_;

  // FrameState cache (see fsAt): keyed by pc + slot-state version.
  bool fsCacheValid_ = false;
  std::uint32_t fsCachePc_ = 0;
  std::uint32_t fsCacheVersion_ = 0;
  ir::NodeId fsCache_ = ir::kInvalidNodeId;
  std::uint32_t slotsVersion_ = 0;

  ir::NodeId undef_ = ir::kInvalidNodeId;
  std::unordered_map<std::int32_t, ir::NodeId> constI_;
  std::unordered_map<std::int64_t, ir::NodeId> constL_;
  std::unordered_map<float, ir::NodeId> constF_;
  std::unordered_map<double, ir::NodeId> constD_;
  std::uint32_t nextDeoptId_ = kBuilderDeoptIdBase + 1;
};

// === PART 3: phases A-C ===

BuildResult Builder::run() {
  const std::uint32_t nodesBefore = g_.nodeCount();
  if (scanStructure() && computeBlocks() && computeOrder() &&
      stabilizeTypes() && materialize()) {
    patchBackedges();
    // Trivial-phi collapse is a CALLER-wide sweep; in inline mode the
    // driver owns post-inline cleanup (the pipeline's fusion key), so the
    // sweep is skipped to keep the inline transformation local.
    if (iw_ == nullptr) {
      removeTrivialPhis();
    }
  }
  out_.ok = ok();
  if (body_ != nullptr) {
    body_->ok = ok();
    body_->diags = out_.diags;
    body_->nodesAdded = g_.nodeCount() - nodesBefore;
  }
  return std::move(out_);
}

// --- Phase A: defensive structural scan -------------------------------------

bool Builder::scanStructure() {
  if (m_.isSynchronized()) {
    fail(0, "synchronized methods are not compilable in builder v1: the "
            "FrameState does not yet carry the held-monitor record deopt "
            "must reproduce (interp_contract.md section 1; MSG-007)");
    return false;
  }
  if (m_.isAbstract() ||
      (m_.flags & rbc::method_flags::Native) != 0) {
    fail(0, "abstract/native methods have no body to build");
    return false;
  }
  if (m_.code.empty()) {
    fail(0, "empty method body");
    return false;
  }
  if (nSlots() == 0 || nSlots() > kMaxSlots) {
    fail(0, "locals+registers out of builder range (" +
                std::to_string(nSlots()) + ")");
    return false;
  }
  if (iw_ == nullptr && g_.nodeCount() != 1) {
    // Node 0 = the constructor's Start; anything else means a used graph.
    // (Inline mode builds into the caller's graph by design.)
    fail(0, "graph is not fresh (node 0 = Start expected)");
    return false;
  }
  for (std::uint32_t pc = 0; pc < m_.code.size(); ++pc) {
    tick();
    const rbc::Ins& ins = m_.code[pc];
    if (ins.op >= rbc::opCount()) {
      fail(pc, "invalid opcode " + std::to_string(ins.op));
      continue;
    }
    const rbc::Op op = ins.opcode();
    const rbc::OpInfo& oi = rbc::info(op);
    if (oi.sig == rbc::Sig::Reg || oi.sig == rbc::Sig::RegImm ||
        oi.sig == rbc::Sig::RegCp || oi.sig == rbc::Sig::RegSlot ||
        oi.sig == rbc::Sig::SlotReg || oi.sig == rbc::Sig::RegReg ||
        oi.sig == rbc::Sig::RegRegImm || oi.sig == rbc::Sig::RegRegCp) {
      if (ins.dst >= m_.numRegs && oi.sig != rbc::Sig::SlotReg &&
          oi.sig != rbc::Sig::RegRegRegCp) {
        fail(pc, "dst register r" + std::to_string(ins.dst) +
                     " out of range");
      }
    }
    if (ins.a >= m_.numRegs &&
        (oi.sig == rbc::Sig::Reg || oi.sig == rbc::Sig::RegReg ||
         oi.sig == rbc::Sig::RegBranch || oi.sig == rbc::Sig::RegRegBranch ||
         oi.sig == rbc::Sig::RegRegImm || oi.sig == rbc::Sig::RegRegCp ||
         oi.sig == rbc::Sig::RegCpBranch)) {
      fail(pc, "source register r" + std::to_string(ins.a) +
                   " out of range");
    }
    if (oi.sig == rbc::Sig::RegRegReg && ins.b >= m_.numRegs) {
      fail(pc, "source register r" + std::to_string(ins.b) +
                   " out of range");
    }
    if (isBranchSig(oi.sig) && oi.sig != rbc::Sig::RegCpBranch &&
        ins.imm >= m_.code.size()) {
      fail(pc, "branch target " + std::to_string(ins.imm) +
                   " out of range");
    }
    // v1 scope refusals (mirror T0's v0 refusals; see graph_builder.md).
    switch (op) {
    case rbc::Op::Invokedynamic:
      fail(pc, "invokedynamic is not compilable in builder v1 (no linkage "
               "runtime)");
      break;
    case rbc::Op::Multianewarray:
      fail(pc, "multianewarray is not compilable in builder v1 (v0 defers "
               "multi-dim allocation)");
      break;
    case rbc::Op::GuardClass:
      fail(pc, "guard_class is verifier-only in v0 and not compilable");
      break;
    default:
      break;
    }
  }
  for (const rbc::ExceptionHandler& h : m_.handlers) {
    if (h.start >= h.end || h.end > m_.code.size() ||
        h.handler >= m_.code.size()) {
      fail(0, "exception handler range [" + std::to_string(h.start) + "," +
                  std::to_string(h.end) + ") handler " +
                  std::to_string(h.handler) + " out of range");
    }
  }
  return ok();
}

// --- Phase B: blocks, CFG, dominators, loops, order --------------------------

bool Builder::computeBlocks() {
  const std::uint32_t n = static_cast<std::uint32_t>(m_.code.size());
  std::vector<char> leader(n, 0);
  leader[0] = 1;
  for (std::uint32_t pc = 0; pc < n; ++pc) {
    tick();
    const rbc::Ins& ins = m_.code[pc];
    if (ins.op >= rbc::opCount()) {
      continue;
    }
    const rbc::Op op = ins.opcode();
    const rbc::OpInfo& oi = rbc::info(op);
    if (isBranchSig(oi.sig) && oi.sig != rbc::Sig::RegCpBranch &&
        ins.imm < n) {
      leader[ins.imm] = 1;
    }
    if (oi.sig == rbc::Sig::RegCpBranch) {
      std::vector<std::uint32_t> ts;
      std::uint32_t def = 0;
      (void)switchTargets(pc, ins, ts, def);
      for (const std::uint32_t t : ts) {
        if (t < n) {
          leader[t] = 1;
        }
      }
      if (def < n) {
        leader[def] = 1;
      }
    }
    const bool terminator = isBranchSig(oi.sig) || isReturnOp(op) ||
                            op == rbc::Op::Athrow || op == rbc::Op::DeoptTrap;
    if (terminator && pc + 1 < n) {
      leader[pc + 1] = 1;
    }
  }
  for (const rbc::ExceptionHandler& h : m_.handlers) {
    if (h.handler < n) {
      leader[h.handler] = 1;
    }
  }
  for (std::uint32_t pc = 0; pc < n; ++pc) {
    if (leader[pc] != 0) {
      starts_.push_back(pc);
    }
  }
  nb_ = static_cast<std::uint32_t>(starts_.size());
  blockOf_.assign(n, 0);
  for (std::uint32_t b = 0; b < nb_; ++b) {
    const std::uint32_t e = (b + 1 < nb_) ? starts_[b + 1] : n;
    for (std::uint32_t p = starts_[b]; p < e; ++p) {
      blockOf_[p] = b;
    }
  }

  // Successor edges per block terminator, in FIXED order (taken-edge-first
  // for conditionals, then switch cases in table order, then default).
  // This order is later the phi-input order at merges (determinism).
  succ_.assign(nb_, {});
  for (std::uint32_t b = 0; b < nb_; ++b) {
    tick();
    const std::uint32_t bEnd = (b + 1 < nb_) ? starts_[b + 1] : n;
    const std::uint32_t last = bEnd - 1;
    const rbc::Ins& ins = m_.code[last];
    if (ins.op >= rbc::opCount()) {
      continue;
    }
    const rbc::Op op = ins.opcode();
    const rbc::OpInfo& oi = rbc::info(op);
    auto addSucc = [&](std::uint32_t target) {
      if (target < n) {
        succ_[b].push_back(blockOf_[target]);
      }
    };
    if (oi.sig == rbc::Sig::Branch) {
      addSucc(ins.imm); // goto: single edge
    } else if (oi.sig == rbc::Sig::RegBranch ||
               oi.sig == rbc::Sig::RegRegBranch) {
      addSucc(ins.imm); // taken edge first
      if (last + 1 < n) {
        addSucc(last + 1);
      }
    } else if (oi.sig == rbc::Sig::RegCpBranch) {
      std::vector<std::uint32_t> ts;
      std::uint32_t def = 0;
      (void)switchTargets(last, ins, ts, def);
      for (const std::uint32_t t : ts) {
        addSucc(t);
      }
      addSucc(def);
    } else if (canFallThrough(op) && last + 1 < n) {
      addSucc(last + 1);
    }
  }

  // Per-block slot write sets (loop phi decisions + type analysis reuse).
  writes_.assign(nb_, std::vector<char>(nSlots(), 0));
  for (std::uint32_t b = 0; b < nb_; ++b) {
    const std::uint32_t bEnd = (b + 1 < nb_) ? starts_[b + 1] : n;
    for (std::uint32_t pc = starts_[b]; pc < bEnd; ++pc) {
      tick();
      const rbc::Ins& ins = m_.code[pc];
      if (ins.op >= rbc::opCount()) {
        continue;
      }
      const rbc::Op op = ins.opcode();
      const rbc::OpInfo& oi = rbc::info(op);
      switch (oi.sig) {
      case rbc::Sig::RegSlot: // loads: dst = reg
        writes_[b][regIdx(ins.dst)] = 1;
        break;
      case rbc::Sig::SlotReg: // stores: writes the LOCAL slot
        if (ins.imm < m_.numLocals) {
          writes_[b][localIdx(ins.imm)] = 1;
        }
        break;
      case rbc::Sig::Call:
      case rbc::Sig::CallQuick:
        if (ins.dst < m_.numRegs &&
            (oi.result != rbc::RType::Bottom || isQuickCall(op))) {
          writes_[b][regIdx(ins.dst)] = 1; // void calls write nothing
        }
        break;
      case rbc::Sig::Reg:
      case rbc::Sig::RegImm:
      case rbc::Sig::RegCp:
      case rbc::Sig::RegReg:
      case rbc::Sig::RegRegImm:
      case rbc::Sig::RegRegCp:
      case rbc::Sig::RegRegReg:
        if (oi.result != rbc::RType::Bottom && ins.dst < m_.numRegs) {
          writes_[b][regIdx(ins.dst)] = 1;
        }
        // Quickened field loads keep dst's pre-type: still a (re)write of
        // the register for loop-phi purposes (the def identity changes).
        if (ins.dst < m_.numRegs &&
            (op == rbc::Op::GetfieldQuick || op == rbc::Op::Getfield ||
             op == rbc::Op::Getstatic ||
             op == rbc::Op::Ldc)) {
          writes_[b][regIdx(ins.dst)] = 1;
        }
        break;
      default:
        break;
      }
    }
  }
  return ok();
}

bool Builder::computeOrder() {
  // Reachability over normal edges from block 0.
  reachable_.assign(nb_, 0);
  std::vector<std::uint32_t> stack{0};
  reachable_[0] = 1;
  while (!stack.empty()) {
    const std::uint32_t b = stack.back();
    stack.pop_back();
    tick();
    for (const std::uint32_t t : succ_[b]) {
      if (t < nb_ && reachable_[t] == 0) {
        reachable_[t] = 1;
        stack.push_back(t);
      }
    }
  }

  // RPO via iterative DFS (fixed successor order => deterministic).
  std::vector<std::uint32_t> post;
  post.reserve(nb_);
  {
    std::vector<std::pair<std::uint32_t, std::size_t>> work{{0, 0}};
    std::vector<char> done(nb_, 0);
    std::vector<char> onStack(nb_, 0);
    while (!work.empty()) {
      tick();
      auto& [b, i] = work.back();
      if (i < succ_[b].size()) {
        const std::uint32_t t = succ_[b][i++];
        if (t < nb_ && reachable_[t] && !done[t] && !onStack[t]) {
          onStack[t] = 1;
          work.push_back({t, 0});
        }
        continue;
      }
      done[b] = 1;
      onStack[b] = 0;
      post.push_back(b);
      work.pop_back();
    }
  }
  std::vector<std::uint32_t> rpo(post.rbegin(), post.rend());
  std::vector<std::uint32_t> rpoPos(nb_, 0xFFFFFFFFu);
  for (std::uint32_t i = 0; i < rpo.size(); ++i) {
    rpoPos[rpo[i]] = i;
  }

  // Preds over all edges (fwd vs back split after dominators).
  std::vector<std::vector<std::uint32_t>> preds(nb_);
  for (std::uint32_t b = 0; b < nb_; ++b) {
    for (const std::uint32_t t : succ_[b]) {
      if (t < nb_ && reachable_[t] && reachable_[b]) {
        preds[t].push_back(b);
      }
    }
  }

  // Dominators (Cooper-Harvey-Kennedy, iterative over RPO).
  idom_.assign(nb_, 0xFFFFFFFFu);
  if (!rpo.empty()) {
    idom_[rpo[0]] = rpo[0];
    bool changed = true;
    while (changed && ok()) {
      changed = false;
      for (std::uint32_t i = 1; i < rpo.size(); ++i) {
        tick();
        const std::uint32_t b = rpo[i];
        std::uint32_t newIdom = 0xFFFFFFFFu;
        for (const std::uint32_t p : preds[b]) {
          if (idom_[p] == 0xFFFFFFFFu) {
            continue;
          }
          if (newIdom == 0xFFFFFFFFu) {
            newIdom = p;
          } else {
            // intersect(p, newIdom)
            std::uint32_t x = p, y = newIdom;
            while (x != y) {
              while (rpoPos[x] > rpoPos[y]) {
                x = idom_[x];
              }
              while (rpoPos[y] > rpoPos[x]) {
                y = idom_[y];
              }
            }
            newIdom = x;
          }
        }
        if (newIdom != 0xFFFFFFFFu && idom_[b] != newIdom) {
          idom_[b] = newIdom;
          changed = true;
        }
      }
    }
  }

  // Backedge classification: edge (b -> t) is a backedge iff t dom b.
  const auto dominates = [&](std::uint32_t a, std::uint32_t b) {
    std::uint32_t x = b;
    while (x != 0xFFFFFFFFu) {
      if (x == a) {
        return true;
      }
      if (x == idom_[x]) {
        break;
      }
      x = idom_[x];
    }
    return false;
  };
  fwdPreds_.assign(nb_, {});
  backPreds_.assign(nb_, {});
  for (std::uint32_t b = 0; b < nb_; ++b) {
    if (!reachable_[b]) {
      continue;
    }
    for (const std::uint32_t t : succ_[b]) {
      if (t < nb_ && reachable_[t]) {
        if (dominates(t, b)) {
          backPreds_[t].push_back(b);
        } else {
          fwdPreds_[t].push_back(b);
        }
      }
    }
  }

  // Natural loops: one per header; bodies = header + backward reachability
  // from each backedge source, stopping at the header.
  std::vector<std::uint32_t> loopIndexOfHeader(nb_, 0xFFFFFFFFu);
  for (std::uint32_t h = 0; h < nb_; ++h) {
    tick();
    if (backPreds_[h].empty() || !reachable_[h]) {
      continue;
    }
    const std::uint32_t idx = static_cast<std::uint32_t>(loops_.size());
    loopIndexOfHeader[h] = idx;
    Loop loop;
    loop.header = h;
    std::vector<char> inLoop(nb_, 0);
    inLoop[h] = 1;
    std::vector<std::uint32_t> work;
    for (const std::uint32_t src : backPreds_[h]) {
      if (!inLoop[src]) {
        inLoop[src] = 1;
        work.push_back(src);
      }
    }
    while (!work.empty()) {
      const std::uint32_t b = work.back();
      work.pop_back();
      tick();
      for (const std::uint32_t p : preds[b]) {
        if (reachable_[p] && !inLoop[p]) {
          inLoop[p] = 1;
          work.push_back(p);
        }
      }
    }
    for (std::uint32_t b = 0; b < nb_; ++b) {
      if (inLoop[b]) {
        loop.blocks.push_back(b);
      }
    }
    loops_.push_back(std::move(loop));
  }

  // Innermost loop per block (smallest body wins; ties by header pc).
  loopOf_.assign(nb_, 0xFFFFFFFFu);
  for (std::uint32_t i = 0; i < loops_.size(); ++i) {
    for (const std::uint32_t b : loops_[i].blocks) {
      if (loopOf_[b] == 0xFFFFFFFFu ||
          loops_[i].blocks.size() <
              loops_[loopOf_[b]].blocks.size()) {
        loopOf_[b] = i;
      }
    }
  }

  // Kahn materialization order over FORWARD edges (block index = pc order
  // is the deterministic tie-break). A leftover cycle = irreducible flow.
  std::vector<std::uint32_t> indeg(nb_, 0);
  for (std::uint32_t b = 0; b < nb_; ++b) {
    if (!reachable_[b]) {
      continue;
    }
    for (const std::uint32_t p : fwdPreds_[b]) {
      (void)p;
      ++indeg[b];
    }
  }
  std::vector<std::uint32_t> ready;
  for (std::uint32_t b = 0; b < nb_; ++b) {
    if (reachable_[b] && indeg[b] == 0) {
      ready.push_back(b);
    }
  }
  std::vector<char> queued(nb_, 0);
  for (const std::uint32_t b : ready) {
    queued[b] = 1;
  }
  order_.clear();
  while (!ready.empty()) {
    tick();
    std::pop_heap(ready.begin(), ready.end(), std::greater<>());
    const std::uint32_t b = ready.back();
    ready.pop_back();
    order_.push_back(b);
    for (const std::uint32_t t : succ_[b]) {
      if (t < nb_ && reachable_[t]) {
        // Count every forward edge from b to t.
        bool forward = !dominates(t, b);
        if (forward && indeg[t] > 0) {
          if (--indeg[t] == 0 && !queued[t]) {
            queued[t] = 1;
            ready.push_back(t);
            std::push_heap(ready.begin(), ready.end(), std::greater<>());
          }
        }
      }
    }
  }
  std::uint32_t reachableCount = 0;
  for (std::uint32_t b = 0; b < nb_; ++b) {
    if (reachable_[b]) {
      ++reachableCount;
    }
  }
  if (order_.size() != reachableCount) {
    fail(0, "irreducible control flow: a forward-edge cycle exists; builder "
            "v1 requires reducible methods");
    return false;
  }
  return ok();
}

// --- Phase C: bytecode type stabilization (suite item 2) ---------------------

bool Builder::stabilizeTypes() {
  const std::uint32_t n = static_cast<std::uint32_t>(m_.code.size());
  types_.assign(static_cast<std::size_t>(n) * nSlots(), rbc::RType::Bottom);

  // Entry state: locals from the descriptor (receiver first for instance
  // methods); everything else Bottom.
  std::vector<rbc::RType> entry(nSlots(), rbc::RType::Bottom);
  {
    std::vector<rbc::RType> params;
    (void)rbc::parseParams(m_.descriptor, params);
    std::uint32_t next = 0;
    if (!m_.isStatic()) {
      entry[localIdx(0)] = rbc::RType::Ref;
      next = 1;
    }
    for (std::size_t i = 0; i < params.size() && next < m_.numLocals; ++i) {
      entry[localIdx(next++)] = params[i];
    }
  }

  std::vector<std::vector<rbc::RType>> in(nb_);
  std::vector<char> visited(nb_, 0), inQueue(nb_, 0);
  std::deque<std::uint32_t> queue;
  const auto enq = [&](std::uint32_t b) {
    if (!inQueue[b]) {
      inQueue[b] = 1;
      queue.push_back(b);
    }
  };

  // Seed the entry block (assignment, not join: first edge).
  in[0] = entry;
  visited[0] = 1;
  enq(0);

  // Handler-entry seeds/joins discovered while walking covered pcs,
  // mirroring the RBC verifier: r0 = Ref (the delivered exception), every
  // other register Bottom, locals = the pre-instruction local types.
  const auto seedHandler = [&](std::uint32_t b,
                                const std::vector<rbc::RType>& locals) {
    if (!visited[b]) {
      in[b].assign(nSlots(), rbc::RType::Bottom);
      if (m_.numRegs > 0) {
        in[b][regIdx(0)] = rbc::RType::Ref;
      }
      for (std::uint32_t s = 0; s < m_.numLocals; ++s) {
        in[b][localIdx(s)] = locals[s];
      }
      visited[b] = 1;
      enq(b);
      return;
    }
    bool changed = false;
    for (std::uint32_t s = 0; s < m_.numRegs; ++s) {
      const rbc::RType edge = (s == 0) ? rbc::RType::Ref : rbc::RType::Bottom;
      const rbc::RType j = rbc::join(in[b][regIdx(s)], edge);
      if (j != in[b][regIdx(s)]) {
        in[b][regIdx(s)] = j;
        changed = true;
      }
    }
    for (std::uint32_t s = 0; s < m_.numLocals; ++s) {
      const rbc::RType j = rbc::join(in[b][localIdx(s)], locals[s]);
      if (j != in[b][localIdx(s)]) {
        in[b][localIdx(s)] = j;
        changed = true;
      }
    }
    if (changed) {
      enq(b);
    }
  };

  // One transfer step: type S (by value) through instruction at pc.
  const auto step = [&](std::vector<rbc::RType>& S, std::uint32_t pc) {
    tick();
    const rbc::Ins& ins = m_.code[pc];
    if (ins.op >= rbc::opCount()) {
      return;
    }
    const rbc::Op op = ins.opcode();
    const rbc::OpInfo& oi = rbc::info(op);
    if (oi.sig == rbc::Sig::SlotReg) {
      // Store families type the LOCAL slot; the family type rides in
      // operandA (istore: Int ... astore: Ref) - result is Bottom.
      if (ins.imm < m_.numLocals && oi.operandA != rbc::RType::Bottom) {
        S[localIdx(ins.imm)] = oi.operandA;
      }
      return;
    }
    if (oi.sig == rbc::Sig::RegSlot) {
      if (ins.dst < m_.numRegs && ins.imm < m_.numLocals) {
        S[regIdx(ins.dst)] = S[localIdx(ins.imm)];
      }
      return;
    }
    switch (oi.sig) {
    case rbc::Sig::Reg:
    case rbc::Sig::RegImm:
    case rbc::Sig::RegCp:
    case rbc::Sig::RegReg:
    case rbc::Sig::RegRegImm:
    case rbc::Sig::RegRegCp:
    case rbc::Sig::RegRegReg: {
      rbc::RType result = oi.result;
      if (op == rbc::Op::GetfieldQuick) {
        // Quickened field loads carry resolved offsets, not cp entries;
        // dst keeps its pre-established type (rbc_spec.md SS6.2).
        result = ins.dst < m_.numRegs ? S[regIdx(ins.dst)]
                                      : rbc::RType::Bottom;
      } else if (op == rbc::Op::Ldc) {
        // Probe without diagnostics: kind sets the type, and MT/MH refuse
        // during lowering (the analysis just leaves dst untouched).
        if (ins.imm < m_.cp.size()) {
          const rbc::Const::Kind k = m_.cp[ins.imm].kind;
          if (k == rbc::Const::Kind::String ||
              k == rbc::Const::Kind::Class) {
            result = rbc::RType::Ref;
          }
        }
      } else if (op == rbc::Op::Getfield || op == rbc::Op::Getstatic) {
        if (const rbc::Const* k =
                cpAt(pc, ins.imm, rbc::Const::Kind::FieldRef)) {
          result = fieldDescType(k->str3);
        }
      }
      if (result != rbc::RType::Bottom && ins.dst < m_.numRegs) {
        S[regIdx(ins.dst)] = result;
      }
      return;
    }
    case rbc::Sig::Call: {
      // Kind probe WITHOUT diagnostics: an invokeinterface site legally
      // carries an InterfaceMethodRef, and a cpAt(MethodRef) miss would
      // record a spurious failure diag (found by the ICDG Phase 2
      // interface-call tests - no earlier surface exercised the family).
      // Both kinds have the same (class, name, descriptor) shape.
      if (ins.imm < m_.cp.size()) {
        const rbc::Const& c = m_.cp[ins.imm];
        if (c.kind == rbc::Const::Kind::MethodRef ||
            c.kind == rbc::Const::Kind::InterfaceMethodRef) {
          const rbc::RType ret = rbc::parseReturn(c.str3);
          if (ret != rbc::RType::Bottom && ins.dst < m_.numRegs) {
            S[regIdx(ins.dst)] = ret;
          }
        }
      }
      return;
    }
    default:
      // CallQuick (quickened dst types are pre-established: S is unchanged
      // by omission), branches, returns, monitors, guards: no slot effect.
      return;
    }
  };

  // Fixpoint.
  while (!queue.empty() && ok()) {
    const std::uint32_t b = queue.front();
    queue.pop_front();
    inQueue[b] = 0;
    tick();
    std::vector<rbc::RType> S = in[b];
    const std::uint32_t bEnd = (b + 1 < nb_) ? starts_[b + 1] : n;
    for (std::uint32_t pc = starts_[b]; pc < bEnd; ++pc) {
      // Exception-edge seeding BEFORE the instruction's effects (the
      // faulting instruction's stores do not complete).
      if (!m_.handlers.empty() && isInvokeOp(m_.code[pc].opcode())) {
        for (const rbc::ExceptionHandler& h : m_.handlers) {
          if (pc >= h.start && pc < h.end && h.handler < n) {
            seedHandler(blockOf_[h.handler], S);
          }
        }
      }
      step(S, pc);
    }
    // Propagate to forward successors (join; first edge seeds).
    for (const std::uint32_t t : succ_[b]) {
      if (t >= nb_ || !reachable_[t]) {
        continue;
      }
      if (!visited[t]) {
        in[t] = S;
        visited[t] = 1;
        enq(t);
      } else {
        bool changed = false;
        for (std::uint32_t s = 0; s < nSlots(); ++s) {
          const rbc::RType j = rbc::join(in[t][s], S[s]);
          if (j != in[t][s]) {
            in[t][s] = j;
            changed = true;
          }
        }
        if (changed) {
          enq(t);
        }
      }
    }
  }

  // Materialize per-pc states (BEFORE pc) for visited blocks.
  for (std::uint32_t b = 0; b < nb_; ++b) {
    if (!visited[b] || !reachable_[b]) {
      continue;
    }
    std::vector<rbc::RType> S = in[b];
    const std::uint32_t bEnd = (b + 1 < nb_) ? starts_[b + 1] : n;
    for (std::uint32_t pc = starts_[b]; pc < bEnd; ++pc) {
      for (std::uint32_t s = 0; s < nSlots(); ++s) {
        types_[static_cast<std::size_t>(pc) * nSlots() + s] = S[s];
      }
      step(S, pc);
    }
  }
  return ok();
}

} // namespace

// === PART 4: Phase D/E + entry ===

namespace {

// --- Phase D: materialization ------------------------------------------------

ir::NodeId Builder::fsAt(std::uint32_t pc) {
  // The FrameState captures ALL frame slots, locals first then registers
  // (interp_contract.md section 1: deopt materializes exactly those
  // sizes). Undef slots materialize as T0's Bottom-tagged Values.
  // Guards and the guarded op of ONE instruction share the node: the slot
  // state only changes at instruction boundaries, so (pc, version) keys it.
  if (fsCacheValid_ && fsCachePc_ == pc && fsCacheVersion_ == slotsVersion_) {
    return fsCache_;
  }
  // Inline mode: every callee FrameState chains to the call-site snapshot
  // (FrameStateDesc.caller) so deopt can reconstruct the inlined frame
  // stack (Rule 75; the chain is the inlining depth seam, ir_spec 5.1).
  const ir::FrameStateId caller =
      iw_ != nullptr ? iw_->fsCaller : ir::kInvalidFrameState;
  const ir::NodeId fs = g_.makeFrameState(
      mid_, pc, std::span<const ir::NodeId>(slots_), caller);
  fsCacheValid_ = true;
  fsCachePc_ = pc;
  fsCacheVersion_ = slotsVersion_;
  fsCache_ = fs;
  // Type-consistency defense (the builder's own Law-36-style guard): every
  // captured def must be assignable to the stabilized slot type. A
  // mismatch means the SSA construction diverged from the verified types.
  for (std::uint32_t s = 0; s < nSlots() && ok(); ++s) {
    const rbc::RType want = types_[static_cast<std::size_t>(pc) * nSlots() + s];
    const ir::IRType got = ir::resultTypeOf(g_, slots_[s]);
    const ir::IRType wantIr = rtypeToIr(want);
    if (got != wantIr &&
        !ir::isAssignable(got, wantIr)) {
      fail(pc, "internal: slot " + std::to_string(s) +
                   " def type " + ir::typeName(got) +
                   " disagrees with stabilized type " +
                   ir::typeName(wantIr));
      break;
    }
  }
  return fs;
}

bool Builder::coveredByHandler(std::uint32_t pc) const noexcept {
  for (const rbc::ExceptionHandler& h : m_.handlers) {
    if (pc >= h.start && pc < h.end) {
      return true;
    }
  }
  return false;
}

const rbc::Const* Builder::cpAt(std::uint32_t pc, std::uint32_t idx,
                                rbc::Const::Kind want) {
  if (idx >= m_.cp.size()) {
    fail(pc, "constant pool index " + std::to_string(idx) + " out of range");
    return nullptr;
  }
  const rbc::Const& c = m_.cp[idx];
  if (c.kind != want) {
    fail(pc, "constant pool entry " + std::to_string(idx) + " is " +
                 c.kindName() + ", expected " +
                 (want == rbc::Const::Kind::FieldRef    ? "fieldref"
                  : want == rbc::Const::Kind::MethodRef ? "methodref"
                  : want == rbc::Const::Kind::InterfaceMethodRef
                      ? "interfacemethodref"
                  : want == rbc::Const::Kind::Class     ? "class"
                                                        : "utf8/string"));
    return nullptr;
  }
  return &c;
}

bool Builder::switchTargets(std::uint32_t pc, const rbc::Ins& ins,
                            std::vector<std::uint32_t>& targets,
                            std::uint32_t& defaultTarget) {
  const rbc::Const* c = cpAt(pc, ins.imm, rbc::Const::Kind::SwitchTable);
  if (c == nullptr) {
    return false;
  }
  const std::vector<std::int32_t>& v = c->ints;
  targets.clear();
  defaultTarget = 0;
  if (ins.opcode() == rbc::Op::Tableswitch) {
    if (v.size() < 3) {
      fail(pc, "malformed tableswitch payload");
      return false;
    }
    const std::int64_t low = v[0];
    const std::int64_t high = v[1];
    if (low > high || 3 + (high - low) + 1 !=
                          static_cast<std::int64_t>(v.size())) {
      fail(pc, "malformed tableswitch payload");
      return false;
    }
    for (std::size_t i = 3; i < v.size(); ++i) {
      if (v[i] < 0 || v[i] >= static_cast<std::int32_t>(m_.code.size())) {
        fail(pc, "tableswitch target out of range");
        return false;
      }
      targets.push_back(static_cast<std::uint32_t>(v[i]));
    }
    defaultTarget = static_cast<std::uint32_t>(v[2]);
  } else {
    if (v.size() < 2 || v[0] < 0 ||
        2 + 2 * static_cast<std::int64_t>(v[0]) !=
            static_cast<std::int64_t>(v.size())) {
      fail(pc, "malformed lookupswitch payload");
      return false;
    }
    for (std::int64_t i = 0; i < v[0]; ++i) {
      const std::int32_t t = v[static_cast<std::size_t>(3 + 2 * i)];
      if (t < 0 || t >= static_cast<std::int32_t>(m_.code.size())) {
        fail(pc, "lookupswitch target out of range");
        return false;
      }
      targets.push_back(static_cast<std::uint32_t>(t));
    }
    defaultTarget = static_cast<std::uint32_t>(v[1]);
  }
  if (defaultTarget >= m_.code.size()) {
    fail(pc, "switch default target out of range");
    return false;
  }
  return true;
}

ir::NodeId Builder::nullGuard(std::uint32_t pc, ir::NodeId obj) {
  const ir::NodeId isnull = g_.make(ir::NodeKind::IsNull, {obj});
  const ir::NodeId notnull = g_.make(ir::NodeKind::Not, {isnull});
  const ir::NodeId fs = fsAt(pc);
  curCtrl_ = g_.make(ir::NodeKind::Guard, {curCtrl_, notnull, fs},
                     static_cast<std::uint32_t>(ir::GuardKind::NullCheck),
                     nextDeoptId());
  return curCtrl_;
}

ir::NodeId Builder::zeroGuard(std::uint32_t pc, ir::NodeId divisor,
                               bool isLong) {
  // Long divisors guard through L2I: NeI takes Int operands and there is
  // no NeL kind. L2I(divisor) == 0 iff divisor is 0 OR a nonzero multiple
  // of 2^32 - the guard then deopts on a non-trapping divisor, which is
  // always observably equivalent (deopt-to-T0 re-execution; the spurious
  // case is astronomically rare). The clean CmpL(divisor, 0L) comparison
  // is blocked until the IR team fixes resultTypeOf(CmpL) - currently
  // classified Long-producing although Node.h defines the result as int
  // (reported as MSG-009 together with I2L and the fp-compare group).
  ir::NodeId ne = divisor;
  if (isLong) {
    const ir::NodeId narrowed =
        g_.make(ir::NodeKind::L2I, {divisor});
    ne = g_.make(ir::NodeKind::NeI, {narrowed, constInt(0)});
  } else {
    ne = g_.make(ir::NodeKind::NeI, {divisor, constInt(0)});
  }
  const ir::NodeId fs = fsAt(pc);
  curCtrl_ = g_.make(ir::NodeKind::Guard, {curCtrl_, ne, fs},
                     static_cast<std::uint32_t>(ir::GuardKind::ZeroCheck),
                     nextDeoptId());
  return curCtrl_;
}

ir::NodeId Builder::boundsGuard(std::uint32_t pc, ir::NodeId array,
                                ir::NodeId index) {
  const ir::NodeId len = g_.make(ir::NodeKind::ArrayLength, {array});
  const ir::NodeId ge = g_.make(ir::NodeKind::GeI, {index, constInt(0)});
  const ir::NodeId lt = g_.make(ir::NodeKind::LtI, {index, len});
  const ir::NodeId cond = g_.make(ir::NodeKind::AndI, {ge, lt});
  const ir::NodeId fs = fsAt(pc);
  curCtrl_ = g_.make(ir::NodeKind::Guard, {curCtrl_, cond, fs},
                     static_cast<std::uint32_t>(ir::GuardKind::BoundsCheck),
                     nextDeoptId());
  return curCtrl_;
}

ir::IRType Builder::fieldResultType(const rbc::Const& c) {
  return rtypeToIr(fieldDescType(c.str3));
}

ir::NodeId Builder::applyLoopExits(std::uint32_t pc, std::uint32_t target,
                                   ir::NodeId ctrl) {
  // Loops containing curBlock_ but not the target, innermost first: one
  // LoopExit per crossed boundary, each a safepoint/deopt point (Rule 126)
  // with the FrameState at the terminating instruction.
  const auto enclosing = [&](std::uint32_t li) -> std::uint32_t {
    // The smallest loop (other than li) whose body contains li's header.
    std::uint32_t best = 0xFFFFFFFFu;
    std::size_t bestSize = static_cast<std::size_t>(-1);
    for (std::uint32_t j = 0; j < loops_.size(); ++j) {
      if (j == li) {
        continue;
      }
      if (std::find(loops_[j].blocks.begin(), loops_[j].blocks.end(),
                    loops_[li].header) != loops_[j].blocks.end() &&
          loops_[j].blocks.size() < bestSize) {
        bestSize = loops_[j].blocks.size();
        best = j;
      }
    }
    return best;
  };
  std::vector<std::uint32_t> crossed;
  std::uint32_t cur = loopOf_[curBlock_];
  while (cur != 0xFFFFFFFFu) {
    const Loop& loop = loops_[cur];
    const bool inTarget = std::find(loop.blocks.begin(), loop.blocks.end(),
                                    target) != loop.blocks.end();
    if (!inTarget) {
      crossed.push_back(cur);
    }
    cur = enclosing(cur);
  }
  for (const std::uint32_t li : crossed) {
    (void)li;
    const ir::NodeId fs = fsAt(pc);
    ctrl = g_.make(ir::NodeKind::LoopExit, {ctrl, fs});
  }
  return ctrl;
}

void Builder::addEdge(std::uint32_t pc, std::uint32_t targetPc,
                      ir::NodeId ctrl) {
  if (targetPc >= m_.code.size()) {
    fail(pc, "branch target " + std::to_string(targetPc) +
                 " out of range");
    return;
  }
  const std::uint32_t target = blockOf_[targetPc];
  // Backedge? (the target block dominates the source block)
  std::uint32_t x = curBlock_;
  bool back = false;
  while (x != 0xFFFFFFFFu) {
    if (x == target) {
      back = true;
      break;
    }
    if (x == idom_[x]) {
      break;
    }
    x = idom_[x];
  }
  if (back) {
    const ir::NodeId loopEnd = g_.make(ir::NodeKind::LoopEnd, {ctrl});
    BackedgePatch patch;
    patch.header = target;
    patch.loopEnd = loopEnd;
    patch.mem = curMem_;
    patch.slots = slots_;
    patches_.push_back(std::move(patch));
    EdgeExit e;
    e.target = target;
    e.ctrl = loopEnd;
    exit_[curBlock_].edges.push_back(e);
    return;
  }
  ctrl = applyLoopExits(pc, target, ctrl);
  EdgeExit e;
  e.target = target;
  e.ctrl = ctrl;
  exit_[curBlock_].edges.push_back(e);
}

bool Builder::lowerCall(std::uint32_t pc, const rbc::Ins& ins) {
  const rbc::Op op = ins.opcode();
  const bool quick = isQuickCall(op);

  ir::MethodId methodId = 0;
  ir::IRType ret = ir::IRType::Bottom;
  if (quick) {
    methodId = ins.imm;
    // Quickened calls carry no descriptor: the dst's pre-established type
    // is the result type (rbc_spec.md SS6.2).
    const rbc::RType pre = ins.dst < m_.numRegs
                               ? types_[static_cast<std::size_t>(pc) *
                                            nSlots() +
                                        regIdx(ins.dst)]
                               : rbc::RType::Bottom;
    if (pre == rbc::RType::Bottom) {
      fail(pc, "quickened call dst register not pre-typed (unverified "
               "stream?)");
      return false;
    }
    ret = rtypeToIr(pre);
  } else {
    const rbc::Const::Kind want = op == rbc::Op::Invokeinterface
                                      ? rbc::Const::Kind::InterfaceMethodRef
                                      : rbc::Const::Kind::MethodRef;
    const rbc::Const* c = cpAt(pc, ins.imm, want);
    if (c == nullptr) {
      return false;
    }
    methodId = res_.methodId(c->str, c->str2, c->str3);
    ret = rtypeToIr(rbc::parseReturn(c->str3));
  }

  if (ins.b > m_.numRegs || static_cast<std::uint32_t>(ins.a) +
                                    static_cast<std::uint32_t>(ins.b) >
                                m_.numRegs) {
    fail(pc, "call argument window r" + std::to_string(ins.a) + "..r" +
                 std::to_string(ins.a + ins.b) + " out of range");
    return false;
  }
  // The callee reads exactly its declared parameters (plus the receiver);
  // T0 copies the full b window but the tail is unread garbage - passing
  // it would feed Undef into Call args (unverifiable). Descriptor-derived
  // count for un-quickened calls; quick calls keep b (no descriptor - a
  // quickened Undef arg refuses below as an unverified stream).
  std::uint32_t argCount = ins.b;
  if (!quick) {
    const rbc::Const& c = m_.cp[ins.imm];
    argCount = rbc::paramCount(c.str3);
    if (hasReceiver(op)) {
      ++argCount;
    }
    if (argCount > ins.b) {
      argCount = ins.b; // fewer than declared: the tail stays unreadable
    }
  }

  // Receiver null guard for dispatching flavors (deopt-to-T0 re-executes
  // the call, which is behavior-preserving by construction).
  if (hasReceiver(op)) {
    if (ins.b == 0) {
      fail(pc, "dispatching invoke without a receiver argument");
      return false;
    }
    // Receiver defense: the stabilized type must be a reference. (RBC
    // streams that dispatch on a primitive receiver slip past some v0
    // verifier paths; the builder refuses instead of emitting an
    // IsNull(Int) the IR verifier would reject.)
    const rbc::RType rt = ins.a < m_.numRegs
                              ? types_[static_cast<std::size_t>(pc) *
                                           nSlots() +
                                       regIdx(ins.a)]
                              : rbc::RType::Bottom;
    if (rt != rbc::RType::Ref && rt != rbc::RType::Null) {
      fail(pc, "call receiver r" + std::to_string(ins.a) +
                   " has stabilized type " + rbc::typeName(rt) +
                   ", expected ref");
      return false;
    }
    nullGuard(pc, defReg(pc, ins.a));
  }

  // JVMS 5.5 first-use trigger on static calls (T0 pin: the quickened
  // static call still honors it).
  if (op == rbc::Op::Invokestatic || op == rbc::Op::InvokestaticQuick) {
    ir::TypeId cls = 0;
    if (quick) {
      cls = 0; // the quickened id names a program method; the class is the
               // program class in the v0 one-class world (T0 pin)
    } else {
      const rbc::Const* c =
          cpAt(pc, ins.imm, rbc::Const::Kind::MethodRef);
      if (c == nullptr) {
        return false;
      }
      cls = res_.classId(c->str);
    }
    curMem_ = g_.make(ir::NodeKind::ClassInit, {curCtrl_, curMem_}, cls);
    curCtrl_ = curMem_;
  }

  const ir::NodeId fs = fsAt(pc);
  std::vector<ir::NodeId> inputs{curCtrl_, curMem_};
  for (std::uint32_t i = 0; i < argCount; ++i) {
    const ir::NodeId arg = defReg(pc, ins.a + i);
    if (quick && g_.node(arg).kind == ir::NodeKind::Undef) {
      fail(pc, "quickened call argument r" + std::to_string(ins.a + i) +
                   " is never written (unverified stream?)");
      return false;
    }
    inputs.push_back(arg);
  }
  inputs.push_back(fs);

  ir::NodeKind kind = ir::NodeKind::CallStatic;
  switch (op) {
  case rbc::Op::Invokevirtual:
  case rbc::Op::InvokevirtualQuick:
    kind = ir::NodeKind::CallVirtual;
    break;
  case rbc::Op::Invokeinterface:
  case rbc::Op::InvokeinterfaceQuick:
    kind = ir::NodeKind::CallInterface;
    break;
  case rbc::Op::Invokespecial:
  case rbc::Op::InvokespecialQuick:
  case rbc::Op::Invokestatic:
  case rbc::Op::InvokestaticQuick:
    kind = ir::NodeKind::CallStatic; // direct dispatch
    break;
  default:
    break;
  }
  const ir::NodeId call = g_.make(kind, inputs, methodId,
                                  static_cast<std::uint32_t>(ret));
  const ir::NodeId except = g_.make(ir::NodeKind::CallExcept, {call});

  if (coveredByHandler(pc) || (iw_ != nullptr && iw_->siteCovered)) {
    // Exception POLICY v1: exception deopt (class 2.3). The Deopt node's
    // control being the CallExcept signals the class; the exception value
    // is the CallExcept's value (transport convention for the deopt stub,
    // graph_builder.md section 5). T0 re-enters THE EXCEPTION ALGORITHM at
    // the call pc with pendingException set. Inline mode: the siteCovered
    // extension routes a callee's ESCAPING nested-call exception the same
    // way - the algorithm finds no handler in the callee, unwinds the
    // reconstructed callee frame, and the deopt runtime re-enters the
    // caller at the call pc with the pending exception (docs/inlining.md
    // section 4).
    g_.make(ir::NodeKind::Deopt, {except, fs}, nextDeoptId());
  } else {
    g_.make(ir::NodeKind::Unwind, {except, except});
  }

  curCtrl_ = call;
  curMem_ = call;
  if (ins.dst < m_.numRegs && ret != ir::IRType::Bottom) {
    setReg(pc, ins.dst, call);
  }
  return ok();
}

bool Builder::lowerInstruction(std::uint32_t pc, const rbc::Ins& ins) {
  const rbc::Op op = ins.opcode();
  switch (op) {
  // --- misc ---------------------------------------------------------------
  case rbc::Op::Nop:
    return true;
  case rbc::Op::SafepointPoll:
    // T2's codegen owns poll placement (codegen-team charter); the RBC
    // explicit poll is the no-own-poll-tier hook. Semantically invisible.
    return true;

  // --- constants ------------------------------------------------------------
  case rbc::Op::AconstNull:
    setReg(pc, ins.dst, g_.constantNull());
    return true;
  case rbc::Op::Iconst:
    setReg(pc, ins.dst, constInt(static_cast<std::int32_t>(ins.imm)));
    return true;
  case rbc::Op::Fconst: {
    float v = 0.0F;
    static_assert(sizeof(v) == sizeof(ins.imm), "fconst bit width");
    std::memcpy(&v, &ins.imm, sizeof(v));
    setReg(pc, ins.dst, constFloat(v));
    return true;
  }
  case rbc::Op::Lconst: {
    const rbc::Const* c = cpAt(pc, ins.imm, rbc::Const::Kind::Int64);
    if (c == nullptr) {
      return false;
    }
    setReg(pc, ins.dst, constLong(c->i64));
    return true;
  }
  case rbc::Op::Dconst: {
    const rbc::Const* c = cpAt(pc, ins.imm, rbc::Const::Kind::Double);
    if (c == nullptr) {
      return false;
    }
    setReg(pc, ins.dst, constDouble(c->f64));
    return true;
  }
  case rbc::Op::Ldc: {
    // Probe WITHOUT diagnostics: the kind decides the path, and the
    // refused kinds (MethodType/MethodHandle) report themselves.
    if (ins.imm >= m_.cp.size()) {
      fail(pc, "constant pool index " + std::to_string(ins.imm) +
                   " out of range");
      return false;
    }
    const rbc::Const& c = m_.cp[ins.imm];
    switch (c.kind) {
    case rbc::Const::Kind::String:
      setReg(pc, ins.dst, g_.constantSym(res_.symbolId(c.str)));
      return true;
    case rbc::Const::Kind::Class: {
      // ldc-of-Class is a JVMS 5.5 initialization trigger (T0 pin).
      const ir::TypeId cls = res_.classId(c.str);
      curMem_ = g_.make(ir::NodeKind::ClassInit, {curCtrl_, curMem_}, cls);
      curCtrl_ = curMem_;
      setReg(pc, ins.dst, g_.constantSym(res_.symbolId(c.str)));
      return true;
    }
    case rbc::Const::Kind::MethodType:
      fail(pc, "ldc of methodtype is not compilable in v0 (no "
               "method-handle runtime)");
      return false;
    case rbc::Const::Kind::MethodHandle:
      fail(pc, "ldc of methodhandle is not compilable in v0 (no "
               "method-handle runtime)");
      return false;
    default:
      fail(pc, "ldc of unsupported constant kind");
      return false;
    }
  }

  // --- local slots <-> registers (pure SSA aliasing; no nodes) --------------
  case rbc::Op::Iload:
  case rbc::Op::Lload:
  case rbc::Op::Fload:
  case rbc::Op::Dload:
  case rbc::Op::Aload:
    setReg(pc, ins.dst, defLocal(pc, ins.imm));
    return true;
  case rbc::Op::Istore:
  case rbc::Op::Lstore:
  case rbc::Op::Fstore:
  case rbc::Op::Dstore:
  case rbc::Op::Astore:
    setLocal(pc, ins.imm, defReg(pc, ins.a));
    return true;
  case rbc::Op::Imove:
  case rbc::Op::Lmove:
  case rbc::Op::Fmove:
  case rbc::Op::Dmove:
  case rbc::Op::Amove:
    setReg(pc, ins.dst, defReg(pc, ins.a));
    return true;

  // --- int arithmetic ---------------------------------------------------------
  case rbc::Op::Iadd: case rbc::Op::Isub: case rbc::Op::Imul:
  case rbc::Op::Ishl: case rbc::Op::Ishr: case rbc::Op::Iushr:
  case rbc::Op::Iand: case rbc::Op::Ior: case rbc::Op::Ixor: {
    // The RBC opcode order (Iadd..Ixor) has Idiv/Irem/Ineg at offsets
    // 3-5 - they lower through their own guarded cases below, so the
    // table carries placeholder rows to keep the index arithmetic exact.
    // The layout is pinned by static_asserts: an opcode renumbering can
    // never silently reopen the out-of-bounds hole this table had in
    // T2-IR2 (Iand/Ior/Ixor read past the end; shifts mapped to the
    // bitwise kinds - caught by the pass suite's bitwise tests).
    static_assert(static_cast<std::uint32_t>(rbc::Op::Idiv) -
                      static_cast<std::uint32_t>(rbc::Op::Iadd) == 3u);
    static_assert(static_cast<std::uint32_t>(rbc::Op::Ineg) -
                      static_cast<std::uint32_t>(rbc::Op::Iadd) == 5u);
    static_assert(static_cast<std::uint32_t>(rbc::Op::Ishl) -
                      static_cast<std::uint32_t>(rbc::Op::Iadd) == 6u);
    static_assert(static_cast<std::uint32_t>(rbc::Op::Ixor) -
                      static_cast<std::uint32_t>(rbc::Op::Iadd) == 11u);
    static constexpr ir::NodeKind kinds[] = {
        ir::NodeKind::AddI, ir::NodeKind::SubI, ir::NodeKind::MulI,
        ir::NodeKind::Start, // Idiv: guarded case below
        ir::NodeKind::Start, // Irem: guarded case below
        ir::NodeKind::Start, // Ineg: unary case below
        ir::NodeKind::ShlI, ir::NodeKind::ShrI, ir::NodeKind::UShrI,
        ir::NodeKind::AndI, ir::NodeKind::OrI, ir::NodeKind::XorI};
    const std::uint32_t base =
        static_cast<std::uint32_t>(rbc::Op::Iadd);
    setReg(pc, ins.dst,
           g_.make(kinds[static_cast<std::uint32_t>(op) - base],
                   {defReg(pc, ins.a), defReg(pc, ins.b)}));
    return true;
  }
  case rbc::Op::Idiv:
  case rbc::Op::Irem: {
    zeroGuard(pc, defReg(pc, ins.b));
    const ir::NodeKind k = op == rbc::Op::Idiv ? ir::NodeKind::DivI
                                                : ir::NodeKind::RemI;
    setReg(pc, ins.dst,
           g_.make(k, {defReg(pc, ins.a), defReg(pc, ins.b)}));
    return true;
  }
  case rbc::Op::Ineg:
    setReg(pc, ins.dst,
           g_.make(ir::NodeKind::NegI, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::Iinc:
    setReg(pc, ins.dst,
           g_.make(ir::NodeKind::AddI,
                   {defReg(pc, ins.dst),
                    constInt(static_cast<std::int32_t>(ins.imm))}));
    return true;

  // --- long arithmetic ----------------------------------------------------------
  case rbc::Op::Ladd: case rbc::Op::Lsub: case rbc::Op::Lmul:
  case rbc::Op::Lshl: case rbc::Op::Lshr: case rbc::Op::Lushr:
  case rbc::Op::Land: case rbc::Op::Lor: case rbc::Op::Lxor: {
    // Same discipline as the int table above: Ldiv/Lrem/Lneg sit at
    // offsets 3-5 with their own cases; layout pinned by asserts.
    static_assert(static_cast<std::uint32_t>(rbc::Op::Ldiv) -
                      static_cast<std::uint32_t>(rbc::Op::Ladd) == 3u);
    static_assert(static_cast<std::uint32_t>(rbc::Op::Lneg) -
                      static_cast<std::uint32_t>(rbc::Op::Ladd) == 5u);
    static_assert(static_cast<std::uint32_t>(rbc::Op::Lshl) -
                      static_cast<std::uint32_t>(rbc::Op::Ladd) == 6u);
    static_assert(static_cast<std::uint32_t>(rbc::Op::Lxor) -
                      static_cast<std::uint32_t>(rbc::Op::Ladd) == 11u);
    static constexpr ir::NodeKind kinds[] = {
        ir::NodeKind::AddL, ir::NodeKind::SubL, ir::NodeKind::MulL,
        ir::NodeKind::Start, // Ldiv: guarded case below
        ir::NodeKind::Start, // Lrem: guarded case below
        ir::NodeKind::Start, // Lneg: unary case below
        ir::NodeKind::ShlL, ir::NodeKind::ShrL, ir::NodeKind::UShrL,
        ir::NodeKind::AndL, ir::NodeKind::OrL, ir::NodeKind::XorL};
    const std::uint32_t base = static_cast<std::uint32_t>(rbc::Op::Ladd);
    setReg(pc, ins.dst,
           g_.make(kinds[static_cast<std::uint32_t>(op) - base],
                   {defReg(pc, ins.a), defReg(pc, ins.b)}));
    return true;
  }
  case rbc::Op::Ldiv:
  case rbc::Op::Lrem: {
    zeroGuard(pc, defReg(pc, ins.b), /*isLong=*/true);
    const ir::NodeKind k = op == rbc::Op::Ldiv ? ir::NodeKind::DivL
                                                : ir::NodeKind::RemL;
    setReg(pc, ins.dst,
           g_.make(k, {defReg(pc, ins.a), defReg(pc, ins.b)}));
    return true;
  }
  case rbc::Op::Lneg:
    setReg(pc, ins.dst,
           g_.make(ir::NodeKind::NegL, {defReg(pc, ins.a)}));
    return true;

  // --- float / double arithmetic ---------------------------------------------------
  case rbc::Op::Fadd: case rbc::Op::Fsub: case rbc::Op::Fmul:
  case rbc::Op::Fdiv: case rbc::Op::Frem: {
    static constexpr ir::NodeKind kinds[] = {
        ir::NodeKind::AddF, ir::NodeKind::SubF, ir::NodeKind::MulF,
        ir::NodeKind::DivF, ir::NodeKind::RemF};
    const std::uint32_t base = static_cast<std::uint32_t>(rbc::Op::Fadd);
    setReg(pc, ins.dst,
           g_.make(kinds[static_cast<std::uint32_t>(op) - base],
                   {defReg(pc, ins.a), defReg(pc, ins.b)}));
    return true;
  }
  case rbc::Op::Fneg:
    setReg(pc, ins.dst,
           g_.make(ir::NodeKind::NegF, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::Dadd: case rbc::Op::Dsub: case rbc::Op::Dmul:
  case rbc::Op::Ddiv: case rbc::Op::Drem: {
    static constexpr ir::NodeKind kinds[] = {
        ir::NodeKind::AddD, ir::NodeKind::SubD, ir::NodeKind::MulD,
        ir::NodeKind::DivD, ir::NodeKind::RemD};
    const std::uint32_t base = static_cast<std::uint32_t>(rbc::Op::Dadd);
    setReg(pc, ins.dst,
           g_.make(kinds[static_cast<std::uint32_t>(op) - base],
                   {defReg(pc, ins.a), defReg(pc, ins.b)}));
    return true;
  }
  case rbc::Op::Dneg:
    setReg(pc, ins.dst,
           g_.make(ir::NodeKind::NegD, {defReg(pc, ins.a)}));
    return true;

  // --- comparisons -------------------------------------------------------------
  case rbc::Op::Icmp:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::CmpI,
                                {defReg(pc, ins.a), defReg(pc, ins.b)}));
    return true;
  case rbc::Op::Lcmp:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::CmpL,
                                {defReg(pc, ins.a), defReg(pc, ins.b)}));
    return true;
  case rbc::Op::Fcmpl:
  case rbc::Op::Fcmpg: {
    const ir::NodeKind k = op == rbc::Op::Fcmpl ? ir::NodeKind::CmpFl
                                                 : ir::NodeKind::CmpFg;
    setReg(pc, ins.dst,
           g_.make(k, {defReg(pc, ins.a), defReg(pc, ins.b)}));
    return true;
  }
  case rbc::Op::Dcmpl:
  case rbc::Op::Dcmpg: {
    const ir::NodeKind k = op == rbc::Op::Dcmpl ? ir::NodeKind::CmpDl
                                                 : ir::NodeKind::CmpDg;
    setReg(pc, ins.dst,
           g_.make(k, {defReg(pc, ins.a), defReg(pc, ins.b)}));
    return true;
  }

  // --- conversions (Rule 33: explicit only) ----------------------------------------
  case rbc::Op::I2l:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::I2L, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::I2f:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::I2F, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::I2d:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::I2D, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::L2i:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::L2I, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::L2f:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::L2F, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::L2d:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::L2D, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::F2i:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::F2I, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::F2l:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::F2L, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::F2d:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::F2D, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::D2i:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::D2I, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::D2l:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::D2L, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::D2f:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::D2F, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::I2b:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::I2B, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::I2c:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::I2C, {defReg(pc, ins.a)}));
    return true;
  case rbc::Op::I2s:
    setReg(pc, ins.dst, g_.make(ir::NodeKind::I2S, {defReg(pc, ins.a)}));
    return true;

  // --- fields -------------------------------------------------------------------------
  case rbc::Op::Getfield: {
    const rbc::Const* c = cpAt(pc, ins.imm, rbc::Const::Kind::FieldRef);
    if (c == nullptr) {
      return false;
    }
    const ir::FieldId f = res_.fieldId(c->str, c->str2, c->str3);
    const ir::NodeId obj = defReg(pc, ins.a);
    nullGuard(pc, obj);
    const ir::NodeId load = g_.make(ir::NodeKind::LoadField,
                                    {curCtrl_, curMem_, obj}, f,
                                    static_cast<std::uint32_t>(
                                        fieldResultType(*c)));
    curCtrl_ = load;
    setReg(pc, ins.dst, load);
    return true;
  }
  case rbc::Op::Putfield: {
    const rbc::Const* c = cpAt(pc, ins.imm, rbc::Const::Kind::FieldRef);
    if (c == nullptr) {
      return false;
    }
    const ir::FieldId f = res_.fieldId(c->str, c->str2, c->str3);
    const ir::NodeId obj = defReg(pc, ins.a);
    nullGuard(pc, obj);
    const ir::NodeId store =
        g_.make(ir::NodeKind::StoreField,
                {curCtrl_, curMem_, obj, defReg(pc, ins.b)}, f);
    curCtrl_ = store;
    curMem_ = store;
    return true;
  }
  case rbc::Op::GetfieldQuick: {
    const rbc::RType pre = ins.dst < m_.numRegs
                               ? types_[static_cast<std::size_t>(pc) *
                                            nSlots() +
                                        regIdx(ins.dst)]
                               : rbc::RType::Bottom;
    if (pre == rbc::RType::Bottom) {
      fail(pc, "quickened field load dst not pre-typed (unverified "
               "stream?)");
      return false;
    }
    const ir::NodeId obj = defReg(pc, ins.a);
    nullGuard(pc, obj);
    const ir::NodeId load = g_.make(
        ir::NodeKind::LoadField, {curCtrl_, curMem_, obj}, ins.imm,
        static_cast<std::uint32_t>(rtypeToIr(pre)));
    curCtrl_ = load;
    setReg(pc, ins.dst, load);
    return true;
  }
  case rbc::Op::PutfieldQuick: {
    const ir::NodeId obj = defReg(pc, ins.a);
    nullGuard(pc, obj);
    const ir::NodeId store = g_.make(
        ir::NodeKind::StoreField,
        {curCtrl_, curMem_, obj, defReg(pc, ins.b)}, ins.imm);
    curCtrl_ = store;
    curMem_ = store;
    return true;
  }
  case rbc::Op::Getstatic: {
    const rbc::Const* c = cpAt(pc, ins.imm, rbc::Const::Kind::FieldRef);
    if (c == nullptr) {
      return false;
    }
    const ir::FieldId f = res_.fieldId(c->str, c->str2, c->str3);
    const ir::TypeId cls = res_.classId(c->str);
    curMem_ = g_.make(ir::NodeKind::ClassInit, {curCtrl_, curMem_}, cls);
    curCtrl_ = curMem_;
    const ir::NodeId load = g_.make(ir::NodeKind::LoadStatic,
                                    {curCtrl_, curMem_}, f,
                                    static_cast<std::uint32_t>(
                                        fieldResultType(*c)));
    curCtrl_ = load;
    setReg(pc, ins.dst, load);
    return true;
  }
  case rbc::Op::Putstatic: {
    const rbc::Const* c = cpAt(pc, ins.imm, rbc::Const::Kind::FieldRef);
    if (c == nullptr) {
      return false;
    }
    const ir::FieldId f = res_.fieldId(c->str, c->str2, c->str3);
    const ir::TypeId cls = res_.classId(c->str);
    curMem_ = g_.make(ir::NodeKind::ClassInit, {curCtrl_, curMem_}, cls);
    curCtrl_ = curMem_;
    const ir::NodeId store = g_.make(
        ir::NodeKind::StoreStatic, {curCtrl_, curMem_, defReg(pc, ins.dst)},
        f);
    curCtrl_ = store;
    curMem_ = store;
    return true;
  }

  // --- arrays --------------------------------------------------------------------------
  case rbc::Op::NewArray: {
    const ir::NodeId len = defReg(pc, ins.a);
    const ir::NodeId arr = g_.make(
        ir::NodeKind::NewArray, {curCtrl_, len},
        static_cast<std::uint32_t>(elemIrOfAtype(
            static_cast<rbc::Atype>(ins.imm))));
    curCtrl_ = arr;
    setReg(pc, ins.dst, arr);
    return true;
  }
  case rbc::Op::AnewArray: {
    const rbc::Const* c = cpAt(pc, ins.imm, rbc::Const::Kind::Class);
    if (c == nullptr) {
      return false;
    }
    const ir::TypeId cls = res_.classId(c->str);
    const ir::NodeId len = defReg(pc, ins.a);
    const ir::NodeId arr =
        g_.make(ir::NodeKind::NewRefArray, {curCtrl_, len}, cls);
    curCtrl_ = arr;
    setReg(pc, ins.dst, arr);
    return true;
  }
  case rbc::Op::Arraylength: {
    const ir::NodeId arr = defReg(pc, ins.a);
    nullGuard(pc, arr);
    setReg(pc, ins.dst, g_.make(ir::NodeKind::ArrayLength, {arr}));
    return true;
  }
  case rbc::Op::Iaload: case rbc::Op::Laload: case rbc::Op::Faload:
  case rbc::Op::Daload: case rbc::Op::Aaload: case rbc::Op::Baload:
  case rbc::Op::Caload: case rbc::Op::Saload: {
    ir::IRType elem = ir::IRType::Int;
    switch (op) {
    case rbc::Op::Laload:
      elem = ir::IRType::Long;
      break;
    case rbc::Op::Faload:
      elem = ir::IRType::Float;
      break;
    case rbc::Op::Daload:
      elem = ir::IRType::Double;
      break;
    case rbc::Op::Aaload:
      elem = ir::IRType::Ref;
      break;
    default:
      break; // b/c/s/i loads surface Int (sub-integral folding)
    }
    const ir::NodeId arr = defReg(pc, ins.a);
    const ir::NodeId idx = defReg(pc, ins.b);
    nullGuard(pc, arr);
    boundsGuard(pc, arr, idx);
    const ir::NodeId load = g_.make(
        ir::NodeKind::LoadElem, {curCtrl_, curMem_, arr, idx},
        static_cast<std::uint32_t>(elem));
    curCtrl_ = load;
    setReg(pc, ins.dst, load);
    return true;
  }
  case rbc::Op::Iastore: case rbc::Op::Lastore: case rbc::Op::Fastore:
  case rbc::Op::Dastore: case rbc::Op::Aastore: case rbc::Op::Bastore:
  case rbc::Op::Castore: case rbc::Op::Sastore: {
    ir::IRType elem = ir::IRType::Int;
    switch (op) {
    case rbc::Op::Lastore:
      elem = ir::IRType::Long;
      break;
    case rbc::Op::Fastore:
      elem = ir::IRType::Float;
      break;
    case rbc::Op::Dastore:
      elem = ir::IRType::Double;
      break;
    case rbc::Op::Aastore:
      elem = ir::IRType::Ref;
      break;
    default:
      break; // b/c/s/i stores take Int; narrowing is store-side memory
             // semantics (rbc_spec.md SS3.13), not a value conversion
    }
    const ir::NodeId arr = defReg(pc, ins.a);
    const ir::NodeId idx = defReg(pc, ins.b);
    const ir::NodeId val = defReg(pc, ins.dst); // dst = the stored value
    nullGuard(pc, arr);
    boundsGuard(pc, arr, idx);
    const ir::NodeId store = g_.make(
        ir::NodeKind::StoreElem, {curCtrl_, curMem_, arr, idx, val},
        static_cast<std::uint32_t>(elem));
    curCtrl_ = store;
    curMem_ = store;
    return true;
  }

  // --- objects / type ops --------------------------------------------------------------
  case rbc::Op::New: {
    const rbc::Const* c = cpAt(pc, ins.imm, rbc::Const::Kind::Class);
    if (c == nullptr) {
      return false;
    }
    const ir::TypeId cls = res_.classId(c->str);
    curMem_ = g_.make(ir::NodeKind::ClassInit, {curCtrl_, curMem_}, cls);
    curCtrl_ = curMem_;
    const ir::NodeId obj = g_.make(ir::NodeKind::New, {curCtrl_}, cls);
    curCtrl_ = obj;
    setReg(pc, ins.dst, obj);
    return true;
  }
  case rbc::Op::Checkcast: {
    const rbc::Const* c = cpAt(pc, ins.imm, rbc::Const::Kind::Class);
    if (c == nullptr) {
      return false;
    }
    const ir::TypeId cls = res_.classId(c->str);
    const ir::NodeId cast =
        g_.make(ir::NodeKind::CheckCast, {curCtrl_, defReg(pc, ins.a)}, cls);
    curCtrl_ = cast;
    setReg(pc, ins.dst, cast);
    return true;
  }
  case rbc::Op::Instanceof: {
    const rbc::Const* c = cpAt(pc, ins.imm, rbc::Const::Kind::Class);
    if (c == nullptr) {
      return false;
    }
    const ir::TypeId cls = res_.classId(c->str);
    setReg(pc, ins.dst,
           g_.make(ir::NodeKind::InstanceOf, {defReg(pc, ins.a)}, cls));
    return true;
  }
  case rbc::Op::Monitorenter: {
    const ir::NodeId obj = defReg(pc, ins.a);
    nullGuard(pc, obj);
    curMem_ = g_.make(ir::NodeKind::MonitorEnter, {curCtrl_, curMem_, obj});
    curCtrl_ = curMem_;
    return true;
  }
  case rbc::Op::Monitorexit: {
    const ir::NodeId obj = defReg(pc, ins.a);
    nullGuard(pc, obj);
    curMem_ = g_.make(ir::NodeKind::MonitorExit, {curCtrl_, curMem_, obj});
    curCtrl_ = curMem_;
    return true;
  }

  // --- calls ----------------------------------------------------------------------
  case rbc::Op::Invokevirtual:
  case rbc::Op::Invokespecial:
  case rbc::Op::Invokestatic:
  case rbc::Op::Invokeinterface:
  case rbc::Op::InvokevirtualQuick:
  case rbc::Op::InvokespecialQuick:
  case rbc::Op::InvokestaticQuick:
  case rbc::Op::InvokeinterfaceQuick:
    return lowerCall(pc, ins);

  // --- deopt / tiering hooks ----------------------------------------------------------
  case rbc::Op::DeoptTrap: {
    g_.make(ir::NodeKind::Deopt, {curCtrl_, fsAt(pc)}, ins.imm);
    return true;
  }
  case rbc::Op::GuardNonNull: {
    const ir::NodeId isnull =
        g_.make(ir::NodeKind::IsNull, {defReg(pc, ins.a)});
    const ir::NodeId notnull = g_.make(ir::NodeKind::Not, {isnull});
    curCtrl_ = g_.make(
        ir::NodeKind::Guard, {curCtrl_, notnull, fsAt(pc)},
        static_cast<std::uint32_t>(ir::GuardKind::NullCheck), ins.imm);
    return true;
  }

  // --- branches / returns / athrow / switches: terminators --------------------
  case rbc::Op::Goto:
  case rbc::Op::Ifeq: case rbc::Op::Ifne: case rbc::Op::Iflt:
  case rbc::Op::Ifge: case rbc::Op::Ifgt: case rbc::Op::Ifle:
  case rbc::Op::Ifnull: case rbc::Op::Ifnonnull:
  case rbc::Op::IfIcmpeq: case rbc::Op::IfIcmpne: case rbc::Op::IfIcmplt:
  case rbc::Op::IfIcmpge: case rbc::Op::IfIcmpgt: case rbc::Op::IfIcmple:
  case rbc::Op::IfAcmpeq: case rbc::Op::IfAcmpne:
  case rbc::Op::Tableswitch:
  case rbc::Op::Lookupswitch:
  case rbc::Op::Return: case rbc::Op::Ireturn: case rbc::Op::Lreturn:
  case rbc::Op::Freturn: case rbc::Op::Dreturn: case rbc::Op::Areturn:
  case rbc::Op::Athrow:
    return lowerTerminator(pc, ins);

  default:
    // Multianewarray / Invokedynamic / GuardClass already refused in the
    // structural scan; anything else is a gap to close deliberately.
    fail(pc, "opcode " + std::string(rbc::opName(op)) +
                 " is not lowerable in builder v1");
    return false;
  }
}

bool Builder::lowerTerminator(std::uint32_t pc, const rbc::Ins& ins) {
  const rbc::Op op = ins.opcode();
  // IfTrue is ALWAYS the taken edge: conditions are composed so truthiness
  // means "branch to imm".
  if (op == rbc::Op::Goto) {
    addEdge(pc, ins.imm, curCtrl_);
    return true;
  }
  if (op == rbc::Op::Ifeq || op == rbc::Op::Ifne || op == rbc::Op::Iflt ||
      op == rbc::Op::Ifge || op == rbc::Op::Ifgt || op == rbc::Op::Ifle) {
    ir::NodeKind test = ir::NodeKind::EqI;
    switch (op) {
    case rbc::Op::Ifeq:
      test = ir::NodeKind::EqI;
      break;
    case rbc::Op::Ifne:
      test = ir::NodeKind::NeI;
      break;
    case rbc::Op::Iflt:
      test = ir::NodeKind::LtI;
      break;
    case rbc::Op::Ifge:
      test = ir::NodeKind::GeI;
      break;
    case rbc::Op::Ifgt:
      test = ir::NodeKind::GtI;
      break;
    case rbc::Op::Ifle:
      test = ir::NodeKind::LeI;
      break;
    default:
      break;
    }
    const ir::NodeId cond = g_.make(test, {defReg(pc, ins.a), constInt(0)});
    branchOut(pc, cond, ins.imm);
    return true;
  }
  if (op == rbc::Op::Ifnull || op == rbc::Op::Ifnonnull) {
    const ir::NodeId isnull = g_.make(ir::NodeKind::IsNull,
                                      {defReg(pc, ins.a)});
    ir::NodeId cond = isnull;
    if (op == rbc::Op::Ifnonnull) {
      cond = g_.make(ir::NodeKind::Not, {isnull});
    }
    branchOut(pc, cond, ins.imm);
    return true;
  }
  if (op == rbc::Op::IfIcmpeq || op == rbc::Op::IfIcmpne ||
      op == rbc::Op::IfIcmplt || op == rbc::Op::IfIcmpge ||
      op == rbc::Op::IfIcmpgt || op == rbc::Op::IfIcmple) {
    ir::NodeKind test = ir::NodeKind::EqI;
    switch (op) {
    case rbc::Op::IfIcmpeq:
      test = ir::NodeKind::EqI;
      break;
    case rbc::Op::IfIcmpne:
      test = ir::NodeKind::NeI;
      break;
    case rbc::Op::IfIcmplt:
      test = ir::NodeKind::LtI;
      break;
    case rbc::Op::IfIcmpge:
      test = ir::NodeKind::GeI;
      break;
    case rbc::Op::IfIcmpgt:
      test = ir::NodeKind::GtI;
      break;
    case rbc::Op::IfIcmple:
      test = ir::NodeKind::LeI;
      break;
    default:
      break;
    }
    const ir::NodeId cond = g_.make(
        test, {defReg(pc, ins.a), defReg(pc, ins.b)});
    branchOut(pc, cond, ins.imm);
    return true;
  }
  if (op == rbc::Op::IfAcmpeq || op == rbc::Op::IfAcmpne) {
    const ir::NodeId eq = g_.make(ir::NodeKind::RefEq,
                                  {defReg(pc, ins.a), defReg(pc, ins.b)});
    ir::NodeId cond = eq;
    if (op == rbc::Op::IfAcmpne) {
      cond = g_.make(ir::NodeKind::Not, {eq}); // unequal = NOT identical
    }
    branchOut(pc, cond, ins.imm);
    return true;
  }
  if (op == rbc::Op::Tableswitch || op == rbc::Op::Lookupswitch) {
    std::vector<std::uint32_t> targets;
    std::uint32_t def = 0;
    if (!switchTargets(pc, ins, targets, def)) {
      return false;
    }
    const rbc::Const& c = m_.cp[ins.imm];
    const std::uint32_t tableId = res_.switchTableId(c.ints);
    const ir::NodeId sw = g_.make(ir::NodeKind::Switch,
                                  {curCtrl_, defReg(pc, ins.a)}, tableId);
    for (std::size_t i = 0; i < targets.size(); ++i) {
      const ir::NodeId caseP = g_.make(
          ir::NodeKind::SwitchCase, {sw}, static_cast<std::uint32_t>(i));
      addEdge(pc, targets[i], caseP);
    }
    const ir::NodeId defP = g_.make(ir::NodeKind::SwitchDefault, {sw});
    addEdge(pc, def, defP);
    return true;
  }
  if (isReturnOp(op)) {
    if (iw_ != nullptr) {
      // Inline exit: record the state flowing back to the call site; the
      // driver merges the exits into the caller's control/memory/value
      // (no Return terminal is created).
      detail::InlineExit e;
      e.ctrl = curCtrl_;
      e.mem = curMem_;
      e.value =
          op == rbc::Op::Return ? ir::kInvalidNodeId : defReg(pc, ins.a);
      body_->exits.push_back(e);
      return true;
    }
    if (op == rbc::Op::Return) {
      g_.make(ir::NodeKind::Return, {curCtrl_});
    } else {
      g_.make(ir::NodeKind::Return, {curCtrl_, defReg(pc, ins.a)});
    }
    return true;
  }
  if (op == rbc::Op::Athrow) {
    // Inline mode: an athrow ESCAPING the callee routes through the call
    // site's policy - a covered site takes the deopt below (T0 re-executes
    // the athrow in the reconstructed callee frame, finds no handler
    // there, unwinds it, and the deopt runtime re-enters the caller at
    // the call pc with the pending exception - the FrameState caller
    // chain carries the stack, docs/inlining.md section 4); an uncovered
    // site keeps the Unwind (the callee's exception value propagates out
    // of the caller directly).
    if (coveredByHandler(pc) || (iw_ != nullptr && iw_->siteCovered)) {
      // Caught athrow: deopt; T0 re-executes the athrow (idempotent: the
      // exception value rides in the FrameState's rA slot) and dispatches
      // through THE EXCEPTION ALGORITHM with full type info.
      g_.make(ir::NodeKind::Deopt, {curCtrl_, fsAt(pc)}, nextDeoptId());
    } else {
      // Uncaught: null would make this an NPE-at-this-pc instead of the
      // thrown value, so guard first (deopt re-executes; T0 raises the
      // NPE and unwinds it), then unwind with the value.
      nullGuard(pc, defReg(pc, ins.a));
      const ir::NodeId ex = defReg(pc, ins.a);
      g_.make(ir::NodeKind::Unwind, {curCtrl_, ex});
    }
    return true;
  }
  fail(pc, "unhandled terminator");
  return false;
}

void Builder::branchOut(std::uint32_t pc, ir::NodeId cond,
                        std::uint32_t target) {
  const ir::NodeId iff = g_.make(ir::NodeKind::If, {curCtrl_, cond});
  const ir::NodeId t = g_.make(ir::NodeKind::IfTrue, {iff});
  const ir::NodeId f = g_.make(ir::NodeKind::IfFalse, {iff});
  addEdge(pc, target, t);
  const std::uint32_t n = static_cast<std::uint32_t>(m_.code.size());
  if (pc + 1 < n) {
    addEdge(pc, pc + 1, f);
  } else {
    fail(pc, "conditional branch falls off the end of code");
  }
}

void Builder::materializeEntry(std::uint32_t b) {
  // Incoming forward edges (from materialized blocks; order = block index,
  // then edge creation order - deterministic). Block 0 additionally has the
  // implicit entry edge (Start), which becomes its only forward edge: any
  // real edge into block 0 is a backedge by dominance.
  struct InEdge {
    ir::NodeId ctrl;
    ir::NodeId mem;
    std::vector<ir::NodeId> slots;
  };
  std::vector<InEdge> in;
  for (std::uint32_t p = 0; p < nb_; ++p) {
    if (!reachable_[p] || !exit_[p].materialized) {
      continue;
    }
    for (const EdgeExit& e : exit_[p].edges) {
      if (e.target == b) {
        InEdge ie;
        ie.ctrl = e.ctrl;
        ie.mem = exit_[p].mem;
        ie.slots = exit_[p].slots;
        in.push_back(std::move(ie));
      }
    }
  }
  if (b == 0) {
    // The implicit entry edge: control and memory from Start; parameters
    // typed by the descriptor; every other slot Undef. In inline mode the
    // entry state is the call site's: control/memory predecessors and the
    // caller's argument defs (receiver first) as the parameter locals.
    std::vector<ir::NodeId> entrySlots(nSlots(), ir::kInvalidNodeId);
    if (iw_ != nullptr) {
      const std::size_t take =
          iw_->args.size() < m_.numLocals ? iw_->args.size()
                                          : static_cast<std::size_t>(m_.numLocals);
      for (std::size_t i = 0; i < take; ++i) {
        entrySlots[localIdx(static_cast<std::uint32_t>(i))] = iw_->args[i];
      }
      for (std::uint32_t s = 0; s < nSlots(); ++s) {
        if (entrySlots[s] == ir::kInvalidNodeId) {
          entrySlots[s] = undef();
        }
      }
    } else {
      std::vector<rbc::RType> params;
      (void)rbc::parseParams(m_.descriptor, params);
      std::uint32_t next = 0;
      if (!m_.isStatic()) {
        entrySlots[localIdx(0)] = g_.parameter(0, ir::IRType::Ref);
        next = 1;
      }
      for (std::size_t i = 0; i < params.size() && next < m_.numLocals; ++i) {
        entrySlots[localIdx(next)] =
            g_.parameter(next, rtypeToIr(params[i]));
        ++next;
      }
      for (std::uint32_t s = 0; s < m_.numLocals; ++s) {
        if (entrySlots[localIdx(s)] == ir::kInvalidNodeId) {
          entrySlots[localIdx(s)] = undef();
        }
      }
      for (std::uint32_t r = 0; r < m_.numRegs; ++r) {
        entrySlots[regIdx(r)] = undef();
      }
    }
    const ir::NodeId entryCtrl0 =
        iw_ != nullptr ? iw_->entryCtrl : g_.startNode();
    const ir::NodeId entryMem0 =
        iw_ != nullptr ? iw_->entryMem : g_.startNode();
    if (backPreds_[0].empty()) {
      // Plain entry: no merge, no phis.
      curCtrl_ = entryCtrl0;
      curMem_ = entryMem0;
      slots_ = std::move(entrySlots);
      return;
    }
    // Block 0 is also a loop header (a backward goto to pc 0).
    InEdge ie;
    ie.ctrl = entryCtrl0;
    ie.mem = entryMem0;
    ie.slots = std::move(entrySlots);
    in.push_back(std::move(ie));
  }

  const bool isHeader = !backPreds_[b].empty();

  // Entry control: LoopBegin for headers (forward preds now; LoopEnds
  // appended in Phase E), Region for >=2 forward preds, the edge control
  // itself for exactly one.
  ir::NodeId entryCtrl = ir::kInvalidNodeId;
  if (isHeader) {
    std::vector<ir::NodeId> ctrls;
    for (const InEdge& ie : in) {
      ctrls.push_back(ie.ctrl);
    }
    if (ctrls.empty()) {
      ctrls.push_back(g_.startNode());
    }
    entryCtrl = g_.make(ir::NodeKind::LoopBegin, ctrls);
    loopBeginOf_[b] = entryCtrl;
  } else if (in.size() == 1) {
    entryCtrl = in[0].ctrl;
  } else {
    std::vector<ir::NodeId> ctrls;
    for (const InEdge& ie : in) {
      ctrls.push_back(ie.ctrl);
    }
    if (ctrls.empty()) {
      ctrls.push_back(g_.startNode());
    }
    entryCtrl = g_.make(ir::NodeKind::Region, ctrls);
  }
  curCtrl_ = entryCtrl;

  // Entry memory: a merge phi over the incoming states. Headers ALWAYS get
  // one (the backedge memory state is unknown until Phase E; an unchanged
  // memory around the loop patches the phi with itself - the loop-invariant
  // marker - and trivial-phi cleanup then folds it away).
  {
    bool same = true;
    for (std::size_t i = 1; i < in.size(); ++i) {
      if (in[i].mem != in[0].mem) {
        same = false;
        break;
      }
    }
    if (!isHeader && (in.size() <= 1 || same)) {
      curMem_ = in.empty() ? g_.startNode() : in[0].mem;
    } else {
      std::vector<ir::NodeId> mems;
      for (const InEdge& ie : in) {
        mems.push_back(ie.mem);
      }
      if (mems.empty()) {
        mems.push_back(g_.startNode());
      }
      std::vector<ir::NodeId> phiInputs{entryCtrl};
      for (const ir::NodeId m : mems) {
        phiInputs.push_back(m);
      }
      curMem_ = g_.make(ir::NodeKind::Phi,
                        std::span<const ir::NodeId>(phiInputs));
      if (isHeader) {
        memPhiOf_[b] = curMem_;
      }
    }
  }

  // Per-slot phis. Headers: slots modified anywhere in the loop (nested
  // bodies included - natural-loop bodies nest) get loop phis with pending
  // backedge inputs; unmodified slots whose forward defs differ still need
  // a phi, and their backedge input will be the phi itself (the value
  // flows around unchanged). Plain merges: a phi exactly when the incoming
  // defs differ (Undef inputs legal: the "Bottom on this path" marker).
  std::vector<char> modified(nSlots(), 0);
  if (isHeader) {
    const std::uint32_t li = loopOf_[b];
    if (li != 0xFFFFFFFFu) {
      for (const std::uint32_t blk : loops_[li].blocks) {
        for (std::uint32_t s = 0; s < nSlots(); ++s) {
          if (writes_[blk][s]) {
            modified[s] = 1;
          }
        }
      }
    }
  }
  for (std::uint32_t s = 0; s < nSlots(); ++s) {
    ir::NodeId def = in.empty() ? undef() : in[0].slots[s];
    bool same = true;
    for (std::size_t i = 1; i < in.size(); ++i) {
      if (in[i].slots[s] != def) {
        same = false;
        break;
      }
    }
    const bool needsPhi = isHeader ? (modified[s] || !same) : !same;
    if (!needsPhi) {
      slots_[s] = same ? def : def; // all agree (or single edge)
    } else {
      std::vector<ir::NodeId> phiInputs{entryCtrl};
      for (const InEdge& ie : in) {
        phiInputs.push_back(ie.slots[s]);
      }
      if (phiInputs.size() == 1) {
        phiInputs.push_back(undef());
      }
      slots_[s] = g_.make(ir::NodeKind::Phi,
                          std::span<const ir::NodeId>(phiInputs));
      if (isHeader) {
        slotPhiOf_[b][s] = slots_[s];
      }
    }
  }
}

bool Builder::materializeBlock(std::uint32_t b) {
  curBlock_ = b;
  materializeEntry(b);
  const std::uint32_t bEnd =
      (b + 1 < nb_) ? starts_[b + 1]
                    : static_cast<std::uint32_t>(m_.code.size());
  for (std::uint32_t pc = starts_[b]; pc < bEnd && ok(); ++pc) {
    tick();
    if (!lowerInstruction(pc, m_.code[pc])) {
      return false;
    }
  }
  // Fall-through block end (no terminator at the last pc): single edge.
  const rbc::Ins& lastIns = m_.code[bEnd - 1];
  const rbc::OpInfo& oi = rbc::info(lastIns.opcode());
  const bool terminator = isBranchSig(oi.sig) || isReturnOp(lastIns.opcode()) ||
                          lastIns.opcode() == rbc::Op::Athrow ||
                          lastIns.opcode() == rbc::Op::DeoptTrap;
  if (!terminator && bEnd < m_.code.size()) {
    addEdge(bEnd - 1, bEnd, curCtrl_);
  }
  exit_[b].mem = curMem_;
  exit_[b].slots = slots_;
  exit_[b].materialized = true;
  return ok();
}

bool Builder::materialize() {
  exit_.assign(nb_, BlockExit{});
  loopBeginOf_.assign(nb_, ir::kInvalidNodeId);
  memPhiOf_.assign(nb_, ir::kInvalidNodeId);
  slotPhiOf_.assign(nb_,
                    std::vector<ir::NodeId>(nSlots(), ir::kInvalidNodeId));
  for (const std::uint32_t b : order_) {
    if (!materializeBlock(b)) {
      return false;
    }
  }
  return ok();
}

// --- Phase E: backedge patches + trivial-phi cleanup --------------------------

void Builder::patchBackedges() {
  // LoopBegins and header phis were created with forward inputs only; each
  // patch appends the backedge control to the LoopBegin and the backedge
  // definitions to every pending header phi (slot phis AND the memory
  // phi), preserving phi-arity == 1 + region-predecessors. An unmodified
  // slot's backedge definition IS its phi (the loop-invariant marker), so
  // the append lands as a self-input - exactly the Graal loop-phi shape.
  for (const BackedgePatch& patch : patches_) {
    g_.appendInput(loopBeginOf_[patch.header], patch.loopEnd);
    if (memPhiOf_[patch.header] != ir::kInvalidNodeId) {
      g_.appendInput(memPhiOf_[patch.header], patch.mem);
    }
    std::vector<ir::NodeId>& phis = slotPhiOf_[patch.header];
    for (std::uint32_t s = 0; s < phis.size(); ++s) {
      if (phis[s] != ir::kInvalidNodeId) {
        g_.appendInput(phis[s], patch.slots[s]);
      }
    }
  }
}

void Builder::removeTrivialPhis() {
  // A phi whose non-self value inputs are all one node X is X (possibly
  // after other phis collapse - iterate to a fixpoint, bounded by the phi
  // count). replaceNode rewires FrameStates and users (Rule 14).
  bool changed = true;
  std::uint32_t rounds = 0;
  const std::uint32_t maxRounds = g_.nodeCount() + 2;
  while (changed && rounds++ < maxRounds) {
    changed = false;
    for (ir::NodeId n = 0; n < g_.nodeCount(); ++n) {
      const ir::Node& nd = g_.node(n);
      if (nd.isDead() || nd.kind != ir::NodeKind::Phi || nd.numInputs < 2) {
        continue;
      }
      ir::NodeId candidate = ir::kInvalidNodeId;
      bool trivial = true;
      for (std::uint16_t s = 1; s < nd.numInputs && trivial; ++s) {
        const ir::NodeId in = g_.input(n, s);
        if (in == n) {
          continue; // self input: loop-invariant marker
        }
        if (candidate == ir::kInvalidNodeId) {
          candidate = in;
        } else if (in != candidate) {
          trivial = false;
        }
      }
      if (trivial && candidate != ir::kInvalidNodeId &&
          !g_.node(candidate).isDead()) {
        g_.replaceNode(n, candidate);
        changed = true;
      }
    }
  }
}

} // namespace

// --- CounterResolver ---------------------------------------------------------

ir::TypeId CounterResolver::classId(std::string_view internalName) {
  return intern(internalName);
}

ir::FieldId CounterResolver::fieldId(std::string_view cls,
                                     std::string_view name,
                                     std::string_view descriptor) {
  return intern(std::string(cls) + "\x01" + std::string(name) + "\x01" +
                std::string(descriptor));
}

ir::MethodId CounterResolver::methodId(std::string_view cls,
                                       std::string_view name,
                                       std::string_view descriptor) {
  return intern(std::string(cls) + "\x02" + std::string(name) + "\x02" +
                std::string(descriptor));
}

ir::SymbolId CounterResolver::symbolId(std::string_view payload) {
  return intern(payload);
}

std::uint32_t
CounterResolver::switchTableId(const std::vector<std::int32_t>& payload) {
  std::string key;
  key.reserve(payload.size() * 4);
  for (const std::int32_t v : payload) {
    key.append(reinterpret_cast<const char*>(&v), sizeof(v));
  }
  return intern(key);
}

std::uint32_t CounterResolver::intern(std::string_view key) {
  // Deterministic first-encounter interning: linear scan is fine for
  // methods (small pools) and keeps ids == encounter order.
  for (std::size_t i = 0; i < keys_.size(); ++i) {
    if (keys_[i] == key) {
      return static_cast<std::uint32_t>(i + 1);
    }
  }
  keys_.emplace_back(key);
  return static_cast<std::uint32_t>(keys_.size());
}

// --- entry --------------------------------------------------------------------

BuildResult buildGraph(const rbc::Method& m, SymbolResolver& res,
                       ir::Graph& g, ir::MethodId methodId) {
  BuildResult out;
  Builder builder(m, res, g, methodId, out);
  return builder.run();
}

// --- inline body build (the inliner's trusted seam; see PassInternal.h) ----

detail::InlineBodyResult
detail::buildInlineBody(const rbc::Method& m, SymbolResolver& res,
                        ir::Graph& g, ir::MethodId frameMethodId,
                        const detail::InlineSiteWiring& w) {
  BuildResult out;
  detail::InlineBodyResult body;
  Builder builder(m, res, g, frameMethodId, out, &w, &body);
  (void)builder.run();
  const ir::NodeId firstNew =
      body.nodesAdded <= g.nodeCount() ? g.nodeCount() - body.nodesAdded
                                       : g.nodeCount();
  for (ir::NodeId n = firstNew; n < g.nodeCount(); ++n) {
    if (!g.node(n).isDead() && g.node(n).kind == ir::NodeKind::Deopt) {
      ++body.deoptsEmitted;
    }
  }
  return body;
}

} // namespace b2::passes

