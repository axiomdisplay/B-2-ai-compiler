// B-2 codegen - stencil body emission: one machine-code template per
// manifest stencil, plus the internal instantiation templates.
//
// WHY THIS FILE EXISTS:
// This is the codegen team's side of docs/stencils.md SS5 ("generated
// opcode templates"): the single place that knows what an RBC op's compiled
// form looks like. tools/stencilgen calls buildArchive() at BUILD time,
// emits the archive, and the runtime instantiator only ever copies and
// patches what comes out of here (Stencil Rule 1).
//
// BODY CONVENTIONS (docs/codegen_contract.md SS3/SS6/SS8):
// - RBP = activation base; slots at [rbp + kSlotsBase + idx*16 + delta].
// - Every result write stores payload FIRST, then the type tag, so a tag
//   never names a stale payload (observability discipline).
// - Int payloads may be stored sign-extended (T0 stores zero-extended);
//   readers use 32-bit accesses only - pinned in the contract SS3.
// - Helper-mediated ops follow the uniform tail:
//     mov rdi, rbp; <args>; call helper;
//     mov [rbp+K_TrapKind], eax; test eax, eax; jne <deopt thunk>
//   Helpers write their own results into activation slots and never return
//   on trap paths without leaving act->pending_exc set.
// - Fusions must not write any intermediate register that is dead at the
//   fusion-start boundary (regs die at handler entry; frames that unwind
//   are discarded - the observable state is locals + monitors only).

#include "compiler/codegen/src/X64Emitter.h"

#include <cstdint>
#include <string>
#include <vector>

#include "b2/baseline/StencilSet.h"
#include "b2/codegen/Archive.h"
#include "b2/codegen/Instantiate.h"
#include "b2/rbc/Opcode.h"

