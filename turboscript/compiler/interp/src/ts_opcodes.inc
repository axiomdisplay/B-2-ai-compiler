// TurboScript opcode table — the exhaustive v0.1 set from
// turboscript/docs/bytecode_spec.md Section 5.
// TS_OP(name, format, effect, has_ic, operand_roles)
// Roles reference encoded field positions: 0=A, 1=B, 2=C, 3=D, 4=E, where
// word1 carries (op, A, B) and the optional suffix word carries (C, D, E).
// Immediates (argc, cell8, offsets) are NOT register roles; the verifier and
// assembler read them from fixed fields per format.
//
// Role patterns used below:
//   W1_RR two-address math (dst in+out): reads {A,B}, writes {A}
//   W1_RR copy (Mov/GetPrototype):       reads {B},   writes {A}
//   W1_R in-place transform:             reads {A},   writes {A}
//   W1_R load:                                        writes {A}
//   W2_RR_D (dst,a,b):                   reads {B,C}, writes {A}
//   W2_RR_V (obj,key,val):               reads {A,B,C}
//   W2_RR_C LoadContext (dst,ctx,imm):   reads {B},   writes {A}
//   W2_RR_C NewContext (dst,par,imm):    reads {B},   writes {A}
//   W2_RC_V StoreContext (ctx,imm,val):  reads {A,C}
//   W2_CALL: reads A(func),B(argsBase)[,D(this)]; C=argc imm; writes E(dst)

// --- Constants and moves ---------------------------------------------------
TS_OP(Nop,            W0,      Pure,  false, roles(0, {0,0,0,0}, 0, {0,0}))
TS_OP(Mov,            W1_RR,   Pure,  false, roles(1, {kPosB}, 1, {kPosA}))
TS_OP(LoadConst,      W2_I24D, Pure,  false, roles(0, {0,0,0,0}, 1, {kPosA}))
TS_OP(LoadConstS,     W1_RI8,  Pure,  false, roles(0, {0,0,0,0}, 1, {kPosA}))
TS_OP(LoadUndefined,  W1_R,    Pure,  false, roles(0, {0,0,0,0}, 1, {kPosA}))
TS_OP(LoadNull,       W1_R,    Pure,  false, roles(0, {0,0,0,0}, 1, {kPosA}))
TS_OP(LoadTrue,       W1_R,    Pure,  false, roles(0, {0,0,0,0}, 1, {kPosA}))
TS_OP(LoadFalse,      W1_R,    Pure,  false, roles(0, {0,0,0,0}, 1, {kPosA}))

// --- Globals ---------------------------------------------------------------
TS_OP(DefineGlobalVar, W2_I24,  Write, false, roles(0, {0,0,0,0}, 0, {0,0}))
TS_OP(LoadGlobal,      W2_I24D, Read,  false, roles(0, {0,0,0,0}, 1, {kPosA}))
TS_OP(StoreGlobal,     W2_I24S, Write, false, roles(1, {kPosA}, 0, {0,0}))

// --- Property access (IC sites) --------------------------------------------
TS_OP(GetProperty,    W2_RR_D, Invoke, true,  roles(2, {kPosB, kPosC}, 1, {kPosA}))
TS_OP(SetProperty,    W2_RR_V, Invoke, true,  roles(3, {kPosA, kPosB, kPosC}, 0, {0,0}))
TS_OP(DeleteProperty, W2_RR_D, Write,  false, roles(2, {kPosB, kPosC}, 1, {kPosA}))
TS_OP(HasProperty,    W2_RR_D, Read,   false, roles(2, {kPosB, kPosC}, 1, {kPosA}))
TS_OP(GetPrototype,   W1_RR,   Read,   false, roles(1, {kPosB}, 1, {kPosA}))
TS_OP(SetPrototype,   W2_RR_D, Write,  false, roles(2, {kPosB, kPosC}, 1, {kPosA}))
TS_OP(Instanceof,     W2_RR_D, Invoke, false, roles(2, {kPosB, kPosC}, 1, {kPosA}))
TS_OP(In,             W2_RR_D, Invoke, false, roles(2, {kPosB, kPosC}, 1, {kPosA}))

// --- Calls, closures, context -----------------------------------------------
TS_OP(Call,           W2_CALL, Invoke, true,  roles(2, {kPosA, kPosB}, 1, {kPosE}))
TS_OP(CallMethod,     W2_CALL, Invoke, true,  roles(3, {kPosA, kPosB, kPosD}, 1, {kPosE}))
TS_OP(Construct,      W2_CALL, Invoke, true,  roles(2, {kPosA, kPosB}, 1, {kPosE}))
TS_OP(Return,         W1_R,    Pure,   false, roles(1, {kPosA}, 0, {0,0}))
TS_OP(NewClosure,     W2_I24D, Alloc,  false, roles(0, {0,0,0,0}, 1, {kPosA}))
// NewContext also establishes the frame's current context (documented
// two-effect op: bytecode_spec.md 5.4).
TS_OP(NewContext,     W2_RR_C, Alloc,  false, roles(1, {kPosB}, 1, {kPosA}))
TS_OP(LoadContext,    W2_RR_C, Read,   false, roles(1, {kPosB}, 1, {kPosA}))
TS_OP(StoreContext,   W2_RC_V, Write,  false, roles(2, {kPosA, kPosC}, 0, {0,0}))
TS_OP(GetContext,     W1_R,    Read,   false, roles(0, {0,0,0,0}, 1, {kPosA}))

// --- Object allocation -------------------------------------------------------
TS_OP(NewObject,      W1_R,    Alloc,  false, roles(0, {0,0,0,0}, 1, {kPosA}))

