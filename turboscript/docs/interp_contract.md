# TurboScript Tier 0 Interpreter Contract

**Status:** Draft v0.2
**Owner:** TurboScript Interp Team
**Last Updated:** 2026-09-14
**Governing Laws:** `docs/laws/turboscript_compiler_laws.md`
**Implements:** Part I Tier 0; Rules 4, 6, 7, 8, 9, 16, 23, 26, 32, 41, 47, 52, 58, 60, 72, 74, 83, 90, 96, 114, 119, 120, 124, 143

---

## 1. Execution Model

Tier 0 is the direct-threaded register interpreter and the universal
correctness fallback (Rule 96): every executable function runs here, and
deoptimization from any higher tier must be able to reconstruct Tier 0 state
exactly (Rules 4 and 83 — implemented as the `enterAt` entry API, Section 6).

- **Dispatch:** computed goto (`&&label` / `goto *`) when compiled with
  GNU-compatible compilers; an exhaustive `switch` fallback otherwise. The
  dispatch table is generated once at interpreter start from the opcode
  table. The two paths must be semantically identical (enforced by running
  the full corpus through both in CI; the fallback is selectable at build
  time with `TS_NO_COMPUTED_GOTO=1`).
- **Frames:** heap-allocated `Frame` objects owned by the Isolate frame
  stack. A frame holds: function pointer, closure context, the register
  file (exactly `registerCount` tagged values), pc, and the pending
  exception slot used by `Rethrow`.
- **Calls:** `Call`/`CallMethod`/`Construct` push a frame and recurse into
  the frame runner (C++ recursion). The JS call depth is bounded by
  `kDefaultMaxCallDepth` (Rule 90): exceeding it raises a JavaScript
  `RangeError` ("Maximum call stack size exceeded"), never a native crash.

## 2. C++ Constraint Compliance

| Law | Implementation |
|---|---|
| Rule 6 (no exceptions on hot path) | Built with `-fno-exceptions`. All fallible APIs return `TsResult<T>` (`std::expected`). JS exceptions are values flowing through the frame stack per `bytecode_spec.md` Section 7 — they are not C++ exceptions. |
| Rule 7 (zero-allocation hot path) | The dispatch loop performs no C++ allocation on its steady path: register files are sized once at frame entry; property lookups use pre-interned keys; the heap uses arena ownership (see below). Deopt/enterAt materialization is a controlled, budgeted path. |
| Rule 8 (no RTTI) | Built with `-fno-rtti`. The value model is a tagged union; no `dynamic_cast` exists. |
| Rule 9 (no shared_ptr/function in hot code) | Raw pointers + indices only inside the interpreter; ownership lives in the Heap. |
| Rule 16 (interned symbols) | All property keys and global names are interned `SymbolId` (uint32). No `std::string` comparisons on the hot path. |
| Rule 23 (no magic constants) | Every threshold (call depth, megamorphic limit, smi range, string limit, array length/dense bounds) is a named `constexpr` in `ts_core.h`. |
| Rule 32 (bitmasked orthogonal state) | Object/property attribute state is `Flags<P propAttrs>` bitmasks; raw int flags are forbidden. |
| Rule 48 (`[[nodiscard]]`) | All `TsResult` returns are `[[nodiscard]]`. |

## 3. Value Model (strict-mode semantics, v0.2)

- Tagged values: Undefined, Null, Boolean, Smi (int32), HeapNumber (double),
  String (UTF-16), Object (incl. arrays), BigInt. (Symbol reserved; not
  user-visible yet.)
- Smi normalization: integral, in [-2^31, 2^31-1], and not negative zero.
  Negative zero is always a HeapNumber (Rule 72: `Object.is(-0, +0) = false`
  must be observable; `SameValue`/`SameValueZero` opcodes cover it).
- Numbers print via shortest round-trip formatting with ECMAScript
  decimal/exponential selection rules.
- `ToPrimitive`, `ToNumber`, `ToString`, `ToPropertyKey` implement the
  ECMAScript algorithms including user-code hooks (valueOf/toString/
  Symbol.toPrimitive when present), invoked through the call machinery.
- Objects: shape-based property storage (transition tree per isolate),
  prototype chains, accessor (getter/setter) properties, strict-mode store
  semantics (TypeError on frozen/non-writable stores). Dictionary mode is
  deferred (bytecode_spec.md Section 11).
- **Arrays (v0.2):** an Object may be an exotic array (`isArray`). Element
  and `length` behavior is enforced INSIDE the shared property operations
  (`getProperty`/`setProperty`/`deletePropertyImpl`/`hasPropertyImpl` walk
  the prototype chain themselves and consult array state at every node), so
  `GetProperty`/`SetProperty` and `GetElement`/`SetElement` cannot diverge —
  one semantic source (Rule 96).
  - **Elements kinds** PackedSmi/HoleySmi/PackedDouble/HoleyDouble/
    PackedTagged/HoleyTagged are real invariants enforced on every element
    store, with eager monotonic transitions (smi -> double -> tagged,
    packed -> holey). Design note: Tier 0 keeps ONE tagged element backend;
    the kinds gate hole semantics and feed the Element-kind feedback sites,
    while unboxed element backends remain a stencil-layer (Tier 1) concern.
  - **Holes** arise only from delete, growth past the end, or length
    shrink. Hole reads walk the prototype chain; holes are not own
    properties for `HasProperty`/`In`.
  - **length** is writable/non-enumerable/non-configurable: reads yield the
    length as a Number; writes run ArraySetLength (RangeError unless
    `ToUint32(v) == ToNumber(v)`; shrink truncates via hole-ification and
    sparse-entry removal; growth allocates nothing).
  - **Sparse elements:** indices >= `kMaxDenseElements` (2^20) live in an
    ordered `std::map` (deterministic, Rule 124); no 2^32-slot allocation is
    ever performed; `4294967295` is always a named property, never an
    index.