namespace b2::codegen {

// RType tag values as stored in Value.type (cross-checked below).
inline constexpr std::uint32_t kTagBottom = 0;
inline constexpr std::uint32_t kTagInt = 1;
inline constexpr std::uint32_t kTagLong = 2;
inline constexpr std::uint32_t kTagFloat = 3;
inline constexpr std::uint32_t kTagDouble = 4;
inline constexpr std::uint32_t kTagNull = 5;
inline constexpr std::uint32_t kTagRef = 6;

static_assert(kTagBottom == static_cast<std::uint32_t>(rbc::RType::Bottom));
static_assert(kTagInt == static_cast<std::uint32_t>(rbc::RType::Int));
static_assert(kTagLong == static_cast<std::uint32_t>(rbc::RType::Long));
static_assert(kTagFloat == static_cast<std::uint32_t>(rbc::RType::Float));
static_assert(kTagDouble == static_cast<std::uint32_t>(rbc::RType::Double));
static_assert(kTagNull == static_cast<std::uint32_t>(rbc::RType::Null));
static_assert(kTagRef == static_cast<std::uint32_t>(rbc::RType::Ref));

namespace {

using PS = PatchSource;
using HK = PatchKind;
using HT = HoleTag;

// jcc condition numbers (0..15) matching X64::jccHole/setcc.
inline constexpr std::uint8_t kCcO = 0, kCcNo = 1, kCcB = 2, kCcAe = 3,
  kCcE = 4, kCcNe = 5, kCcBe = 6, kCcA = 7, kCcS = 8, kCcNs = 9,
  kCcP = 10, kCcNp = 11, kCcL = 12, kCcGe = 13, kCcLe = 14, kCcG = 15;

// The uniform helper-call trap tail (after args are in place).
void helperTail(X64& x, std::uint8_t helperId) {
  x.callHelper(helperId);
  x.storeEaxToAct(kActOffTrapKind);
  x.testReg32(kA, kA);
  x.jccHole(kCcNe, PS::DeoptIdSource, HT::Layout);
}

// One helper arg: either a hole (kind/source/tag/step) or a constant.
struct Arg {
  HK kind = HK::Imm32;
  PS source = PS::None;
  HT tag = HT::Plan;
  std::uint8_t step = 0;
  bool isConst = false;
  std::uint32_t constant = 0;
};
[[nodiscard]] constexpr Arg argHole(HK k, PS s, HT t, std::uint8_t step = 0) {
  return Arg{k, s, t, step, false, 0};
}
[[nodiscard]] constexpr Arg argConst(std::uint32_t v) {
  return Arg{HK::Imm32, PS::None, HT::Plan, 0, true, v};
}

// Full helper call with imm32 args (arg2..arg5); unused args are constants.
void helperCall(X64& x, std::uint8_t helperId, const Arg& a2,
                const Arg& a3, const Arg& a4, const Arg& a5 = argConst(0)) {
  x.passActivation();
  if (a2.isConst) { x.movRegImm32(kHelperArg2, a2.constant); }
  else { x.arg2Imm32Hole(a2.kind, a2.source, a2.tag, a2.step, 0); }
  if (a3.isConst) { x.movRegImm32(kHelperArg3, a3.constant); }
  else { x.arg3Imm32Hole(a3.kind, a3.source, a3.tag, a3.step, 0); }
  if (a4.isConst) { x.movRegImm32(kHelperArg4, a4.constant); }
  else { x.arg4Imm32Hole(a4.kind, a4.source, a4.tag, a4.step, 0); }
  if (a5.isConst) { x.movRegImm32(kHelperArg5, a5.constant); }
  else { x.arg5Imm32Hole(a5.kind, a5.source, a5.tag, a5.step, 0); }
  helperTail(x, helperId);
}

// Store RAX (payload) + tag to the dst register slot of `step`.
void storeResult64(X64& x, std::uint8_t step, std::uint32_t tag) {
  x.storeSlot(kA, PS::InsDst, step, kDeltaPayload);
  x.storeImm32Slot(PS::InsDst, tag, step, kDeltaTag);
}

// movsxd rax, eax then store (int results).
void storeResult32(X64& x, std::uint8_t step) {
  x.movsxdReg(kA, kA);
  storeResult64(x, step, kTagInt);
}

// --- int binary arithmetic (non-trapping) ---------------------------------------
void intBinary(X64& x, X64::Alu op) {
  x.loadSlot32(kA, PS::InsA);
  x.aluSlot32(kA, op, PS::InsB);
  storeResult32(x, 0);
}

void intMul(X64& x) {
  x.loadSlot32(kA, PS::InsA);
  x.imulSlot32(kA, PS::InsB);
  storeResult32(x, 0);
}

// --- int division family (traps on zero divisor) ---------------------------------
void intDivRem(X64& x, bool remainder) {
  x.loadSlot32(kA, PS::InsA);
  x.loadSlot32(kT, PS::InsB);
  x.testReg32(kT, kT);
  x.jccHole(kCcE, PS::DeoptIdSource, HT::Layout); // zero divisor -> thunk
  x.cdq();
  x.idivReg32(kT);
  if (remainder) {
    x.movRegReg(kA, kC); // remainder lives in edx
  }
  storeResult32(x, 0);
}

// --- float / double compare producing -1/0/1 -------------------------------------
void floatCompare(X64& x, bool isDouble, bool nanGreater) {
  // xor FIRST (it sets FLAGS; the ucomis flags must survive to the setcc).
  x.xorRegReg32(kB, kB);
  if (isDouble) {
    x.loadDouble(X64::Xmm::X0, PS::InsA);
    x.ucomisDouble(X64::Xmm::X0, PS::InsB);
  } else {
    x.loadFloat(X64::Xmm::X0, PS::InsA);
    x.ucomisFloat(X64::Xmm::X0, PS::InsB);
  }
  const int nan = x.newLabel();
  const int write = x.newLabel();
  x.jccLocal(kCcP, nan);
  x.setcc(kB, kCcA);
  x.setcc(kC, kCcB);
  x.movzxR32R8(kC, kC);
  x.aluRegReg32(kB, X64::Alu::Sub, kC);
  x.jmpLocal(write);
  x.bind(nan);
  x.movRegImm32(kB, nanGreater ? 1u : 0xFFFF'FFFFu);
  x.bind(write);
  x.movsxdReg(kA, kB);
  storeResult64(x, 0, kTagInt);
}

// --- conversions --------------------------------------------------------------------
// Load a float/double boundary constant into xmm1.
void loadBoundary(X64& x, bool isDouble, std::uint64_t doubleBits,
                  std::uint32_t floatBits) {
  if (isDouble) {
    x.movRegImm64(kT, doubleBits);
    // movq xmm1, r10 (66 49 0F 6E CA)
    x.u8(0x66); x.u8(0x49); x.u8(0x0F); x.u8(0x6E); x.u8(0xCA);
  } else {
    x.movRegImm32(kA, floatBits);
    x.movdToXmm1(kA); // movd xmm1, eax
  }
}

void convertFloatToInt32(X64& x, bool isDouble) {
  if (isDouble) {
    x.loadDouble(X64::Xmm::X0, PS::InsA);
  } else {
    x.loadFloat(X64::Xmm::X0, PS::InsA);
  }
  const int nan = x.newLabel();
  const int over = x.newLabel();
  const int under = x.newLabel();
  const int write = x.newLabel();
  x.ucomisSelf(isDouble); // NaN check: compare the value with ITSELF
  x.jccLocal(kCcP, nan);
  loadBoundary(x, isDouble, 0x41E0000000000000ull, 0x4F000000u); // 2^31
  if (isDouble) {
    x.u8(0x66); x.u8(0x0F); x.u8(0x2E); x.u8(0xC1); // ucomisd xmm0, xmm1
  } else {
    x.u8(0x0F); x.u8(0x2E); x.u8(0xC1); // ucomiss xmm0, xmm1
  }
  x.jccLocal(kCcAe, over);
  loadBoundary(x, isDouble, 0xC1E0000000000000ull, 0xCF000000u); // -2^31
  if (isDouble) {
    x.u8(0x66); x.u8(0x0F); x.u8(0x2E); x.u8(0xC1);
  } else {
    x.u8(0x0F); x.u8(0x2E); x.u8(0xC1);
  }
  x.jccLocal(kCcBe, under);
  if (isDouble) {
    x.cvttDoubleToI32();
  } else {
    x.cvttFloatToI32();
  }
  x.jmpLocal(write);
  x.bind(over);
  x.movRegImm32(kA, 0x7FFF'FFFFu);
  x.jmpLocal(write);
  x.bind(under);
  x.movRegImm32(kA, 0x8000'0000u);
  x.jmpLocal(write);
  x.bind(nan);
  x.xorRegReg32(kA, kA);
  x.bind(write);
  storeResult32(x, 0);
}

void convertFloatToInt64(X64& x, bool isDouble) {
  if (isDouble) {
    x.loadDouble(X64::Xmm::X0, PS::InsA);
  } else {
    x.loadFloat(X64::Xmm::X0, PS::InsA);
  }
  const int nan = x.newLabel();
  const int over = x.newLabel();
  const int under = x.newLabel();
  const int write = x.newLabel();
  x.ucomisSelf(isDouble); // NaN check: compare the value with ITSELF
  x.jccLocal(kCcP, nan);
  loadBoundary(x, isDouble, 0x43E0000000000000ull, 0x5F000000u); // 2^63
  if (isDouble) {
    x.u8(0x66); x.u8(0x0F); x.u8(0x2E); x.u8(0xC1);
  } else {
    x.u8(0x0F); x.u8(0x2E); x.u8(0xC1);
  }
  x.jccLocal(kCcAe, over);
  loadBoundary(x, isDouble, 0xC3E0000000000000ull, 0xDF000000u); // -2^63
  if (isDouble) {
    x.u8(0x66); x.u8(0x0F); x.u8(0x2E); x.u8(0xC1);
  } else {
    x.u8(0x0F); x.u8(0x2E); x.u8(0xC1);
  }
  x.jccLocal(kCcBe, under);
  if (isDouble) {
    x.cvttDoubleToI64();
  } else {
    x.cvttFloatToI64();
  }
  x.jmpLocal(write);
  x.bind(over);
  x.movRegImm64(kA, 0x7FFF'FFFF'FFFF'FFFFull);
  x.jmpLocal(write);
  x.bind(under);
  x.movRegImm64(kA, 0x8000'0000'0000'0000ull);
  x.jmpLocal(write);
  x.bind(nan);
  x.xorRegReg32(kA, kA);
  x.bind(write);
  storeResult64(x, 0, kTagLong);
}

// --- branch bodies --------------------------------------------------------------------
void branchZero(X64& x, std::uint8_t cc) {
  x.loadSlot32(kA, PS::InsA);
  x.aluImm32_32(kA, X64::Alu::Cmp, 0);
  x.jccHole(cc, PS::BranchTarget, HT::Plan);
}

void branchIcmp(X64& x, std::uint8_t cc) {
  x.loadSlot32(kA, PS::InsA);
  x.aluSlot32(kA, X64::Alu::Cmp, PS::InsB);
  x.jccHole(cc, PS::BranchTarget, HT::Plan);
}

void branchAcmp(X64& x, bool equal) {
  const int done = x.newLabel();
  const int eq = x.newLabel();
  x.xorRegReg32(kB, kB); // ecx = 0
  x.loadSlot32(kA, PS::InsA, 0, kDeltaTag);
  x.aluSlot32(kA, X64::Alu::Cmp, PS::InsB, 0, kDeltaTag);
  x.jccLocal(kCcNe, done); // tags differ -> not equal
  x.aluImm32_32(kA, X64::Alu::Cmp, kTagNull);
  x.jccLocal(kCcE, eq); // both null -> equal
  x.loadSlot(kA, PS::InsA);
  x.aluSlot(kA, X64::Alu::Cmp, PS::InsB);
  x.jccLocal(kCcE, eq); // same payload -> equal
  x.jmpLocal(done);
  x.bind(eq);
  x.movRegImm32(kB, 1);
  x.bind(done);
  x.testReg32(kB, kB);
  x.jccHole(equal ? kCcNe : kCcE, PS::BranchTarget, HT::Plan);
}

// --- element kinds for array helpers (rbc::Atype codes; Ref = 0) ----------------------
constexpr std::uint32_t kElemRef = 0;

[[nodiscard]] std::uint32_t elemKindOf(rbc::Op op) noexcept {
  switch (op) {
    case rbc::Op::Iaload: case rbc::Op::Iastore: return 10; // Atype::Int
    case rbc::Op::Laload: case rbc::Op::Lastore: return 11;
    case rbc::Op::Faload: case rbc::Op::Fastore: return 6;
    case rbc::Op::Daload: case rbc::Op::Dastore: return 7;
    case rbc::Op::Aaload: case rbc::Op::Aastore: return kElemRef;
    case rbc::Op::Baload: case rbc::Op::Bastore: return 8;
    case rbc::Op::Caload: case rbc::Op::Castore: return 5;
    case rbc::Op::Saload: case rbc::Op::Sastore: return 9;
    default: return kElemRef;
  }
}

// --- call bodies ----------------------------------------------------------------------

void callBody(X64& x, HK targetKind, PS targetSrc) {
  x.passActivation();
  x.arg2Imm32Hole(HK::SlotOffset, PS::InsA, HT::Stream, 0, 0); // arg base
  x.arg3Imm32Hole(HK::Imm32, PS::InsB, HT::Stream, 0, 0);      // arg count
  x.arg4Imm32Hole(targetKind, targetSrc, HT::Plan, 0, 0);      // packed target
  x.arg5Imm32Hole(HK::SlotOffset, PS::InsDst, HT::Stream, 0, 0); // dst
  helperTail(x, static_cast<std::uint8_t>(HelperId::Call));
}

// --- one opcode stencil body ------------------------------------------------------------
//
// Returns false for ops whose compiled form is intentionally EMPTY:
//   - Nop/SafepointPoll emit 1 byte of nop (not empty);
//   - Tableswitch/Lookupswitch: EMPTY record, expanded at instantiation
//     (case chains of __switch_case + __entry/goto);
//   - NeedsRuntimeFeature ops and manifest-only entries: EMPTY records -
//     the plan builder never selects them, the instantiator refuses them.
[[nodiscard]] bool emitOpBody(rbc::Op op, X64& x) {
  const rbc::Sig sig = rbc::info(op).sig;

  switch (op) {
    // --- no-ops ---------------------------------------------------------------
    case rbc::Op::Nop:
    case rbc::Op::SafepointPoll:
      // v1 divergence (documented, codegen_contract SS9): polls are 1-byte
      // nops; nothing sets the T1 safepoint flag yet, so the observable
      // behavior is identical. The real poll lands with the runtime.
      x.nop();
      return true;

    // --- constants --------------------------------------------------------------
    case rbc::Op::AconstNull:
      x.storeImm32Slot(PS::InsDst, kTagNull, 0, kDeltaTag);
      x.storeImm32Slot64(PS::InsDst, 0, 0, kDeltaPayload);
      return true;
    case rbc::Op::Iconst:
      x.storeImm32Slot(PS::InsDst, kTagInt, 0, kDeltaTag);
      x.storeImm32Hole64(PS::InsDst, 0, HK::Imm32, PS::InsImm, HT::Plan, 0);
      return true;
    case rbc::Op::Fconst:
      x.storeImm32Slot(PS::InsDst, kTagFloat, 0, kDeltaTag);
      x.movEaxImm32Hole(HK::Imm32, PS::InsImm, HT::Plan, 0);
      x.storeSlot(kA, PS::InsDst, 0, kDeltaPayload);
      return true;
    case rbc::Op::Lconst:
      x.storeImm32Slot(PS::InsDst, kTagLong, 0, kDeltaTag);
      x.movRaxImm64Hole(HK::Imm64, PS::CpIndex, HT::CpPayload);
      x.storeSlot(kA, PS::InsDst, 0, kDeltaPayload);
      return true;
    case rbc::Op::Dconst:
      x.storeImm32Slot(PS::InsDst, kTagDouble, 0, kDeltaTag);
      x.movRaxImm64Hole(HK::Imm64, PS::CpIndex, HT::CpPayload);
      x.storeSlot(kA, PS::InsDst, 0, kDeltaPayload);
      return true;
    case rbc::Op::Ldc:
      helperCall(x, static_cast<std::uint8_t>(HelperId::LdcConst),
                 argHole(HK::ConstPoolIndex, PS::CpIndex, HT::Plan),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream),
                 argConst(0), argConst(0));
      return true;

    // --- local <-> register moves --------------------------------------------------
    case rbc::Op::Iload: case rbc::Op::Lload: case rbc::Op::Fload:
    case rbc::Op::Dload: case rbc::Op::Aload:
      x.loadValue(X64::Xmm::X0, PS::FrameSlot, 0, kDeltaTag, HT::Plan);
      x.storeValue(X64::Xmm::X0, PS::InsDst, 0, kDeltaTag, HT::Stream);
      return true;
    case rbc::Op::Istore: case rbc::Op::Lstore: case rbc::Op::Fstore:
    case rbc::Op::Dstore: case rbc::Op::Astore:
      x.loadValue(X64::Xmm::X0, PS::InsA, 0, kDeltaTag, HT::Stream);
      x.storeValue(X64::Xmm::X0, PS::FrameSlot, 0, kDeltaTag, HT::Plan);
      return true;
    case rbc::Op::Imove: case rbc::Op::Lmove: case rbc::Op::Fmove:
    case rbc::Op::Dmove: case rbc::Op::Amove:
      x.loadValue(X64::Xmm::X0, PS::InsA, 0, kDeltaTag);
      x.storeValue(X64::Xmm::X0, PS::InsDst, 0, kDeltaTag);
      return true;

    // --- int arithmetic ---------------------------------------------------------------
    case rbc::Op::Iadd: intBinary(x, X64::Alu::Add); return true;
    case rbc::Op::Isub: intBinary(x, X64::Alu::Sub); return true;
    case rbc::Op::Iand: intBinary(x, X64::Alu::And); return true;
    case rbc::Op::Ior:  intBinary(x, X64::Alu::Or);  return true;
    case rbc::Op::Ixor: intBinary(x, X64::Alu::Xor); return true;
    case rbc::Op::Imul: intMul(x); return true;
    case rbc::Op::Idiv: intDivRem(x, false); return true;
    case rbc::Op::Irem: intDivRem(x, true); return true;
    case rbc::Op::Ineg:
      x.loadSlot32(kA, PS::InsA);
      x.negReg32(kA);
      storeResult32(x, 0);
      return true;
    case rbc::Op::Ishl: case rbc::Op::Ishr: case rbc::Op::Iushr:
      x.loadSlot32(kA, PS::InsA);
      x.loadSlot32(kB, PS::InsB); // hardware masks cl to 5 bits
      x.shiftByCl(kA, op == rbc::Op::Ishl ? X64::Shift::Shl
                    : op == rbc::Op::Ishr ? X64::Shift::Sar
                                          : X64::Shift::Shr,
                  false);
      storeResult32(x, 0);
      return true;
    case rbc::Op::Iinc:
      x.loadSlot32(kA, PS::InsDst, 0, kDeltaPayload);
      x.addEaxImm32Hole(HK::Imm32, PS::InsImm, HT::Plan, 0);
      storeResult32(x, 0);
      return true;

    // --- long arithmetic ------------------------------------------------------------------
    case rbc::Op::Ladd: case rbc::Op::Lsub: case rbc::Op::Land:
    case rbc::Op::Lor: case rbc::Op::Lxor: {
      const X64::Alu a = op == rbc::Op::Ladd ? X64::Alu::Add
                         : op == rbc::Op::Lsub ? X64::Alu::Sub
                         : op == rbc::Op::Land ? X64::Alu::And
                         : op == rbc::Op::Lor ? X64::Alu::Or
                                              : X64::Alu::Xor;
      x.loadSlot(kA, PS::InsA);
      x.aluSlot(kA, a, PS::InsB);
      storeResult64(x, 0, kTagLong);
      return true;
    }
    case rbc::Op::Lmul:
      x.loadSlot(kA, PS::InsA);
      x.imulSlot(kA, PS::InsB);
      storeResult64(x, 0, kTagLong);
      return true;
    case rbc::Op::Ldiv: case rbc::Op::Lrem: {
      x.loadSlot(kA, PS::InsA);
      x.loadSlot(kT, PS::InsB);
      // test r10, r10 (REX.W 85 /r)
      x.u8(0x4D); x.u8(0x85); x.u8(0xD2);
      x.jccHole(kCcE, PS::DeoptIdSource, HT::Layout);
      x.cqo();
      x.idivReg64(kT);
      if (op == rbc::Op::Lrem) {
        x.movRegReg(kA, kC);
      }
      storeResult64(x, 0, kTagLong);
      return true;
    }
    case rbc::Op::Lneg:
      x.loadSlot(kA, PS::InsA);
      x.negReg64(kA);
      storeResult64(x, 0, kTagLong);
      return true;
    case rbc::Op::Lshl: case rbc::Op::Lshr: case rbc::Op::Lushr:
      x.loadSlot(kA, PS::InsA);
      x.loadSlot(kB, PS::InsB); // hardware masks cl to 6 bits
      x.shiftByCl(kA, op == rbc::Op::Lshl ? X64::Shift::Shl
                    : op == rbc::Op::Lshr ? X64::Shift::Sar
                                          : X64::Shift::Shr,
                  true);
      storeResult64(x, 0, kTagLong);
      return true;

    // --- float arithmetic --------------------------------------------------------------------
    case rbc::Op::Fadd: case rbc::Op::Fsub: case rbc::Op::Fmul:
    case rbc::Op::Fdiv: {
      const X64::SseOp s = op == rbc::Op::Fadd ? X64::SseOp::Add
                         : op == rbc::Op::Fsub ? X64::SseOp::Sub
                         : op == rbc::Op::Fmul ? X64::SseOp::Mul
                                               : X64::SseOp::Div;
      x.loadFloat(X64::Xmm::X0, PS::InsA);
      x.sseFloat(X64::Xmm::X0, s, PS::InsB);
      x.storeFloat(X64::Xmm::X0, PS::InsDst);
      x.storeImm32Slot(PS::InsDst, kTagFloat, 0, kDeltaTag);
      return true;
    }
    case rbc::Op::Fneg:
      x.loadSlot(kA, PS::InsA);
      x.xorEaxImm32(0x8000'0000u);
      x.storeSlot(kA, PS::InsDst, 0, kDeltaPayload);
      x.storeImm32Slot(PS::InsDst, kTagFloat, 0, kDeltaTag);
      return true;
    case rbc::Op::Frem:
      x.passActivation();
      x.loadFloat(X64::Xmm::X0, PS::InsA);
      x.loadFloat(X64::Xmm::X1, PS::InsB);
      x.callHelper(static_cast<std::uint8_t>(HelperId::FmodF));
      x.storeFloat(X64::Xmm::X0, PS::InsDst);
      x.storeImm32Slot(PS::InsDst, kTagFloat, 0, kDeltaTag);
      return true;
    case rbc::Op::Dadd: case rbc::Op::Dsub: case rbc::Op::Dmul:
    case rbc::Op::Ddiv: {
      const X64::SseOp s = op == rbc::Op::Dadd ? X64::SseOp::Add
                         : op == rbc::Op::Dsub ? X64::SseOp::Sub
                         : op == rbc::Op::Dmul ? X64::SseOp::Mul
                                               : X64::SseOp::Div;
      x.loadDouble(X64::Xmm::X0, PS::InsA);
      x.sseDouble(X64::Xmm::X0, s, PS::InsB);
      x.storeDouble(X64::Xmm::X0, PS::InsDst);
      x.storeImm32Slot(PS::InsDst, kTagDouble, 0, kDeltaTag);
      return true;
    }
    case rbc::Op::Dneg:
      x.loadSlot(kA, PS::InsA);
      x.movRegImm64(kB, 0x8000'0000'0000'0000ull);
      x.aluRegReg(kA, X64::Alu::Xor, kB);
      x.storeSlot(kA, PS::InsDst, 0, kDeltaPayload);
      x.storeImm32Slot(PS::InsDst, kTagDouble, 0, kDeltaTag);
      return true;
    case rbc::Op::Drem:
      x.passActivation();
      x.loadDouble(X64::Xmm::X0, PS::InsA);
      x.loadDouble(X64::Xmm::X1, PS::InsB);
      x.callHelper(static_cast<std::uint8_t>(HelperId::FmodD));
      x.storeDouble(X64::Xmm::X0, PS::InsDst);
      x.storeImm32Slot(PS::InsDst, kTagDouble, 0, kDeltaTag);
      return true;

    // --- comparisons ---------------------------------------------------------------------------
    case rbc::Op::Icmp:
    case rbc::Op::Lcmp: {
      // The -1/0/1 result is built in ECX (kB); sign-extend IT into RAX
      // for the store (storeResult32 extends kA). NOTE: xor sets FLAGS,
      // so the zeroing MUST precede the compare (a lesson pinned by the
      // differential tests: xor-after-cmp silently reads zeroed flags).
      x.xorRegReg32(kB, kB);
      if (op == rbc::Op::Icmp) {
        x.loadSlot32(kA, PS::InsA);
        x.aluSlot32(kA, X64::Alu::Cmp, PS::InsB);
      } else {
        x.loadSlot(kA, PS::InsA);
        x.aluSlot(kA, X64::Alu::Cmp, PS::InsB);
      }
      x.setcc(kB, kCcA);
      x.setcc(kC, kCcB);
      x.movzxR32R8(kC, kC);
      x.aluRegReg32(kB, X64::Alu::Sub, kC);
      x.movsxdReg(kA, kB);
      storeResult64(x, 0, kTagInt);
      return true;
    }
    case rbc::Op::Fcmpl: floatCompare(x, false, false); return true;
    case rbc::Op::Fcmpg: floatCompare(x, false, true); return true;
    case rbc::Op::Dcmpl: floatCompare(x, true, false); return true;
    case rbc::Op::Dcmpg: floatCompare(x, true, true); return true;

    // --- conversions ------------------------------------------------------------------------------
    case rbc::Op::I2l:
      x.movsxdSlot(kA, PS::InsA);
      storeResult64(x, 0, kTagLong);
      return true;
    case rbc::Op::L2i:
      x.loadSlot32(kA, PS::InsA); // truncate + zero-extend
      x.storeSlot(kA, PS::InsDst, 0, kDeltaPayload);
      x.storeImm32Slot(PS::InsDst, kTagInt, 0, kDeltaTag);
      return true;
    case rbc::Op::I2f:
      x.cvtsi2ssSlot(PS::InsA, false);
      x.storeFloat(X64::Xmm::X0, PS::InsDst);
      x.storeImm32Slot(PS::InsDst, kTagFloat, 0, kDeltaTag);
      return true;
    case rbc::Op::I2d:
      x.cvtsi2sdSlot(PS::InsA, false);
      x.storeDouble(X64::Xmm::X0, PS::InsDst);
      x.storeImm32Slot(PS::InsDst, kTagDouble, 0, kDeltaTag);
      return true;
    case rbc::Op::L2f:
      x.cvtsi2ssSlot(PS::InsA, true);
      x.storeFloat(X64::Xmm::X0, PS::InsDst);
      x.storeImm32Slot(PS::InsDst, kTagFloat, 0, kDeltaTag);
      return true;
    case rbc::Op::L2d:
      x.cvtsi2sdSlot(PS::InsA, true);
      x.storeDouble(X64::Xmm::X0, PS::InsDst);
      x.storeImm32Slot(PS::InsDst, kTagDouble, 0, kDeltaTag);
      return true;
    case rbc::Op::F2i: convertFloatToInt32(x, false); return true;
    case rbc::Op::D2i: convertFloatToInt32(x, true); return true;
    case rbc::Op::F2l: convertFloatToInt64(x, false); return true;
    case rbc::Op::D2l: convertFloatToInt64(x, true); return true;
    case rbc::Op::F2d:
      x.loadFloat(X64::Xmm::X0, PS::InsA);
      x.cvtFloatDouble(true);
      x.storeDouble(X64::Xmm::X0, PS::InsDst);
      x.storeImm32Slot(PS::InsDst, kTagDouble, 0, kDeltaTag);
      return true;
    case rbc::Op::D2f:
      x.loadDouble(X64::Xmm::X0, PS::InsA);
      x.cvtFloatDouble(false);
      x.storeFloat(X64::Xmm::X0, PS::InsDst);
      x.storeImm32Slot(PS::InsDst, kTagFloat, 0, kDeltaTag);
      return true;
    case rbc::Op::I2b:
      x.extendLoad(kA, X64::Ext::Sx8, PS::InsA);
      storeResult32(x, 0);
      return true;
    case rbc::Op::I2c:
      x.extendLoad(kA, X64::Ext::Zx16, PS::InsA);
      storeResult32(x, 0);
      return true;
    case rbc::Op::I2s:
      x.extendLoad(kA, X64::Ext::Sx16, PS::InsA);
      storeResult32(x, 0);
      return true;

    // --- branches ----------------------------------------------------------------------------------
    case rbc::Op::Goto:
      x.jmpHole(PS::BranchTarget, HT::Plan);
      return true;
    case rbc::Op::Ifeq: branchZero(x, kCcE); return true;
    case rbc::Op::Ifne: branchZero(x, kCcNe); return true;
    case rbc::Op::Iflt: branchZero(x, kCcL); return true;
    case rbc::Op::Ifge: branchZero(x, kCcGe); return true;
    case rbc::Op::Ifgt: branchZero(x, kCcG); return true;
    case rbc::Op::Ifle: branchZero(x, kCcLe); return true;
    case rbc::Op::Ifnull:
      x.loadSlot32(kA, PS::InsA, 0, kDeltaTag);
      x.aluImm32_32(kA, X64::Alu::Cmp, kTagNull);
      x.jccHole(kCcE, PS::BranchTarget, HT::Plan);
      return true;
    case rbc::Op::Ifnonnull:
      x.loadSlot32(kA, PS::InsA, 0, kDeltaTag);
      x.aluImm32_32(kA, X64::Alu::Cmp, kTagNull);
      x.jccHole(kCcNe, PS::BranchTarget, HT::Plan);
      return true;
    case rbc::Op::IfIcmpeq: branchIcmp(x, kCcE); return true;
    case rbc::Op::IfIcmpne: branchIcmp(x, kCcNe); return true;
    case rbc::Op::IfIcmplt: branchIcmp(x, kCcL); return true;
    case rbc::Op::IfIcmpge: branchIcmp(x, kCcGe); return true;
    case rbc::Op::IfIcmpgt: branchIcmp(x, kCcG); return true;
    case rbc::Op::IfIcmple: branchIcmp(x, kCcLe); return true;
    case rbc::Op::IfAcmpeq: branchAcmp(x, true); return true;
    case rbc::Op::IfAcmpne: branchAcmp(x, false); return true;
    case rbc::Op::Tableswitch:
    case rbc::Op::Lookupswitch:
      // EMPTY record: the instantiator expands the case chain from
      // cp[imm].ints (kInternalSwitchCase copies + a goto for default).
      return false;

    // --- fields ------------------------------------------------------------------------------------
    case rbc::Op::Getfield:
    case rbc::Op::GetfieldQuick:
      helperCall(x, static_cast<std::uint8_t>(HelperId::GetField),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argHole(HK::FieldOffset,
                         op == rbc::Op::Getfield ? PS::RuntimeField : PS::InsImm,
                         HT::Plan),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream));
      return true;
    case rbc::Op::Putfield:
    case rbc::Op::PutfieldQuick:
      helperCall(x, static_cast<std::uint8_t>(HelperId::PutField),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argHole(HK::FieldOffset,
                         op == rbc::Op::Putfield ? PS::RuntimeField : PS::InsImm,
                         HT::Plan),
                 argHole(HK::SlotOffset, PS::InsB, HT::Stream));
      return true;
    case rbc::Op::Getstatic:
      helperCall(x, static_cast<std::uint8_t>(HelperId::GetStatic),
                 argHole(HK::FieldOffset, PS::RuntimeField, HT::Plan),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream),
                 argConst(0));
      return true;
    case rbc::Op::Putstatic:
      // Sig::RegCp: the VALUE travels in dst (rbc_spec "field = <dst>").
      helperCall(x, static_cast<std::uint8_t>(HelperId::PutStatic),
                 argHole(HK::FieldOffset, PS::RuntimeField, HT::Plan),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream),
                 argConst(0));
      return true;

    // --- arrays ------------------------------------------------------------------------------------
    case rbc::Op::NewArray:
      helperCall(x, static_cast<std::uint8_t>(HelperId::NewArray),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argHole(HK::Imm32, PS::InsImm, HT::Stream),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream),
                 argConst(0)); // flags: primitive array
      return true;
    case rbc::Op::AnewArray:
      helperCall(x, static_cast<std::uint8_t>(HelperId::NewArray),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argHole(HK::KlassId, PS::RuntimeClass, HT::Plan),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream),
                 argConst(1)); // flags: ref array
      return true;
    case rbc::Op::Arraylength:
      helperCall(x, static_cast<std::uint8_t>(HelperId::ArrayLength),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream),
                 argConst(0));
      return true;
    case rbc::Op::Iaload: case rbc::Op::Laload: case rbc::Op::Faload:
    case rbc::Op::Daload: case rbc::Op::Aaload: case rbc::Op::Baload:
    case rbc::Op::Caload: case rbc::Op::Saload:
      helperCall(x, static_cast<std::uint8_t>(HelperId::ArrayLoad),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argHole(HK::SlotOffset, PS::InsB, HT::Stream),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream),
                 argConst(elemKindOf(op)));
      return true;
    case rbc::Op::Iastore: case rbc::Op::Lastore: case rbc::Op::Fastore:
    case rbc::Op::Dastore: case rbc::Op::Aastore: case rbc::Op::Bastore:
    case rbc::Op::Castore: case rbc::Op::Sastore:
      helperCall(x, static_cast<std::uint8_t>(HelperId::ArrayStore),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argHole(HK::SlotOffset, PS::InsB, HT::Stream),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream),
                 argConst(elemKindOf(op)));
      return true;
    case rbc::Op::Multianewarray:
      // v1 honest gap: no body; the instantiator refuses methods reaching
      // here (the method stays on T0). Documented in SS12.
      return false;