// --- Arithmetic (full JS semantics; hooks may run) ---------------------------
TS_OP(Add,            W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(Sub,            W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(Mul,            W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(Div,            W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(Mod,            W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(Pow,            W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(Neg,            W1_R,  Invoke, true, roles(1, {kPosA}, 1, {kPosA}))
TS_OP(Inc,            W1_R,  Invoke, true, roles(1, {kPosA}, 1, {kPosA}))
TS_OP(Dec,            W1_R,  Invoke, true, roles(1, {kPosA}, 1, {kPosA}))

// --- Bitwise (ToInt32/ToUint32, Rule 72) --------------------------------------
TS_OP(BitAnd,         W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(BitOr,          W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(BitXor,         W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(Shl,            W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(Shr,            W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(UShr,           W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(BitNot,         W1_R,  Invoke, true, roles(1, {kPosA}, 1, {kPosA}))

// --- BigInt family (TypeError on mixing; Rule 72) ------------------------------
TS_OP(BigIntAdd,      W1_RR, Throw, false, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(BigIntSub,      W1_RR, Throw, false, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(BigIntMul,      W1_RR, Throw, false, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(BigIntDiv,      W1_RR, Throw, false, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(BigIntMod,      W1_RR, Throw, false, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(BigIntNeg,      W1_R,  Throw, false, roles(1, {kPosA}, 1, {kPosA}))

// --- Comparison (dst = boolean result) ------------------------------------------
TS_OP(Lt,             W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(Le,             W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(Gt,             W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(Ge,             W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(AbstractEq,     W1_RR, Invoke, true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(StrictEq,       W1_RR, Pure,   true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(SameValue,      W1_RR, Pure,   true, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(SameValueZero,  W1_RR, Pure,   true, roles(2, {kPosA, kPosB}, 1, {kPosA}))

// --- Logic, type, conversion ---------------------------------------------------
TS_OP(LogicalNot,     W1_R, Pure,   false, roles(1, {kPosA}, 1, {kPosA}))
TS_OP(ToBoolean,      W1_R, Pure,   false, roles(1, {kPosA}, 1, {kPosA}))
TS_OP(TypeOf,         W1_R, Pure,   false, roles(1, {kPosA}, 1, {kPosA}))
TS_OP(ToNumber,       W1_R, Invoke, true,  roles(1, {kPosA}, 1, {kPosA}))
TS_OP(ToNumeric,      W1_R, Invoke, false, roles(1, {kPosA}, 1, {kPosA}))
TS_OP(ToString,       W1_R, Invoke, false, roles(1, {kPosA}, 1, {kPosA}))
TS_OP(ToInt32,        W1_R, Invoke, false, roles(1, {kPosA}, 1, {kPosA}))
TS_OP(ToUint32,       W1_R, Invoke, false, roles(1, {kPosA}, 1, {kPosA}))
TS_OP(ToBigInt,       W1_R, Invoke, false, roles(1, {kPosA}, 1, {kPosA}))
TS_OP(ToPrimitive,    W1_R, Invoke, false, roles(1, {kPosA}, 1, {kPosA}))

// --- Strings ------------------------------------------------------------------
TS_OP(StringConcat,   W1_RR, Alloc, false, roles(2, {kPosA, kPosB}, 1, {kPosA}))
TS_OP(StringLength,   W1_R,  Read,  false, roles(1, {kPosA}, 1, {kPosA}))
TS_OP(CharCodeAt,     W2_RR_D, Read, false, roles(2, {kPosB, kPosC}, 1, {kPosA}))
TS_OP(StringFromCharCode, W1_R, Invoke, false, roles(1, {kPosA}, 1, {kPosA}))

// --- Control flow and exceptions -----------------------------------------------
TS_OP(Jmp,            W1_J,  Pure, false, roles(0, {0,0,0,0}, 0, {0,0}))
TS_OP(JmpWide,        W2_BR, Pure, false, roles(0, {0,0,0,0}, 0, {0,0}))
TS_OP(JmpTrue,        W1_BR, Pure, true,  roles(1, {kPosA}, 0, {0,0}))
TS_OP(JmpFalse,       W1_BR, Pure, true,  roles(1, {kPosA}, 0, {0,0}))
TS_OP(JmpTrueWide,    W2_BR, Pure, true,  roles(1, {kPosA}, 0, {0,0}))
TS_OP(JmpFalseWide,   W2_BR, Pure, true,  roles(1, {kPosA}, 0, {0,0}))
TS_OP(Throw,          W1_R,  Throw, false, roles(1, {kPosA}, 0, {0,0}))
TS_OP(Rethrow,        W0,    Throw, false, roles(0, {0,0,0,0}, 0, {0,0}))

// --- Arrays (v0.2: bytecode_spec.md Section 5.12) ------------------------------
// GetElement/SetElement are keyed-access opcodes with IDENTICAL semantics to
// GetProperty/SetProperty (full ToPropertyKey routing, prototype chain, user
// hooks); the frontend picks them when the key is index-like so the feedback
// vector records element-kind sites (Section 8) instead of shape sites.
TS_OP(NewArray,       W1_R,    Alloc,  false, roles(0, {0,0,0,0}, 1, {kPosA}))
TS_OP(GetElement,     W2_RR_D, Invoke, true,  roles(2, {kPosB, kPosC}, 1, {kPosA}))
TS_OP(SetElement,     W2_RR_V, Invoke, true,  roles(3, {kPosA, kPosB, kPosC}, 0, {0,0}))
