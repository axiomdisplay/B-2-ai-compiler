# TurboScript Bytecode Specification (TSBC v0.2)

**Status:** Draft v0.2 — implemented by the T0 interpreter (`compiler/interp/`)
**Owner:** TurboScript Interp Team
**Last Updated:** 2026-09-14
**Governing Laws:** `docs/laws/turboscript_compiler_laws.md` (Part 0, Part I Tier 0, Rules 15, 16, 23, 32, 33, 47, 90, 96, 105, 124)
**Related:** `docs/laws/turboscript_master_architecture_spec.md` (Section 2 node taxonomy), `interp_contract.md`

**v0.2 changes:** arrays are first-class (Section 5.12: `NewArray`,
`GetElement`, `SetElement`), the feedback vector gains the Element slot kind
(Section 8), and the Section 11 register marks arrays implemented.

This document defines the TurboScript Bytecode (TSBC) format: a typed,
register-based, effect-carrying bytecode that can be validated (this spec +
`tsrun --verify-only`), profiled (feedback vector, Part I Tier 0), interpreted
(Tier 0), and later baseline-compiled (Tier 1 stencils) and optimized through
the Sea of Nodes pipeline (Tier 2).

There is no compression in this document. Every opcode, every format, every
validation rule is stated explicitly.

---

## 1. Module Layout

A TSBC module (assembled from a `.tsbc` text file, or later emitted by the
frontend) contains:

| Element | Description |
|---|---|
| Constant pool | Up to 2^24 constants. Each is one of: Smi (i32), Double (f64), String (UTF-16 code units), BigInt literal. |
| Function table | Up to 2^24 functions. Each function is defined in Section 2. |
| Global name table | Up to 2^24 interned global binding names (SymbolId, Rule 16). |
| Entry function index | The function executed at module start. |

All tables are index-based (32-bit indices, no pointers) so the format is
trivially serializable, per the index-based design law (Rule 15) applied to
bytecode.

## 2. Function Layout

Each function contains:

| Element | Constraint |
|---|---|
| name | Interned symbol (diagnostics and stack traces only; resolution is by index) |
| registerCount | 1..254. Registers r0..r253 are usable. r254 and r255 are reserved (Section 6). |
| paramCount | 0..registerCount. Parameters occupy r0..r(paramCount-1); they are defined at entry. |
| contextCellCount | 0..255 cells captured by this function's closure context. |
| bytecode | Vector of 32-bit instruction words; only the low 24 bits of each word are meaningful (Section 3). |
| handlerTable | Exception handlers (Section 7). |
| feedbackLayout | Feedback slot kinds per slot, computed by the verifier (Section 8). |

## 3. Instruction Encoding

Every instruction slot is a 24-bit word, stored as a 32-bit `uint32_t` with
the upper 8 bits zero:

```text
word := op(8) | A(8) | B(8)        // A = bits 8..15, B = bits 16..23
```

- `op` selects the opcode and its format (Section 5).
- Instructions that need a wide operand carry one **suffix word**:
  `suffix := C(8) | D(8) | E(8)`.
- Instructions that need a 24-bit unsigned immediate use the suffix word as
  `imm24 := C | D<<8 | E<<16`.