    // --- objects / monitors / throws ------------------------------------------------------------------
    case rbc::Op::New:
      helperCall(x, static_cast<std::uint8_t>(HelperId::NewObject),
                 argHole(HK::KlassId, PS::RuntimeClass, HT::Plan),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream),
                 argConst(0));
      return true;
    case rbc::Op::Checkcast:
      helperCall(x, static_cast<std::uint8_t>(HelperId::CheckCast),
                 argHole(HK::KlassId, PS::RuntimeClass, HT::Plan),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream));
      return true;
    case rbc::Op::Instanceof:
      helperCall(x, static_cast<std::uint8_t>(HelperId::InstanceOf),
                 argHole(HK::KlassId, PS::RuntimeClass, HT::Plan),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argHole(HK::SlotOffset, PS::InsDst, HT::Stream));
      return true;
    case rbc::Op::Monitorenter:
      helperCall(x, static_cast<std::uint8_t>(HelperId::MonitorEnter),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argConst(0), argConst(0));
      return true;
    case rbc::Op::Monitorexit:
      helperCall(x, static_cast<std::uint8_t>(HelperId::MonitorExit),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argConst(0), argConst(0));
      return true;
    case rbc::Op::Athrow:
      helperCall(x, static_cast<std::uint8_t>(HelperId::Athrow),
                 argHole(HK::SlotOffset, PS::InsA, HT::Stream),
                 argConst(0), argConst(0));
      return true;

    // --- calls ------------------------------------------------------------------------------------------
    case rbc::Op::InvokestaticQuick:
    case rbc::Op::InvokespecialQuick:
      callBody(x, HK::MethodId, PS::InsImm);
      return true;
    case rbc::Op::InvokevirtualQuick:
    case rbc::Op::InvokeinterfaceQuick:
      callBody(x, HK::CallSiteId, PS::InsImm);
      return true;
    case rbc::Op::Invokestatic:
    case rbc::Op::Invokespecial:
      callBody(x, HK::MethodId, PS::RuntimeMethod);
      return true;
    case rbc::Op::Invokevirtual:
    case rbc::Op::Invokeinterface:
      callBody(x, HK::MethodId, PS::RuntimeMethod);
      return true;

    // --- returns -------------------------------------------------------------------------------------------
    case rbc::Op::Return:
      x.xorRegReg32(kA, kA);
      x.popRbp();
      x.ret();
      return true;
    case rbc::Op::Ireturn: case rbc::Op::Lreturn: case rbc::Op::Freturn:
    case rbc::Op::Dreturn: case rbc::Op::Areturn:
      x.loadValue(X64::Xmm::X0, PS::InsA, 0, kDeltaTag);
      x.storeValueToAct(X64::Xmm::X0, kActOffRetValue);
      x.xorRegReg32(kA, kA);
      x.popRbp();
      x.ret();
      return true;

    // --- guards -----------------------------------------------------------------------------------------------
    case rbc::Op::GuardNonNull:
      x.loadSlot32(kA, PS::InsA, 0, kDeltaTag);
      x.aluImm32_32(kA, X64::Alu::Cmp, kTagNull);
      x.jccHole(kCcE, PS::DeoptIdSource, HT::Plan);
      return true;

    // --- never-selected / runtime-feature ops -----------------------------------------------------------------
    case rbc::Op::Invokedynamic:
    case rbc::Op::GuardClass:
    case rbc::Op::DeoptTrap:
      return false;

    default:
      break;
  }
  (void)sig;
  return false;
}

