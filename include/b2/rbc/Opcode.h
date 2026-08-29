#pragma once
// B-2 RBC - opcode set, operand signatures, and effect metadata.
//
// WHY THIS FILE EXISTS:
// Every tier consumes RBC: T0 dispatches on `Op` directly-threaded, T1 maps
// each opcode (and later fused superinstruction) to a precompiled stencil
// (docs/stencils.md), T2/T3 build sea-of-nodes IR from it. The opcode set is
// therefore the most load-bearing constant in the middle end, and its
// metadata (operand signature, effect flags) is the single source of truth
// for the verifier, the text printer/parser, the quickener, and stencil
// selection.
//
// DESIGN RULES (normative, see docs/rbc_spec.md):
// - Instructions are fixed-width 12-byte records (see Rbc.h: Ins).
// - Branch targets are instruction INDICES (not byte offsets): relocation-
//   friendly, trivial pc maps, safe in-place quickening (opcode swap only).
// - Ops are typed per JVM primitive family (iadd/ladd/fadd/dadd); Java
//   boolean/byte/char/short values live in Int registers.
// - Quickened variants are separate opcodes (_quick suffix) written in place
//   by the quickener; the un-quickened forms resolve through the constant
//   pool, the quickened forms carry resolved offsets/ids in `imm`.

#include <cstdint>

#include "b2/rbc/Type.h"

namespace b2::rbc {

enum class Op : std::uint16_t {
  // --- misc -------------------------------------------------------------
  Nop = 0,
  SafepointPoll, // explicit poll point (loops/backedges; T0/T1 must honor)

  // --- constants --------------------------------------------------------
  AconstNull, // dst = null
  Iconst,     // dst = (int32_t)imm
  Fconst,     // dst = bit_cast<float>(imm)
  Lconst,     // dst = cp[imm].long
  Dconst,     // dst = cp[imm].double
  Ldc,        // dst = cp[imm] (string/class/methodtype/methodhandle)

  // --- local slots <-> registers ----------------------------------------
  Iload, // dst = (int) local[imm]
  Lload, // dst = (long) local[imm]
  Fload, // dst = (float) local[imm]
  Dload, // dst = (double) local[imm]
  Aload, // dst = (ref) local[imm]
  Istore, // local[imm] = (int) a
  Lstore, // local[imm] = (long) a
  Fstore, // local[imm] = (float) a
  Dstore, // local[imm] = (double) a
  Astore, // local[imm] = (ref) a

  // --- typed moves -------------------------------------------------------
  Imove, // dst = a (int)
  Lmove, // dst = a (long)
  Fmove, // dst = a (float)
  Dmove, // dst = a (double)
  Amove, // dst = a (ref)

  // --- int arithmetic ----------------------------------------------------
  Iadd, Isub, Imul, Idiv, Irem, Ineg,
  Ishl, Ishr, Iushr, Iand, Ior, Ixor,
  Iinc, // dst = dst + (int32_t)imm

  // --- long arithmetic ---------------------------------------------------
  Ladd, Lsub, Lmul, Ldiv, Lrem, Lneg,
  Lshl, Lshr, Lushr, Land, Lor, Lxor,

  // --- float arithmetic --------------------------------------------------
  Fadd, Fsub, Fmul, Fdiv, Frem, Fneg,

  // --- double arithmetic -------------------------------------------------
  Dadd, Dsub, Dmul, Ddiv, Drem, Dneg,

  // --- comparisons -------------------------------------------------------
  Icmp,   // dst = (a > b) - (a < b)          : int
  Lcmp,   // dst = (a > b) - (a < b)          : long
  Fcmpl,  // dst = -1/0/1, NaN compares less  : float
  Fcmpg,  // dst = -1/0/1, NaN compares greater: float
  Dcmpl,  // dst = -1/0/1, NaN compares less  : double
  Dcmpg,  // dst = -1/0/1, NaN compares greater: double

  // --- conversions -------------------------------------------------------
  I2l, I2f, I2d, L2i, L2f, L2d,
  F2i, F2l, F2d, D2i, D2l, D2f,
  I2b, I2c, I2s,

  // --- branches (imm = target instruction index) --------------------------
  Goto,       // unconditional
  Ifeq,       // if a == 0 (int)
  Ifne,       // if a != 0 (int)
  Iflt,       // if a <  0 (int)
  Ifge,       // if a >= 0 (int)
  Ifgt,       // if a >  0 (int)
  Ifle,       // if a <= 0 (int)
  Ifnull,     // if a == null
  Ifnonnull,  // if a != null
  IfIcmpeq, IfIcmpne, IfIcmplt, IfIcmpge, IfIcmpgt, IfIcmple,
  IfAcmpeq, IfAcmpne,
  Tableswitch,  // a = selector (int); cp[imm] = SwitchTable
  Lookupswitch, // a = selector (int); cp[imm] = LookupTable

  // --- fields -------------------------------------------------------------
  Getfield,  // dst = a.field      ; cp[imm] = FieldRef
  Putfield,  // a.field = b        ; cp[imm] = FieldRef
  Getstatic, // dst = field        ; cp[imm] = FieldRef
  Putstatic, // field = a          ; cp[imm] = FieldRef
  GetfieldQuick,  // quickened: imm = resolved field offset (bytes)
  PutfieldQuick,  // quickened: imm = resolved field offset (bytes)

