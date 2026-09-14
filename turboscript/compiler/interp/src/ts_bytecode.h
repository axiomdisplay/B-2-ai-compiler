// TurboScript — opcode table (single source of truth).
// The X-macro drives: the enum, format/width lookups, effect classes, IC
// assignment, the verifier's operand checks, the assembler's operand
// grammar, the disassembler, and both dispatch paths.
// Spec: turboscript/docs/bytecode_spec.md Section 5 (exhaustive).
#pragma once

#include "ts_core.h"

namespace ts {

enum class Opcode : uint8_t {
#define TS_OP(name, fmt, effect, ic, operands_doc) k##name,
#include "ts_opcodes.inc"
#undef TS_OP
};

enum class OpFormat : uint8_t {
  W0,        // op,0,0
  W1_R,      // op,dst,0
  W1_RI8,    // op,dst,imm8(signed)
  W1_RR,     // op,dst,src
  W1_J,      // op,offLo,offHi (signed 16)
  W1_BR,     // op,cond,off8(signed)
  W2_I24,    // op,0,0 ; imm24
  W2_I24D,   // op,dst,0 ; imm24
  W2_I24S,   // op,src,0 ; imm24
  W2_RR,     // op,a,b ; c,0,0   (roles per opcode)
  W2_RR_D,   // op,dst,a ; b,0,0
  W2_RR_V,   // op,obj,key ; val,0,0
  W2_RR_C,   // op,dst,ctx ; cell8,0,0
  W2_RC_V,   // op,ctx,cell8 ; val,0,0
  W2_CALL,   // op,func,argsBase ; argc8,this8,dst8
  W2_BR,     // op,cond,0 ; off24(signed)
};

enum class OpEffect : uint8_t {
  Pure,
  Read,
  Alloc,
  Invoke,
  Throw,
  Write,
};

struct OpcodeInfo {
  const char* name;
  OpFormat format;
  OpEffect effect;
  bool ic;  // owns a feedback slot
};

// Operand roles per format, for the verifier and assembler.
struct OperandRoles {
  // Per-format register operand slots that are READS (used values).
  static constexpr uint8_t kMaxReads = 4;
  static constexpr uint8_t kMaxWrites = 2;
  uint8_t readCount;
  uint8_t readPos[kMaxReads];   // position index within (A,B,C,D,E) fields
  uint8_t writeCount;
  uint8_t writePos[kMaxWrites]; // fields written
  bool hasImm8;                 // W1_RI8 signed immediate
  bool hasJump;                 // jump offset (field layout per format)
};

constexpr OperandRoles roles(uint8_t rc, const uint8_t (&r)[4], uint8_t wc,
                             const uint8_t (&w)[2], bool imm8 = false,
                             bool jump = false) {
  OperandRoles o{};
  o.readCount = rc;
  for (int i = 0; i < 4; i++) o.readPos[i] = r[i];
  o.writeCount = wc;
  for (int i = 0; i < 2; i++) o.writePos[i] = w[i];
  o.hasImm8 = imm8;
  o.hasJump = jump;
  return o;
}

// Field position constants: 0=A,1=B,2=C,3=D,4=E.
constexpr uint8_t kPosA = 0, kPosB = 1, kPosC = 2, kPosD = 3, kPosE = 4;

// Format table keyed by OpFormat: word width in 24-bit units.
constexpr uint32_t formatWordCount(OpFormat f) {
  switch (f) {
    case OpFormat::W0:
    case OpFormat::W1_R:
    case OpFormat::W1_RI8:
    case OpFormat::W1_RR:
    case OpFormat::W1_J:
    case OpFormat::W1_BR:
      return 1;
    default:
      return 2;
  }
}

inline OpcodeInfo opcodeInfo(Opcode op) {
  switch (op) {
#define TS_OP(name, fmt, effect, ic, roles_expr)                          \
  case Opcode::k##name:                                                   \
    return OpcodeInfo{#name, OpFormat::fmt, OpEffect::effect, ic};
#include "ts_opcodes.inc"
#undef TS_OP
  }
  return OpcodeInfo{"Invalid", OpFormat::W0, OpEffect::Pure, false};
}

// True iff the byte value is a defined v0.1 opcode (Rule 105: the verifier
// rejects undefined opcode bytes rather than dispatching into a hole).
inline bool opcodeDefined(uint32_t op) {
  switch (op) {
#define TS_OP(name, fmt, effect, ic, roles_expr) \
  case static_cast<uint32_t>(Opcode::k##name):   \
    return true;
#include "ts_opcodes.inc"
#undef TS_OP
    default:
      return false;
  }
}

inline OperandRoles opcodeRoles(Opcode op) {
  switch (op) {
#define TS_OP(name, fmt, effect, ic, roles_expr) \
  case Opcode::k##name:                          \
    return roles_expr;
#include "ts_opcodes.inc"
#undef TS_OP
  }
  return roles(0, {0, 0, 0, 0}, 0, {0, 0});
}

}  // namespace ts