// --- superinstruction bodies (the seven v0 fusions) ------------------------------------------
//
// Step indices matter: Stream/Plan holes record which pattern instruction
// supplies the operand. The fused body only writes values that are live at
// the fusion-START boundary or later (the intermediate producer registers
// are dead: regs die at handler entry, unwound frames are discarded).

void emitSuperBody(const baseline::StencilDesc& d, X64& x) {
  const std::string_view name = d.name;
  if (name == "iload_iload_iadd" || name == "iload_iload_isub" ||
      name == "iload_iload_imul") {
    x.loadSlot32(kA, PS::FrameSlot, 0, kDeltaPayload, HT::Plan);
    if (name == "iload_iload_imul") {
      x.imulSlot32(kA, PS::FrameSlot, 1, HT::Plan);
    } else {
      const X64::Alu alu = name == "iload_iload_iadd" ? X64::Alu::Add
                                                      : X64::Alu::Sub;
      x.aluSlot32(kA, alu, PS::FrameSlot, 1, kDeltaPayload, HT::Plan);
    }
    storeResult32(x, 2);
    return;
  }
  if (name == "aload_getfield") {
    // receiver = locals[l] (step0); field read via helper; result to
    // getfield's dst (step1). The aload's dst register is never written
    // (dead by the producer-consumer link; invisible at the boundary).
    x.passActivation();
    x.arg2Imm32Hole(HK::SlotOffset, PS::FrameSlot, HT::Plan, 0, 0);
    x.arg3Imm32Hole(HK::FieldOffset, PS::RuntimeField, HT::Plan, 1, 0);
    x.arg4Imm32Hole(HK::SlotOffset, PS::InsDst, HT::Stream, 1, 0);
    helperTail(x, static_cast<std::uint8_t>(HelperId::GetField));
    return;
  }
  if (name == "aload_arraylength_if") {
    // len = arraylength(locals[l]) written to step1's dst (live after the
    // fusion); branch if len > step2.a (the consumer register).
    x.passActivation();
    x.arg2Imm32Hole(HK::SlotOffset, PS::FrameSlot, HT::Plan, 0, 0);
    x.arg3Imm32Hole(HK::SlotOffset, PS::InsDst, HT::Stream, 1, 0);
    helperTail(x, static_cast<std::uint8_t>(HelperId::ArrayLength));
    x.loadSlot32(kA, PS::InsA, 2);
    x.aluSlot32(kA, X64::Alu::Cmp, PS::InsB, 2);
    x.jccHole(kCcG, PS::BranchTarget, HT::Plan, 2);
    return;
  }
  if (name == "iinc_goto") {
    x.loadSlot32(kA, PS::InsDst, 0, kDeltaPayload);
    x.addEaxImm32Hole(HK::Imm32, PS::InsImm, HT::Plan, 0);
    storeResult32(x, 0);
    x.jmpHole(PS::BranchTarget, HT::Plan, 1);
    return;
  }  if (name == "aload_getfield_ireturn") {
    // The helper writes the field Value straight into the activation's
    // return slot (the fixed control-block offset as a CONSTANT arg), then
    // the embedded exit sequence returns.
    x.passActivation();
    x.arg2Imm32Hole(HK::SlotOffset, PS::FrameSlot, HT::Plan, 0, 0);
    x.arg3Imm32Hole(HK::FieldOffset, PS::RuntimeField, HT::Plan, 1, 0);
    x.movRegImm32(kHelperArg4, kActOffRetValue);
    helperTail(x, static_cast<std::uint8_t>(HelperId::GetField));
    x.xorRegReg32(kA, kA);
    x.popRbp();
    x.ret();
    return;
  }
  // Unknown fusion: empty body -> instantiation refuses (defensive; the
  // builtin manifest never reaches this).
}