- BigInt: sign + 32-bit limb magnitude; add/sub/mul/div/mod/neg, comparisons,
  ToString/FromString base 10. Mixed Number/BigInt arithmetic → TypeError;
  BigInt division/modulo by 0n → RangeError (Rule 72).

## 4. Exception Handling

Per `bytecode_spec.md` Section 7 (handler table). Guarantees:

- Exception state is reconstructed exactly at handler entry: pc = handlerPc,
  catchReg = exception value, all other registers retain pre-try values.
- `Rethrow` re-raises the frame's pending exception.
- Uncaught exceptions at the module root print
  `Uncaught <name>: <message>` to stderr and exit with status 1.
- Runtime errors (TypeError, RangeError, ReferenceError) carry a message and
  a TurboScript stack trace (function name + word pc), satisfying the
  frame-reconstruction spirit of Rule 75 at the diagnostic level.

## 5. Feedback Collection (always on)

Per `bytecode_spec.md` Section 8. Feedback writes are plain stores (T0 is
single-threaded, Rule 119: no locks on hot paths); counters saturate instead
of overflowing (Rule 114). `--dump-feedback` prints the vector; the format is
stable text for tier-downstream tooling.

## 6. Deopt Entry (`enterAt`, Rules 4/83)

```cpp
TsResult<Value> enterAt(const Closure*, uint32_t pc,
                        std::span<const Value> registers);
```

- Rebuilds a frame at exactly `pc` with exactly the supplied register file
  and runs it. This is the mechanism a deoptimizer will use to hand control
  back to Tier 0.
- The verifier guarantees pc is an instruction boundary; `enterAt` re-checks
  and fails with a diagnostic otherwise (untrusted input rule, Rule 105).

## 7. Determinism and Replay (Rule 124, Rule 140)

- No hash randomization in property storage (insertion-ordered shape slots
  and deterministic symbol interning); identical input → identical output,
  identically ordered.
- `--dump` prints the module (constants, functions, opcode disassembly,
  handler table, feedback layout) — this is the replay/inspection artifact
  for interpreter bugs.

## 8. Testing Contract (Rules 34, 35, 41, 52, 60)

- Corpus tests are self-contained `.tsbc` + `.out` pairs under
  `tests/interp/corpus/`; they run identically on any machine (Rule 52).
  The suite covers arithmetic (Smi/double/BigInt), string semantics, the
  equality family (AbstractEq/StrictEq/SameValue/SameValueZero), bitwise
  ToInt32/ToUint32 edges, control flow (short + wide branch forms), globals,
  closures over context cells, recursion, nested exception handlers with
  Rethrow, object shapes/prototypes/instanceof, ToPrimitive user-code hooks,
  and Rule 90 stack-overflow behavior. The v0.2 array corpus
  (`array_*.tsbc`) covers: element store/get, PackedSmi -> PackedDouble ->
  PackedTagged transitions, growth past the end, exotic length
  (read/write/truncate/grow, RangeError on non-uint32-equal lengths,
  non-configurable delete), delete-induced holes with prototype-chain
  fallback, holey-double widening after delete, canonical/non-canonical key
  routing (`"1"` vs `"01"`), named properties on arrays, the 2^32
  boundaries (4294967294 sparse element, 4294967295 named property),
  sparse-shrink interaction, BigInt keys (`1n` -> element 1), and
  Get/SetElement vs Get/SetProperty parity (Rule 96).
- Test names encode the behavior proven (`bigint_mixed_number_add_throws`,
  ...) (Rule 41). The enterAt reconstruction guarantee (Rules 4/83) is
  proven by the `deopt_enter_at_reconstructs_registers` checks in
  `tests/interp/unit_enter_at.cpp`, run by `make unit` on both dispatch
  paths, including non-boundary-pc and register-file-size rejections
  (Rule 105).
- Negative tests (verifier/assembler rejections) assert the exact diagnostic
  text (Rule 47).
- Both dispatch paths (computed goto and switch fallback) run the full
  corpus in `make test`; `make test` must exit 0 from a clean checkout.

## 9. Out of Scope for v0.2 (tracked in bytecode_spec.md Section 11)

Proxy, generators/async, sloppy mode, GC, user symbols, eval,
Array.prototype builtins. Each is owned and expiry-dated there per Rule
143. Nothing in this list is silently degradable: opcodes or semantics that
depend on them do not exist in v0.2 bytecode.
