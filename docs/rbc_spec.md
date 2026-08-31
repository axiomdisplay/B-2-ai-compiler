# B-2 Register Bytecode (RBC) Specification (v0)

Owner: IR Team (middle end; see `docs/teams/ir-team.md`)

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

The frozen public headers under `include/b2/rbc/` (`Type.h`, `Opcode.h`, `Rbc.h`,
`Verifier.h`, `RbcText.h`, `RbcBuilder.h`) are the C++ contract this document
specifies. Where a header comment is normative, this document restates it; where
a header is ambiguous or silent, this document pins the interpretation and
lists the pin in §10.1. If this document and the headers disagree on a point
the headers state explicitly, the headers win and this document is defective.

RBC is the **convergence point** of both B-2 entry paths and the sole input of
all four execution tiers:

```text
Java source                        Java classfiles
     |                                   |
     v                                   v
frontend lowering                   loader
     |                                   |
     +──────────────┬────────────────────┘
                    v
        Register Bytecode (this spec)
                    |
                    v
            verifier (§5)   <- hard gate: Law "verifier before quickener
                    |           before execution"
                    v
            quickener (§6)
                    |
        +-----------+------------+-----------+
        v           v            v           v
       T0          T1            T2          T3
  direct-      stencil        sea-of-      offline
  threaded     compositor     nodes IR     T2 pipeline
```

Every tier consumes the same instruction stream:

- **T0** dispatches directly-threaded on `Op` (Amendment B, `docs/laws.md`).
- **T1** maps each opcode to a precompiled stencil 1:1 (`docs/stencils.md` §3.1).
- **T2/T3** build the sea-of-nodes IR from RBC (Amendment B.1/B.5).
- **Deopt** from every tier lands at T0 with `(pc, frame)`
  (`docs/deopt_backend.md` Part A).

Scope of this document: the frame model, instruction encoding, opcode set,
type system, verification rules, quickening contract, text format,
determinism/serialization rules, tier consumption mapping, and the honest v0
limitation list. The sea-of-nodes IR that T2/T3 build *from* RBC is specified
elsewhere (IR team charter, `docs/teams/ir-team.md`).

Laws cited by number follow `docs/laws.md` numbering ("Rule N"); the RBC
headers refer to the same laws as "Law N" (e.g. "Law 15" = Rule 15,
"Law 124" = Rule 124, "Law 10x" = the Rule 10 family). The divergence is
cosmetic; this document uses the `laws.md` numbering.

---

# 1. Frame model

## 1.1 Frame contents

A frame is the unit of execution, suspension, and deoptimization. It contains:

```text
frame = { locals l0..lM-1          (M = numLocals)
          registers r0..rN-1       (N = numRegs)
          monitors                 (monitor stack, deopt_backend.md Part A §3)
          pending exception        (while an exception is in flight)
          pc                       (instruction index, §2) }
```

`Method::numLocals` and `Method::numRegs` fix M and N per method. Local slots
and working registers are **frame-resident and typed** (§4); the per-program-
point type of every slot and register is proven by the verifier (§5).

## 1.2 Local slots (l0..lM-1)

- Frame-resident, typed storage named by the frontend/lowering.
- **Parameters come first**: for an instance method, `l0` is the receiver
  (`Ref`) and `l1..` hold the parameters in declaration order; for a static
  method, `l0..` hold the parameters in declaration order.
- Declared locals follow the parameters in an order chosen by the producer
  (lowering or classfile translator); RBC itself is indifferent.
- **One slot per parameter, always.** `long` and `double` parameters occupy a
  single local slot each. This is a deliberate divergence from the JVM, where
  category-2 locals consume two slots (JVMS 2.6.1):

  1. The frame is the deoptimization unit. Deopt reconstruction is
     `pc + locals + register file + monitors` (`docs/deopt_backend.md` Part A
     §1, §3); flat, one-value-per-slot storage makes that reconstruction a
     memcpy and makes the T1 frame trivially T0-compatible
     (`docs/stencils.md` §8).
  2. Slot-index arithmetic becomes uniform: no cat-2 pairing rules, no
     "unpaired local" verifier errors, no `l1`/`l2` aliasing hazards.
  3. GC maps stay flat: one type tag per slot (`docs/stencils.md` §8.2).

  `isCategory2()` is retained in `Type.h` for JVM spec parity only; it has no
  slot-allocation meaning in RBC.

- A local slot holds exactly one value of its verified type; the type is
  established by the entry state (parameters) or by a `*store` opcode and must
  remain join-stable at every program point (§5.2), in particular across
  protected ranges (§5.3).

## 1.3 Working registers (r0..rN-1)

