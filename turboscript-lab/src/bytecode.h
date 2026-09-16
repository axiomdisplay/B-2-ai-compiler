// TurboScript Tier 0 — bytecode.h
// Register bytecode: fixed 32-bit words = [opcode:8][a:8][b:8][c:8].
// Spec basis (Laws Part I Tier 0): "24-bit fixed-width instructions with
// 256 virtual registers per frame" -> 8-bit opcode + 24-bit payload.
// Two-word instructions (property/global access, closure, try) append one
// extra word carrying a 16-bit index. Jump offsets are in words, relative to
// the instruction AFTER the jump (stable under backpatching).
//
// Inline cache feedback slots (Part I Tier 0: "collects initial Inline Cache
// shape feedback") live in BytecodeFunction::ics and are patched at runtime.
#pragma once
#include "value.h"

namespace ts {

class String;
class Object;
class BytecodeFunction;
class Runtime;

enum Op : uint8_t {
    // ---- loads / moves ----
    OP_LdSmi,      // a=dst, b=signed imm8
    OP_LdConst,    // a=dst, bc=constIdx16
    OP_LdUndef,    // a=dst
    OP_LdNull,     // a=dst
    OP_LdTrue,     // a=dst
    OP_LdFalse,    // a=dst
    OP_LdZero,     // a=dst
    OP_LdThis,     // a=dst (frame receiver)
    OP_Mov,        // a=dst, b=src
    OP_MovChk,     // a=dst, b=src; throws ReferenceError if src is TDZ (let/const read)
    // ---- arithmetic / bitwise (a=dst, b=lhs, c=rhs) ----
    OP_Add, OP_Sub, OP_Mul, OP_Div, OP_Mod,
    // superinstructions: immediate operands (fused constant operand)
    OP_AddImm,     // a=dst, b=src, c=signed imm8  (dst = src + imm)
    OP_SubImm,     // a=dst, b=src, c=signed imm8  (dst = src - imm)
    OP_MulSmi,     // a=dst, b=src, c=signed imm8  (dst = src * imm)
    OP_LtSmi, OP_LeSmi, OP_GtSmi, OP_GeSmi, // a=dst, b=lhs, c=signed imm8
    OP_BitAnd, OP_BitOr, OP_BitXor, OP_Shl, OP_Shr, OP_UShr,
    // ---- unary ----
    OP_Neg,        // a=dst, b=src
    OP_ToNum,      // a=dst, b=src (unary +)
    OP_BitNot,     // a=dst, b=src
    OP_Not,        // a=dst, b=src (logical !)
    OP_TypeOf,     // a=dst, b=src
    OP_Inc,        // a=reg (in place)
    OP_Dec,        // a=reg (in place)
    // ---- comparison (a=dst, b=lhs, c=rhs) ----
    OP_Eq, OP_Ne, OP_StrictEq, OP_StrictNe,
    OP_Lt, OP_Le, OP_Gt, OP_Ge,
    // ---- control flow ----
    OP_Jmp,        // abc = signed24 word offset (rel. to next ip)
    OP_JmpIfFalse, // a=cond, bc = signed16 offset
    OP_JmpIfTrue,  // a=cond, bc = signed16 offset
    OP_JmpIfNotNU, // a=val, bc = signed16 offset (jump if NOT null/undefined; ?? fast path)
    // ---- property access (two-word: next word = icIdx16) ----
    OP_GetNamed,   // a=dst, b=obj, ext.ic16
    OP_SetNamed,   // a=obj, b=src, ext.ic16
    OP_GetGlobal,  // a=dst, ext.ic16
    OP_SetGlobal,  // a=src, ext.ic16
    OP_GetKeyed,   // a=dst, b=obj, c=key
    OP_SetKeyed,   // a=obj, b=key, c=val
    OP_Delete,     // a=dst(bool), b=obj, ext.ic16 (property form); delete obj[k] uses GetKeyed-style: see compiler
    // ---- lexical scope ----
    OP_GetCtx,     // a=dst, b=depth(hops from frame root ctx), c=idx
    OP_SetCtx,     // a=depth, b=idx, c=src
    OP_NewCtx,     // a=nCells -> pushes inner scope context
    OP_ExitCtx,    // pops inner scope context
    OP_LdTDZ,      // a=dst (let/const pre-initialization sentinel)
    OP_CheckTDZ,   // a=reg (context cell read guard)
    // ---- calls / closures ----
    OP_Call,       // a=fnReg&dst, b=firstArg, c=argc  (this = last receiver)
    OP_CallN,      // same, but this = undefined (plain calls)
    OP_New,        // a=fnReg&dst, b=firstArg, c=argc
    OP_Return,     // a=retval
    OP_LdFn,       // a=dst (current function object; function-expr self-name)
    OP_Closure,    // a=dst, ext.funcIdx16
    // ---- literals ----
    OP_NewArray,   // a=dst, b=elemCount, c=firstElemReg
    OP_NewObject,  // a=dst
    // ---- exceptions ----
    OP_TryEnter,   // ext.handlerIdx16
    OP_TryExit,
    OP_Throw,      // a=value
    // ---- misc ----
    OP_InstanceOf, // a=dst, b=lhs, c=rhs
    OP_InOp,       // a=dst, b=key, c=obj
    OP_DeleteKeyed,// a=dst(bool), b=obj, c=key
    OP_ForInKeys,  // a=dst(array of keys), b=obj
    OP_NOP,
    OP_COUNT
};

// Two-word ops: the word AFTER the opcode word.
struct ExtWord {
    uint32_t w;
    inline uint32_t idx16() const { return w & 0xFFFF; }
};
static inline uint32_t makeExt(uint32_t idx) { return idx & 0xFFFF; }
static inline bool opIsTwoWord(uint8_t op) {
    switch (op) {
        case OP_GetNamed: case OP_SetNamed: case OP_GetGlobal: case OP_SetGlobal:
        case OP_Closure: case OP_TryEnter: case OP_Delete:
            return true;
        default: return false;
    }
}

// ---- inline cache states ----
enum ICState : uint8_t { IC_UNINIT = 0, IC_MONO = 1, IC_MEGA = 2 };

struct IC {
    String* key = nullptr;      // property key (interned)
    Object* recvShapeOwner = nullptr; // unused; shape below is the check
    void*   recvShape = nullptr;   // expected receiver Shape*
    Object* holder = nullptr;      // prototype holder (0 => own)
    void*   holderShape = nullptr; // expected holder Shape*
    uint32_t slot = 0;
    uint8_t  depth = 0;            // proto depth to holder (0 = own)
    uint8_t  state = IC_UNINIT;
};

struct Handler {
    uint32_t start, end;    // ip range [start, end) covered
    uint32_t target;        // catch ip
    uint16_t ctxDepth;      // scope contexts to pop before running handler
    uint8_t  catchReg;      // 0xFF => no catch reg (unwind/finally path)
    bool     isFinally;     // unwind handler: must rethrow/continue completion
};

struct BytecodeFunction {
    uint32_t* code = nullptr;   uint32_t codeLen = 0;
    Value*    consts = nullptr; uint32_t nConsts = 0;
    IC*       ics = nullptr;    uint32_t nICs = 0;
    Handler*  handlers = nullptr; uint32_t nHandlers = 0;
    BytecodeFunction** funcs = nullptr; uint32_t nFuncs = 0;
    String*   name = nullptr;
    uint16_t  nRegs = 0;       // total virtual registers (<= 256, spec Part I)
    uint16_t  nParams = 0;
    bool      strict = true;
    bool      isArrow = false;
    bool      hasSimpleParams = true; // no defaults/rest (MVP: always true)
    uint32_t  frameSize() const { return nRegs; }
};

} // namespace ts
