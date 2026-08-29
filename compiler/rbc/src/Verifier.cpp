// B-2 RBC - structural and type verifier (Task ME-C).
//
// WHY THIS FILE (see include/b2/rbc/Verifier.h): T1 composes machine-code
// stencils directly from RBC with no IR safety net, and T0 must never see a
// malformed instruction, so verify() is the last line of defense before any
// tier executes a method (docs/laws.md Part VIII/VII, docs/deopt_backend.md:
// "verifier before quickener before execution"). The verifier is deliberately
// TOTAL: arbitrary garbage input produces a bounded diagnostic list, never a
// crash, never recursion (there is none in this file), and never unbounded
// work: the dataflow runs on a finite lattice with a monotone descent
// argument plus a hard elementary-operation budget that fails CLOSED (the
// method is rejected with a diagnostic, never accepted unsoundly).
//
// PHASES (normative):
//   0. guards: empty-body legality (abstract vs concrete), u16 register-file
//      sanity, code size that fits pc/branch u32 operands.
//   1. structural (single pass over code[]): opcode validity, register
//      ranges per Sig, call argument span, local slot ranges, cp index range
//      and per-op kind, branch targets, switch-table payload shape,
//      exception-handler table sanity.
//   2. type checking: abstract interpretation over basic blocks (iterative
//      worklist, no recursion), per-instruction transfer driven by info(op),
//      locals typed by what is stored, handler-entry states, protected-range
//      local-type stability, return-type agreement, fall-off-the-end.
//   3. result: ok = zero diagnostics (capped at 100, frontend parity).
//
// Diagnostic pc convention: instruction index for per-instruction issues,
// pc = 0 for method-level issues (empty body, descriptor, budgets).
//
// Emission order (deterministic): phase 0 guards, phase 1 per-pc in pc order
// (opcode, registers, call span, slot, atype, cp, branch, switch payload),
// then exception handlers, then phase 2 (fall-off-end, descriptor/param
// gates, per-pc in pc order over the final dataflow states, then
// protected-range stability). Phase 2 diagnostics are produced by a single
// deterministic final sweep over the fixpoint states, so each pc is judged
// exactly once and no diagnostic is duplicated by worklist revisits.

