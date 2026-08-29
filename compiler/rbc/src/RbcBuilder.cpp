// B-2 RBC - method builder used by frontend lowering and tests.
//
// WHY THIS FILE EXISTS:
// Lowering (and tests) must not hand-assemble Ins records — operand-layout
// knowledge would leak across the codebase (RbcBuilder.h). The builder owns
// that knowledge, resolves labels to instruction indices at finish(), and
// refuses to produce structurally malformed methods where it can.
//
// Emission mapping notes (docs/rbc_spec.md §2.3, §10.1 pins — the factories
// are generic per Sig family and do NOT validate op-to-Sig correspondence;
// that is the verifier's job):
// - putstatic:     emitRegCp(op, valueReg, cpIdx) — dst is READ as the value
//                  (P1), so the "dst" parameter carries the value register.
// - array stores:  emitRegRegReg(op, valueReg, arrayReg, indexReg) (P3).
// - putfield:      emitRegRegRegCp(op, /*dst=*/0, objReg, valueReg, cpIdx)
//                  — dst is canonical 0 (V-S10).
// - guard_class:   emitCall(op, /*dst=*/0, testedReg, deoptId, cpIdx) —
//                  Call is the only (dst, a, b, imm) factory and matches the
//                  GuardCp field layout; emitCall also serves CallQuick,
//                  with the resolved id passed as cpIdx.
//
// Switches (P6/P7): the cp payload uses the CANONICAL layouts shared with
// the Verifier and RbcText (docs/rbc_spec.md): tableswitch
// [low, high, defaultTarget, target(low..high)...] and lookupswitch
// [N, defaultTarget, match1, target1, ...]. The DEFAULT target is the
// fall-through instruction pc + 1 — emitSwitch deliberately takes no default
// label. A switch may never be the last instruction of a method (V-S7).

#include <algorithm>
#include <bit>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "b2/rbc/RbcBuilder.h"