- Jump offsets are in **word units**, relative to the jump instruction's own
  word, and may be negative (two's complement in the offset field).

The base width is 24 bits as required by Part I Tier 0 ("24-bit fixed-width
instructions"); multi-word instructions are used only where 16 operand bits
cannot carry the operand (calls, 24-bit indices, wide jumps). The width of
every opcode is fixed by its format and is part of this specification.

## 4. Register Model

- 256 virtual registers per frame (Part I Tier 0). r0..r253 usable;
  r254 (`new.target` slot, reserved for future Construct metadata) and
  r255 (reserved) are invalid in any operand.
- Registers are **untyped slots holding tagged values**; typing lives in
  the opcodes, mirroring the conversion-node discipline of Rule 33 (all
  conversions are explicit opcodes: `ToNumber`, `ToString`, `ToInt32`,
  `ToUint32`, `ToBigInt`, `ToPropertyKey`, `ToPrimitive`, `ToNumeric`).
- `paramCount` registers are defined on entry; all other registers are
  **undefined until written** and the verifier enforces definedness on all
  paths (Section 9.4). Reading an undefined register is a verification
  error, never a runtime behavior.

## 5. Opcode Table (Exhaustive)

Effects, one per opcode (effect-carrying requirement, Part 0 / Rule 121
applied to bytecode):

| Effect | Meaning |
|---|---|
| `Pure` | Cannot call user code, cannot throw, cannot observe mutable state. |
| `Read` | Observes mutable state; cannot call user code; cannot throw. |
| `Alloc` | Allocates; cannot call user code; cannot throw. |
| `Invoke` | May call user code (ToPrimitive hooks, getters, setters, callables) and may throw. |
| `Throw` | Raises a JavaScript exception unconditionally or conditionally without invoking user code. |
| `Write` | Mutates state; may throw (e.g. strict-mode frozen stores) without invoking user code. |

`IC` marks opcodes that own a feedback slot (Section 8).

### 5.1 Constants and moves

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 1 | `Nop` | W0 | — | Pure | No operation. |
| 2 | `Mov` | W1_RR | dst, src | Pure | dst = src (tagged copy). |
| 3 | `LoadConst` | W2_I24D | dst, imm24 | Pure | dst = constant pool[imm24]. |
| 4 | `LoadConstS` | W1_RI8 | dst, imm8 | Pure | dst = Smi(imm8), imm8 in [-128,127]. |
| 5 | `LoadUndefined` | W1_R | dst | Pure | dst = undefined. |
| 6 | `LoadNull` | W1_R | dst | Pure | dst = null. |
| 7 | `LoadTrue` | W1_R | dst | Pure | dst = true. |
| 8 | `LoadFalse` | W1_R | dst | Pure | dst = false. |

### 5.2 Globals

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 9 | `DefineGlobalVar` | W2_I24 | name24 | Write | Creates global binding `name` = undefined if absent; existing binding is kept. |
| 10 | `LoadGlobal` | W2_I24D | dst, name24 | Read | dst = global[name]; missing binding → undefined. |
| 11 | `StoreGlobal` | W2_I24S | src, name24 | Write | global[name] = src. Storing into a binding that does not exist creates it (script semantics). |

### 5.3 Property access (all IC sites)

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 12 | `GetProperty` | W2_RR_D | objDst, key, pad | Invoke, IC | objDst = ToPropertyKey'd get of `objDst[key]`, walking the prototype chain; invokes getters. |
| 13 | `SetProperty` | W2_RR_V | obj, key, val | Invoke, IC | `obj[key] = val` with strict-mode store semantics; invokes setters. |
| 14 | `DeleteProperty` | W2_RR_D | dst, obj, key | Write | dst = `delete obj[key]` (strict semantics; false only for non-configurable own props). |
| 15 | `HasProperty` | W2_RR_D | dst, obj, key | Read | dst = `key in obj` (own or prototype chain). |
| 16 | `GetPrototype` | W1_RR | dst, obj | Read | dst = obj.[[Prototype]] (object or null). |
| 17 | `SetPrototype` | W2_RR_D | dst, obj, proto | Write | dst = [[SetPrototypeOf]] result (false on cycle); proto may be object or null. |
| 18 | `Instanceof` | W2_RR_D | dst, obj, ctor | Invoke | dst = `obj instanceof ctor`; ctor must be callable (TypeError otherwise). |
| 19 | `In` | W2_RR_D | dst, obj, key | Invoke | dst = `key in obj` with ToPropertyKey on key. |

### 5.4 Calls, closures, context

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 20 | `Call` | W2_CALL | func, argsBase, argc, dst | Invoke, IC | dst = func(undefined-this, args[0..argc)). func must be callable (TypeError otherwise). |
| 21 | `CallMethod` | W2_CALL | func, this, argsBase, argc, dst | Invoke, IC | dst = func(thisVal, args). |
| 22 | `Construct` | W2_CALL | func, argsBase, argc, dst | Invoke, IC | dst = new func(args). If func returns an object, that object; otherwise the newly created binding object with prototype func.prototype. |
| 23 | `Return` | W1_R | src | Pure | Returns src from the current frame. |
| 24 | `NewClosure` | W2_I24D | dst, funcIdx24 | Alloc | dst = closure over the current context for function table[funcIdx24]. |
| 25 | `NewContext` | W2_RR_C | dst, ctx, cellCount8 | Alloc | dst = new context with cellCount8 cells (each the hole) whose parent is `ctx` (a context or null); also becomes the frame's current context. |
| 26 | `LoadContext` | W2_RR_C | dst, ctx, cell8 | Read | dst = ctx[cell8]; hole reads are a runtime TypeError (TDZ-style hole escape is an interpreter bug, not a program value). |
| 27 | `StoreContext` | W2_RC_V | ctx, cell8, val | Write | ctx[cell8] = val. |

### 5.5 Arithmetic (full JavaScript semantics; ToPrimitive/ToNumber hooks may run)

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 28 | `Add` | W1_RR | dst, src | Invoke, IC | dst = dst + src (string concat if either primitive is a string; BigInt+BigInt → BigInt; mixed BigInt/Number → TypeError). |
| 29 | `Sub` | W1_RR | dst, src | Invoke, IC | dst = dst - src (ToNumeric). |
| 30 | `Mul` | W1_RR | dst, src | Invoke, IC | IEEE 754 multiply. |
| 31 | `Div` | W1_RR | dst, src | Invoke, IC | IEEE 754 divide (1/0 = Infinity; 0/0 = NaN). |
| 32 | `Mod` | W1_RR | dst, src | Invoke, IC | IEEE 754 remainder (JS `%`, not C fmod semantics). |
| 33 | `Pow` | W1_RR | dst, src | Invoke, IC | dst = dst ** src (edge cases per spec: NaN**0 = 1, 1**NaN = NaN). |
| 34 | `Neg` | W1_R | dst | Invoke, IC | dst = -dst (ToNumeric). |
| 35 | `Inc` | W1_R | dst | Invoke, IC | dst = dst + 1. |
| 36 | `Dec` | W1_R | dst | Invoke, IC | dst = dst - 1. |

### 5.6 Bitwise (ToInt32/ToUint32 semantics, Rule 72)

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 37 | `BitAnd` | W1_RR | dst, src | Invoke, IC | dst = ToInt32(dst) & ToInt32(src). |
| 38 | `BitOr` | W1_RR | dst, src | Invoke, IC | ToInt32 bitwise or. |
| 39 | `BitXor` | W1_RR | dst, src | Invoke, IC | ToInt32 bitwise xor. |
| 40 | `Shl` | W1_RR | dst, src | Invoke, IC | ToInt32(dst) << (src & 31). |
| 41 | `Shr` | W1_RR | dst, src | Invoke, IC | sign-propagating right shift. |
| 42 | `UShr` | W1_RR | dst, src | Invoke, IC | ToUint32 zero-fill right shift. |
| 43 | `BitNot` | W1_R | dst | Invoke, IC | dst = ~ToInt32(dst). |

### 5.7 BigInt family (operands must both be BigInt or TypeError; Rule 72)

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 44 | `BigIntAdd` | W1_RR | dst, src | Throw | Arbitrary-precision add. |
| 45 | `BigIntSub` | W1_RR | dst, src | Throw | Arbitrary-precision subtract. |
| 46 | `BigIntMul` | W1_RR | dst, src | Throw | Arbitrary-precision multiply. |
| 47 | `BigIntDiv` | W1_RR | dst, src | Throw | Truncated division; division by 0n → RangeError. |
| 48 | `BigIntMod` | W1_RR | dst, src | Throw | Remainder with dividend sign; 0n divisor → RangeError. |
| 49 | `BigIntNeg` | W1_R | dst | Throw | Arbitrary-precision negation. |

### 5.8 Comparison

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 50 | `Lt` | W1_RR | dst, src | Invoke, IC | dst = dst < src (strings compare by UTF-16 code units; otherwise numeric; NaN → false). |
| 51 | `Le` | W1_RR | dst, src | Invoke, IC | dst = dst <= src. |
| 52 | `Gt` | W1_RR | dst, src | Invoke, IC | dst = dst > src. |
| 53 | `Ge` | W1_RR | dst, src | Invoke, IC | dst = dst >= src. |
| 54 | `AbstractEq` | W1_RR | dst, src | Invoke, IC | dst = (dst == src), full coercion algorithm. |
| 55 | `StrictEq` | W1_RR | dst, src | Pure | dst = (dst === src): same type, +0 === -0, BigInt value equality, object identity. |
| 56 | `SameValue` | W1_RR | dst, src | Pure | `Object.is` semantics: distinguishes -0/+0, NaN === NaN. |
| 57 | `SameValueZero` | W1_RR | dst, src | Pure | SameValue but -0/+0 equal. |

### 5.9 Logic, type, conversion

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 58 | `LogicalNot` | W1_R | dst | Pure | dst = !ToBoolean(dst). |
| 59 | `ToBoolean` | W1_R | dst | Pure | dst = ToBoolean(dst). |
| 60 | `TypeOf` | W1_R | dst | Pure | dst = typeof dst ("undefined","object","boolean","number","string","symbol","bigint","function"). |
| 61 | `ToNumber` | W1_R | dst | Invoke, IC | dst = Number(dst); BigInt → TypeError. |
| 62 | `ToNumeric` | W1_R | dst | Invoke | BigInt stays BigInt; otherwise ToNumber. |
| 63 | `ToString` | W1_R | dst | Invoke | dst = String(dst) (shortest round-trip number formatting; Symbol → TypeError). |
| 64 | `ToInt32` | W1_R | dst | Invoke | modulo-2^32 signed reduction of ToNumber. |
| 65 | `ToUint32` | W1_R | dst | Invoke | modulo-2^32 unsigned reduction of ToNumber. |
| 66 | `ToBigInt` | W1_R | dst | Invoke | BigInt(dst); fractional/NaN input → RangeError; Symbol → TypeError. |
| 67 | `ToPrimitive` | W1_R | dst | Invoke | dst = ToPrimitive(dst, hint=number) (Symbol.toPrimitive, valueOf, toString). |

### 5.10 Strings

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 68 | `StringConcat` | W1_RR | dst, src | Alloc | dst = ToString(dst) + ToString(src); Symbol → TypeError. |
| 69 | `StringLength` | W1_R | dst | Read | dst = Smi(UTF-16 code unit count); non-string → TypeError. |
| 70 | `CharCodeAt` | W2_RR_D | dst, str, idx | Read | dst = UTF-16 code unit at idx (NaN when out of range). |
| 71 | `StringFromCharCode` | W1_R | dst | Pure | dst = string from ToUint16(dst) code unit. |

### 5.11 Control flow and exceptions

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 72 | `Jmp` | W1_J | off16 | Pure | pc += off16. |
| 73 | `JmpWide` | W2_BR | off24 | Pure | pc += off24. |
| 74 | `JmpTrue` | W1_BR / W2_BR | cond, off | Pure, IC | If ToBoolean(cond) pc += off. |
| 75 | `JmpFalse` | W1_BR / W2_BR | cond, off | Pure, IC | If !ToBoolean(cond) pc += off. |
| 76 | `Throw` | W1_R | src | Throw | Raise src as a JavaScript exception. |
| 77 | `Rethrow` | W0 | — | Throw | Re-raise the exception currently being handled. |
| 78 | `NewObject` | W1_R | dst | Alloc | dst = new ordinary object with no own properties (shape assigned on first store). Object/prototype opcodes operate on it. |
| 79 | `GetContext` | W1_R | dst | Read | dst = the frame's current context (from the closure's captured context); null if none. Enables closures to read captured cells. |

## 5.12 Arrays (v0.2)

| # | Opcode | Format | Operands | Effect | Semantics |
|---|---|---|---|---|---|
| 80 | `NewArray` | W1_R | dst | Alloc | dst = new array: length 0, elements kind PackedSmi, no elements, [[Prototype]] = null (Array.prototype is a builtin-layer feature, Section 11). |
| 81 | `GetElement` | W2_RR_D | dst, obj, key | Invoke, IC | dst = `obj[key]` with EXACTLY the semantics of `GetProperty` (ToPropertyKey routing, prototype chain, getters). The frontend chooses `GetElement` over `GetProperty` when the key is index-like so the feedback slot records element-kind sites (Section 8) instead of shape sites. |
| 82 | `SetElement` | W2_RR_V | obj, key, val | Invoke, IC | `obj[key] = val` with EXACTLY the semantics of `SetProperty` (strict-mode store; setters; array exotic routing). Same opcode-choice rule as `GetElement`. |

Array exotic behavior (enforced inside the shared property implementation,
not per opcode — one semantic source, Rule 96):

- **Array index:** a key is an array index iff it is a canonical numeric
  string (ECMA-262) whose value is < 2^32-1. `"0"` is an index; `"00"`,
  `"+0"`, and `"4294967295"` are named properties. Number keys route
  through ToString, so `1`, `1.0`, and `"1"` address the same element;
  `1n` addresses element 1 (`ToPropertyKey(1n) = "1"`).
- **Elements kinds:** PackedSmi / HoleySmi / PackedDouble / HoleyDouble /
  PackedTagged / HoleyTagged. Kind invariants are enforced on every element
  store (a PackedSmi array can never contain a non-Smi); transitions are
  eager and monotonic: smi -> double -> tagged, packed -> holey. Holes are
  created by `DeleteProperty` on an index, by growth past the end
  (SetElement), and by `length` shrink — never by length growth alone.
- **Hole reads** fall through the prototype chain (the array's own storage
  does not shadow); holes are not own properties, so `HasProperty`/`In`
  report false for them.
- **length:** writable, non-enumerable, non-configurable exotic property.
  Reading it yields the current length as a Number (Smi when it fits).
  Writing it runs ArraySetLength: `ToUint32(newLen)` must equal
  `ToNumber(newLen)` or a RangeError is thrown (strict-mode receiver
  semantics; failed writes leave length untouched). Shrinking truncates:
  dense slots >= newLen are hole-ified and sparse entries >= newLen are
  dropped. Growing never allocates or creates own elements.
- **Element stores** update `length` to `index + 1` when `index >= length`
  (uint32-safe: the largest index is 2^32-2). Stores at indices >= 2^20
  (`kMaxDenseElements`) go to a sparse ordered map; no dense allocation of
  2^32 slots ever happens.
- **delete arr.length** returns false (non-configurable); deleting an
  element hole-ifies it and never changes length.

## 6. Instruction Formats (Exhaustive)

| Format | Words | Layout |
|---|---|---|
| W0 | 1 | op, 0, 0 |
| W1_R | 1 | op, dst, 0 |
| W1_RI8 | 1 | op, dst, imm8 (signed) |
| W1_RR | 1 | op, dst, src |
| W1_J | 1 | op, offLo, offHi (signed 16-bit word offset) |
| W1_BR | 1 | op, cond, off8 (signed) |
| W2_I24 | 1 + 1 | op, 0, 0 ; imm24 |
| W2_I24D | 1 + 1 | op, dst, 0 ; imm24 |
| W2_I24S | 1 + 1 | op, src, 0 ; imm24 |
| W2_RR | 1 + 1 | op, A, B ; C, 0, 0 (three-register op; exact roles per opcode) |
| W2_RR_D | 1 + 1 | op, dst, A ; B, 0, 0 |
| W2_RR_V | 1 + 1 | op, obj, key ; val, 0, 0 |
| W2_RR_C | 1 + 1 | op, dst, ctx ; cell8, 0, 0 |
| W2_RC_V | 1 + 1 | op, ctx, cell8 ; val, 0, 0 |
| W2_CALL | 1 + 1 | op, func, argsBase ; argc8, this8, dst8 |
| W2_BR | 1 + 1 | op, cond, 0 ; off24 (signed) |

`argc` is 0..255; `argsBase + argc <= registerCount` is verified. `this8`
encodes a register; `Call` ignores it (this = undefined).

## 7. Exception Handlers

Each function carries a handler table:

```text
handler := { startPc, endPc, handlerPc, catchReg }
```

- If an exception is raised while pc is in [startPc, endPc), and this handler
  is the innermost enclosing one, the frame's pc becomes handlerPc and
  register `catchReg` receives the exception value.
- Handlers are tried innermost-first, then up the frame stack. A frame with
  no matching handler is destroyed and the exception propagates to the
  caller (the call site re-raises).
- `Rethrow` re-raises the exception value most recently delivered to this
  frame (tracked per frame; consuming it is the frontend's job).
- An exception raised while executing a handler is matched against handlers
  enclosing the handler's pc range (per ECMAScript semantics).

## 8. Feedback Vector (Tier 0 Profile Collection, Part I Tier 0)

T0 always collects profile data; tier transitions consume it (Rule 2, Rule
44). The verifier assigns one feedback slot per IC site in pc order.
Slots are typed:

| Slot kind | Owner opcodes | Recorded data |
|---|---|---|
| Property | GetProperty, SetProperty | Up to 4 seen shapes with hit counts; megamorphic flag after the 5th distinct shape. |
| Element | GetElement, SetElement | Up to 4 seen element-kind identities (0 = non-array receiver, 1..6 = elements kind + 1) with hit counts; megamorphic flag after the 5th. |
| Binary | All arithmetic/bitwise/comparison IC ops | Type histogram: Smi / HeapNumber / String / BigInt / Object / Other counts (8 counters). |
| Branch | JmpTrue, JmpFalse | Taken / not-taken counts (branch probabilities). |
| Call | Call, CallMethod, Construct | Ring of 4 callee function indices with counts; unknown-callee count. |

Feedback is deterministic given the same input program (Rule 124) and is
dumpable via `tsrun --dump-feedback`. Counters are saturating (Rule 114
applied: no overflow UB).

## 9. Validation Rules (the Verifier)

`tsrun --verify-only` runs the verifier. A module is valid iff all of:

1. **Structure** — table indices in range; entry function exists; every
   function's registerCount/paramCount/contextCellCount within limits
   (Section 2); paramCount <= registerCount.
2. **Registers** — every operand register < registerCount and not reserved
   (r254, r255); call windows satisfy argsBase + argc <= registerCount.
3. **Immediates** — constant/function/global indices < table sizes (assembler
   rejects unknown labels; verifier re-checks after serialization).
4. **Flow** — starting from pc 0 with params defined and all other registers
   undefined, a worklist dataflow over branch/handler edges tracks the
   defined-set per pc (set-union join):
   - reading a register possibly-undefined on some path → error,
   - unreachable instructions → error (no dead bytecode, Rule 60),
   - jump targets inside the function and not inside a multi-word
     instruction's suffix word → error,
   - falling off the end of the bytecode without Return → error.
5. **Handlers** — handler ranges in bounds, non-empty, handlerPc inside the
   function, catchReg a valid register; nested/overlapping ranges are legal
   (innermost wins).
6. **Context** — cell8 <= 255 enforced by format; the runtime throws
   ReferenceError on out-of-range cell access against the closure's context.
7. **Effect sanity** — informational only in v0.1: the effect class of each
   opcode is reported in `--dump`.

Diagnostics follow Rule 47: `file:line: error: message (expected vs actual;
hint)`.

## 10. Text Assembly Format (.tsbc)

```text
.module <name>
.const <label> = <literal>          ; literal: 42, -7, 3.5, 1e21, "str\uXXXX\n",
                                    ; or 123n for BigInt
.global <name>                      ; declares a global binding name
.entry <function-label>

.function <@label> <nparams> <nregs>
  <opcode> <operands>...            ; e.g. Add r1, r2
  Jmp .Lloop
.Lloop:
  .catch rX, .LtryStart, .LtryEnd, .Lhandler
  ; rX receives the exception value; try range is [LtryStart, LtryEnd)
.end
```

- Labels: `.L*` local to a function; `.const`/`.function` labels are
  module-scope and referenced with `@label`.
- Register operands are `rN`. Immediate call arg counts are plain decimals.
- Comments: `;` to end of line. Strings support \n \t \r \\ \" \0 \uXXXX.
- The assembler resolves labels, assigns feedback slots (verifier confirms),
  picks short/wide branch forms, and reports the first error with line
  number and hint (Rule 47).

## 11. Known Divergences and Deferred Features (Rule 143 register, interp scope)

| Feature | State | Owner | Target |
|---|---|---|---|
| Arrays (elements kinds, length exotic behavior) | **Implemented v0.2** (Section 5.12; dense tagged backend with enforced kind invariants + sparse map beyond 2^20) | Interp team | done |
| Array.prototype builtins (join/push/pop/slice/...) | Deferred; arrays exist but carry no methods | Interp team | v0.3 |
| Proxy (13 traps) | Deferred; semantics ordinary-object | Interp team | v0.3 |
| Generators / async (suspension) | Deferred; no suspend opcodes yet | Interp team | v0.3 |
| Sloppy mode (mapped arguments, with) | Strict-mode semantics only | Interp team | v0.3 |
| GC (heap is arena-owned, non-collecting) | Deferred | GC team | v0.4 |
| Symbol literals / well-known symbols from user code | Deferred | Frontend team | v0.3 |
| eval / Function constructor | Not representable in v0.2 bytecode | Frontend team | v0.4 |

Every divergence above is tracked, owned, and expiry-dated per Rule 143;
none silently changes observable semantics of the features that ARE
implemented.
