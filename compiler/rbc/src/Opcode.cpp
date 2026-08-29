// B-2 RBC - opcode metadata table.
//
// WHY THIS FILE EXISTS:
// Opcode.h freezes the Op enum, the Sig operand-layout contract, and the Eff
// effect flags, but every consumer (verifier, text printer/parser, quickener,
// stencil selection, T0 dispatch) needs the per-opcode mapping as DATA, not
// as scattered switches. This file is that single source of truth: one
// constexpr table indexed exactly by enum order, trivially copyable so T0
// can host it in .rodata next to the dispatch table (docs/rbc_spec.md §3,
// §3.19). `opName` strings are the text-format keywords — the text parser
// looks them up by name, so they are load-bearing, not decoration.
//
// Operand/type conventions used below (docs/rbc_spec.md §2.3, §10.1 pins):
// - result = RType::Bottom means "writes nothing" (put*/stores/branches/
//   returns) OR "writes/reads a descriptor-derived type" (getfield,
//   getstatic, invoke*; the verifier resolves it from the cp entry, §5.2).
//   Quickened forms are post-verification and carry Bottom.
// - putstatic (P1): Sig::RegCp, dst is READ as the value register.
// - aconst_null (P2): Sig::RegImm with imm unused (canonical 0).
// - array stores (P3): Sig::RegRegReg with dst = stored value (read),
//   a = array reference, b = index; a/b keep their load-side meaning.
// - checkcast (P4): RegRegCp (Opcode.h also lists it under RegReg; the cp
//   operand is required, so RegRegCp is normative).
// - multianewarray (P5): b = dimension count, counts live in consecutive
//   registers a..a+b-1, each Int.
//
// Effect assignments follow the ME-B task contract, which is normative for
// this file. Where the task is silent, docs/rbc_spec.md §3 decides:
// arraylength = CanTrap (NPE; spec §3.13) and every *_quick opcode carries
// Eff::Quickened (spec §6.2 — the flag exists exactly for them). Known
// divergences from rbc_spec.md §3 (P14), recorded for reconciliation:
// invokes carry C|T|S without R/W/A (CanCall subsumes them for any
// reordering decision); getstatic/putstatic carry no CanCall; ldc carries
// no CanAllocate; instanceof carries no ReadsHeap; allocation and monitor
// ops carry IsSafepoint; deopt_trap carries no IsSafepoint; the guards
// carry CanBranch in addition to CanTrap.

#include <cstddef>

#include "b2/rbc/Opcode.h"