namespace b2::rbc {
namespace {

// WHY sentinels: the frozen header gives bind() no error channel and the
// builder no extra members, so double-bind misuse must be detectable at
// finish() through boundAt_ alone. Instruction indices can never reach
// these values (code is bounded by memory long before 4G instructions).
constexpr std::uint32_t kUnbound = 0xFFFFFFFFu;
constexpr std::uint32_t kDoubleBound = 0xFFFFFFFEu;

// Ins register fields are u16, so at most 65536 registers (r0..r65535) can
// be addressed; a numRegs above that can never be referenced (V-S2).
constexpr std::uint32_t kMaxRegs = 65536u;

bool isSwitchOp(Op op) noexcept {
  return op == Op::Tableswitch || op == Op::Lookupswitch;
}

// Full-value equality for pool interning (Law/Rule 124: deterministic,
// replayable pools — identical constants share one index in first-use order).
bool sameConst(const Const& a, const Const& b) noexcept {
  if (a.kind != b.kind) {
    return false;
  }
  switch (a.kind) {
  case Const::Kind::Int32:
    return a.i32 == b.i32;
  case Const::Kind::Int64:
    return a.i64 == b.i64;
  case Const::Kind::Float:
    // WHY bitwise: == would merge 0.0 with -0.0 and collapse distinct NaN
    // payloads, silently rewriting constants in rebuilt methods.
    return std::bit_cast<std::uint32_t>(a.f32) ==
           std::bit_cast<std::uint32_t>(b.f32);
  case Const::Kind::Double:
    return std::bit_cast<std::uint64_t>(a.f64) ==
           std::bit_cast<std::uint64_t>(b.f64);
  case Const::Kind::Utf8:
  case Const::Kind::String:
  case Const::Kind::Class:
  case Const::Kind::MethodType:
    return a.str == b.str;
  case Const::Kind::NameType:
  case Const::Kind::MethodHandle:
  case Const::Kind::InvokeDynamic:
    return a.str == b.str && a.str2 == b.str2;
  case Const::Kind::FieldRef:
  case Const::Kind::MethodRef:
  case Const::Kind::InterfaceMethodRef:
    return a.str == b.str && a.str2 == b.str2 && a.str3 == b.str3;
  case Const::Kind::SwitchTable:
    return a.ints == b.ints;
  }
  return false; // corrupted kind byte
}

// Does this call instruction write its dst register?
//
// WHY callee-descriptor lookup: void-ness belongs to the CALLEE's
// descriptor, not the containing method's (RbcBuilder.h: "dst = result
// register (valid unless void; then dst is unused and canonical 0)"). For
// unquickened calls the builder can read the MethodRef from its own pool;
// when it cannot (quickened ids repurpose imm, malformed pools, non-call
// ops) it conservatively skips the dst check and leaves it to the verifier,
// which has full cp knowledge. This is what lets a registerless void method
// such as `void f() { g(); }` (invokestatic dst=0 a=0 b=0, numRegs=0)
// finish cleanly instead of tripping a false r0-out-of-range error.
bool callWritesResult(const std::vector<Const>& cp, const Ins& ins) noexcept {
  if (static_cast<std::size_t>(ins.imm) >= cp.size()) {
    return false;
  }
  const Const& c = cp[ins.imm];
  std::string_view desc;
  if (c.kind == Const::Kind::MethodRef ||
      c.kind == Const::Kind::InterfaceMethodRef) {
    desc = c.str3; // class, name, descriptor
  } else if (c.kind == Const::Kind::InvokeDynamic) {
    desc = c.str2; // name, descriptor
  } else {
    return false;
  }
  // Bottom = void (or a malformed callee descriptor; the verifier owns
  // reporting that).
  return parseReturn(desc) != RType::Bottom;
}

std::string regRangeError(std::uint16_t reg, std::size_t pc,
                          std::uint32_t numRegs) {
  return "register r" + std::to_string(reg) + " out of range at pc " +
         std::to_string(pc) + " (numRegs " + std::to_string(numRegs) + ")";
}

std::string slotRangeError(std::uint32_t slot, std::size_t pc,
                           std::uint32_t numLocals) {
  return "local slot " + std::to_string(slot) + " out of range at pc " +
         std::to_string(pc) + " (numLocals " + std::to_string(numLocals) +
         ")";
}

} // namespace

RbcBuilder::RbcBuilder(std::string name, std::string descriptor,
                       std::uint16_t flags)
    : name_(std::move(name)), descriptor_(std::move(descriptor)),
      flags_(flags) {}

void RbcBuilder::setRegs(std::uint32_t n) noexcept {
  // WHY plain setter (not monotonic max): lowering a size afterwards is
  // caller-visible misuse — finish() then fails the register-bound check,
  // which beats silently clamping and emitting out-of-range operands.
  numRegs_ = n;
}

void RbcBuilder::setLocals(std::uint32_t n) noexcept { numLocals_ = n; }

std::uint32_t RbcBuilder::newReg() noexcept {
  // Returns the id of the register being allocated and grows numRegs_ so
  // every handed-out id passes finish()'s register-bound check.
  // (Vector-free, so the noexcept contract is trivially safe.)
  return numRegs_++;
}

// --- constant pool ---------------------------------------------------------

std::uint32_t RbcBuilder::internConst(Const c) {
  // Linear scan in first-use order: deterministic (Law/Rule 124) and, for
  // v0 pool sizes, faster than any index structure would be honest about.
  for (std::size_t i = 0; i < cp_.size(); ++i) {
    if (sameConst(cp_[i], c)) {
      return static_cast<std::uint32_t>(i);
    }
  }
  cp_.push_back(std::move(c));
  return static_cast<std::uint32_t>(cp_.size() - 1);
}

std::uint32_t RbcBuilder::constInt(std::int32_t v) {
  Const c;
  c.kind = Const::Kind::Int32;
  c.i32 = v;
  return internConst(std::move(c));
}

std::uint32_t RbcBuilder::constLong(std::int64_t v) {
  Const c;
  c.kind = Const::Kind::Int64;
  c.i64 = v;
  return internConst(std::move(c));
}

std::uint32_t RbcBuilder::constFloat(float v) {
  Const c;
  c.kind = Const::Kind::Float;
  c.f32 = v;
  return internConst(std::move(c));
}

std::uint32_t RbcBuilder::constDouble(double v) {
  Const c;
  c.kind = Const::Kind::Double;
  c.f64 = v;
  return internConst(std::move(c));
}

std::uint32_t RbcBuilder::constString(std::string v) {
  Const c;
  c.kind = Const::Kind::String;
  c.str = std::move(v);
  return internConst(std::move(c));
}

std::uint32_t RbcBuilder::constClass(std::string internalName) {
  Const c;
  c.kind = Const::Kind::Class;
  c.str = std::move(internalName);
  return internConst(std::move(c));
}

std::uint32_t RbcBuilder::constFieldRef(std::string cls, std::string name,
                                        std::string descriptor) {
  Const c;
  c.kind = Const::Kind::FieldRef;
  c.str = std::move(cls);
  c.str2 = std::move(name);
  c.str3 = std::move(descriptor);
  return internConst(std::move(c));
}

std::uint32_t RbcBuilder::constMethodRef(std::string cls, std::string name,
                                         std::string descriptor) {
  Const c;
  c.kind = Const::Kind::MethodRef;
  c.str = std::move(cls);
  c.str2 = std::move(name);
  c.str3 = std::move(descriptor);
  return internConst(std::move(c));
}

std::uint32_t RbcBuilder::constSwitch(std::vector<std::int32_t> payload) {
  // Raw table intern (payload as-is, targets already resolved indices) for
  // hand-assembled methods and tests; emitSwitch builds patched tables
  // automatically and is what lowering should use.
  Const c;
  c.kind = Const::Kind::SwitchTable;
  c.ints = std::move(payload);
  return internConst(std::move(c));
}

// --- emission ---------------------------------------------------------------

std::uint32_t RbcBuilder::rawEmit(const Ins& i) {
  code_.push_back(i);
  return static_cast<std::uint32_t>(code_.size() - 1);
}

std::uint32_t RbcBuilder::emit(Op op) { return rawEmit(Ins(op, 0, 0, 0, 0)); }

std::uint32_t RbcBuilder::emitReg(Op op, std::uint16_t a) {
  return rawEmit(Ins(op, 0, a, 0, 0));
}

std::uint32_t RbcBuilder::emitRegImm(Op op, std::uint16_t dst,
                                     std::uint32_t imm) {
  return rawEmit(Ins(op, dst, 0, 0, imm));
}

std::uint32_t RbcBuilder::emitRegCp(Op op, std::uint16_t dst,
                                    std::uint32_t cpIdx) {
  return rawEmit(Ins(op, dst, 0, 0, cpIdx));
}

std::uint32_t RbcBuilder::emitRegSlot(Op op, std::uint16_t dst,
                                      std::uint32_t slot) {
  return rawEmit(Ins(op, dst, 0, 0, slot));
}

std::uint32_t RbcBuilder::emitSlotReg(Op op, std::uint16_t a,
                                      std::uint32_t slot) {
  return rawEmit(Ins(op, 0, a, 0, slot));
}

std::uint32_t RbcBuilder::emitRegReg(Op op, std::uint16_t dst,
                                     std::uint16_t a) {
  return rawEmit(Ins(op, dst, a, 0, 0));
}

std::uint32_t RbcBuilder::emitRegRegReg(Op op, std::uint16_t dst,
                                        std::uint16_t a, std::uint16_t b) {
  return rawEmit(Ins(op, dst, a, b, 0));
}

std::uint32_t RbcBuilder::emitRegRegImm(Op op, std::uint16_t dst,
                                        std::uint16_t a, std::uint32_t imm) {
  return rawEmit(Ins(op, dst, a, 0, imm));
}

std::uint32_t RbcBuilder::emitRegRegCp(Op op, std::uint16_t dst,
                                       std::uint16_t a, std::uint32_t cpIdx) {
  return rawEmit(Ins(op, dst, a, 0, cpIdx));
}

std::uint32_t RbcBuilder::emitRegRegRegCp(Op op, std::uint16_t dst,
                                          std::uint16_t a, std::uint16_t b,
                                          std::uint32_t cpIdx) {
  return rawEmit(Ins(op, dst, a, b, cpIdx));
}

std::uint32_t RbcBuilder::emitCall(Op op, std::uint16_t dst,
                                   std::uint16_t argBase,
                                   std::uint16_t argCount,
                                   std::uint32_t cpIdx) {
  return rawEmit(Ins(op, dst, argBase, argCount, cpIdx));
}

std::uint32_t RbcBuilder::emitGuard(Op op, std::uint16_t a,
                                    std::uint32_t deoptId) {
  return rawEmit(Ins(op, 0, a, 0, deoptId));
}

std::uint32_t RbcBuilder::emitTrap(std::uint32_t deoptId) {
  return rawEmit(Ins(Op::DeoptTrap, 0, 0, 0, deoptId));
}

// --- branches -----------------------------------------------------------------

std::uint32_t RbcBuilder::emitBranch(Op op, Label target) {
  const std::uint32_t at = rawEmit(Ins(op, 0, 0, 0, 0)); // imm patched later
  patches_.push_back(Patch{at, target.id, kBranchSlot});
  return at;
}

std::uint32_t RbcBuilder::emitRegBranch(Op op, std::uint16_t a, Label target) {
  const std::uint32_t at = rawEmit(Ins(op, 0, a, 0, 0));
  patches_.push_back(Patch{at, target.id, kBranchSlot});
  return at;
}

std::uint32_t RbcBuilder::emitRegRegBranch(Op op, std::uint16_t a,
                                           std::uint16_t b, Label target) {
  const std::uint32_t at = rawEmit(Ins(op, 0, a, b, 0));
  patches_.push_back(Patch{at, target.id, kBranchSlot});
  return at;
}

std::uint32_t RbcBuilder::emitSwitch(Op op, std::uint16_t a,
                                     const std::vector<std::int32_t>& payload,
                                     std::vector<Label> targets) {
  // WHY canonical layouts: the Verifier and RbcText both define the
  // SwitchTable payload normatively (docs/rbc_spec.md):
  //   tableswitch  [low, high, defaultTarget, target(low..high)...]
  //   lookupswitch [N, defaultTarget, match1, target1, ...]
  // so the builder must emit exactly those shapes. Targets are LABELS, so
  // their slots are zeroed here and patched at finish(); the default is
  // the fall-through instruction (pc + 1), a plain index, no label needed.
  //
  // WHY appended, never interned: interning the UNRESOLVED payload would
  // let two switches with equal matches share one entry and cross-patch
  // each other's targets. One dedicated entry per switch is deterministic
  // by construction (Rule 124).
  //
  // emitSwitch has no error channel, so misuse (count mismatch, duplicate
  // matches, non-contiguous tableswitch matches, empty tableswitch) is
  // recorded in pendingError_ and fails finish() - never silently
  // dropping a case.
  if (!isSwitchOp(op)) {
    // Caller misuse: no payload; recorded patches degrade to branch
    // patches at finish(). Garbage in, verifier-rejected garbage out.
    const std::uint32_t at = rawEmit(Ins(op, 0, a, 0, 0));
    for (const Label t : targets) {
      patches_.push_back(Patch{at, t.id, kBranchSlot});
    }
    return at;
  }

  const std::uint32_t at = static_cast<std::uint32_t>(code_.size());
  if (payload.size() != targets.size()) {
    pendingError_ = "switch at pc " + std::to_string(at) + " has " +
                   std::to_string(payload.size()) + " matches for " +
                   std::to_string(targets.size()) + " targets";
  }

  // Sort (match, target) pairs by match; the canonical layouts require
  // ascending matches regardless of caller order.
  std::vector<std::pair<std::int32_t, Label>> pairs;
  pairs.reserve(payload.size());
  for (std::size_t i = 0; i < payload.size() && i < targets.size(); ++i) {
    pairs.emplace_back(payload[i], targets[i]);
  }
  std::sort(pairs.begin(), pairs.end(),
            [](const auto& x, const auto& y) { return x.first < y.first; });
  for (std::size_t i = 1; i < pairs.size(); ++i) {
    if (pairs[i].first == pairs[i - 1].first && pendingError_.empty()) {
      pendingError_ = "switch at pc " + std::to_string(at) +
                     " has duplicate match " + std::to_string(pairs[i].first);
    }
  }

  Const c;
  c.kind = Const::Kind::SwitchTable;
  if (op == Op::Tableswitch) {
    if (pairs.empty()) {
      if (pendingError_.empty()) {
        pendingError_ = "tableswitch at pc " + std::to_string(at) +
                       " requires at least one match";
      }
      c.ints = {0, -1, static_cast<std::int32_t>(at + 1)};
    } else {
      const std::int32_t low = pairs.front().first;
      const std::int32_t high = pairs.back().first;
      const bool contiguous =
          static_cast<std::int64_t>(high) - static_cast<std::int64_t>(low) + 1 ==
          static_cast<std::int64_t>(pairs.size());
      if (!contiguous && pendingError_.empty()) {
        pendingError_ = "tableswitch at pc " + std::to_string(at) +
                       " matches must be contiguous ascending " +
                       "(use lookupswitch for sparse matches)";
      }
      c.ints.push_back(low);
      c.ints.push_back(high);
      c.ints.push_back(static_cast<std::int32_t>(at + 1)); // default
      for (std::size_t i = 0; i < pairs.size(); ++i) {
        c.ints.push_back(0); // target slot, patched at finish()
        patches_.push_back(Patch{at, pairs[i].second.id,
                                 static_cast<std::uint32_t>(3 + i)});
      }
    }
  } else {
    c.ints.push_back(static_cast<std::int32_t>(pairs.size())); // N
    c.ints.push_back(static_cast<std::int32_t>(at + 1));       // default
    for (std::size_t i = 0; i < pairs.size(); ++i) {
      c.ints.push_back(pairs[i].first);
      c.ints.push_back(0); // target slot, patched at finish()
      patches_.push_back(Patch{at, pairs[i].second.id,
                               static_cast<std::uint32_t>(3 + 2 * i)});
    }
  }

  cp_.push_back(std::move(c));
  const std::uint32_t cpIdx = static_cast<std::uint32_t>(cp_.size() - 1);
  return rawEmit(Ins(op, 0, a, 0, cpIdx));
}

// --- labels ---------------------------------------------------------------------

RbcBuilder::Label RbcBuilder::newLabel() noexcept {
  // Allocation-bounded by the number of labels the caller requests
  // (one entry per newLabel call); OOM follows the codebase-wide
  // terminate convention.
  Label l;
  l.id = static_cast<std::uint32_t>(labels_.size());
  labels_.push_back(l);
  boundAt_.push_back(kUnbound);
  return l;
}

void RbcBuilder::bind(Label l) {
  // WHY sentinel: a second bind must fail finish(), but bind() returns
  // void and the builder has no other error state — kDoubleBound marks the
  // label so finish() reports it.
  if (!l.isValid() || l.id >= labels_.size()) {
    return; // defensive: not a label from newLabel(); ignore, never crash
  }
  if (boundAt_[l.id] == kUnbound) {
    boundAt_[l.id] = static_cast<std::uint32_t>(code_.size());
  } else {
    boundAt_[l.id] = kDoubleBound;
  }
}

std::uint32_t RbcBuilder::here() const noexcept {
  return static_cast<std::uint32_t>(code_.size());
}

// --- exception handlers ------------------------------------------------------------

void RbcBuilder::addHandler(std::uint32_t start, std::uint32_t end,
                            std::uint32_t handler,
                            std::int32_t catchTypeCp) {
  // WHY no validation here: ranges are validated at finish(), when code_
  // and cp_ have their final sizes (callers routinely emit the handler body
  // after registering the entry).
  ExceptionHandler h;
  h.start = start;
  h.end = end;
  h.handler = handler;
  h.catchType = catchTypeCp;
  handlers_.push_back(h);
}

RbcBuilder::Label RbcBuilder::handlerLabel() { return newLabel(); }

// --- finish ---------------------------------------------------------------------------

RbcBuilder::FinishResult RbcBuilder::finish(Method& out) {
  // 0. Emit-time misuse recorded by emitSwitch (no error channel there).
  if (!pendingError_.empty()) {
    return {false, pendingError_};
  }

  // 1. Double-bound labels (bind() misuse is only detectable here).
  for (std::size_t i = 0; i < labels_.size(); ++i) {
    if (boundAt_[i] == kDoubleBound) {
      return {false, "label L" + std::to_string(i) + " is already bound"};
    }
  }

  // 2. Resolve patches. Branch patches write the target into imm; switch
  // patches write it into the instruction's canonical SwitchTable payload
  // at the explicit payload slot recorded at emitSwitch time (table:
  // 3 + i; lookup: 3 + 2*i) — no ordinal reconstruction, so patch order
  // and inter-instruction interleaving cannot corrupt a table.
  for (const Patch& p : patches_) {
    if (p.at >= code_.size()) {
      // Unreachable (patches are created from rawEmit indices); kept so a
      // future refactor can never turn a stray patch into UB.
      return {false,
              "internal error: patch references instruction " +
                  std::to_string(p.at)};
    }
    if (p.labelId >= labels_.size()) {
      // Defensive: a default-constructed/garbage Label object.
      return {false, "label L" + std::to_string(p.labelId) + " is not bound"};
    }
    const std::uint32_t target = boundAt_[p.labelId];
    if (target == kUnbound) {
      return {false,
              "label L" + std::to_string(p.labelId) + " is not bound"};
    }
    if (target == kDoubleBound) {
      return {false,
              "label L" + std::to_string(p.labelId) + " is already bound"};
    }
    if (target >= code_.size()) {
      return {false, "branch target out of range"};
    }
    if (p.slot == kBranchSlot) {
      code_[p.at].imm = target;
      continue;
    }
    // Switch payload patch: slot addresses cp_[imm].ints directly.
    if (isSwitchOp(code_[p.at].opcode())) {
      const std::uint32_t cpIdx = code_[p.at].imm;
      if (static_cast<std::size_t>(cpIdx) >= cp_.size() ||
          cp_[cpIdx].kind != Const::Kind::SwitchTable) {
        return {false, "switch at pc " + std::to_string(p.at) +
                           " does not reference a switch table"};
      }
      if (static_cast<std::size_t>(p.slot) >= cp_[cpIdx].ints.size()) {
        return {false,
                "internal error: switch patch slot out of range at pc " +
                    std::to_string(p.at)};
      }
      cp_[cpIdx].ints[p.slot] = static_cast<std::int32_t>(target);
    } else {
      code_[p.at].imm = target;
    }
  }

  // 3. Switch shape sanity against the canonical layouts (defense in depth:
  // emitSwitch constructs exactly these shapes, so a violation means builder
  // state was corrupted) + fall-through default (V-S6 shape, V-S7).
  for (std::size_t pc = 0; pc < code_.size(); ++pc) {
    if (!isSwitchOp(code_[pc].opcode())) {
      continue;
    }
    if (pc + 1 >= code_.size()) {
      return {false,
              "switch at pc " + std::to_string(pc) +
                  " must not be the last instruction (default target is "
                  "pc+1)"};
    }
    const std::uint32_t cpIdx = code_[pc].imm;
    if (static_cast<std::size_t>(cpIdx) >= cp_.size() ||
        cp_[cpIdx].kind != Const::Kind::SwitchTable) {
      return {false, "switch at pc " + std::to_string(pc) +
                         " does not reference a switch table"};
    }
    const std::vector<std::int32_t>& ints = cp_[cpIdx].ints;
    const std::int64_t sz = static_cast<std::int64_t>(ints.size());
    if (code_[pc].opcode() == Op::Tableswitch) {
      const std::int64_t low = ints.size() > 0 ? ints[0] : 0;
      const std::int64_t high = ints.size() > 1 ? ints[1] : -1;
      if (sz < 3 || 3 + (high - low) + 1 != sz) {
        return {false, "switch at pc " + std::to_string(pc) +
                           " has a malformed tableswitch payload"};
      }
    } else {
      const std::int64_t n = ints.size() > 0 ? ints[0] : -1;
      if (sz < 2 || 2 + 2 * n != sz || n < 0) {
        return {false, "switch at pc " + std::to_string(pc) +
                           " has a malformed lookupswitch payload"};
      }
    }
  }

  // 4. Frame sizing ceiling (register fields are u16).
  if (numRegs_ > kMaxRegs) {
    return {false,
            "too many registers (" + std::to_string(numRegs_) +
                ", max 65536)"};
  }

  // 5. Register and slot bounds. Checks run against the FINAL numRegs_ /
  // numLocals_ so callers may size the method after emission. Ops whose
  // Sig fields play non-register roles get explicit handling:
  //   Call/CallQuick — b is an argument COUNT; dst is unused for void
  //     callees (callee descriptor via cp when available);
  //   Multianewarray — b is the dimension count (dims live in a..a+b-1);
  //   Putfield/PutfieldQuick — dst is canonical 0/unused (V-S10).
  for (std::size_t pc = 0; pc < code_.size(); ++pc) {
    const Ins& ins = code_[pc];
    const Op op = ins.opcode();
    const Sig sig = info(op).sig;
    const bool isCall = (sig == Sig::Call || sig == Sig::CallQuick);
    const bool isPutfield = (op == Op::Putfield || op == Op::PutfieldQuick);
    const bool isMana = (op == Op::Multianewarray);
    if (isCall) {
      if (callWritesResult(cp_, ins) && ins.dst >= numRegs_) {
        return {false, regRangeError(ins.dst, pc, numRegs_)};
      }
      if (ins.b > 0 &&
          static_cast<std::uint32_t>(ins.a) + ins.b > numRegs_) {
        return {false,
                "call argument registers out of range at pc " +
                    std::to_string(pc) + " (r" + std::to_string(ins.a) +
                    " + " + std::to_string(ins.b) + " > numRegs " +
                    std::to_string(numRegs_) + ")"};
      }
    } else if (isMana) {
      if (ins.dst >= numRegs_) {
        return {false, regRangeError(ins.dst, pc, numRegs_)};
      }
      if (ins.a >= numRegs_) {
        return {false, regRangeError(ins.a, pc, numRegs_)};
      }
      if (ins.b > 0 &&
          static_cast<std::uint32_t>(ins.a) + ins.b > numRegs_) {
        return {false,
                "multianewarray dimension registers out of range at pc " +
                    std::to_string(pc) + " (r" + std::to_string(ins.a) +
                    " + " + std::to_string(ins.b) + " > numRegs " +
                    std::to_string(numRegs_) + ")"};
      }
    } else if (isPutfield) {
      if (ins.a >= numRegs_) {
        return {false, regRangeError(ins.a, pc, numRegs_)};
      }
      if (ins.b >= numRegs_) {
        return {false, regRangeError(ins.b, pc, numRegs_)};
      }
    } else {
      switch (sig) {
      case Sig::None:
      case Sig::Branch:
      case Sig::Trap:
      case Sig::Call:
      case Sig::CallQuick:
        break; // no register operands (Call/CallQuick handled above)
      case Sig::Reg:
      case Sig::RegBranch:
      case Sig::RegCpBranch:
      case Sig::Guard:
      case Sig::GuardCp:
        if (ins.a >= numRegs_) {
          return {false, regRangeError(ins.a, pc, numRegs_)};
        }
        break;
      case Sig::RegImm:
      case Sig::RegCp:
        if (ins.dst >= numRegs_) {
          return {false, regRangeError(ins.dst, pc, numRegs_)};
        }
        break;
      case Sig::RegSlot:
        if (ins.dst >= numRegs_) {
          return {false, regRangeError(ins.dst, pc, numRegs_)};
        }
        if (ins.imm >= numLocals_) {
          return {false, slotRangeError(ins.imm, pc, numLocals_)};
        }
        break;
      case Sig::SlotReg:
        if (ins.a >= numRegs_) {
          return {false, regRangeError(ins.a, pc, numRegs_)};
        }
        if (ins.imm >= numLocals_) {
          return {false, slotRangeError(ins.imm, pc, numLocals_)};
        }
        break;
      case Sig::RegRegBranch:
        if (ins.a >= numRegs_) {
          return {false, regRangeError(ins.a, pc, numRegs_)};
        }
        if (ins.b >= numRegs_) {
          return {false, regRangeError(ins.b, pc, numRegs_)};
        }
        break;
      case Sig::RegReg:
      case Sig::RegRegImm:
      case Sig::RegRegCp:
        if (ins.dst >= numRegs_) {
          return {false, regRangeError(ins.dst, pc, numRegs_)};
        }
        if (ins.a >= numRegs_) {
          return {false, regRangeError(ins.a, pc, numRegs_)};
        }
        break;
      case Sig::RegRegReg:
      case Sig::RegRegRegCp:
        // Includes array stores (P3): dst is the value register (read).
        if (ins.dst >= numRegs_) {
          return {false, regRangeError(ins.dst, pc, numRegs_)};
        }
        if (ins.a >= numRegs_) {
          return {false, regRangeError(ins.a, pc, numRegs_)};
        }
        if (ins.b >= numRegs_) {
          return {false, regRangeError(ins.b, pc, numRegs_)};
        }
        break;
      }
    }
  }

  // 6. Exception handlers (V-S9 shape; cp is final at this point).
  for (const ExceptionHandler& h : handlers_) {
    if (h.start >= h.end || static_cast<std::size_t>(h.end) > code_.size()) {
      return {false,
              "exception handler range out of code (start " +
                  std::to_string(h.start) + ", end " + std::to_string(h.end) +
                  ", code size " + std::to_string(code_.size()) + ")"};
    }
    if (static_cast<std::size_t>(h.handler) >= code_.size()) {
      return {false,
              "exception handler entry out of range (handler " +
                  std::to_string(h.handler) + ", code size " +
                  std::to_string(code_.size()) + ")"};
    }
    if (h.catchType >= 0) {
      const auto ct = static_cast<std::size_t>(h.catchType);
      if (ct >= cp_.size() || cp_[ct].kind != Const::Kind::Class) {
        return {false,
                "exception handler catch type c" +
                    std::to_string(h.catchType) +
                    " is not a Class constant"};
      }
    }
  }

  // 7. Publish. On any failure above, `out` is left untouched.
  out.name = name_;
  out.descriptor = descriptor_;
  out.flags = flags_;
  out.numRegs = numRegs_;
  out.numLocals = numLocals_;
  out.code = code_;
  out.cp = cp_;
  out.handlers = handlers_;
  return {true, ""};
}

} // namespace b2::rbc