#include "b2/rbc/Verifier.h"

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace b2::rbc {
namespace {

using K = Const::Kind;

// --- tunables --------------------------------------------------------------
constexpr std::size_t kMaxDiags = 100; // diagnostic cap (frontend DiagnosticEngine parity)
constexpr std::uint32_t kMaxU16 = 65535; // Ins.dst/a/b are u16: register files can never need more
constexpr std::uint64_t kOpBudget = 1ull << 30; // elementary ops before fail-closed abort
constexpr std::size_t kMaxStateSlots = std::size_t{1} << 28; // total flow-state slots (~256 MiB)
constexpr std::size_t kMaxBreakpoints = std::size_t{1} << 23; // protected-range change points

// --- switch-table canonical payload layouts (NORMATIVE) --------------------
// WHY: this is the payload contract shared with the RbcText printer/parser
// and RbcBuilder agents; the verifier enforces it so T1's switch stencils can
// index the table blind. cp[imm] must be Kind::SwitchTable and:
//   Tableswitch: ints = [low, high, default, t(low), ..., t(high)]
//     - ints.size() == 3 + (high - low + 1), low <= high
//     - selector s in [low, high] jumps to ints[3 + (s - low)];
//       any other selector jumps to ints[2] (default).
//   Lookupswitch: ints = [N, default, match0, t0, ..., match(N-1), t(N-1)]
//     - ints.size() == 2 + 2N, N >= 0
//     - match0 < match1 < ... < match(N-1) (strictly increasing)
//     - a matching selector jumps to its pair's target, else to the default.
// All targets (default included) are instruction indices < code.size().

// --- descriptor scanning ----------------------------------------------------
// Scans one FIELD type descriptor at the start of s ("I", "J", "F", "D",
// "Z", "B", "C", "S", "L<name>;", "[..."). boolean/byte/char/short fold into
// Int (RType lattice rule); "L..;" and arrays are Ref. "V" is NOT a field
// type. Returns ok=false on anything else.
struct FieldScan {
  bool ok = false;
  RType type = RType::Bottom;
  std::size_t used = 0;
};

FieldScan scanField(std::string_view s) noexcept {
  FieldScan r;
  if (s.empty()) {
    return r;
  }
  switch (s[0]) {
  case 'I': case 'Z': case 'B': case 'C': case 'S':
    r.ok = true;
    r.type = RType::Int;
    r.used = 1;
    return r;
  case 'J':
    r.ok = true;
    r.type = RType::Long;
    r.used = 1;
    return r;
  case 'F':
    r.ok = true;
    r.type = RType::Float;
    r.used = 1;
    return r;
  case 'D':
    r.ok = true;
    r.type = RType::Double;
    r.used = 1;
    return r;
  case 'L': {
    const std::size_t semi = s.find(';');
    if (semi == std::string_view::npos || semi < 2) { // "L" or "L;": empty class name
      return r;
    }
    r.ok = true;
    r.type = RType::Ref;
    r.used = semi + 1;
    return r;
  }
  case '[': {
    std::size_t i = 1;
    while (i < s.size() && s[i] == '[') {
      ++i;
    }
    if (i >= s.size()) {
      return r;
    }
    if (s[i] == 'L') {
      const std::size_t semi = s.find(';', i);
      if (semi == std::string_view::npos || semi < i + 2) {
        return r;
      }
      r.ok = true;
      r.type = RType::Ref;
      r.used = semi + 1;
      return r;
    }
    switch (s[i]) {
    case 'I': case 'J': case 'F': case 'D':
    case 'Z': case 'B': case 'C': case 'S':
      r.ok = true;
      r.type = RType::Ref;
      r.used = i + 1;
      return r;
    default:
      return r;
    }
  }
  default:
    return r;
  }
}

// Scans a full METHOD descriptor "(params)return". The return part is "V"
// (void) or exactly one field type that consumes the rest of the string.
// WHY a local scanner instead of parseReturn(): parseReturn returns RType
// and Bottom on malformed input, but a well-formed void descriptor has no
// RType mapping, so parseReturn("()V") == Bottom is indistinguishable from
// garbage like "()X". This verifier needs the void/type/malformed trichotomy
// (Return requires 'V', invoke dst is written only for non-void), so it
// scans the return part itself. parseParams() is still used as the authority
// for entry local types (shared contract with lowering).
struct RetScan {
  bool ok = false;
  bool isVoid = false;
  RType type = RType::Bottom;
};

RetScan scanMethodDesc(std::string_view d) noexcept {
  RetScan r;
  if (d.empty() || d[0] != '(') {
    return r;
  }
  std::size_t i = 1;
  while (i < d.size() && d[i] != ')') {
    const FieldScan fs = scanField(d.substr(i));
    if (!fs.ok) {
      return r;
    }
    i += fs.used;
  }
  if (i >= d.size() || d[i] != ')') {
    return r;
  }
  ++i;
  if (i == d.size()) {
    return r; // missing return type
  }
  if (d[i] == 'V' && i + 1 == d.size()) {
    r.ok = true;
    r.isVoid = true;
    return r;
  }
  const FieldScan fs = scanField(d.substr(i));
  if (!fs.ok || fs.used != d.size() - i) {
    return r; // trailing garbage after the return type
  }
  r.ok = true;
  r.type = fs.type;
  return r;
}

const char* retName(const RetScan& rs) noexcept {
  return rs.isVoid ? "void" : typeName(rs.type);
}

// --- Sig-driven operand layout ----------------------------------------------
// Which Ins fields are REGISTERS for each Sig (per include/b2/rbc/Opcode.h):
// b is NOT a register for Call/CallQuick (arg count) or GuardCp (deopt id).
struct RegUse {
  bool dst = false;
  bool a = false;
  bool b = false;
};

constexpr RegUse regUse(Sig s) noexcept {
  switch (s) {
  case Sig::None:
    return {};
  case Sig::Reg:
    return {false, true, false};
  case Sig::RegImm:
    return {true, false, false};
  case Sig::RegCp:
    return {true, false, false};
  case Sig::RegSlot:
    return {true, false, false};
  case Sig::SlotReg:
    return {false, true, false};
  case Sig::RegReg:
    return {true, true, false};
  case Sig::RegRegReg:
    return {true, true, true};
  case Sig::RegRegImm:
    return {true, true, false};
  case Sig::RegRegCp:
    return {true, true, false};
  case Sig::RegRegRegCp:
    return {true, true, true};
  case Sig::Branch:
    return {};
  case Sig::RegBranch:
    return {false, true, false};
  case Sig::RegRegBranch:
    return {false, true, true};
  case Sig::RegCpBranch:
    return {false, true, false};
  case Sig::Call:
    return {true, true, false}; // b = arg count
  case Sig::CallQuick:
    return {true, true, false}; // b = arg count
  case Sig::Guard:
    return {false, true, false};
  case Sig::GuardCp:
    return {false, true, false}; // b = deopt id
  case Sig::Trap:
    return {};
  }
  return {};
}

constexpr bool sigUsesCpIndex(Sig s) noexcept {
  return s == Sig::RegCp || s == Sig::RegRegCp || s == Sig::RegRegRegCp ||
         s == Sig::Call || s == Sig::RegCpBranch;
}

constexpr bool isBranchSig(Sig s) noexcept {
  return s == Sig::Branch || s == Sig::RegBranch || s == Sig::RegRegBranch;
}

constexpr bool isReturnOp(Op op) noexcept {
  switch (op) {
  case Op::Return: case Op::Ireturn: case Op::Lreturn:
  case Op::Freturn: case Op::Dreturn: case Op::Areturn:
    return true;
  default:
    return false;
  }
}

// Ops that can NOT fall through to pc+1. Everything else (including the
// guard ops, which deopt out of the method on failure) falls through.
constexpr bool canFallThrough(Op op) noexcept {
  switch (op) {
  case Op::Goto: case Op::Tableswitch: case Op::Lookupswitch:
  case Op::Athrow: case Op::DeoptTrap:
  case Op::Return: case Op::Ireturn: case Op::Lreturn:
  case Op::Freturn: case Op::Dreturn: case Op::Areturn:
    return false;
  default:
    return true;
  }
}

// Which cp kind does the op need? (GetfieldQuick/PutfieldQuick and ops
// without kind constraints return true for everything.)
bool cpKindOk(Op op, K k) noexcept {
  switch (op) {
  case Op::Lconst:
    return k == K::Int64;
  case Op::Dconst:
    return k == K::Double;
  case Op::Ldc:
    return k == K::String || k == K::Class || k == K::MethodType ||
           k == K::MethodHandle;
  case Op::New: case Op::AnewArray: case Op::Checkcast:
  case Op::Instanceof: case Op::Multianewarray:
    return k == K::Class;
  case Op::Getfield: case Op::Putfield: case Op::Getstatic: case Op::Putstatic:
    return k == K::FieldRef;
  case Op::Invokevirtual: case Op::Invokespecial:
    return k == K::MethodRef;
  case Op::Invokeinterface: case Op::Invokestatic:
    return k == K::MethodRef || k == K::InterfaceMethodRef;
  case Op::Invokedynamic:
    return k == K::InvokeDynamic;
  case Op::Tableswitch: case Op::Lookupswitch:
    return k == K::SwitchTable;
  case Op::GuardClass:
    // Op comment: "if klass(a) != cp[imm]". Not part of the cp-Sig list, but
    // the opcode demonstrably dereferences the pool: check it (resolved
    // ambiguity; see report).
    return k == K::Class;
  default:
    return true; // quick field ops (raw offset) and kind-unconstrained ops
  }
}

const char* expectedCpName(Op op) noexcept {
  switch (op) {
  case Op::Lconst:
    return "long";
  case Op::Dconst:
    return "double";
  case Op::Ldc:
    return "string, class, methodtype or methodhandle";
  case Op::New: case Op::AnewArray: case Op::Checkcast:
  case Op::Instanceof: case Op::Multianewarray: case Op::GuardClass:
    return "class";
  case Op::Getfield: case Op::Putfield: case Op::Getstatic: case Op::Putstatic:
    return "field";
  case Op::Invokevirtual: case Op::Invokespecial:
    return "method";
  case Op::Invokeinterface: case Op::Invokestatic:
    return "method or interface method";
  case Op::Invokedynamic:
    return "invoke dynamic";
  case Op::Tableswitch: case Op::Lookupswitch:
    return "switch table";
  default:
    return "any";
  }
}

// --- diagnostics ------------------------------------------------------------
struct DiagSink {
  VerifyResult& r;

  explicit DiagSink(VerifyResult& res) noexcept : r(res) {}

  [[nodiscard]] bool full() const noexcept {
    return r.diags.size() >= kMaxDiags;
  }

  void add(std::uint32_t pc, std::string msg) {
    // WHY: hard cap like the frontend DiagnosticEngine - after 100 entries we
    // stop appending (and the caller bails out of the current phase), but the
    // result is already a rejection either way.
    if (r.diags.size() >= kMaxDiags) {
      return;
    }
    VerifyDiag d;
    d.pc = pc;
    d.message = std::move(msg);
    r.diags.push_back(std::move(d));
  }
};

std::string operandMsg(std::uint32_t pc, const char* opName, RType expected,
                       std::uint32_t slot, RType found, bool isLocal) {
  std::string s = "pc ";
  s += std::to_string(pc);
  s += ": ";
  s += opName;
  s += " expects ";
  s += typeName(expected);
  s += " in ";
  s += isLocal ? "l" : "r";
  s += std::to_string(slot);
  s += ", found ";
  s += typeName(found);
  return s;
}

// --- switch payload validation (shared by phase 1, block building) ----------
// Validates the canonical layouts documented above. Emits diagnostics only
// when sink != nullptr; collects the VALID targets (default included) into
// *targets when non-null. Returns overall payload validity.
bool validateSwitch(const Method& m, std::uint32_t pc, const Ins& ins,
                    DiagSink* sink, std::vector<std::uint32_t>* targets) {
  if (ins.imm >= m.cp.size()) {
    return false;
  }
  const Const& c = m.cp[ins.imm];
  if (c.kind != K::SwitchTable) {
    return false;
  }
  const std::vector<std::int32_t>& v = c.ints;
  const std::uint64_t sz = static_cast<std::uint64_t>(v.size());
  const std::uint64_t n = static_cast<std::uint64_t>(m.code.size());
  bool ok = true;

  auto targetOk = [&](std::int32_t t) -> bool {
    if (t < 0 || static_cast<std::uint64_t>(t) >= n) {
      if (sink != nullptr) {
        sink->add(pc, "switch target " + std::to_string(static_cast<long long>(t)) +
                          " out of range (code size " + std::to_string(m.code.size()) + ")");
      }
      return false;
    }
    if (targets != nullptr) {
      targets->push_back(static_cast<std::uint32_t>(t));
    }
    return true;
  };

  if (static_cast<Op>(ins.op) == Op::Tableswitch) {
    if (sz < 3) {
      if (sink != nullptr) {
        sink->add(pc, "malformed tableswitch payload (size " + std::to_string(v.size()) + ")");
      }
      return false;
    }
    const std::int64_t low = v[0];
    const std::int64_t high = v[1];
    if (low > high) {
      if (sink != nullptr) {
        sink->add(pc, "tableswitch low " + std::to_string(low) + " exceeds high " +
                          std::to_string(high));
      }
      ok = false;
    }
    if (3 + (high - low) + 1 != static_cast<std::int64_t>(sz)) {
      if (sink != nullptr) {
        sink->add(pc, "malformed tableswitch payload (size " + std::to_string(v.size()) + ")");
      }
      ok = false;
    }
    if (!ok) {
      return false; // cannot walk the targets safely
    }
    if (!targetOk(v[2])) { // default target
      ok = false;
    }
    for (std::uint64_t i = 3; i < sz; ++i) {
      if (!targetOk(v[static_cast<std::size_t>(i)])) {
        ok = false;
      }
    }
    return ok;
  }

  // Lookupswitch
  if (sz < 2) {
    if (sink != nullptr) {
      sink->add(pc, "malformed lookupswitch payload (size " + std::to_string(v.size()) + ")");
    }
    return false;
  }
  const std::int64_t cnt = v[0];
  if (cnt < 0 || 2 + 2 * cnt != static_cast<std::int64_t>(sz)) {
    if (sink != nullptr) {
      sink->add(pc, "malformed lookupswitch payload (size " + std::to_string(v.size()) + ")");
    }
    return false;
  }
  for (std::int64_t i = 0; i < cnt; ++i) {
    if (i > 0 &&
        v[static_cast<std::size_t>(2 + 2 * i)] <= v[static_cast<std::size_t>(2 + 2 * (i - 1))]) {
      if (sink != nullptr) {
        sink->add(pc, "lookupswitch matches not strictly increasing at entry " +
                          std::to_string(i));
      }
      ok = false;
    }
    if (!targetOk(v[static_cast<std::size_t>(3 + 2 * i)])) {
      ok = false;
    }
  }
  if (!targetOk(v[1])) { // default target
    ok = false;
  }
  return ok;
}

// --- abstract interpretation state -------------------------------------------
struct FlowState {
  std::vector<RType> regs;
  std::vector<RType> locals;
};

void verifyImpl(const Method& m, VerifyResult& r) {
  DiagSink sink{r};

  // --- PHASE 0: guards -------------------------------------------------------
  if (m.code.size() > 0xFFFFFFFFull) {
    // pc and branch targets are u32: refuse methods we cannot even address.
    sink.add(0, "method too large to verify");
    return;
  }
  if (m.code.empty() && !m.isAbstract()) {
    sink.add(0, "empty method body");
  }
  if (!m.code.empty() && m.isAbstract()) {
    sink.add(0, "abstract method must have empty code");
  }
  if (m.numRegs > kMaxU16) {
    sink.add(0, "numRegs " + std::to_string(m.numRegs) + " exceeds 65535");
  }
  if (m.numLocals > kMaxU16) {
    sink.add(0, "numLocals " + std::to_string(m.numLocals) + " exceeds 65535");
  }
  const std::uint32_t n = static_cast<std::uint32_t>(m.code.size());

  // --- PHASE 1: structural pass ----------------------------------------------
  for (std::uint32_t pc = 0; pc < n && !sink.full(); ++pc) {
    const Ins& ins = m.code[pc];
    if (ins.op >= opCount()) {
      // info() is only defined for op < Op::_Count; never call it on garbage.
      sink.add(pc, "invalid opcode " + std::to_string(ins.op) + " at pc " +
                       std::to_string(pc));
      continue;
    }
    const Op op = static_cast<Op>(ins.op);
    const OpInfo& oi = info(op);
    const RegUse ru = regUse(oi.sig);

    if (ru.dst && ins.dst >= m.numRegs) {
      sink.add(pc, "register r" + std::to_string(ins.dst) + " out of range (numRegs=" +
                       std::to_string(m.numRegs) + ")");
    }
    if (ru.a && ins.a >= m.numRegs) {
      sink.add(pc, "register r" + std::to_string(ins.a) + " out of range (numRegs=" +
                       std::to_string(m.numRegs) + ")");
    }
    if (ru.b && ins.b >= m.numRegs) {
      sink.add(pc, "register r" + std::to_string(ins.b) + " out of range (numRegs=" +
                       std::to_string(m.numRegs) + ")");
    }
    // Call arguments occupy consecutive registers a..a+b-1; T1 stencils read
    // them blind, so the whole span must exist (defensive extra, spec gap).
    if ((oi.sig == Sig::Call || oi.sig == Sig::CallQuick) && ins.b != 0 &&
        ins.a < m.numRegs &&
        static_cast<std::uint64_t>(ins.a) + ins.b > m.numRegs) {
      sink.add(pc, "register r" + std::to_string(m.numRegs) + " out of range (numRegs=" +
                       std::to_string(m.numRegs) + ")");
    }
    if ((oi.sig == Sig::RegSlot || oi.sig == Sig::SlotReg) && ins.imm >= m.numLocals) {
      sink.add(pc, "local slot l" + std::to_string(ins.imm) +
                       " out of range (numLocals=" + std::to_string(m.numLocals) + ")");
    }
    // NewArray imm is a JVM atype code (Atype: 4..11); anything else would
    // select a nonexistent stencil (defensive extra).
    if (op == Op::NewArray && (ins.imm < 4u || ins.imm > 11u)) {
      sink.add(pc, "invalid array type code " + std::to_string(ins.imm) + " at pc " +
                       std::to_string(pc));
    }
    const bool usesCp = sigUsesCpIndex(oi.sig) || op == Op::GuardClass;
    if (usesCp) {
      if (ins.imm >= m.cp.size()) {
        sink.add(pc, "constant pool index " + std::to_string(ins.imm) +
                         " out of range (size " + std::to_string(m.cp.size()) + ")");
      } else if (!cpKindOk(op, m.cp[ins.imm].kind)) {
        sink.add(pc, "constant pool entry " + std::to_string(ins.imm) + " is " +
                         m.cp[ins.imm].kindName() + ", but " + oi.name + " requires " +
                         expectedCpName(op));
      }
    }
    if (isBranchSig(oi.sig) && ins.imm >= n) {
      sink.add(pc, "branch target " + std::to_string(ins.imm) +
                       " out of range (code size " + std::to_string(n) + ")");
    }
    if (oi.sig == Sig::RegCpBranch) {
      validateSwitch(m, pc, ins, &sink, nullptr);
    }
  }

  // --- PHASE 1b: exception handlers -----------------------------------------
  for (const ExceptionHandler& h : m.handlers) {
    if (sink.full()) {
      break;
    }
    if (h.start > h.end || h.end > n) {
      sink.add(0, "exception handler range [" + std::to_string(h.start) + "," +
                      std::to_string(h.end) + ") invalid");
    } else if (h.start == h.end) {
      sink.add(0, "exception handler range [" + std::to_string(h.start) + "," +
                      std::to_string(h.end) + ") is empty");
    }
    if (h.handler >= n) {
      sink.add(0, "exception handler target " + std::to_string(h.handler) +
                      " out of range");
    }
    if (h.catchType < -1 ||
        (h.catchType >= 0 &&
         (static_cast<std::uint64_t>(h.catchType) >= m.cp.size() ||
          m.cp[static_cast<std::size_t>(h.catchType)].kind != K::Class))) {
      sink.add(0, "exception handler catch type " + std::to_string(h.catchType) +
                      " is not a Class constant");
    }
  }

  // --- PHASE 2: type checking via abstract interpretation ---------------------
  if (n == 0 || sink.full()) {
    return; // nothing to type-check / already rejected past the cap
  }

  // Fall-off-the-end is purely structural (independent of the descriptor), so
  // it is checked before the descriptor gate to keep the diagnostic.
  if (m.code.back().op < opCount() && canFallThrough(static_cast<Op>(m.code.back().op))) {
    sink.add(n - 1, "control falls off the end of the method");
  }

  // Descriptor gate: entry locals come from parseParams (shared contract with
  // lowering); the return trichotomy comes from the local scanner.
  std::vector<RType> params;
  if (!parseParams(m.descriptor, params)) {
    sink.add(0, "malformed method descriptor");
    return;
  }
  const RetScan rs = scanMethodDesc(m.descriptor);
  if (!rs.ok) {
    sink.add(0, "malformed method descriptor");
    return;
  }
  RetScan methodRet = rs;

  FlowState entry;
  entry.regs.assign(m.numRegs, RType::Bottom);
  entry.locals.assign(m.numLocals, RType::Bottom);
  {
    const std::uint64_t base = m.isStatic() ? 0u : 1u; // receiver for instances
    const std::uint64_t needed = base + static_cast<std::uint64_t>(params.size());
    if (needed > m.numLocals) {
      // Fail closed: cannot even seed the entry state.
      sink.add(0, "method parameters require " + std::to_string(needed) +
                      " local slots but numLocals=" + std::to_string(m.numLocals));
      return;
    }
    if (base == 1u && m.numLocals > 0) {
      entry.locals[0] = RType::Ref;
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
      entry.locals[static_cast<std::size_t>(base) + i] = params[i];
    }
  }

  // --- basic blocks -----------------------------------------------------------
  // Leaders: pc 0, valid branch targets, handler entries, and the instruction
  // after any branch/return/throw/switch/deopt_trap. A block's outgoing edges
  // all originate from its LAST instruction (fallthrough and/or branch
  // target and/or switch targets + default).
  std::vector<char> leader(n, 0);
  leader[0] = 1;
  for (std::uint32_t pc = 0; pc < n; ++pc) {
    const Ins& ins = m.code[pc];
    if (ins.op >= opCount()) {
      continue; // flagged above; no trustworthy layout
    }
    const Op op = static_cast<Op>(ins.op);
    const OpInfo& oi = info(op);
    if (isBranchSig(oi.sig) && ins.imm < n) {
      leader[ins.imm] = 1;
    }
    if (oi.sig == Sig::RegCpBranch) {
      std::vector<std::uint32_t> ts;
      validateSwitch(m, pc, ins, nullptr, &ts);
      for (const std::uint32_t t : ts) {
        if (t < n) {
          leader[t] = 1;
        }
      }
    }
    const bool terminator = isBranchSig(oi.sig) || oi.sig == Sig::RegCpBranch ||
                            isReturnOp(op) || op == Op::Athrow ||
                            op == Op::DeoptTrap;
    if (terminator && pc + 1 < n) {
      leader[pc + 1] = 1;
    }
  }
  for (const ExceptionHandler& h : m.handlers) {
    if (h.handler < n) {
      leader[h.handler] = 1;
    }
  }
  std::vector<std::uint32_t> starts;
  for (std::uint32_t pc = 0; pc < n; ++pc) {
    if (leader[pc] != 0) {
      starts.push_back(pc);
    }
  }
  const std::uint32_t nb = static_cast<std::uint32_t>(starts.size());
  std::vector<std::uint32_t> blockOf(n);
  for (std::uint32_t b = 0; b < nb; ++b) {
    const std::uint32_t e = (b + 1 < nb) ? starts[b + 1] : n;
    for (std::uint32_t p = starts[b]; p < e; ++p) {
      blockOf[p] = b;
    }
  }
  std::vector<std::vector<std::uint32_t>> succ(nb);
  for (std::uint32_t b = 0; b < nb; ++b) {
    const std::uint32_t bEnd = (b + 1 < nb) ? starts[b + 1] : n;
    const std::uint32_t last = bEnd - 1;
    const Ins& ins = m.code[last];
    if (ins.op >= opCount()) {
      continue; // dead end; flagged
    }
    const Op op = static_cast<Op>(ins.op);
    const OpInfo& oi = info(op);
    auto addSucc = [&](std::uint32_t target) {
      if (target >= n) {
        return;
      }
      const std::uint32_t tb = blockOf[target];
      if (succ[b].empty() || succ[b].back() != tb) { // dedupe consecutive
        succ[b].push_back(tb);
      }
    };
    if (canFallThrough(op) && last + 1 < n) {
      addSucc(last + 1);
    }
    if (isBranchSig(oi.sig)) {
      addSucc(ins.imm);
    }
    if (oi.sig == Sig::RegCpBranch) {
      std::vector<std::uint32_t> ts;
      validateSwitch(m, last, ins, nullptr, &ts);
      for (const std::uint32_t t : ts) {
        addSucc(t);
      }
    }
  }

  // --- worklist dataflow -------------------------------------------------------
  // WHY this design: a standard iterative FORWARD dataflow over the basic
  // blocks with stored per-block in-states. join() is the frozen Type.h
  // least upper bound: join(Bottom, X) == Bottom and incompatible merges
  // collapse to Bottom, so every slot's in-state value only ever descends
  // the chain  T -> Ref (Null/Ref merges only) -> Bottom  (at most two
  // changes per slot per block). The lattice is finite, the re-enqueue
  // condition is strictly "in-state changed", and the queue is deduplicated
  // with an in-queue flag, so the worklist terminates and its memory stays
  // bounded by the block count. A hard operation budget and a state-slot
  // ceiling make the practical bound independent of pathological inputs:
  // on breach the verifier REJECTS (fail-closed), never accepts.
  // Bottom semantics at a reachable instruction = "conflicting or never
  // assigned on some path" (JVM definite-assignment flavor): any USE of a
  // Bottom slot is a diagnostic, NOT an error-free skip.
  std::vector<FlowState> inStates(nb);
  std::vector<char> visited(nb, 0);
  std::vector<char> inQueue(nb, 0);
  std::deque<std::uint32_t> queue;
  std::uint64_t ops = 0;
  bool dead = false;
  std::size_t stateSlots = 0;

  // Breakpoint recording (only active during the final sweep).
  std::vector<RType>* runningPtr = nullptr;
  std::vector<std::vector<std::pair<std::uint32_t, RType>>>* bpsPtr = nullptr;
  std::size_t bpCount = 0;

  auto tick = [&]() {
    if (++ops > kOpBudget && !dead) {
      dead = true;
      sink.add(0, "verification budget exceeded");
    }
  };

  auto enq = [&](std::uint32_t b) {
    if (inQueue[b] == 0) {
      inQueue[b] = 1;
      queue.push_back(b);
    }
  };

  // WHY seeds ASSIGN instead of joining: an unvisited block has no state yet;
  // joining the first incoming state against an implicit all-Bottom state
  // would poison every slot to Bottom. First edge seeds, later edges join.
  auto seed = [&](std::uint32_t b, const FlowState& st) -> bool {
    inStates[b].regs = st.regs;
    inStates[b].locals = st.locals;
    stateSlots += static_cast<std::size_t>(m.numRegs) + m.numLocals;
    if (stateSlots > kMaxStateSlots) {
      dead = true;
      sink.add(0, "method too large to verify");
      return false;
    }
    visited[b] = 1;
    return true;
  };

  // Exception edge seed: at handler entry the caught exception is delivered
  // in r0 (docs/rbc_spec.md §5 — the RBC equivalent of the JVM pushing the
  // exception onto the operand stack); every other register is Bottom (CS-1),
  // and the locals are the edge's local types.
  auto seedHandler = [&](std::uint32_t b, const std::vector<RType>& locals) -> bool {
    inStates[b].regs.assign(m.numRegs, RType::Bottom);
    if (!inStates[b].regs.empty()) {
      inStates[b].regs[0] = RType::Ref;
    }
    inStates[b].locals = locals;
    stateSlots += static_cast<std::size_t>(m.numRegs) + m.numLocals;
    if (stateSlots > kMaxStateSlots) {
      dead = true;
      sink.add(0, "method too large to verify");
      return false;
    }
    visited[b] = 1;
    return true;
  };

  auto joinInto = [&](FlowState& dst, const FlowState& src) -> bool {
    bool changed = false;
    for (std::size_t i = 0; i < dst.regs.size(); ++i) {
      tick();
      if (i >= src.regs.size()) {
        break;
      }
      const RType j = join(dst.regs[i], src.regs[i]);
      if (j != dst.regs[i]) {
        dst.regs[i] = j;
        changed = true;
      }
    }
    for (std::size_t i = 0; i < dst.locals.size(); ++i) {
      tick();
      if (i >= src.locals.size()) {
        break;
      }
      const RType j = join(dst.locals[i], src.locals[i]);
      if (j != dst.locals[i]) {
        dst.locals[i] = j;
        changed = true;
      }
    }
    return changed;
  };

  auto cpAt = [&](std::uint32_t idx, Op op) -> const Const* {
    if (idx >= m.cp.size()) {
      return nullptr;
    }
    const Const& c = m.cp[idx];
    return cpKindOk(op, c.kind) ? &c : nullptr;
  };

  // One abstract-interpretation step. Called (a) silently by the worklist to
  // compute types and (b) once per pc by the final sweep with emitD = true to
  // emit diagnostics with the FIXPOINT state - the same code path guarantees
  // the checked states are exactly the converged ones, each pc judged once.
  // Every state-vector index is re-guarded: phase-1-flagged garbage never
  // touches the vectors (totally defensive, no UB on hostile input).
  auto stepIns = [&](std::uint32_t pc, const Ins& ins, FlowState& S, bool emitD) {
    if (dead || ins.op >= opCount()) {
      return;
    }
    const Op op = static_cast<Op>(ins.op);
    const OpInfo& oi = info(op);
    const RegUse ru = regUse(oi.sig);
    // Defensive: bound every field against the ACTUAL state vector sizes,
    // not only against what the Sig claims - op-keyed writes below (Iinc,
    // descriptor-typed field/call results) touch dst even if a mismatched
    // metadata table assigns a Sig without a dst operand. Phase-1-flagged
    // garbage never touches the vectors; there is no UB on hostile input.
    const bool dstOk = ins.dst < S.regs.size();
    const bool aOk = ins.a < S.regs.size();
    const bool bOk = ins.b < S.regs.size();
    const bool slotOk = ins.imm < S.locals.size();
    if ((ru.dst && !dstOk) || (ru.a && !aOk) || (ru.b && !bOk)) {
      return;
    }
    const bool isCall = oi.sig == Sig::Call || oi.sig == Sig::CallQuick;
    auto setDst = [&](RType t) {
      if (dstOk) {
        S.regs[ins.dst] = t;
      }
    };

    // Operand checks. Call/CallQuick are skipped: a is an arg BASE register
    // and b an arg COUNT, so operandA/operandB cannot describe their types
    // (per-argument descriptor checks are a documented v0 gap: the
    // receiver-in-args convention is not pinned yet).
    if (emitD && !isCall) {
      if (ru.a && oi.operandA != RType::Bottom &&
          !isAssignable(S.regs[ins.a], oi.operandA)) {
        sink.add(pc, operandMsg(pc, oi.name, oi.operandA, ins.a, S.regs[ins.a], false));
      }
      if (ru.b && oi.operandB != RType::Bottom &&
          !isAssignable(S.regs[ins.b], oi.operandB)) {
        sink.add(pc, operandMsg(pc, oi.name, oi.operandB, ins.b, S.regs[ins.b], false));
      }
    }

    // Iinc is the one read-modify-write dst op: dst must already be Int and
    // stays Int.
    if (op == Op::Iinc) {
      if (dstOk) {
        if (emitD && !isAssignable(S.regs[ins.dst], RType::Int)) {
          sink.add(pc, operandMsg(pc, oi.name, RType::Int, ins.dst, S.regs[ins.dst], false));
        }
        S.regs[ins.dst] = RType::Int;
      }
      return;
    }

    // Loads: check the local slot's type against the loaded type, then write
    // the result to dst.
    if (oi.sig == Sig::RegSlot) {
      if (!slotOk) {
        return; // flagged in phase 1
      }
      if (oi.result != RType::Bottom) {
        if (emitD && !isAssignable(S.locals[ins.imm], oi.result)) {
          sink.add(pc, operandMsg(pc, oi.name, oi.result, ins.imm, S.locals[ins.imm], true));
        }
        setDst(oi.result);
      } else {
        setDst(RType::Bottom); // defensive: unknown load type
      }
      return;
    }

    // Stores: locals are typed by WHAT IS STORED (the register's current
    // type), and a local may be re-typed across the method - just not within
    // a protected range (checked after the sweep).
    if (oi.sig == Sig::SlotReg) {
      if (!slotOk) {
        return; // flagged in phase 1
      }
      const RType stored = S.regs[ins.a];
      S.locals[ins.imm] = stored;
      if (bpsPtr != nullptr && runningPtr != nullptr &&
          ins.imm < runningPtr->size()) {
        if ((*runningPtr)[ins.imm] != stored) {
          (*runningPtr)[ins.imm] = stored;
          if (bpCount < kMaxBreakpoints) {
            ++bpCount;
            (*bpsPtr)[ins.imm].emplace_back(pc + 1u, stored);
          } else {
            dead = true;
            sink.add(0, "verification budget exceeded");
          }
        }
      }
      return;
    }

    // Return-type agreement (operand types were checked above).
    if (emitD && isReturnOp(op)) {
      bool agree = false;
      if (op == Op::Return) {
        agree = methodRet.isVoid;
      } else if (op == Op::Ireturn) {
        agree = !methodRet.isVoid && methodRet.type == RType::Int;
      } else if (op == Op::Lreturn) {
        agree = !methodRet.isVoid && methodRet.type == RType::Long;
      } else if (op == Op::Freturn) {
        agree = !methodRet.isVoid && methodRet.type == RType::Float;
      } else if (op == Op::Dreturn) {
        agree = !methodRet.isVoid && methodRet.type == RType::Double;
      } else { // Areturn: register may be Ref or Null (operand check), method must return Ref
        agree = !methodRet.isVoid && methodRet.type == RType::Ref;
      }
      if (!agree) {
        sink.add(pc, "pc " + std::to_string(pc) + ": " + oi.name +
                         " does not match method return type " + retName(methodRet));
      }
    }

    // Destination writes. Descriptor-typed ops (field loads, invokes) derive
    // the dst type from the cp entry: a static OpInfo table cannot express
    // descriptor-dependent results. For invokes: 'V' return writes nothing;
    // parse failure is a diagnostic plus a Ref fallback (sound for the dst).
    switch (op) {
    case Op::Getfield:
    case Op::Getstatic: {
      const Const* c = cpAt(ins.imm, op);
      if (c != nullptr) {
        const FieldScan fs = scanField(c->str3);
        if (fs.ok && fs.used == c->str3.size()) {
          setDst(fs.type);
        } else {
          if (emitD) {
            sink.add(pc, "malformed FieldRef descriptor at pc " + std::to_string(pc));
          }
          setDst(RType::Ref);
        }
      }
      // cp entry invalid: flagged in phase 1; leave dst untouched.
      break;
    }
    case Op::Invokevirtual:
    case Op::Invokespecial:
    case Op::Invokestatic:
    case Op::Invokeinterface:
    case Op::Invokedynamic: {
      const Const* c = cpAt(ins.imm, op);
      if (c != nullptr) {
        const std::string_view desc =
            (op == Op::Invokedynamic) ? std::string_view{c->str2} : std::string_view{c->str3};
        const RetScan rscan = scanMethodDesc(desc);
        if (!rscan.ok) {
          if (emitD) {
            if (op == Op::Invokedynamic) {
              sink.add(pc, "malformed InvokeDynamic descriptor at pc " + std::to_string(pc));
            } else {
              sink.add(pc, "malformed MethodRef descriptor at pc " + std::to_string(pc));
            }
          }
          setDst(RType::Ref);
        } else if (!rscan.isVoid) {
          setDst(rscan.type); // void: dst is not written
        }
      }
      break;
    }
    default:
      // result == Bottom means "writes nothing" (branches, stores, ...);
      // the dst register is left alone. Quickened call/field ops carry
      // resolved ids instead of descriptors; their info() result is trusted
      // (pipeline law: verification runs BEFORE quickening).
      if (oi.result != RType::Bottom) {
        setDst(oi.result);
      }
      break;
    }
  };

  // --- run the worklist --------------------------------------------------------
  if (!seed(0, entry)) {
    return;
  }
  enq(0);
  while (!queue.empty() && !dead && !sink.full()) {
    const std::uint32_t b = queue.front();
    queue.pop_front();
    inQueue[b] = 0;
    tick();
    FlowState S = inStates[b];
    const std::uint32_t bEnd = (b + 1 < nb) ? starts[b + 1] : n;
    for (std::uint32_t pc = starts[b]; pc < bEnd && !dead; ++pc) {
      tick();
      // Exception edges: an exception raised AT pc transfers to the handler
      // with the locals as they are BEFORE pc executes (the faulting
      // instruction's effects, including its store, do not complete) and
      // with NO live registers.
      if (!m.handlers.empty()) {
        for (const ExceptionHandler& h : m.handlers) {
          tick();
          if (h.start >= h.end || h.end > n || h.handler >= n) {
            continue; // flagged in phase 1
          }
          if (pc < h.start || pc >= h.end) {
            continue;
          }
          const std::uint32_t hb = blockOf[h.handler];
          if (!visited[hb]) {
            if (!seedHandler(hb, S.locals)) {
              break;
            }
            enq(hb);
          } else {
            bool changed = false;
            // The exception edge's register state is r0 = Ref (the delivered
            // exception, rbc_spec.md SS5.3 spec pin) with every other
            // register Bottom. Join against THAT edge state - joining against
            // all-Bottom would clobber the r0 delivery pin whenever a second
            // edge (any later pc of a multi-instruction protected range)
            // joins into an already-visited handler, which is every realistic
            // try block. For i > 0, join(X, Bottom) == Bottom (exact
            // element-wise join, unchanged); r0 stays Ref across pure
            // exception edges and degrades to Bottom only when a normal edge
            // brought an incompatible r0 type (conservative and sound).
            for (std::size_t i = 0; i < inStates[hb].regs.size(); ++i) {
              tick();
              const RType edgeReg = (i == 0) ? RType::Ref : RType::Bottom;
              const RType j = join(inStates[hb].regs[i], edgeReg);
              if (j != inStates[hb].regs[i]) {
                inStates[hb].regs[i] = j;
                changed = true;
              }
            }
            for (std::size_t i = 0; i < inStates[hb].locals.size() &&
                                    i < S.locals.size(); ++i) {
              tick();
              const RType j = join(inStates[hb].locals[i], S.locals[i]);
              if (j != inStates[hb].locals[i]) {
                inStates[hb].locals[i] = j;
                changed = true;
              }
            }
            if (changed) {
              enq(hb);
            }
          }
        }
        if (dead) {
          break;
        }
      }
      stepIns(pc, m.code[pc], S, false);
    }
    if (dead) {
      break;
    }
    for (const std::uint32_t s : succ[b]) {
      tick();
      if (s >= nb) {
        continue; // defensive: never index inStates unchecked
      }
      if (!visited[s]) {
        if (!seed(s, S)) {
          break;
        }
        enq(s);
      } else if (joinInto(inStates[s], S)) {
        enq(s); // re-enqueue only when the merged in-state changed
      }
    }
  }
  if (dead) {
    return; // diagnostic already emitted at the failure site
  }

  // --- final sweep: diagnostics + protected-range change points -----------------
  // WHY a final sweep instead of diagnosing inside the worklist: worklist
  // revisits would duplicate (or vary) per-pc diagnostics; a single pass over
  // the converged in-states is deterministic, judges each pc exactly once,
  // and cannot miss errors (type errors are monotone: once a slot descends
  // below what an operand needs, it stays there at the fixpoint).
  //
  // Protected-range rule (v0, CS-1): a handler may be entered from ANY pc of
  // its range, so the only state a handler body can rely on is the locals.
  // v0 therefore requires each local to hold the SAME RType at every
  // REACHABLE pc of every protected range (re-typing a local inside a try
  // region is a lowering contract violation: use a fresh local instead).
  // All-Bottom is allowed (an unused local, or a range that never executes:
  // unreachable pcs are skipped by both the exception edges and this check).
  // The check is implemented with change points: the per-pc local type is
  // piecewise constant, changing only at block leaders (in-state) and after
  // stores, so "some pc in (start, end) has a different type than pc start"
  // is exactly "a change point lies strictly inside (start, end)".
  std::vector<RType> running = inStates[0].locals; // block 0 is always visited
  std::vector<std::vector<std::pair<std::uint32_t, RType>>> bps(m.numLocals);
  runningPtr = &running;
  bpsPtr = &bps;
  for (std::uint32_t b = 0; b < nb && !dead && !sink.full(); ++b) {
    if (visited[b] == 0) {
      continue; // unreachable code is not type-checked (JVM parity)
    }
    const std::uint32_t bEnd = (b + 1 < nb) ? starts[b + 1] : n;
    if (!m.handlers.empty()) {
      for (std::size_t i = 0; i < inStates[b].locals.size() && i < running.size(); ++i) {
        if (inStates[b].locals[i] != running[i]) {
          const RType t = inStates[b].locals[i];
          running[i] = t;
          if (bpCount < kMaxBreakpoints) {
            ++bpCount;
            bps[i].emplace_back(starts[b], t);
          } else {
            dead = true;
            sink.add(0, "verification budget exceeded");
            break;
          }
        }
      }
      if (dead) {
        break;
      }
    }
    FlowState S = inStates[b];
    for (std::uint32_t pc = starts[b]; pc < bEnd && !dead; ++pc) {
      tick();
      stepIns(pc, m.code[pc], S, true);
    }
  }
  runningPtr = nullptr;
  bpsPtr = nullptr;
  if (dead) {
    return;
  }

  // Stability verdict: for each handler, for each local, the first change
  // point strictly inside (start, end) is the violation (binary search; the
  // lists are pc-ascending because the sweep visits blocks in pc order).
  for (const ExceptionHandler& h : m.handlers) {
    if (h.start >= h.end || h.end > n || h.handler >= n) {
      continue; // flagged in phase 1
    }
    for (std::size_t i = 0; i < bps.size(); ++i) {
      const auto& list = bps[i];
      std::size_t lo = 0;
      std::size_t hi = list.size();
      while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (static_cast<std::uint64_t>(list[mid].first) <=
            static_cast<std::uint64_t>(h.start)) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      if (lo < list.size() &&
          static_cast<std::uint64_t>(list[lo].first) < static_cast<std::uint64_t>(h.end)) {
        sink.add(list[lo].first,
                 "local l" + std::to_string(i) + " changes type within protected range");
      }
    }
  }
}

} // namespace

VerifyResult verify(const Method& m) noexcept {
  VerifyResult r;
  try {
    verifyImpl(m, r);
  } catch (...) {
    // WHY: verify() is noexcept and the method body is untrusted garbage; an
    // allocation failure must degrade to a diagnostic, never std::terminate.
    r.ok = false;
    r.diags.clear();
    VerifyDiag d;
    d.pc = 0;
    d.message = "out of memory"; // short enough for SSO: cannot itself throw
    r.diags.push_back(std::move(d));
    return r;
  }
  r.ok = r.diags.empty();
  return r;
}

} // namespace b2::rbc