// --- internal templates --------------------------------------------------------------------------

void emitInternalEntry(X64& x) {
  x.pushRbp();
  // mov rbp, rdi (48 89 FD): opcode 89 moves r/m64 <- r64, so reg = rdi
  // (source) and rm = rbp (dest) - the ACTIVATION (rdi, the SysV first
  // argument) becomes the frame base.
  x.u8(0x48); x.u8(0x89); x.u8(0xFD);
  x.jmpHole(PS::BranchTarget, HT::Layout);
}

void emitInternalDeoptThunk(X64& x) {
  x.u8(0xBF); // mov edi, imm32 (deopt id)
  x.hole(HK::Imm32, PS::None, HT::Layout, 4);
  x.u8(0xBE); // mov esi, imm32 (trap-site native offset)
  x.hole(HK::Imm32, PS::None, HT::Layout, 4);
  x.storeEdiToAct(kActOffDeoptId);
  x.storeEsiToAct(kActOffDeoptPc);
  x.jmpHole(PS::BranchTarget, HT::Layout); // -> shared deopt tail
}

void emitInternalDeoptExit(X64& x) {
  x.storeImmToAct64(kActOffStatus, 1);
  x.popRbp();
  x.movRegImm32(kA, kExitDeopt);
  x.ret();
}

} // namespace