namespace b2::rbc {
namespace {

// Effect aliases in docs/rbc_spec.md §3 notation.
constexpr Eff kR = Eff::ReadsHeap;   // R
constexpr Eff kW = Eff::WritesHeap;  // W
constexpr Eff kC = Eff::CanCall;     // C
constexpr Eff kT = Eff::CanTrap;     // T
constexpr Eff kA = Eff::CanAllocate; // A
constexpr Eff kB = Eff::CanBranch;   // B
constexpr Eff kS = Eff::IsSafepoint; // S
constexpr Eff kQ = Eff::Quickened;   // Q

// Indexed EXACTLY by Op enum order; kOpInfoCount asserts full coverage.
constexpr OpInfo kOpInfo[] = {
    // --- misc -------------------------------------------------------------
    {"nop", Sig::None, RType::Bottom, RType::Bottom, RType::Bottom, Eff::None},
    {"safepoint_poll", Sig::None, RType::Bottom, RType::Bottom, RType::Bottom,
     kS},

    // --- constants ----------------------------------------------------------
    // aconst_null (P2): RegImm with imm unused — no bare-(dst) Sig exists.
    {"aconst_null", Sig::RegImm, RType::Null, RType::Bottom, RType::Bottom,
     Eff::None},
    {"iconst", Sig::RegImm, RType::Int, RType::Bottom, RType::Bottom,
     Eff::None},
    {"fconst", Sig::RegImm, RType::Float, RType::Bottom, RType::Bottom,
     Eff::None},
    {"lconst", Sig::RegCp, RType::Long, RType::Bottom, RType::Bottom,
     Eff::None},
    {"dconst", Sig::RegCp, RType::Double, RType::Bottom, RType::Bottom,
     Eff::None},
    // ldc: CanTrap for resolution/class-init (task contract).
    {"ldc", Sig::RegCp, RType::Ref, RType::Bottom, RType::Bottom, kT},

    // --- local slots <-> registers -------------------------------------------
    {"iload", Sig::RegSlot, RType::Int, RType::Bottom, RType::Bottom,
     Eff::None},
    {"lload", Sig::RegSlot, RType::Long, RType::Bottom, RType::Bottom,
     Eff::None},
    {"fload", Sig::RegSlot, RType::Float, RType::Bottom, RType::Bottom,
     Eff::None},
    {"dload", Sig::RegSlot, RType::Double, RType::Bottom, RType::Bottom,
     Eff::None},
    {"aload", Sig::RegSlot, RType::Ref, RType::Bottom, RType::Bottom,
     Eff::None},
    {"istore", Sig::SlotReg, RType::Bottom, RType::Int, RType::Bottom,
     Eff::None},
    {"lstore", Sig::SlotReg, RType::Bottom, RType::Long, RType::Bottom,
     Eff::None},
    {"fstore", Sig::SlotReg, RType::Bottom, RType::Float, RType::Bottom,
     Eff::None},
    {"dstore", Sig::SlotReg, RType::Bottom, RType::Double, RType::Bottom,
     Eff::None},
    {"astore", Sig::SlotReg, RType::Bottom, RType::Ref, RType::Bottom,
     Eff::None},

    // --- typed moves ----------------------------------------------------------
    {"imove", Sig::RegReg, RType::Int, RType::Int, RType::Bottom, Eff::None},
    {"lmove", Sig::RegReg, RType::Long, RType::Long, RType::Bottom,
     Eff::None},
    {"fmove", Sig::RegReg, RType::Float, RType::Float, RType::Bottom,
     Eff::None},
    {"dmove", Sig::RegReg, RType::Double, RType::Double, RType::Bottom,
     Eff::None},
    {"amove", Sig::RegReg, RType::Ref, RType::Ref, RType::Bottom, Eff::None},

    // --- int arithmetic -------------------------------------------------------
    // idiv/irem trap on zero divide; the rest wrap silently.
    {"iadd", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, Eff::None},
    {"isub", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, Eff::None},
    {"imul", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, Eff::None},
    {"idiv", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, kT},
    {"irem", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, kT},
    {"ineg", Sig::RegReg, RType::Int, RType::Int, RType::Bottom, Eff::None},
    {"ishl", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, Eff::None},
    {"ishr", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, Eff::None},
    {"iushr", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, Eff::None},
    {"iand", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, Eff::None},
    {"ior", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, Eff::None},
    {"ixor", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, Eff::None},
    // iinc: the only read-modify-write arithmetic op — dst is both source
    // and destination (typed Int via `result`; field `a` is unused).
    {"iinc", Sig::RegImm, RType::Int, RType::Bottom, RType::Bottom,
     Eff::None},

    // --- long arithmetic ------------------------------------------------------
    // ldiv/lrem trap on zero divide; shift counts (b) are Int, not Long.
    {"ladd", Sig::RegRegReg, RType::Long, RType::Long, RType::Long,
     Eff::None},
    {"lsub", Sig::RegRegReg, RType::Long, RType::Long, RType::Long,
     Eff::None},
    {"lmul", Sig::RegRegReg, RType::Long, RType::Long, RType::Long,
     Eff::None},
    {"ldiv", Sig::RegRegReg, RType::Long, RType::Long, RType::Long, kT},
    {"lrem", Sig::RegRegReg, RType::Long, RType::Long, RType::Long, kT},
    {"lneg", Sig::RegReg, RType::Long, RType::Long, RType::Bottom,
     Eff::None},
    {"lshl", Sig::RegRegReg, RType::Long, RType::Long, RType::Int,
     Eff::None},
    {"lshr", Sig::RegRegReg, RType::Long, RType::Long, RType::Int,
     Eff::None},
    {"lushr", Sig::RegRegReg, RType::Long, RType::Long, RType::Int,
     Eff::None},
    {"land", Sig::RegRegReg, RType::Long, RType::Long, RType::Long,
     Eff::None},
    {"lor", Sig::RegRegReg, RType::Long, RType::Long, RType::Long,
     Eff::None},
    {"lxor", Sig::RegRegReg, RType::Long, RType::Long, RType::Long,
     Eff::None},

    // --- float arithmetic -----------------------------------------------------
    // IEEE 754: fdiv/frem never trap (division by zero yields an infinity).
    {"fadd", Sig::RegRegReg, RType::Float, RType::Float, RType::Float,
     Eff::None},
    {"fsub", Sig::RegRegReg, RType::Float, RType::Float, RType::Float,
     Eff::None},
    {"fmul", Sig::RegRegReg, RType::Float, RType::Float, RType::Float,
     Eff::None},
    {"fdiv", Sig::RegRegReg, RType::Float, RType::Float, RType::Float,
     Eff::None},
    {"frem", Sig::RegRegReg, RType::Float, RType::Float, RType::Float,
     Eff::None},
    {"fneg", Sig::RegReg, RType::Float, RType::Float, RType::Bottom,
     Eff::None},

    // --- double arithmetic ------------------------------------------------------
    {"dadd", Sig::RegRegReg, RType::Double, RType::Double, RType::Double,
     Eff::None},
    {"dsub", Sig::RegRegReg, RType::Double, RType::Double, RType::Double,
     Eff::None},
    {"dmul", Sig::RegRegReg, RType::Double, RType::Double, RType::Double,
     Eff::None},
    {"ddiv", Sig::RegRegReg, RType::Double, RType::Double, RType::Double,
     Eff::None},
    {"drem", Sig::RegRegReg, RType::Double, RType::Double, RType::Double,
     Eff::None},
    {"dneg", Sig::RegReg, RType::Double, RType::Double, RType::Bottom,
     Eff::None},

    // --- comparisons (result is always Int) --------------------------------------
    {"icmp", Sig::RegRegReg, RType::Int, RType::Int, RType::Int, Eff::None},
    {"lcmp", Sig::RegRegReg, RType::Int, RType::Long, RType::Long,
     Eff::None},
    {"fcmpl", Sig::RegRegReg, RType::Int, RType::Float, RType::Float,
     Eff::None},
    {"fcmpg", Sig::RegRegReg, RType::Int, RType::Float, RType::Float,
     Eff::None},
    {"dcmpl", Sig::RegRegReg, RType::Int, RType::Double, RType::Double,
     Eff::None},
    {"dcmpg", Sig::RegRegReg, RType::Int, RType::Double, RType::Double,
     Eff::None},

    // --- conversions ------------------------------------------------------------
    {"i2l", Sig::RegReg, RType::Long, RType::Int, RType::Bottom, Eff::None},
    {"i2f", Sig::RegReg, RType::Float, RType::Int, RType::Bottom, Eff::None},
    {"i2d", Sig::RegReg, RType::Double, RType::Int, RType::Bottom,
     Eff::None},
    {"l2i", Sig::RegReg, RType::Int, RType::Long, RType::Bottom, Eff::None},
    {"l2f", Sig::RegReg, RType::Float, RType::Long, RType::Bottom,
     Eff::None},
    {"l2d", Sig::RegReg, RType::Double, RType::Long, RType::Bottom,
     Eff::None},
    {"f2i", Sig::RegReg, RType::Int, RType::Float, RType::Bottom,
     Eff::None},
    {"f2l", Sig::RegReg, RType::Long, RType::Float, RType::Bottom,
     Eff::None},
    {"f2d", Sig::RegReg, RType::Double, RType::Float, RType::Bottom,
     Eff::None},
    {"d2i", Sig::RegReg, RType::Int, RType::Double, RType::Bottom,
     Eff::None},
    {"d2l", Sig::RegReg, RType::Long, RType::Double, RType::Bottom,
     Eff::None},
    {"d2f", Sig::RegReg, RType::Float, RType::Double, RType::Bottom,
     Eff::None},
    {"i2b", Sig::RegReg, RType::Int, RType::Int, RType::Bottom, Eff::None},
    {"i2c", Sig::RegReg, RType::Int, RType::Int, RType::Bottom, Eff::None},
    {"i2s", Sig::RegReg, RType::Int, RType::Int, RType::Bottom, Eff::None},

    // --- branches -----------------------------------------------------------------
    // WHY no IsSafepoint here: runtime polls are expressed by lowering as
    // explicit safepoint_poll instructions on loop backedges (spec §3.1);
    // branch ops themselves carry only CanBranch.
    {"goto", Sig::Branch, RType::Bottom, RType::Bottom, RType::Bottom, kB},
    {"ifeq", Sig::RegBranch, RType::Bottom, RType::Int, RType::Bottom, kB},
    {"ifne", Sig::RegBranch, RType::Bottom, RType::Int, RType::Bottom, kB},
    {"iflt", Sig::RegBranch, RType::Bottom, RType::Int, RType::Bottom, kB},
    {"ifge", Sig::RegBranch, RType::Bottom, RType::Int, RType::Bottom, kB},
    {"ifgt", Sig::RegBranch, RType::Bottom, RType::Int, RType::Bottom, kB},
    {"ifle", Sig::RegBranch, RType::Bottom, RType::Int, RType::Bottom, kB},
    {"ifnull", Sig::RegBranch, RType::Bottom, RType::Ref, RType::Bottom, kB},
    {"ifnonnull", Sig::RegBranch, RType::Bottom, RType::Ref, RType::Bottom,
     kB},
    {"if_icmpeq", Sig::RegRegBranch, RType::Bottom, RType::Int, RType::Int,
     kB},
    {"if_icmpne", Sig::RegRegBranch, RType::Bottom, RType::Int, RType::Int,
     kB},
    {"if_icmplt", Sig::RegRegBranch, RType::Bottom, RType::Int, RType::Int,
     kB},
    {"if_icmpge", Sig::RegRegBranch, RType::Bottom, RType::Int, RType::Int,
     kB},
    {"if_icmpgt", Sig::RegRegBranch, RType::Bottom, RType::Int, RType::Int,
     kB},
    {"if_icmple", Sig::RegRegBranch, RType::Bottom, RType::Int, RType::Int,
     kB},
    {"if_acmpeq", Sig::RegRegBranch, RType::Bottom, RType::Ref, RType::Ref,
     kB},
    {"if_acmpne", Sig::RegRegBranch, RType::Bottom, RType::Ref, RType::Ref,
     kB},
    {"tableswitch", Sig::RegCpBranch, RType::Bottom, RType::Int,
     RType::Bottom, kB},
    {"lookupswitch", Sig::RegCpBranch, RType::Bottom, RType::Int,
     RType::Bottom, kB},

    // --- fields ---------------------------------------------------------------------
    // getfield/getstatic result and putfield's b are descriptor-derived
    // (Bottom here; the verifier reads the FieldRef descriptor).
    {"getfield", Sig::RegRegCp, RType::Bottom, RType::Ref, RType::Bottom,
     kR | kT},
    {"putfield", Sig::RegRegRegCp, RType::Bottom, RType::Ref, RType::Bottom,
     kW | kT},
    {"getstatic", Sig::RegCp, RType::Bottom, RType::Bottom, RType::Bottom,
     kR | kT},
    // putstatic (P1): dst is READ as the value register — the type comes
    // from the field descriptor (verifier).
    {"putstatic", Sig::RegCp, RType::Bottom, RType::Bottom, RType::Bottom,
     kW | kT},
    // Quickened: imm is a resolved byte offset, not a cp index; these never
    // appear in unquickened streams (spec §6).
    {"getfield_quick", Sig::RegRegCp, RType::Bottom, RType::Ref,
     RType::Bottom, kR | kT | kQ},
    {"putfield_quick", Sig::RegRegRegCp, RType::Bottom, RType::Ref,
     RType::Bottom, kW | kT | kQ},

    // --- arrays -----------------------------------------------------------------------
    // newarray's imm is an Atype code (4..11).
    {"newarray", Sig::RegRegImm, RType::Ref, RType::Int, RType::Bottom,
     kA | kT | kS},
    {"anewarray", Sig::RegRegCp, RType::Ref, RType::Int, RType::Bottom,
     kA | kT | kS},
    // arraylength: task silent; spec §3.13 says CanTrap (NPE).
    {"arraylength", Sig::RegReg, RType::Int, RType::Ref, RType::Bottom, kT},
    {"iaload", Sig::RegRegReg, RType::Int, RType::Ref, RType::Int, kR | kT},
    {"laload", Sig::RegRegReg, RType::Long, RType::Ref, RType::Int,
     kR | kT},
    {"faload", Sig::RegRegReg, RType::Float, RType::Ref, RType::Int,
     kR | kT},
    {"daload", Sig::RegRegReg, RType::Double, RType::Ref, RType::Int,
     kR | kT},
    {"aaload", Sig::RegRegReg, RType::Ref, RType::Ref, RType::Int, kR | kT},
    {"baload", Sig::RegRegReg, RType::Int, RType::Ref, RType::Int, kR | kT},
    {"caload", Sig::RegRegReg, RType::Int, RType::Ref, RType::Int, kR | kT},
    {"saload", Sig::RegRegReg, RType::Int, RType::Ref, RType::Int, kR | kT},
    // Array stores (P3): dst = stored value (read; its type is the element
    // family — Int/Long/Float/Double/Ref — checked by the verifier per
    // opcode), a = array reference, b = index.
    {"iastore", Sig::RegRegReg, RType::Bottom, RType::Ref, RType::Int,
     kW | kT},
    {"lastore", Sig::RegRegReg, RType::Bottom, RType::Ref, RType::Int,
     kW | kT},
    {"fastore", Sig::RegRegReg, RType::Bottom, RType::Ref, RType::Int,
     kW | kT},
    {"dastore", Sig::RegRegReg, RType::Bottom, RType::Ref, RType::Int,
     kW | kT},
    {"aastore", Sig::RegRegReg, RType::Bottom, RType::Ref, RType::Int,
     kW | kT},
    {"bastore", Sig::RegRegReg, RType::Bottom, RType::Ref, RType::Int,
     kW | kT},
    {"castore", Sig::RegRegReg, RType::Bottom, RType::Ref, RType::Int,
     kW | kT},
    {"sastore", Sig::RegRegReg, RType::Bottom, RType::Ref, RType::Int,
     kW | kT},
    // multianewarray (P5): b = dimension count; counts live in consecutive
    // registers a..a+b-1, each Int.
    {"multianewarray", Sig::RegRegRegCp, RType::Ref, RType::Int,
     RType::Bottom, kA | kT | kS},

    // --- objects ------------------------------------------------------------------------
    {"new", Sig::RegCp, RType::Ref, RType::Bottom, RType::Bottom,
     kA | kT | kS},
    {"checkcast", Sig::RegRegCp, RType::Ref, RType::Ref, RType::Bottom, kT},
    {"instanceof", Sig::RegRegCp, RType::Int, RType::Ref, RType::Bottom,
     kT},
    {"monitorenter", Sig::Reg, RType::Bottom, RType::Ref, RType::Bottom,
     kT | kS},
    {"monitorexit", Sig::Reg, RType::Bottom, RType::Ref, RType::Bottom,
     kT | kS},

    // --- calls ---------------------------------------------------------------------------
    // Result and argument types are descriptor-derived (Bottom here): the
    // verifier types each argument register against parseParams of the cp
    // MethodRef and dst against parseReturn (spec §5.2).
    {"invokevirtual", Sig::Call, RType::Bottom, RType::Bottom, RType::Bottom,
     kC | kT | kS},
    {"invokespecial", Sig::Call, RType::Bottom, RType::Bottom, RType::Bottom,
     kC | kT | kS},
    {"invokestatic", Sig::Call, RType::Bottom, RType::Bottom, RType::Bottom,
     kC | kT | kS},
    {"invokeinterface", Sig::Call, RType::Bottom, RType::Bottom,
     RType::Bottom, kC | kT | kS},
    {"invokedynamic", Sig::Call, RType::Bottom, RType::Bottom,
     RType::Bottom, kC | kT | kS},
    // Quickened calls: imm = resolved MethodId / inline-cache site id.
    {"invokevirtual_quick", Sig::CallQuick, RType::Bottom, RType::Bottom,
     RType::Bottom, kC | kT | kS | kQ},
    {"invokespecial_quick", Sig::CallQuick, RType::Bottom, RType::Bottom,
     RType::Bottom, kC | kT | kS | kQ},
    {"invokestatic_quick", Sig::CallQuick, RType::Bottom, RType::Bottom,
     RType::Bottom, kC | kT | kS | kQ},
    {"invokeinterface_quick", Sig::CallQuick, RType::Bottom, RType::Bottom,
     RType::Bottom, kC | kT | kS | kQ},

    // --- returns --------------------------------------------------------------------------
    {"return", Sig::None, RType::Bottom, RType::Bottom, RType::Bottom,
     Eff::None},
    {"ireturn", Sig::Reg, RType::Bottom, RType::Int, RType::Bottom,
     Eff::None},
    {"lreturn", Sig::Reg, RType::Bottom, RType::Long, RType::Bottom,
     Eff::None},
    {"freturn", Sig::Reg, RType::Bottom, RType::Float, RType::Bottom,
     Eff::None},
    {"dreturn", Sig::Reg, RType::Bottom, RType::Double, RType::Bottom,
     Eff::None},
    {"areturn", Sig::Reg, RType::Bottom, RType::Ref, RType::Bottom,
     Eff::None},

    // --- exceptions ------------------------------------------------------------------------
    {"athrow", Sig::Reg, RType::Bottom, RType::Ref, RType::Bottom, kT},

    // --- deopt / tiering hooks ---------------------------------------------------------------
    {"deopt_trap", Sig::Trap, RType::Bottom, RType::Bottom, RType::Bottom,
     kT},
    {"guard_non_null", Sig::Guard, RType::Bottom, RType::Ref, RType::Bottom,
     kT | kB},
    {"guard_class", Sig::GuardCp, RType::Bottom, RType::Ref, RType::Bottom,
     kT | kB},
};

constexpr std::size_t kOpInfoCount = sizeof(kOpInfo) / sizeof(kOpInfo[0]);

// WHY: the table is looked up by raw opcode value; a missing or extra entry
// would silently shift every lookup. Count must match Op::_Count exactly.
static_assert(kOpInfoCount == static_cast<std::size_t>(Op::_Count),
              "kOpInfo must cover every Op exactly once, in enum order");
static_assert(static_cast<std::size_t>(Op::_Count) == 150,
              "v0 opcode count is frozen at 150 (docs/rbc_spec.md §3)");

// Defensive dummy for out-of-range lookups. The verifier checks opcode range
// first (V-S1), but info()/opName() must never be UB on garbage input.
// WHY the literal "bad<op>": the frozen header specifies this exact fallback
// text; a const char* return cannot format the numeric code, so the literal
// marker is used and the caller's diagnostic supplies the number.
constexpr OpInfo kBadOp{"bad<op>", Sig::None, RType::Bottom, RType::Bottom,
                        RType::Bottom, Eff::None};

constexpr bool cstrEq(const char* a, const char* b) noexcept {
  while (*a != '\0' && *b != '\0') {
    if (*a != *b) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == *b;
}

// Spot checks: count alone cannot catch ordering swaps inside the table, so
// the endpoints, the tricky mnemonics, and the spec-pinned signatures are
// asserted here (compile-time, zero runtime cost).
static_assert(cstrEq(kOpInfo[0].name, "nop"), "table must start at Op::Nop");
static_assert(cstrEq(kOpInfo[kOpInfoCount - 1].name, "guard_class"),
              "table must end at Op::GuardClass");
static_assert(cstrEq(kOpInfo[static_cast<std::size_t>(Op::SafepointPoll)].name,
                     "safepoint_poll"),
              "mnemonic pin");
static_assert(cstrEq(kOpInfo[static_cast<std::size_t>(Op::AconstNull)].name,
                     "aconst_null"),
              "mnemonic pin");
static_assert(cstrEq(kOpInfo[static_cast<std::size_t>(Op::Ldc)].name, "ldc"),
              "mnemonic pin");
static_assert(cstrEq(kOpInfo[static_cast<std::size_t>(Op::Iadd)].name,
                     "iadd"),
              "mnemonic pin");
static_assert(cstrEq(kOpInfo[static_cast<std::size_t>(Op::IfIcmpeq)].name,
                     "if_icmpeq"),
              "mnemonic pin");
static_assert(cstrEq(kOpInfo[static_cast<std::size_t>(Op::Tableswitch)].name,
                     "tableswitch"),
              "mnemonic pin");
static_assert(cstrEq(kOpInfo[static_cast<std::size_t>(Op::Lookupswitch)].name,
                     "lookupswitch"),
              "mnemonic pin");
static_assert(cstrEq(kOpInfo[static_cast<std::size_t>(Op::Multianewarray)]
                         .name,
                     "multianewarray"),
              "mnemonic pin");
static_assert(cstrEq(kOpInfo[static_cast<std::size_t>(Op::GetfieldQuick)].name,
                     "getfield_quick"),
              "mnemonic pin");
static_assert(cstrEq(
                  kOpInfo[static_cast<std::size_t>(Op::InvokeinterfaceQuick)]
                      .name,
                  "invokeinterface_quick"),
              "mnemonic pin");
static_assert(cstrEq(kOpInfo[static_cast<std::size_t>(Op::DeoptTrap)].name,
                     "deopt_trap"),
              "mnemonic pin");
static_assert(
    cstrEq(kOpInfo[static_cast<std::size_t>(Op::GuardNonNull)].name,
           "guard_non_null"),
    "mnemonic pin");
// Spec pins P1-P5 (docs/rbc_spec.md §10.1).
static_assert(kOpInfo[static_cast<std::size_t>(Op::Putstatic)].sig ==
                  Sig::RegCp,
              "P1: putstatic is RegCp with dst read as the value");
static_assert(kOpInfo[static_cast<std::size_t>(Op::AconstNull)].sig ==
                  Sig::RegImm,
              "P2: aconst_null is RegImm (imm unused)");
static_assert(kOpInfo[static_cast<std::size_t>(Op::Iastore)].sig ==
                  Sig::RegRegReg,
              "P3: array stores are RegRegReg (dst = value)");
static_assert(kOpInfo[static_cast<std::size_t>(Op::Checkcast)].sig ==
                  Sig::RegRegCp,
              "P4: checkcast is RegRegCp");
static_assert(kOpInfo[static_cast<std::size_t>(Op::Multianewarray)].sig ==
                  Sig::RegRegRegCp,
              "P5: multianewarray is RegRegRegCp");
static_assert(kOpInfo[static_cast<std::size_t>(Op::AconstNull)].result ==
                  RType::Null,
              "aconst_null writes the Null type");
static_assert(kOpInfo[static_cast<std::size_t>(Op::Lshl)].operandB ==
                  RType::Int,
              "long shift count is Int");
static_assert(kOpInfo[static_cast<std::size_t>(Op::GuardClass)].sig ==
                  Sig::GuardCp,
              "guard_class is GuardCp");

} // namespace

const OpInfo& info(Op op) noexcept {
  const std::size_t v = static_cast<std::size_t>(op);
  // Op's underlying type is u16, so the cast can never produce a huge
  // value; still compare against the table size before indexing (never UB).
  if (v < kOpInfoCount) {
    return kOpInfo[v];
  }
  return kBadOp;
}

const char* opName(Op op) noexcept { return info(op).name; }

} // namespace b2::rbc