  // --- arrays -------------------------------------------------------------
  NewArray,      // dst = new prim_array[len = a]; imm = Atype code
  AnewArray,     // dst = new ref_array[len = a]; cp[imm] = class
  Arraylength,   // dst = a.length
  Iaload, Laload, Faload, Daload, Aaload, Baload, Caload, Saload,
  Iastore, Lastore, Fastore, Dastore, Aastore, Bastore, Castore, Sastore,
  Multianewarray, // dst = new cp[imm] dims b; first dim reg a (v0: 1 dim via NewArray/AnewArray)

  // --- objects ------------------------------------------------------------
  New,         // dst = new cp[imm] (class instance; <init> follows via Invokespecial)
  Checkcast,   // dst = (cp[imm]) a ; throws on failure
  Instanceof,  // dst = (a instanceof cp[imm]) ? 1 : 0
  Monitorenter,
  Monitorexit,

  // --- calls --------------------------------------------------------------
  // dst = result register (valid unless void); a = first arg register,
  // args occupy consecutive registers a..a+argCount-1; b = argCount;
  // imm = cp index of MethodRef.
  Invokevirtual,
  Invokespecial,
  Invokestatic,
  Invokeinterface,
  Invokedynamic, // cp[imm] = InvokeDynamic descriptor (v0: verifier-only)
  InvokevirtualQuick,  // quickened: imm = resolved MethodId / IC site id
  InvokespecialQuick,
  InvokestaticQuick,
  InvokeinterfaceQuick,

  // --- returns ------------------------------------------------------------
  Return,   // void
  Ireturn,  // a
  Lreturn,  // a
  Freturn,  // a
  Dreturn,  // a
  Areturn,  // a

  // --- exceptions ---------------------------------------------------------
  Athrow, // throw a

  // --- deopt / tiering hooks (docs/deopt_backend.md) ----------------------
  DeoptTrap,    // unconditional transfer to T0 with DeoptId = imm
  GuardNonNull, // if a == null -> DeoptTrap imm
  GuardClass,   // if klass(a) != cp[imm] -> DeoptTrap b (v0: verifier-only)

  _Count
};

// Operand signature: how an Ins's dst/a/b/imm fields are interpreted.
// This is the contract shared by the verifier, text format, and stencils.
enum class Sig : std::uint8_t {
  None,          // Nop
  Reg,           // (a)                    : monitorenter r, ireturn r
  RegImm,        // (dst, imm)             : iconst, iinc
  RegCp,         // (dst, imm=cp)          : lconst, dconst, ldc, new
  RegSlot,       // (dst, imm=slot)        : iload r, l1
  SlotReg,       // (a, imm=slot)          : istore r, l1
  RegReg,        // (dst, a)               : imove, arraylength, ineg, checkcast
  RegRegReg,     // (dst, a, b)            : iadd, if-less ops that produce values
  RegRegImm,     // (dst, a, imm)          : newarray (dst, len, atype)
  RegRegCp,      // (dst, a, imm=cp)       : anewarray, instanceof, getfield, checkcast
  RegRegRegCp,   // (dst, a, b, imm=cp)    : putfield (obj, value), aastore-like fields
  Branch,        // (imm=target)           : goto
  RegBranch,     // (a, imm=target)        : ifeq r, L
  RegRegBranch,  // (a, b, imm=target)     : if_icmpeq a, b, L
  RegCpBranch,   // (a, imm=cp switch tbl) : tableswitch, lookupswitch
  Call,          // (dst, a=argBase, b=argCount, imm=cp) : invoke*
  CallQuick,     // (dst, a, b, imm=resolved id)         : invoke*_quick
  Guard,         // (a, imm=DeoptId)       : guard_non_null
  GuardCp,       // (a, b=DeoptId, imm=cp) : guard_class
  Trap,          // (imm=DeoptId)          : deopt_trap
};

// Effect flags. Stencils and T2 must respect these when reordering (Law 10x,
// docs/stencils.md effect tags). Multiple flags combine.
enum class Eff : std::uint16_t {
  None      = 0,
  ReadsHeap = 1u << 0,
  WritesHeap = 1u << 1,
  CanCall    = 1u << 2, // may invoke arbitrary Java code
  CanTrap    = 1u << 3, // may throw / deopt / abort
  CanAllocate = 1u << 4,
  CanBranch   = 1u << 5, // control flow target in imm
  IsSafepoint = 1u << 6, // poll + stack map required
  Quickened   = 1u << 7, // quickened variant (never in unquickened streams)
};
[[nodiscard]] constexpr Eff operator|(Eff a, Eff b) noexcept {
  return static_cast<Eff>(static_cast<std::uint16_t>(a) |
                          static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr bool hasEff(Eff v, Eff f) noexcept {
  return (static_cast<std::uint16_t>(v) & static_cast<std::uint16_t>(f)) != 0;
}

// Static per-opcode metadata. Defined in compiler/rbc/src/Opcode.cpp as a
// single constexpr table indexed by opcode; keep it trivially copyable so the
// interpreter can host it in .rodata next to the dispatch table.
struct OpInfo {
  const char* name;  // canonical mnemonic, e.g. "iadd"
  Sig sig;           // operand layout
  RType result;      // type written to dst (Bottom = writes nothing)
  RType operandA;    // required type of a (Bottom = unused/any)
  RType operandB;    // required type of b (Bottom = unused/any)
  Eff effects;       // heap/call/trap/branch effects
};

// Table lookup. `op` must be < Op::_Count (the verifier enforces this first).
[[nodiscard]] const OpInfo& info(Op op) noexcept;

// Canonical mnemonic ("iadd"). Falls back to "bad<op>" for invalid codes.
[[nodiscard]] const char* opName(Op op) noexcept;

[[nodiscard]] constexpr std::uint16_t opCount() noexcept {
  return static_cast<std::uint16_t>(Op::_Count);
}

} // namespace b2::rbc