// --- archive assembly (stencilgen entry point) -----------------------------------------------------

[[nodiscard]] Archive buildArchive(const baseline::StencilSet& set) {
  Archive arch;
  arch.header.manifest_count =
      static_cast<std::uint32_t>(set.stencils.size());
  arch.records.reserve(set.stencils.size() + 4);

  // Manifest-aligned records, in StencilId order.
  for (const baseline::StencilDesc& d : set.stencils) {
    ArchiveRecord rec;
    rec.name = d.name;
    X64 x;
    bool emitted = false;
    if (d.pattern_len == 1) {
      emitted = emitOpBody(d.pattern[0].op, x);
    } else if (d.pattern_len >= 2) {
      emitSuperBody(d, x);
      emitted = true;
    }
    // pattern_len == 0 (manifest-only entries) and returned-false ops get
    // empty records: never selected, never instantiated.
    std::vector<EmittedHole> holes;
    if (emitted) {
      x.resolveLocals();
      rec.code = x.takeCode();
      holes = x.takeHoles();
    }
    // Plan-tagged holes: an IN-ORDER SUBSEQUENCE of the manifest's hole
    // list (matched by kind/source at instantiation). We keep the archive's
    // Plan holes exactly as emitted; validateArchive checks subsequence.
    rec.holes.reserve(holes.size());
    for (const EmittedHole& h : holes) {
      ArchiveHole ah = h.hole;
      rec.holes.push_back(ah);
      if (ah.tag == HoleTag::Plan) {
        ++rec.manifest_holes;
      }
    }
    arch.records.push_back(std::move(rec));
  }

  // Internal templates.
  {
    ArchiveRecord entry;
    entry.name = kInternalEntry;
    X64 x;
    emitInternalEntry(x);
    x.resolveLocals();
    entry.code = x.takeCode();
    for (const EmittedHole& h : x.takeHoles()) {
      entry.holes.push_back(h.hole);
    }
    arch.records.push_back(std::move(entry));
  }
  {
    ArchiveRecord thunk;
    thunk.name = kInternalDeoptThunk;
    X64 x;
    emitInternalDeoptThunk(x);
    x.resolveLocals();
    thunk.code = x.takeCode();
    for (const EmittedHole& h : x.takeHoles()) {
      thunk.holes.push_back(h.hole);
    }
    arch.records.push_back(std::move(thunk));
  }
  {
    ArchiveRecord exit_;
    exit_.name = kInternalDeoptExit;
    X64 x;
    emitInternalDeoptExit(x);
    x.resolveLocals();
    exit_.code = x.takeCode();
    for (const EmittedHole& h : x.takeHoles()) {
      exit_.holes.push_back(h.hole);
    }
    arch.records.push_back(std::move(exit_));
  }
  {
    ArchiveRecord scase;
    scase.name = kInternalSwitchCase;
    X64 x;
    // mov eax, dword [rbp + <selector hole>]   (slot payload)
    x.loadSlot32(kA, PS::InsA, 0, kDeltaPayload);
    // cmp eax, <match hole>                    (SwitchMatch)
    x.u8(0x3D); // cmp eax, imm32 (special form A x)
    x.hole(HK::Imm32, PS::None, HT::SwitchMatch, 4);
    // je <target hole>                         (SwitchTarget)
    x.u8(0x0F);
    x.u8(0x84);
    x.hole(HK::BranchRel32, PS::None, HT::SwitchTarget, 4);
    x.resolveLocals();
    scase.code = x.takeCode();
    for (const EmittedHole& h : x.takeHoles()) {
      scase.holes.push_back(h.hole);
    }
    arch.records.push_back(std::move(scase));
  }

  arch.header.record_count = static_cast<std::uint32_t>(arch.records.size());
  return arch;
}

} // namespace b2::codegen