- Expression temporaries allocated by lowering (or by the classfile
  translator's stack-to-register mapping).
- Registers are addressed by the `u16` fields `dst`/`a`/`b` of `Ins`
  (§2); therefore `numRegs <= 65536` and every register operand must satisfy
  `reg < numRegs` (structural rule V-S2, §5.1). Local slots are addressed via
  the `u32` `imm` field, so `numLocals` has no `u16` ceiling.
- Registers are frame-resident: **all registers and locals survive calls**
  (frame residency is what makes T1's register caching safe,
  `docs/stencils.md` §8.1). An `invoke*` writes only its `dst` (the result);
  argument registers `a..a+b-1` are read, never clobbered.
- **Exception delivery (spec pin, §10.1):** when control transfers to an
  exception handler entry, the pending exception object is delivered in `r0`
  (typed `Ref` at handler entry). A method with handlers needs `numRegs >= 1`.

## 1.4 Why this shape (T0 compatibility)

The frame model is chosen so that T1 deopt is trivial
(`docs/stencils.md` §8):

- The baseline (T1) frame contains the T0 register file in memory; locals and
  the register file live in the frame.
- T1 **may cache registers in native machine registers between flush points**
  (safepoints, calls, deopt points) provided it flushes to the frame before
  any point that requires canonical state (`docs/stencils.md` §8.1).
- Deopt reconstruction is then: find the current RBC pc, flush cached
  registers, materialize nothing speculative, enter T0
  (`docs/stencils.md` §8, `docs/deopt_backend.md` Part A §1). The golden rule
  of `docs/deopt_backend.md` Part A §1 — resumption must be observationally
  indistinguishable from the state T0 would have reached at that RBC
  instruction boundary — is satisfied by construction because there is no
  second frame format to translate.

---

# 2. Instruction encoding

## 2.1 The `Ins` record

Every instruction is a fixed 12-byte record (`Rbc.h`), field order = logical
operand order:

```text
 offset  size  field   meaning
 0       2     op      Op opcode (u16); must be < Op::_Count
 2       2     dst     destination register / result (u16)
 4       2     a       first source register / arg base (u16)
 6       2     b       second source register / arg count (u16)
 8       4     imm     immediate / cp index / branch target / deopt id (u32)
```

```cpp
struct Ins {
  std::uint16_t op;
  std::uint16_t dst;
  std::uint16_t a;
  std::uint16_t b;
  std::uint32_t imm;
};  // 12 bytes, trivially copyable
```

## 2.2 pc is an instruction index, not a byte offset

`pc` is the index of an `Ins` in `Method::code`. Consequences (all normative):

- **Branch targets are instruction indices**, stored in `imm`, and must lie in
  `[0, code.size())` (rule V-S5).
- **Constants, field refs, method refs, class refs, switch tables are constant
  pool indices** into the method's own pool (§8.1), stored in `imm`, except
  for quickened variants where `imm` is repurposed (§6).
- `Method::code` is a `std::vector<Ins>`: no variable-length instructions, no
  padding, no alignment holes. `pc + 1` is always the fall-through
  instruction.

Why fixed-width, index-addressed encoding (from `Rbc.h`/`Opcode.h`, both
normative on this point):

1. **Relocation.** Indices survive code copying, inlining into other arrays,
   and AOT artifact loading without patching arithmetic. No pointer into a
   container that can reallocate (Rule 15 discipline).
2. **pc maps.** Native-pc -> RBC-pc maps for T1/T2 deopt
   (`docs/deopt_backend.md` Part A §8) become a plain sorted table of native
   addresses per instruction index; the RBC side needs no computation at all.
3. **In-place quickening.** The quickener replaces an opcode by swapping the
   `op` field only (§6). Because the record never changes size or moves, no
   neighbor is relocated, no branch is re-patched, and a pc captured before
   quickening remains valid after it.
4. **Trivial interpreter stepping.** `pc + 1` is the next instruction; direct-
   threaded dispatch (T0) walks the array with no decoding.

## 2.3 Operand signatures (`Sig`)

`Sig` (`Opcode.h`) is the shared contract that says how `dst`/`a`/`b`/`imm`
are interpreted for a given opcode. It is used by the verifier, the text
printer/parser, the quickener, and stencil selection. "—" means the field is
unused (canonical form is 0; see §2.4).

| Sig | dst | a | b | imm | Text operand form | Representative opcodes |
|---|---|---|---|---|---|---|
| `None` | — | — | — | — | `mnemonic` | `nop`, `safepoint_poll`, `return` |
| `Reg` | — | source reg | — | — | `m rA` | `monitorenter`, `ireturn`, `athrow` |
| `RegImm` | dst | — | — | immediate | `m rD, imm` | `iconst`, `fconst`, `aconst_null`, `iinc` |
| `RegCp` | dst | — | — | cp index | `m rD, cK` | `lconst`, `dconst`, `ldc`, `new`, `getstatic`, `putstatic`¹ |
| `RegSlot` | dst | — | — | local slot | `m rD, lS` | `iload` .. `aload` |
| `SlotReg` | — | source reg | — | local slot | `m rA, lS` | `istore` .. `astore` |
| `RegReg` | dst | source | — | — | `m rD, rA` | `imove`, `ineg`, `i2l`, `arraylength` |
| `RegRegReg` | dst | source 1 | source 2 | — | `m rD, rA, rB` | `iadd`, `icmp`, `iaload`, `iastore`² |
| `RegRegImm` | dst | source | — | immediate | `m rD, rA, imm` | `newarray` |
| `RegRegCp` | dst | source | — | cp index | `m rD, rA, cK` | `getfield`, `anewarray`, `instanceof`, `checkcast`³ |
| `RegRegRegCp` | — | source 1 | source 2 | cp index | `m rA, rB, cK` | `putfield`, `putfield_quick`, `multianewarray` |
| `Branch` | — | — | — | target pc | `m L` | `goto` |
| `RegBranch` | — | test reg | — | target pc | `m rA, L` | `ifeq` .. `ifle`, `ifnull`, `ifnonnull` |
| `RegRegBranch` | — | test 1 | test 2 | target pc | `m rA, rB, L` | `if_icmpeq` .. `if_icmple`, `if_acmpeq`, `if_acmpne` |
| `RegCpBranch` | — | selector | — | cp (SwitchTable) | `m rA, cK` | `tableswitch`, `lookupswitch` |
| `Call` | result | arg base | arg count | cp MethodRef | `m rD, rA, n, cK` | `invokevirtual`, `invokespecial`, `invokestatic`, `invokeinterface`, `invokedynamic` |
| `CallQuick` | result | arg base | arg count | MethodId / IC site id | `m rD, rA, n, imm` | `invokevirtual_quick` .. `invokeinterface_quick` |
| `Guard` | — | tested reg | — | DeoptId | `m rA, imm` | `guard_non_null` |
| `GuardCp` | — | tested reg | DeoptId | cp Class | `m rA, imm, cK` | `guard_class` |
| `Trap` | — | — | — | DeoptId | `m imm` | `deopt_trap` |

¹ `putstatic`: `dst` holds the value register and is **read, not written**
(spec pin, §10.1).
² Array stores (`*astore`) use `Sig::RegRegReg` with `dst` read as the stored
value, `a` = array reference, `b` = index (spec pin, §10.1).
³ `checkcast` is listed under both `RegReg` and `RegRegCp` in the `Opcode.h`
`Sig` comments; it requires a cp operand (`dst = (cp[imm]) a`), so `RegRegCp`
is normative (inconsistency reported, §10.2).

## 2.4 Canonical unused fields

Where a `Sig` leaves a field unused, the canonical encoding is 0. The verifier
does not reject non-zero unused fields in v0 (except where noted), but the
builder emits 0 and the text printer round-trips 0; golden tests should only
ever see 0. `putfield`/`putfield_quick` (`dst` unused) and void `invoke*`
(`dst` unused) are the notable cases.

---

# 3. Complete opcode reference

`Op` is a `u16` enum; `Op::_Count == 150` in v0. The tables below cover every
opcode exactly once, grouped as in `Opcode.h`. Mnemonics are the canonical
`OpInfo::name` strings (lowercase, JVM-familiar; the `Op` enumerator is the
CamelCase of the mnemonic). Semantics are derived from the `Opcode.h`
comments, which are normative; JVM semantics apply where the comments defer
("JVM parity" rows).

Notation:

- Registers: `rD` = dst, `rA` = a, `rB` = b; locals `lS`; cp `cK`; labels `L`.
- Types: `I` Int, `L` Long, `F` Float, `D` Double, `R` Ref, `N` Null.
- Effects (Eff flags, §3.19): `R` ReadsHeap, `W` WritesHeap, `C` CanCall,
  `T` CanTrap, `A` CanAllocate, `B` CanBranch, `S` IsSafepoint, `Q` Quickened.
  "—" = no flags. Effect assignments are normative from this document; they
  are what `compiler/rbc/src/Opcode.cpp` must implement.
- All arithmetic follows Java/JVM semantics: two's-complement wrapping
  integers, IEEE 754 floats, no traps on overflow.

## 3.1 Misc

| Op | Mnemonic | Operands | Semantics | Traps / effects |
|---|---|---|---|---|
| Nop | `nop` | — | no effect | — |
| SafepointPoll | `safepoint_poll` | — | explicit safepoint poll point; T0/T1 must honor it (poll, stack map). Lowering emits one on every loop backedge | S |

## 3.2 Constants

| Op | Mnemonic | Sig | Operands | Semantics | Traps / effects |
|---|---|---|---|---|---|
| AconstNull | `aconst_null` | RegImm | `rD` | `rD = null` (imm unused, 0); rD:N | — |
| Iconst | `iconst` | RegImm | `rD, imm` | `rD = (int32_t)imm`; rD:I | — |
| Fconst | `fconst` | RegImm | `rD, imm` | `rD = bit_cast<float>(imm)`; rD:F. imm is the IEEE 754 bit pattern; the text form prints it as a decimal integer (§8.4) | — |
| Lconst | `lconst` | RegCp | `rD, cK` | `rD = cp[K].i64` (Kind::Int64); rD:L | — |
| Dconst | `dconst` | RegCp | `rD, cK` | `rD = cp[K].f64` (Kind::Double); rD:D | — |
| Ldc | `ldc` | RegCp | `rD, cK` | `rD =` runtime constant for `cp[K]` of Kind String / Class / MethodType / MethodHandle; rD:R | T (resolution failure), A (string materialization) |

v0 note: no opcode loads cp entries of Kind::Int32 or Kind::Float directly —
`iconst`/`fconst` carry immediates. Those pool kinds exist for builder
convenience, serialization completeness, and future use (§10).

## 3.3 Local slots <-> registers

Loads: `Sig::RegSlot`, `rD` typed by the family, slot type must match (§5.2).
Stores: `Sig::SlotReg`, `local[S] = rA`, slot takes the family type.

| Op | Mnemonic | Operands | Semantics | Types | Traps / effects |
|---|---|---|---|---|---|
| Iload | `iload` | `rD, lS` | `rD = (int) local[S]` | ->I | — |
| Lload | `lload` | `rD, lS` | `rD = (long) local[S]` | ->L | — |
| Fload | `fload` | `rD, lS` | `rD = (float) local[S]` | ->F | — |
| Dload | `dload` | `rD, lS` | `rD = (double) local[S]` | ->D | — |
| Aload | `aload` | `rD, lS` | `rD = (ref) local[S]` | ->R/N | — |
| Istore | `istore` | `rA, lS` | `local[S] = (int) rA` | I-> | — |
| Lstore | `lstore` | `rA, lS` | `local[S] = (long) rA` | L-> | — |
| Fstore | `fstore` | `rA, lS` | `local[S] = (float) rA` | F-> | — |
| Dstore | `dstore` | `rA, lS` | `local[S] = (double) rA` | D-> | — |
| Astore | `astore` | `rA, lS` | `local[S] = (ref) rA` | R/N-> | — |

## 3.4 Typed moves

`Sig::RegReg`; `rD = rA`, same type both sides; no representation change.

| Op | Mnemonic | Types |
|---|---|---|
| Imove | `imove` | I |
| Lmove | `lmove` | L |
| Fmove | `fmove` | F |
| Dmove | `dmove` | D |
| Amove | `amove` | R/N |

## 3.5 Integer arithmetic

`Sig::RegRegReg` (`rA`:`I`, `rB`:`I` -> `rD`:`I`) unless noted; 32-bit
two's-complement wraparound; no overflow traps.

| Op | Mnemonic | Operands | Semantics | Traps / effects |
|---|---|---|---|---|
| Iadd | `iadd` | `rD, rA, rB` | `rD = rA + rB` | — |
| Isub | `isub` | `rD, rA, rB` | `rD = rA - rB` | — |
| Imul | `imul` | `rD, rA, rB` | `rD = rA * rB` | — |
| Idiv | `idiv` | `rD, rA, rB` | `rD = rA / rB` (round toward zero); `MIN_INT / -1 == MIN_INT` | T: ArithmeticException if `rB == 0` |
| Irem | `irem` | `rD, rA, rB` | `rD = rA % rB` (sign of dividend); `MIN_INT % -1 == 0` | T: ArithmeticException if `rB == 0` |
| Ineg | `ineg` | `rD, rA` (RegReg) | `rD = -rA`; `-INT_MIN == INT_MIN` | — |
| Ishl | `ishl` | `rD, rA, rB` | `rD = rA << (rB & 0x1f)` | — |
| Ishr | `ishr` | `rD, rA, rB` | `rD = rA >> (rB & 0x1f)` (arithmetic) | — |
| Iushr | `iushr` | `rD, rA, rB` | `rD = (uint32)rA >> (rB & 0x1f)` (logical) | — |
| Iand | `iand` | `rD, rA, rB` | `rD = rA & rB` | — |
| Ior | `ior` | `rD, rA, rB` | `rD = rA | rB` | — |
| Ixor | `ixor` | `rD, rA, rB` | `rD = rA ^ rB` | — |
| Iinc | `iinc` | `rD, imm` (RegImm) | `rD = rD + (int32_t)imm` — the only read-modify-write arithmetic op; `rD` is both source and destination, type I | — |

## 3.6 Long arithmetic

`Sig::RegRegReg` (`rA`:`L`, `rB`:`L` -> `rD`:`L`) unless noted; 64-bit
wraparound.

| Op | Mnemonic | Operands | Semantics | Traps / effects |
|---|---|---|---|---|
| Ladd | `ladd` | `rD, rA, rB` | `rD = rA + rB` | — |
| Lsub | `lsub` | `rD, rA, rB` | `rD = rA - rB` | — |
| Lmul | `lmul` | `rD, rA, rB` | `rD = rA * rB` | — |
| Ldiv | `ldiv` | `rD, rA, rB` | `rD = rA / rB`; `MIN_LONG / -1 == MIN_LONG` | T: ArithmeticException if `rB == 0` |
| Lrem | `lrem` | `rD, rA, rB` | `rD = rA % rB`; `MIN_LONG % -1 == 0` | T: ArithmeticException if `rB == 0` |
| Lneg | `lneg` | `rD, rA` (RegReg) | `rD = -rA` | — |
| Lshl | `lshl` | `rD, rA, rB` | `rD = rA << (rB & 0x3f)`; `rB` is `I` | — |
| Lshr | `lshr` | `rD, rA, rB` | `rD = rA >> (rB & 0x3f)` (arithmetic); `rB` is `I` | — |
| Lushr | `lushr` | `rD, rA, rB` | `rD = (uint64)rA >> (rB & 0x3f)` (logical); `rB` is `I` | — |
| Land | `land` | `rD, rA, rB` | `rD = rA & rB` | — |
| Lor | `lor` | `rD, rA, rB` | `rD = rA | rB` | — |
| Lxor | `lxor` | `rD, rA, rB` | `rD = rA ^ rB` | — |

(There is no `linc`; `iinc` is int-only, matching `Opcode.h`.)

## 3.7 Float arithmetic

`Sig::RegRegReg` (`rA`:`F`, `rB`:`F` -> `rD`:`F`) unless noted; IEEE 754
single precision; **no traps ever** (division by zero produces an infinity).

| Op | Mnemonic | Operands | Semantics |
|---|---|---|---|
| Fadd | `fadd` | `rD, rA, rB` | `rD = rA + rB` |
| Fsub | `fsub` | `rD, rA, rB` | `rD = rA - rB` |
| Fmul | `fmul` | `rD, rA, rB` | `rD = rA * rB` |
| Fdiv | `fdiv` | `rD, rA, rB` | `rD = rA / rB` |
| Frem | `frem` | `rD, rA, rB` | `rD = IEEE remainder (JVM % semantics)` |
| Fneg | `fneg` | `rD, rA` (RegReg) | `rD = -rA` (flips the sign bit, including on NaN) |

## 3.8 Double arithmetic

`Sig::RegRegReg` (`rA`:`D`, `rB`:`D` -> `rD`:`D`) unless noted; IEEE 754
double precision; no traps.

| Op | Mnemonic | Operands | Semantics |
|---|---|---|---|
| Dadd | `dadd` | `rD, rA, rB` | `rD = rA + rB` |
| Dsub | `dsub` | `rD, rA, rB` | `rD = rA - rB` |
| Dmul | `dmul` | `rD, rA, rB` | `rD = rA * rB` |
| Ddiv | `ddiv` | `rD, rA, rB` | `rD = rA / rB` |
| Drem | `drem` | `rD, rA, rB` | `rD = IEEE remainder (JVM % semantics)` |
| Dneg | `dneg` | `rD, rA` (RegReg) | `rD = -rA` |

## 3.9 Comparisons

All produce `rD`:`I` in `{-1, 0, 1}`; no traps.

| Op | Mnemonic | Operands | Semantics |
|---|---|---|---|
| Icmp | `icmp` | `rD, rA, rB` | `rD = (rA > rB) - (rA < rB)`; `rA,rB`:I |
| Lcmp | `lcmp` | `rD, rA, rB` | `rD = (rA > rB) - (rA < rB)`; `rA,rB`:L |
| Fcmpl | `fcmpl` | `rD, rA, rB` | three-way compare; **NaN compares less** (`rD = -1` if either operand is NaN); `rA,rB`:F |
| Fcmpg | `fcmpg` | `rD, rA, rB` | three-way compare; **NaN compares greater** (`rD = +1`); `rA,rB`:F |
| Dcmpl | `dcmpl` | `rD, rA, rB` | as `fcmpl`, double; `rA,rB`:D |
| Dcmpg | `dcmpg` | `rD, rA, rB` | as `fcmpg`, double; `rA,rB`:D |

## 3.10 Conversions

All `Sig::RegReg` (`rD = convert(rA)`); no traps; JVM conversion semantics.

| Op | Mnemonic | From -> To | Semantics |
|---|---|---|---|
| I2l | `i2l` | I -> L | sign-extend |
| I2f | `i2f` | I -> F | round to nearest (IEEE) |
| I2d | `i2d` | I -> D | exact |
| L2i | `l2i` | L -> I | low 32 bits |
| L2f | `l2f` | L -> F | round to nearest |
| L2d | `l2d` | L -> D | round to nearest |
| F2i | `f2i` | F -> I | NaN -> 0; +-inf / out-of-range clamps to `INT_MIN`/`INT_MAX`; round toward zero |
| F2l | `f2l` | F -> L | NaN -> 0; clamp to `LONG_MIN`/`LONG_MAX`; round toward zero |
| F2d | `f2d` | F -> D | exact widening |
| D2i | `d2i` | D -> I | as `f2i` |
| D2l | `d2l` | D -> L | as `f2l` |
| D2f | `d2f` | D -> F | round to nearest |
| I2b | `i2b` | I -> I | truncate to int8, sign-extend |
| I2c | `i2c` | I -> I | truncate to uint16, zero-extend (char) |
| I2s | `i2s` | I -> I | truncate to int16, sign-extend |

`i2b`/`i2c`/`i2s` exist because `boolean`/`byte`/`char`/`short` all fold into
`Int` at the RBC level (§4.3); these ops re-narrow after mixed arithmetic.

## 3.11 Branches and switches

Targets are instruction indices in `imm`. Conditional tests compare against 0
(JVM `if*` style) or pair registers (`if_icmp*`/`if_acmp*`). No traps; all
branch ops carry effect `B`.

| Op | Mnemonic | Sig | Operands | Semantics | Types |
|---|---|---|---|---|---|
| Goto | `goto` | Branch | `L` | unconditional transfer to `L` | — |
| Ifeq | `ifeq` | RegBranch | `rA, L` | if `rA == 0` goto `L` | rA:I |
| Ifne | `ifne` | RegBranch | `rA, L` | if `rA != 0` goto `L` | rA:I |
| Iflt | `iflt` | RegBranch | `rA, L` | if `rA < 0` goto `L` | rA:I |
| Ifge | `ifge` | RegBranch | `rA, L` | if `rA >= 0` goto `L` | rA:I |
| Ifgt | `ifgt` | RegBranch | `rA, L` | if `rA > 0` goto `L` | rA:I |
| Ifle | `ifle` | RegBranch | `rA, L` | if `rA <= 0` goto `L` | rA:I |
| Ifnull | `ifnull` | RegBranch | `rA, L` | if `rA == null` goto `L` | rA:R/N |
| Ifnonnull | `ifnonnull` | RegBranch | `rA, L` | if `rA != null` goto `L` | rA:R/N |
| IfIcmpeq | `if_icmpeq` | RegRegBranch | `rA, rB, L` | if `rA == rB` goto `L` | rA,rB:I |
| IfIcmpne | `if_icmpne` | RegRegBranch | `rA, rB, L` | if `rA != rB` goto `L` | rA,rB:I |
| IfIcmplt | `if_icmplt` | RegRegBranch | `rA, rB, L` | if `rA < rB` goto `L` | rA,rB:I |
| IfIcmpge | `if_icmpge` | RegRegBranch | `rA, rB, L` | if `rA >= rB` goto `L` | rA,rB:I |
| IfIcmpgt | `if_icmpgt` | RegRegBranch | `rA, rB, L` | if `rA > rB` goto `L` | rA,rB:I |
| IfIcmple | `if_icmple` | RegRegBranch | `rA, rB, L` | if `rA <= rB` goto `L` | rA,rB:I |
| IfAcmpeq | `if_acmpeq` | RegRegBranch | `rA, rB, L` | if `rA` and `rB` are the **same object** (reference identity) goto `L` | rA,rB:R/N |
| IfAcmpne | `if_acmpne` | RegRegBranch | `rA, rB, L` | if not the same object goto `L` | rA,rB:R/N |
| Tableswitch | `tableswitch` | RegCpBranch | `rA, cK` | dense switch: `cp[K]` (Kind::SwitchTable) holds match/target pairs whose matches are consecutive (`low..high`); transfer to the target whose match equals `rA`, else the default (below) | rA:I |
| Lookupswitch | `lookupswitch` | RegCpBranch | `rA, cK` | sparse switch: `cp[K]` holds strictly-ascending match/target pairs; transfer to the matching target, else the default | rA:I |

Switch payload and default (spec pins, §10.1):

- `Const::Kind::SwitchTable` stores the payload in `ints` as flat
  match/target **pairs**: `[m0, t0, m1, t1, ...]`, matches strictly
  ascending. `tableswitch` additionally requires **dense** matches
  (`mi == m0 + i`), so its pairs enumerate `low..high`; `lookupswitch`
  accepts any strictly ascending matches.
- **The default target is the fall-through instruction `pc + 1`** for both
  switch opcodes. A switch may therefore never be the last instruction of a
  method (rule V-S7). Producers that need a non-adjacent default emit the
  default path (or a `goto`) at `pc + 1`.
- Selection semantics are identical for the two opcodes; `tableswitch` is the
  density promise that lets T1/T2 lower to a jump table
  (`docs/stencils.md` §3.1, §11).
- All targets must be valid instruction indices (rule V-S5).

## 3.12 Fields

| Op | Mnemonic | Sig | Operands | Semantics | Types | Traps / effects |
|---|---|---|---|---|---|---|
| Getfield | `getfield` | RegRegCp | `rD, rA, cK` | `rD = rA.field`; `cp[K]` = FieldRef (class, name, descriptor); result typed from the field descriptor | rA:R/N -> rD:field type | R, T (NPE if `rA` null; resolution) |
| Putfield | `putfield` | RegRegRegCp | `rA, rB, cK` | `rA.field = rB`; `dst` unused (0); `cp[K]` = FieldRef | rA:R/N, rB:field type | W, T (NPE; resolution) |
| Getstatic | `getstatic` | RegCp | `rD, cK` | `rD = static field`; `cp[K]` = FieldRef; result typed from the field descriptor | rD:field type | R, C, T (class init) |
| Putstatic | `putstatic` | RegCp | `rD, cK` | `static field = rD` — **`dst` is read, not written** (spec pin, §10.1); `cp[K]` = FieldRef | rD:field type | W, C, T (class init) |
| GetfieldQuick | `getfield_quick` | RegRegCp | `rD, rA, imm` | quickened `getfield`: `imm` = **resolved field byte offset**, not a cp index; same type rules | rA:R/N -> rD:field type | R, T, Q |
| PutfieldQuick | `putfield_quick` | RegRegRegCp | `rA, rB, imm` | quickened `putfield`: `imm` = resolved byte offset; `dst` unused | rA:R/N, rB:field type | W, T, Q |

Quickened forms never appear in unquickened streams (§6).

## 3.13 Arrays

Loads: `Sig::RegRegReg`, `rD = rA[rB]`, `rA` = array reference (`R`/`N`),
`rB` = index (`I`); element type by family. Stores: `Sig::RegRegReg`
(`rD` **read** as the stored value, `rA` = array reference, `rB` = index —
spec pin, §10.1). All array element access traps `NullPointerException` (null
array) and `ArrayIndexOutOfBoundsException` (index out of `[0, length)`).

| Op | Mnemonic | Operands | Semantics | Types | Traps / effects |
|---|---|---|---|---|---|
| NewArray | `newarray` | `rD, rA, atype` (RegRegImm) | `rD = new prim_array<atype>[rA]`; imm = Atype code (4..11) | rA:I -> rD:R | A, T (NegativeArraySizeException if `rA < 0`; OOM) |
| AnewArray | `anewarray` | `rD, rA, cK` (RegRegCp) | `rD = new ref_array<cp[K]>[rA]`; `cp[K]` = Class | rA:I -> rD:R | A, T (negative size; OOM) |
| Arraylength | `arraylength` | `rD, rA` (RegReg) | `rD = rA.length` | rA:R/N -> rD:I | T (NPE) |
| Iaload | `iaload` | `rD, rA, rB` | `rD = rA[rB]` (int[]) | ->I | R, T |
| Laload | `laload` | `rD, rA, rB` | `rD = rA[rB]` (long[]) | ->L | R, T |
| Faload | `faload` | `rD, rA, rB` | `rD = rA[rB]` (float[]) | ->F | R, T |
| Daload | `daload` | `rD, rA, rB` | `rD = rA[rB]` (double[]) | ->D | R, T |
| Aaload | `aaload` | `rD, rA, rB` | `rD = rA[rB]` (ref[]) | ->R/N | R, T |
| Baload | `baload` | `rD, rA, rB` | `rD = (int) rA[rB]` (byte/boolean[], sign-extended) | ->I | R, T |
| Caload | `caload` | `rD, rA, rB` | `rD = (int) rA[rB]` (char[], zero-extended) | ->I | R, T |
| Saload | `saload` | `rD, rA, rB` | `rD = (int) rA[rB]` (short[], sign-extended) | ->I | R, T |
| Iastore | `iastore` | `rD, rA, rB` | `rA[rB] = rD` (int[]) | rD:I | W, T |
| Lastore | `lastore` | `rD, rA, rB` | `rA[rB] = rD` (long[]) | rD:L | W, T |
| Fastore | `fastore` | `rD, rA, rB` | `rA[rB] = rD` (float[]) | rD:F | W, T |
| Dastore | `dastore` | `rD, rA, rB` | `rA[rB] = rD` (double[]) | rD:D | W, T |
| Aastore | `aastore` | `rD, rA, rB` | `rA[rB] = rD` (ref[]); runtime store check | rD:R/N | W, T (also ArrayStoreException) |
| Bastore | `bastore` | `rD, rA, rB` | `rA[rB] = (int8) rD` (byte/boolean[]) | rD:I | W, T |
| Castore | `castore` | `rD, rA, rB` | `rA[rB] = (uint16) rD` (char[]) | rD:I | W, T |
| Sastore | `sastore` | `rD, rA, rB` | `rA[rB] = (int16) rD` (short[]) | rD:I | W, T |
| Multianewarray | `multianewarray` | `rD, rA, rB, cK` (RegRegRegCp) | `rD = new cp[K]` (array class) with `rB` dimensions; **dimension counts occupy consecutive registers `rA..rA+rB-1`**, each `I` (spec pin, §10.1); trailing unspecified dimensions default to length 0 | ->R | A, T (negative size; OOM) |

v0: one-dimensional allocation is expressed with `newarray`/`anewarray`;
`multianewarray` is verified (structurally and type-wise) but full runtime
multi-dimension allocation support arrives with the classfile path (§10).

## 3.14 Objects and monitors

| Op | Mnemonic | Sig | Operands | Semantics | Traps / effects |
|---|---|---|---|---|---|
| New | `new` | RegCp | `rD, cK` | `rD = new cp[K]` (class instance); the reference must be initialized by a following `invokespecial <init>` before use (§5.5); class init may run | A, C, T (class init; OOM) |
| Checkcast | `checkcast` | RegRegCp | `rD, rA, cK` | `rD = (cp[K]) rA`; null passes through unchanged; `cp[K]` = Class | rA:R/N -> rD:R | T (ClassCastException) |
| Instanceof | `instanceof` | RegRegCp | `rD, rA, cK` | `rD = (rA instanceof cp[K]) ? 1 : 0`; null -> 0; `cp[K]` = Class | rA:R/N -> rD:I | R, T (resolution) |
| Monitorenter | `monitorenter` | Reg | `rA` | acquire the monitor of `rA`; may block | rA:R/N | T (NPE) |
| Monitorexit | `monitorexit` | Reg | `rA` | release the monitor of `rA` | rA:R/N | T (NPE; IllegalMonitorStateException) |

Monitor enter/exit balancing is not verified in v0 (§10).

## 3.15 Calls

**Call convention (normative, `Opcode.h`):** for every `invoke*`,

```text
dst = result register (valid unless the callee is void; then dst is
      unused and canonical 0)
a   = first argument register; the arguments occupy consecutive registers
      a .. a + argCount - 1
b   = argCount (includes the receiver where there is one)
imm = cp index of the MethodRef
```

- `invokevirtual`, `invokespecial`, `invokeinterface`: **the receiver is
  argument register `a[0]`**, included in `argCount`.
- `invokestatic`: arguments are the parameters only (no receiver).
- Structural rule V-S3: `a + b <= numRegs` — every argument register must be
  in range.
- The verifier types each argument register against `parseParams(descriptor)`
  of the MethodRef and the result register from `parseReturn(descriptor)`
  (§5.2). Receivers verify as `R`.
- All `invoke*` ops carry effects `R W C T A S` (a call can do anything and is
  a safepoint); `invokestatic`/`invokespecial` additionally trigger class
  initialization on first use.

| Op | Mnemonic | Sig | Operands | Semantics |
|---|---|---|---|---|
| Invokevirtual | `invokevirtual` | Call | `rD, rA, n, cK` | dispatch on the runtime class of receiver `rA[0]`; `cp[K]` = MethodRef (class, name, descriptor) |
| Invokespecial | `invokespecial` | Call | `rD, rA, n, cK` | direct dispatch (JVM semantics: `<init>`, private, `super` calls); `cp[K]` = MethodRef |
| Invokestatic | `invokestatic` | Call | `rD, rA, n, cK` | dispatch on the declaring class; `cp[K]` = MethodRef |
| Invokeinterface | `invokeinterface` | Call | `rD, rA, n, cK` | itable-based dispatch on receiver `rA[0]`; `cp[K]` = InterfaceMethodRef (spec pin, §10.1) |
| Invokedynamic | `invokedynamic` | Call | `rD, rA, n, cK` | `cp[K]` = InvokeDynamic (name, descriptor); **v0: verifier-only, no runtime linkage** (§10) |
| InvokevirtualQuick | `invokevirtual_quick` | CallQuick | `rD, rA, n, imm` | quickened: `imm` = resolved **inline-cache site id** (§6) |
| InvokespecialQuick | `invokespecial_quick` | CallQuick | `rD, rA, n, imm` | quickened: `imm` = resolved **MethodId** |
| InvokestaticQuick | `invokestatic_quick` | CallQuick | `rD, rA, n, imm` | quickened: `imm` = resolved **MethodId** |
| InvokeinterfaceQuick | `invokeinterface_quick` | CallQuick | `rD, rA, n, imm` | quickened: `imm` = resolved **inline-cache site id** |

## 3.16 Returns

All returns terminate the current path (§5.4); they carry no effects of their
own at the RBC level (synchronized-method monitor release on exit is a
method-flag concern, not an opcode effect).

| Op | Mnemonic | Sig | Operands | Semantics | Types |
|---|---|---|---|---|---|
| Return | `return` | None | — | void return | — |
| Ireturn | `ireturn` | Reg | `rA` | return `rA` | rA:I |
| Lreturn | `lreturn` | Reg | `rA` | return `rA` | rA:L |
| Freturn | `freturn` | Reg | `rA` | return `rA` | rA:F |
| Dreturn | `dreturn` | Reg | `rA` | return `rA` | rA:D |
| Areturn | `areturn` | Reg | `rA` | return `rA` | rA:R/N |

The returned value's type must be assignable to `parseReturn(descriptor)`
(§5.2).

## 3.17 Exceptions

| Op | Mnemonic | Sig | Operands | Semantics | Traps / effects |
|---|---|---|---|---|---|
| Athrow | `athrow` | Reg | `rA` | throw `rA`; terminates the path; the runtime searches the enclosing handlers (§5.3) | rA:R/N; T (NPE if `rA` is null) |

## 3.18 Deopt / tiering hooks (`docs/deopt_backend.md`)

| Op | Mnemonic | Sig | Operands | Semantics | Traps / effects |
|---|---|---|---|---|---|
| DeoptTrap | `deopt_trap` | Trap | `imm` | unconditional transfer to T0 with `DeoptId = imm`; terminates the path | T, S |
| GuardNonNull | `guard_non_null` | Guard | `rA, imm` | if `rA == null` -> deopt with `DeoptId = imm`; else no-op | rA:R/N; T |
| GuardClass | `guard_class` | GuardCp | `rA, imm, cK` | if `klass(rA) != cp[K]` -> deopt with `DeoptId = imm` (b); `cp[K]` = Class; **v0: verifier-only** (no tier emits it yet) | rA:R; T |

These opcodes exist so T1/T2 can express eager guard deopt
(`docs/deopt_backend.md` Part A §2.1) directly in the stream. Producers other
than the optimizing tiers must not emit them.

## 3.19 Effect flags

`Eff` (`Opcode.h`) is the per-opcode effect metadata; stencil selection
(T1), the T2 scheduler, and any reordering pass must respect it (Rule 10
family; `docs/stencils.md` §4 effect tags):

| Flag | Meaning |
|---|---|
| `ReadsHeap` | reads Java heap state |
| `WritesHeap` | writes Java heap state |
| `CanCall` | may invoke arbitrary Java code (including `<clinit>`) |
| `CanTrap` | may throw / deopt / abort |
| `CanAllocate` | may allocate |
| `CanBranch` | control-flow target in `imm` |
| `IsSafepoint` | poll + stack map required |
| `Quickened` | quickened variant; never in unquickened streams |

Flags combine with `|`; `hasEff` tests membership. The per-opcode assignments
are the Effects column of §3.1–§3.18; `compiler/rbc/src/Opcode.cpp` implements
them as a single `constexpr` table indexed by opcode, kept trivially copyable
so T0 can host it in `.rodata` next to the dispatch table.

---

# 4. Type system

## 4.1 The `RType` lattice

RBC verification types (`Type.h`), for registers and local slots:

```text
enum class RType : std::uint8_t {
  Bottom = 0,  // unreachable / uninitialized
  Int,         // 32-bit signed integer (boolean/byte/char/short fold into Int)
  Long,        // 64-bit signed integer
  Float,       // 32-bit IEEE 754
  Double,      // 64-bit IEEE 754
  Null,        // the null reference (subtype of Ref)
  Ref,         // object reference (class/interface/array)
};
```

The lattice is **flat except `Null <: Ref`**: every type is above `Bottom`;
`Int`/`Long`/`Float`/`Double`/`Ref` are pairwise incomparable. There is no
`Int <: Long` — all numeric widening/narrowing is explicit conversion opcodes
(§3.10). There is no class-hierarchy subtyping at the RBC level: all object
references verify as `Ref` (class-assignability checks like `checkcast` are
runtime operations, not lattice facts).

`typeName()` strings, used verbatim by diagnostics and the text tools:
`"bottom"`, `"int"`, `"long"`, `"float"`, `"double"`, `"null"`, `"ref"`.

## 4.2 Join and assignability

`join(a, b)` is the least upper bound used at control-flow merges:

| join | Bottom | Int | Long | Float | Double | Null | Ref |
|---|---|---|---|---|---|---|---|
| **Bottom** | Bottom | Int | Long | Float | Double | Null | Ref |
| **Int** | Int | Int | Bottom | Bottom | Bottom | Bottom | Bottom |
| **Long** | Long | Bottom | Long | Bottom | Bottom | Bottom | Bottom |
| **Float** | Float | Bottom | Bottom | Float | Bottom | Bottom | Bottom |
| **Double** | Double | Bottom | Bottom | Bottom | Double | Bottom | Bottom |
| **Null** | Null | Bottom | Bottom | Bottom | Bottom | Null | **Ref** |
| **Ref** | Ref | Bottom | Bottom | Bottom | Bottom | **Ref** | Ref |

Incompatible merges yield `Bottom`: the slot is treated as
dead/uninitialized at the merge point. Any later **use** of such a slot is a
verification error (assignability fails), which is the sound behavior — only
code that provably never reads a conflicting value verifies. (The terse
header comment "incompatible merges mark the point unreachable" refers to
this slot-level deadness, not whole-block unreachability.)

`isAssignable(from, to)`: `from == to`, or `from == Null && to == Ref`.
Operands typed `Null` are acceptable wherever `Ref` is required (null checks
trap at runtime, not in the verifier).

Helpers: `isNumeric` (Int/Long/Float/Double); `isCategory2` (Long/Double —
JVM spec parity only, no slot meaning, §1.2).

## 4.3 Sub-integral folding

`boolean`, `byte`, `char`, `short` do not exist as RBC types: they fold into
`Int` at the RBC level. Narrowing is re-expressed with `i2b`/`i2c`/`i2s`
(§3.10); array element access carries the narrowing in the opcode
(`baload`/`caload`/`saload`, `bastore`/`castore`/`sastore`). A `boolean`
value is an `Int` restricted to 0/1 by producer discipline; RBC does not
verify the restriction.

## 4.4 Per-program-point types

The verifier maintains, for every program point (instruction boundary) it
reaches, two vectors: `localTypes[0..M)` and `regTypes[0..N)`. This
per-point typing is the input contract for downstream consumers:

- T0/T1 use the entry state and the static per-point states for GC maps and
  deopt descriptors (`docs/stencils.md` §8.2, `docs/deopt_backend.md`
  Part A §6).
- T2 uses them to seed node types when building the sea-of-nodes graph.
- The text tools can print them (debug; not part of the canonical text
  output, §7).

---

# 5. Verification rules

Ordering law (from `Verifier.h`, normative): RBC is verified **before** the
quickener runs and **before** any tier executes it. No tier may consume an
unverified stream; the quickener may only consume a verified stream.

The verifier is **total**: arbitrary garbage input produces a bounded
diagnostic list (`VerifyResult::diags`, ordered by emission, capped at 100 by
default), never a crash, never undefined behavior, never an infinite loop.
Diagnostics carry the instruction index they concern (`VerifyDiag::pc`) and a
self-contained message (Rule 47 discipline).

Verification runs in three ordered passes (`Verifier.h`):

## 5.1 Structural checks

Checked for every instruction, before any type reasoning:

- **V-S1 Opcode range.** `op < Op::_Count`. (The `info(op)` table lookup
  requires this; the verifier enforces it first.)
- **V-S2 Register bounds.** Every register field the `Sig` uses
  (`dst`/`a`/`b` as applicable, plus `iinc`'s read-modify-write `dst`) must be
  `< numRegs`; consequently `numRegs <= 65536`. For calls:
  `a + b <= numRegs` (all argument registers in range); for
  `multianewarray`: `a + b <= numRegs`.
- **V-S3 Slot bounds.** `imm < numLocals` for `RegSlot`/`SlotReg` ops.
- **V-S4 cp index and kind.** `imm` in `[0, cp.size())` **and** of the Kind
  the opcode requires:

  | Opcode(s) | Required Kind |
  |---|---|
  | `lconst` / `dconst` | Int64 / Double |
  | `ldc` | String, Class, MethodType, or MethodHandle |
  | `new`, `anewarray`, `checkcast`, `instanceof`, `multianewarray`, `guard_class` | Class |
  | `getfield`, `putfield`, `getstatic`, `putstatic` | FieldRef |
  | `invokevirtual`, `invokespecial`, `invokestatic` | MethodRef |
  | `invokeinterface` | InterfaceMethodRef |
  | `invokedynamic` | InvokeDynamic |
  | `tableswitch`, `lookupswitch` | SwitchTable |

  Quickened forms are exempt where `imm` is repurposed (`getfield_quick` /
  `putfield_quick` carry a byte offset; `invoke*_quick` carry a MethodId or
  IC site id): the index/kind check is skipped, the value is treated as an
  opaque non-negative integer (§6).
- **V-S5 Branch target range.** `imm` (target) in `[0, code.size())` for
  `Branch`/`RegBranch`/`RegRegBranch` ops.
- **V-S6 Switch table well-formedness.** The referenced `SwitchTable` has an
  even-length `ints` payload of match/target pairs with strictly ascending
  matches; every target is a valid instruction index; `tableswitch`
  additionally requires dense matches (`mi == m0 + i`) and at least one pair;
  `lookupswitch` accepts zero pairs (unconditional default).
- **V-S7 Switch fall-through.** A switch is never the last instruction: the
  default target `pc + 1` must exist (`pc + 1 < code.size()`).
- **V-S8 Atype range.** `newarray`'s `imm` in `{4..11}` (the `Atype` codes).
- **V-S9 Exception table sanity.** For every `ExceptionHandler`:
  `start < end <= code.size()`, `handler < code.size()`, and `catchType` is
  either `-1` (catch-all) or a valid cp index of Kind Class.
- **V-S10 Unused-field canonicity** is checked for `dst == 0` on
  `putfield`/`putfield_quick` only (where a non-zero `dst` would be
  ambiguous); other unused fields are tolerated but canonically 0 (§2.4).

## 5.2 Type-checking abstract interpretation

A worklist dataflow over basic blocks (leaders: entry, branch targets,
fall-throughs of conditional branches, switch targets, handler entries):

- **Entry state.** Locals are initialized from the descriptor: for a static
  method, `parseParams(descriptor)` in slot order; for an instance method,
  `l0 = Ref` (receiver) then the parameters. All declared locals beyond the
  parameters start `Bottom`. **All working registers start `Bottom`.**
- **Transfer.** Each instruction's operands are checked against the
  `OpInfo` types (`operandA`, `operandB`) with `isAssignable`; the result
  type (`OpInfo::result`, or a descriptor-derived type for calls and field
  ops) is written to `dst`. Descriptor-derived typing:
  - `invoke*`: each argument register `a[i]` must be assignable to the i-th
    parameter type (receivers verify as `Ref`); `dst` receives
    `parseReturn(descriptor)` — `Bottom` for `void` (nothing written).
  - `getfield`/`getstatic`: `dst` receives the field descriptor's type;
    `putfield`/`putstatic` operands must match it.
  - Store families type their local slot (`istore` -> Int, ..., `astore` ->
    Ref); load families require the slot's current type to match exactly
    (Int for `iload`, Ref/Null for `aload`).
- **Merge.** At each join point the in-states merge slot-wise with `join`
  (§4.2); a fixed point is reached because the lattice is finite-height per
  slot and types only descend (joins are monotone: a slot whose type drops to
  `Bottom` stays `Bottom`).
- **Reachability.** Unreachable instructions are skipped (no diagnostics for
  dead code, JVM `StackMapTable` discipline is not inherited); the fall-
  through of a non-terminating conditional branch is reachable by default.

## 5.3 Exception handlers

For each `ExceptionHandler` covering `[start, end)`:

- **Local stability.** Every local slot must hold **one stable type across
  the whole protected range**: at every covered program point, the slot's
  type must join with the (single) handler-entry type without change. A slot
  whose type varies inside a protected range is a verification error — the
  handler must be able to assume one type per local.
- **Handler entry state.** Locals carry their stable types; registers are
  `Bottom` (no protected-region temporaries survive), **except `r0`, which
  holds the delivered exception object, typed `Ref`** (spec pin, §10.1;
  `Verifier.h`'s "exception entry sees locals but Bottom registers" is read
  with this one carve-out). `numRegs >= 1` is required for any method with
  handlers.
- Handler entries are additional dataflow seeds; a handler body is verified
  like any other code.

## 5.4 Termination

Every execution path must end in `return`/`*return`, `athrow`, or
`deopt_trap`, or reach a **reachable backward branch** (a legal infinite
loop). Falling off the end of `code` is a verification error. Formally: for
every reachable instruction that is not itself a terminator
(`return`/`*return`/`athrow`/`deopt_trap`), the fall-through successor
`pc + 1` must exist; for every reachable conditional branch and switch, all
targets and the fall-through must be valid instructions.

## 5.5 Uninitialized-`this` tracking (deferred)

JVM-style uninitialized-object tracking (the `uninitializedThis`/FlagThisUninit
dance around `new` + `invokespecial <init>`) is **deferred to v1**. This is
acceptable for v0 because:

1. The source path's lowering emits `new` followed immediately (before any
   use of the reference) by `invokespecial <init>` — the property is
   structural in the lowering, not emergent.
2. The classfile path (loader/translator) is not integrated in v0; when it
   is, its translator inherits JVMS guarantees from the classfile verifier's
   output rather than re-deriving them.
3. The runtime is not the verifier: B-2's RBC verifier is a tier-input gate
   for producer-controlled streams in v0, not a hostile-classfile firewall
   (the loader owns that responsibility when the classfile path lands).

The verifier therefore types `new`'s result as an ordinary `Ref`. §10
records the limitation.

---

# 6. Quickening

## 6.1 What the quickener does

Quickening rewrites a verified, unquickened stream into a **quickened** one
by **swapping the `op` field in place**. Nothing else changes: the record
keeps its position, size, register operands, and (where applicable) the
remaining operand fields. This is safe precisely because instructions are
fixed-width and pc is an index (§2.2) — no relocation, no re-patching of
neighbor branches, and any pc captured earlier stays valid.

Quickened variants are **distinct opcodes** (`_quick` suffix in both the
enumerator and the mnemonic), not a mode bit:

| Unquickened | Quickened | Repurposed `imm` | Other fields |
|---|---|---|---|
| `getfield` | `getfield_quick` | resolved field **byte offset** | unchanged (`a` = object, `dst` = result) |
| `putfield` | `putfield_quick` | resolved field **byte offset** | unchanged (`a` = object, `b` = value, `dst` = 0) |
| `invokevirtual` | `invokevirtual_quick` | resolved **inline-cache site id** | unchanged (`a` = arg base, `b` = arg count, `dst` = result) |
| `invokespecial` | `invokespecial_quick` | resolved **MethodId** | unchanged |
| `invokestatic` | `invokestatic_quick` | resolved **MethodId** | unchanged |
| `invokeinterface` | `invokeinterface_quick` | resolved **inline-cache site id** | unchanged |

Virtual/interface calls quicken to inline-cache site ids (their target is
class-dependent); special/static calls quicken to exact MethodIds (their
target is static). The unquickened forms resolve through the constant pool;
the quickened forms carry the resolved offset/id in `imm`
(`Opcode.h`, normative).

## 6.2 Verification of quickened streams

The verifier accepts quickened streams with the **same type rules**: operand
and result typing, local stability, termination all apply unchanged. The only
difference is structural: where `imm` is repurposed (§5.1 V-S4), the cp
index/kind check is skipped and the value is treated as an opaque
non-negative integer. This is sound because quickening preserves types by
construction (it only replaces a resolution step with its memoized result).

Quickened opcodes carry `Eff::Quickened` and **never appear in unquickened
streams** — a producer invariant: the frontend lowering and the classfile
translator emit only unquickened forms; only the quickener introduces
`_quick` opcodes. Quickening an already-quickened stream is a no-op
(idempotent); re-verification after quickening is permitted but optional.

## 6.3 Superinstructions (future extension)

Superinstructions — fused stencils for common RBC sequences such as
`iload_iload_iadd_istore` or `aload_getfield_ireturn` — are a **future
extension** of this opcode set (`docs/stencils.md` §3.2, §11). They will:

- be mined from T0 profile data (frequency-ordered RBC sequence census),
- be separate opcodes in the same `u16` `Op` space — the space is far from
  exhausted (150 of 65536 used in v0) and is therefore extensible,
- obey the appended-only evolution rule: new opcodes are added at the end of
  the enum (before `_Count`), never renumbered, so serialized streams,
  golden tests, and AOT artifacts stay stable (Rule 15/Rule 124 discipline;
  §8).

No superinstruction opcodes exist in v0; the verifier rejects any opcode
`>= Op::_Count` (V-S1), which keeps the door open without committing the
format.

---

# 7. Text format

The text format is printable, parseable RBC (`RbcText.h`): the golden-test
stability oracle (byte-stable text round-trips catch unintended RBC changes
before they become Java-visible; `docs/cpp26_standards.md` "Golden IR Tests")
and the human debug surface for lowering reviews.

## 7.1 Grammar

Reproduced exactly from `RbcText.h` (whitespace-separated; `#` starts a line
comment):

```text
program   := { method }
method    := ".method" [flags...] name descriptor
               {".regs" N} {".locals" N}
               {directive | label | insn} ".end"
flags     := "static" | "public" | "private" | "protected" | "final"
            | "synchronized" | "native" | "abstract" | "varargs"
directive := ".const" cid "=" kind payload
                  kind := "int" i32 | "long" i64 | "float" f | "double" d
                        | "utf8" s | "string" s | "class" s
                        | "nametype" n s | "field" c n s | "method" c n s
                        | "imethod" c n s | "methodtype" s
                        | "methodhandle" k s | "indy" n s
                        | "switch" "{" [int ":" label]* "}"
label     := ident ":"
insn      := mnemonic [operands]
operands  := "r"dec | "l"dec | "c"dec | label | int | atype
```

Spec completion (§10.1, flagged): the grammar above has no directive for
exception handlers, yet methods with handlers must round-trip. This spec
adds exactly one directive, mirroring `ExceptionHandler` field-for-field:

```text
handler-directive := ".handler" start end handler ["catch" cid]
```

with `start`/`end`/`handler` as decimal instruction indices (`start < end`,
`end` exclusive, `handler` in range) and `catch cid` naming a Class const;
omitting `catch` means catch-all (`catchType = -1`).

## 7.2 Tokens

- **Registers** `r0`, `r1`, ...; **local slots** `l0`, `l1`, ...;
  **constants** `c0`, `c1`, ... (decimal indices).
- **Labels**: an identifier followed by `:` defines a label
  (`Lloop:`); a bare identifier is a label reference. Identifiers match
  `[A-Za-z_.$][A-Za-z0-9_.$]*`. Labels may be referenced before they are
  defined (forward branches) and are defined at most once.
- **Ints**: decimal with optional leading `-`. The parser also accepts
  `0x`-prefixed hex; the canonical printer emits decimal only.
- **atype**: where the grammar expects an atype (`newarray`'s operand), the
  eight primitive keywords are accepted and map to the JVM `Atype` codes:
  `boolean`=4, `char`=5, `float`=6, `double`=7, `byte`=8, `short`=9,
  `int`=10, `long`=11 (spec pin, §10.1).
- **Strings** (`s`, `n`, `c`, `k` payloads): double-quoted UTF-8 with `\"`,
  `\\`, `\n`, `\t`, `\r` escapes (spec pin, §10.1).
- **Float/double payloads** (`f`, `d`): decimal or hex-float literals;
  `inf`, `-inf`, `nan`, `-nan` accepted. The canonical printer emits the
  shortest round-trip decimal (`std::to_chars`), which canonicalizes NaN
  payloads (§10).
- Operand order in every instruction is the `Sig` field order (§2.3); the
  mnemonic's operand forms are listed in §3.

## 7.3 Canonical printer

`printRbcText` is deterministic: the same `Program`/`Method` prints
byte-identically (Rule 124 discipline). Rules:

- Constants are emitted **first, in index order**, as `.const` directives.
- One instruction per line; labels are emitted only for instructions that
  are branch targets (including switch-payload targets), one per line, in
  code order.
- `fconst` prints its `imm` as a decimal integer (the bit pattern);
  `iconst`/`iinc` print `imm` as a signed decimal; call/quick operands
  print `b` (arg count) and quickened ids as decimals; cp references use
  the `cK` form.
- `.regs`/`.locals` are always emitted (even when 0); `.handler` directives
  are emitted last, in table order, with numeric indices.
- No blank-line or column alignment beyond single spaces between operands;
  `#` comments are never emitted (they are input-only).

Round-trip: `parseRbcText(printRbcText(p))` reproduces `p` (equal methods,
code, pools, handlers) — the two known v0 exceptions are `Program::className`
(not representable in the grammar; §10) and NaN payload bits (§7.2).

## 7.4 Examples

All four examples are complete, verified-by-construction methods; `#`
comments show instruction indices (pcs) and are not part of the format.

**1. Static int add (straight-line, no pool):**

```text
.method static add (II)I
  .regs 3
  .locals 2
  iload r0, l0        # pc0: r0 = a
  iload r1, l1        # pc1: r1 = b
  iadd r2, r0, r1     # pc2: r2 = a + b
  ireturn r2          # pc3
.end
```

**2. Loop with branches, backedge safepoint, and `iinc`:**

```text
# static int sum(int n) { int s = 0; for (int i = 1; i <= n; i++) s += i; return s; }
.method static sum (I)I
  .regs 4
  .locals 3
  iconst r0, 0          # pc0
  istore r0, l1         # pc1: s = 0
  iconst r0, 1          # pc2
  istore r0, l2         # pc3: i = 1
Lcond:                  # pc4
  iload r0, l2          # pc4: r0 = i
  iload r1, l0          # pc5: r1 = n
  if_icmpgt r0, r1, Lend  # pc6: if i > n goto Lend
  iload r0, l1          # pc7: r0 = s
  iload r1, l2          # pc8: r1 = i
  iadd r2, r0, r1       # pc9
  istore r2, l1         # pc10: s = s + i
  iload r3, l2          # pc11: r3 = i
  iinc r3, 1            # pc12: r3 = r3 + 1
  istore r3, l2         # pc13: i = i + 1
  safepoint_poll        # pc14: backedge poll
  goto Lcond            # pc15
Lend:                   # pc16
  iload r0, l1          # pc16
  ireturn r0            # pc17
.end
```

**3. Exception handler (`.const` class, `.handler`, exception in `r0`):**

```text
# static int div(int a, int b) { try { return a / b; }
#                                catch (ArithmeticException e) { return -1; } }
.method static div (II)I
  .regs 4
  .locals 3
  .const c0 = class "java/lang/ArithmeticException"
  iload r0, l0          # pc0: r0 = a
  iload r1, l1          # pc1: r1 = b
  idiv r2, r0, r1       # pc2: r2 = a / b (traps ArithmeticException on b == 0)
  ireturn r2            # pc3
  astore r0, l2         # pc4: Lhandler; r0 holds the delivered exception
  iconst r1, 1          # pc5
  ineg r2, r1           # pc6: r2 = -1
  ireturn r2            # pc7
  .handler 0 4 4 catch c0
.end
```

**4. Switch (`tableswitch`, string constants, fall-through default):**

```text
# static String name(int d) { switch (d) { case 0: return "zero";
#   case 1: return "one"; case 2: return "two"; default: return "many"; } }
.method static name (I)Ljava/lang/String;
  .regs 2
  .locals 1
  .const c0 = string "zero"
  .const c1 = string "one"
  .const c2 = string "two"
  .const c3 = string "many"
  .const c4 = switch { 0:Lzero 1:Lone 2:Ltwo }
  iload r0, l0          # pc0: r0 = d
  tableswitch r0, c4    # pc1: default -> pc2 (fall-through)
  ldc r1, c3            # pc2: default path: r1 = "many"
  areturn r1            # pc3
Lzero:                  # pc4
  ldc r1, c0            # pc4
  areturn r1            # pc5
Lone:                   # pc6
  ldc r1, c1            # pc6
  areturn r1            # pc7
Ltwo:                   # pc8
  ldc r1, c2            # pc8
  areturn r1            # pc9
.end
```

A sparse `lookupswitch` differs only in the payload (strictly ascending,
non-dense matches):

```text
  .const c1 = switch { 10:Lten 20:Ltwenty 35:Lthirtyfive }
  lookupswitch r0, c1   # default -> next instruction
```

---

# 8. Determinism and serialization

## 8.1 Self-contained methods

Each `Method` **owns its constant pool** (`Method::cp`): every cp index in
its code resolves against its own pool. Methods are therefore self-contained
serialization units, which is what makes them fit for:

- **Golden tests** (`docs/cpp26_standards.md` "Golden IR Tests"): one text
  file per method, byte-stable expected output, exact negative tests via
  `TextError::offset` (Rule 47 discipline).
- **AOT artifacts** (`docs/laws.md` Amendment B, T3): methods serialize and
  load independently; no cross-method pool coupling exists to break.
- **Deterministic replay** (Rule 124, "Law 124" in the headers): the same
  method + compiler version + flags yields the same pool, code, and text.

A `Program` is a compilation unit: a class name plus a list of methods.
`Program::find(name, descriptor)` locates methods; nothing in a method's
encoding refers outside its own `Method` record (Rule 15 discipline: indices
only, never pointers into reallocating containers).

## 8.2 Pool construction discipline

The builder interns constants by full value equality (`internConst`) in
first-use order; identical constants share one index. Producers must not
reorder or renumber pool entries after emission. The canonical text printer
emits the pool in index order (§7.3), so pool layout is observable and
therefore frozen per method version.

## 8.3 Byte-stable printing

`printRbcText` output is a pure function of the `Program`/`Method` value:
no timestamps, no addresses, no locale, no iteration-order dependence
(Rule 124). Golden files diff byte-for-byte; any diff is a representation
change that must be reviewed, never noise.

## 8.4 Numeric canonicalization

- `iconst`/`iinc` immediates: signed 32-bit decimal.
- `fconst`: the `u32` bit pattern as decimal (§3.2).
- `.const` float/double payloads: shortest round-trip decimal; `inf`,
  `-inf`, `nan`, `-nan` spellings. NaN payload bits are not preserved
  through the text format (§10) — golden tests must use canonical NaNs.
- `lconst`/`dconst` carry no numeric immediates (they reference pool
  entries), so 64-bit values never appear inline.

---

# 9. Tier consumption mapping

| Tier | What it does with RBC | Contract references |
|---|---|---|
| **T0** | Direct-threaded dispatch on `Op` (computed goto where the toolchain supports labels-as-values; Amendment B). Executes **quickened** streams. Hosts `OpInfo` in `.rodata` next to the dispatch table. Provides inline caches, profiling, superinstruction census, safepoint polls. | `docs/laws.md` Amendment B; `Opcode.h` |
| **T1** | **No IR**: maps each opcode to a precompiled stencil **1:1** and patches holes (register-slot offsets, field offsets, call targets, IC stubs, deopt ids, branch targets). Uses the T0-compatible frame (§1.4). Future: fused superinstruction stencils mined from T0 profiles. | `docs/stencils.md` §1, §3.1, §8, §11 |
| **T2** | Builds the sea-of-nodes IR from RBC (first IR tier; index-based `NodeId`, explicit effects from `Eff`, guards, `FrameState`, dependencies, deopt metadata). Input: RBC + PGO + class-hierarchy + profile data. | `docs/laws.md` Amendment B.1 |
| **T3** | Offline T2-style pipeline over RBC + AOT artifacts (§8.1). | `docs/laws.md` Amendment B.5 |
| **Deopt** | Every tier deopts **to T0** with `(RBC pc, frame)`: locals + register file + monitors are reconstructed from the T0-compatible frame; pc maps are trivial because pc is an index into fixed 12-byte records (§2.2). | `docs/deopt_backend.md` Part A §1, §3, §8 |

Consumption order is fixed: **verify -> quicken -> execute** (§5). T0 is the
correctness fallback and the exact Java-state reconstruction target for
deoptimization; every compiled tier must be able to deopt back to T0 state.

The `Eff` flags (§3.19) are the reordering contract between RBC and the
tiers: T1 stencil composition and T2 scheduling may only reorder what the
flags permit (Rule 10 family; `docs/stencils.md` §4).

---

# 10. Open v0 limitations

Stated plainly, because every tier downstream builds on this spec:

1. **`invokedynamic` is structurally accepted but not linked.** The verifier
   checks its Call shape, cp Kind (InvokeDynamic), and descriptor typing, but
   there is no bootstrap-method execution, no `CallSite` machinery, and no
   method-handle runtime in v0. Producers treat it as verifier-only.
2. **Monitor balancing is not verified.** `monitorenter`/`monitorexit`
   pairing — including on exceptional exits — is not checked by the
   verifier. Deopt monitor reconstruction relies on runtime bookkeeping
   (`docs/deopt_backend.md` Part A §15.5), not on RBC-level proofs.
3. **Uninitialized-`this` tracking is deferred** (§5.5). Sound in v0 because
   lowering emits `<init>` before any use and the runtime is not the
   classfile verifier; revisit when the classfile path integrates.
4. **NaN boxing is orthogonal.** RBC has no tagged-value opcodes: every
   value's type is the static per-point type of its slot/register (§4.4).
   NaN boxing is a T2/T3 representation lowering concern
   (`docs/stencils.md` §12, `docs/deopt_backend.md` Part B §19) and never
   observable at the RBC level.
5. **`multianewarray`** is verified but full multi-dimension runtime
   allocation lands with the classfile path; v0 frontends lower
   one-dimensional allocation via `newarray`/`anewarray`.
6. **`guard_class` is verifier-only.** No v0 tier emits it; its semantics
   (§3.18) are pinned so T2 can adopt it without a spec change.
7. **Pool kinds `Int32` and `Float` are not loadable** by any v0 opcode
   (§3.2); they exist for builder/serialization completeness and future use.
8. **Text format gaps.** `Program::className` is not representable in the
   grammar (program-level round-trips drop it; use per-method goldens); NaN
   payload bits canonicalize through text (§7.2); exception-handler syntax
   is a spec completion pending a header grammar revision (§7.1).
9. **No line-number table.** `Method` carries no BCI/line mapping in v0;
   stack-trace line info (deopt_backend Part A §3) will need a side table
   keyed by pc.

## 10.1 Spec pins (header underdetermination resolved here)

Decisions this document had to make where the frozen headers are ambiguous or
silent. Each is normative from this document until a header revision says
otherwise; all are flagged for reconciliation when `Opcode.cpp` lands:

| # | Pin | Where | Rationale |
|---|---|---|---|
| P1 | `putstatic` uses `Sig::RegCp` with `dst` read as the value register | §3.12 | symmetric with `getstatic`; no `(a, imm=cp)` Sig exists |
| P2 | `aconst_null` uses `Sig::RegImm`, imm unused (0) | §3.2 | no bare-`(dst)` Sig exists |
| P3 | Array stores use `Sig::RegRegReg` with `dst` = value (read), `a` = array, `b` = index | §3.13 | `a`/`b` keep their load-side meaning (array, index) across the load/store pair |
| P4 | `checkcast` is `RegRegCp` | §3.14 | listed under both `RegReg` and `RegRegCp` in `Opcode.h` Sig comments; it needs `imm=cp` |
| P5 | `multianewarray` dimension counts live in consecutive registers `a..a+b-1` | §3.13 | parallels the call convention; "first dim reg a" + "dims b" |
| P6 | Switch payload is unified match/target pairs; `tableswitch` = dense pairs | §3.11 | only single-pass-parseable reading of the text grammar's `int ":" label` form (round-trip determinism) |
| P7 | Switch default target = fall-through `pc + 1` | §3.11 | grammar has no default syntax; builder's `emitSwitch` has no default label parameter |
| P8 | Handler entry delivers the exception in `r0` (typed `Ref`) | §5.3 | no opcode reads a pending exception; locals are descriptor-fixed; a register must carry it |
| P9 | `.handler` directive (numeric indices) | §7.1 | grammar had no handler syntax; methods with handlers must round-trip |
| P10 | Mnemonics are the JVM-familiar lowercase forms pinned in §3 | §3 | only `"iadd"` was given; the table pins the other 149 |
| P11 | `atype` text operands are the eight primitive keywords | §7.2 | grammar lists `atype` as a distinct operand class from `int` |
| P12 | `invokeinterface` takes Kind InterfaceMethodRef; JVM's invokespecial-to-interface nuance is deferred to the classfile translator | §5.1 | both ref kinds exist in `Const::Kind` |
| P13 | String escapes and float/double payload spellings in text | §7.2 | grammar's `s`/`f`/`d` metavariables were undefined |
| P14 | Effect-flag assignments per opcode | §3 | `Opcode.cpp` does not exist yet; this table is its contract |

## 10.2 Inconsistencies found in the headers (reported, not fixed)

- `Opcode.h` lists `checkcast` under **both** `Sig::RegReg` and
  `Sig::RegRegCp` (P4).
- `RbcText.h`'s grammar block has **no exception-handler directive**, yet
  `RbcBuilder` builds handlers and `Method` serializes them (P9).
- `RbcText.h`'s grammar cannot express `Program::className`; program-level
  round-trips lose it (§10 item 8).
- `Rbc.h`'s `Const::Kind::SwitchTable` comment ("low..high targets for
  table") reads as a divergent table layout; the text grammar forces the
  unified-pairs reading (P6).
- `Verifier.h`'s "exception entry sees locals but Bottom registers" does not
  say where the caught exception is delivered (P8).
- The headers cite "Law 15/124/10x"; `docs/laws.md` numbers these
  Rule 15/Rule 124/Rule 10 (cosmetic; this document uses `laws.md` numbering).
- `Rbc.h` documents `Ins` as "fixed 12-byte records"; the struct's natural
  size is 12 bytes on all supported ABIs, but nothing in the type enforces
  it (no `static_assert`); serializers must not rely on `sizeof` without one.

---

# 11. References

- `include/b2/rbc/{Type,Opcode,Rbc,Verifier,RbcText,RbcBuilder}.h` — the
  frozen C++ contract (normative; this document restates it).
- `docs/laws.md` — Rule 15 (index-based, no raw pointers), Rule 47
  (actionable diagnostics), Rule 124 (deterministic compilation and replay),
  Amendment A/B (tier model), Amendment C (stencils).
- `docs/stencils.md` — §3.1 opcode stencils, §3.2/§11 superinstructions,
  §8 T0-compatible T1 frames, §12 NaN boxing, §14 Stencil Rules.
- `docs/deopt_backend.md` — Part A (canonical T0 state, FrameState, deopt
  flow, monitor reconstruction), Part B §19 (NaN boxing lowering).
- `docs/cpp26_standards.md` — "Golden IR Tests" (byte-stable text oracle),
  two-domain testing contract.
- `docs/teams/ir-team.md` — IR team charter (ownership of this spec).
