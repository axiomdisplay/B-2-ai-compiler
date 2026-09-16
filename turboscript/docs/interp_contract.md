# TurboScript Tier 0 Interpreter Contract

**Status:** Draft v0.5
**Owner:** TurboScript Interp Team
**Last Updated:** 2026-09-16
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
  `kMaxCallDepth` (Rule 90): exceeding it raises a JavaScript
  `RangeError` ("Maximum call stack size exceeded"), never a native crash.

## 2. C++ Constraint Compliance

| Law | Implementation |
|---|---|
| Rule 6 (no exceptions on hot path) | Built with `-fno-exceptions`. All fallible APIs return `TsResult<T>` (`std::expected`). JS exceptions are values flowing through the frame stack per `bytecode_spec.md` Section 7 — they are not C++ exceptions. |
| Rule 7 (zero-allocation hot path) | The dispatch loop performs no C++ allocation on its steady path: register files come from a bounded LIFO pool (`kRegPoolMax`, reused across frames); property lookups use pre-interned keys; the heap uses arena ownership (see below). Deopt/enterAt materialization is a controlled, budgeted path. |
| Rule 8 (no RTTI) | Built with `-fno-rtti`. The value model is a tagged union; no `dynamic_cast` exists. |
| Rule 9 (no shared_ptr/function in hot code) | Raw pointers + indices only inside the interpreter; ownership lives in the Heap. |
| Rule 16 (interned symbols) | All property keys and global names are interned `SymbolId` (uint32). No `std::string` comparisons on the hot path. |
| Rule 23 (no magic constants) | Every threshold (call depth, megamorphic limit, smi range, string limit, array length/dense bounds) is a named `constexpr` in `ts_core.h`. |
| Rule 32 (bitmasked orthogonal state) | Object/property attribute state is `Flags<P propAttrs>` bitmasks; raw int flags are forbidden. |
| Rule 48 (`[[nodiscard]]`) | All `TsResult` returns are `[[nodiscard]]`. |

## 3. Value Model (strict-mode semantics)

### 3.0 64-bit NaN-boxed Representation (v0.5, normative)

A `Value` slot is exactly 8 bytes (`sizeof(Value) == 8`, trivially copyable;
frame reconstruction stays a plain memcpy). Normative encoding:

- **double**: raw IEEE-754 bits, with one exception — every NaN is
  canonicalized to +qNaN (`0x7FF8000000000000`) at boxing. A NaN's sign and
  payload are unobservable through values (no TypedArray layer exists to
  smuggle payloads; `Object.is`/`SameValue`/`SameValueZero` are payload-
  blind; string conversion prints "NaN"), so canonicalization has no
  semantic surface (Rule 72/-0 unaffected: -0.0 is not a NaN and stays raw).
- **tagged**: `0x1FFF` (top 13 bits) | kind nibble (bits 50..47) | payload
  (bits 46..0). The tag prefix forces sign=1, exponent=all-ones,
  mantissa[51]=1 — only negative NaNs can match it, and boxing
  canonicalization guarantees no stored double ever does. **The boundary is
  13 bits, not 12, specifically so -Infinity (mantissa 0) stays a double**;
  a 12-bit tag would have collided with it. Caught by the corpus
  (Rule 36) during the v0.5 port and pinned by compile-time
  `static_assert` proofs in `ts_value.h` (`value_enc` namespace: tag/NaN
  disjointness including ±Inf, Smi round-trips, constant decodes).
- Kind payloads: Undefined/Null/Hole = 0 (single-compare constants);
  Boolean = 0/1; Smi = low 32 bits (kSmiMin/kSmiMax unchanged, sign-
  extending unbox); pointer kinds = the full 47-bit heap pointer
  (Linux user VA < 2^47; deque-backed stable addresses, no base+offset
  arithmetic, no compressed-pointer cage).
- All double boxing funnels through `Value::heapNumber` (the single
  canonicalization point) or `normalizeNumber`/`tsSmiOrNumber` (same
  contract). Fast lanes box results only through these.
- `kind()` decodes doubles to `ValueKind::HeapNumber` (the phantom kind of
  the untagged space); pointer-kind identity comparisons are u64 compares.
- Why: every register read/write, slot load and Mov moves 8 bytes instead
  of 16 — the dominant structural term measured in benchmarks_v0.4.md
  Section 3 (closed: geomean 0.49x -> 0.65x vs Ignition, benchmarks_v0.5.md).

### 3.0.1 Call-path structure (v0.5)

- `Closure` caches its resolved `Function*` at creation; `callClosure`
  does not walk the module function table per call.
- `Isolate::CallSetup` (built once at `loadModule`) holds the per-function
  feedback pointer, pc->slot map pointer and a has-feedback flag;
  `runFrame`'s prologue is one indexed load plus one `recordFeedback_`
  branch. The runtime `recordFeedback_` toggle stays live (Rule 124).
- `frameStack_` is pre-reserved to `kMaxCallDepth` (Rule 90 bound).
- Slot vectors grow with `kMinSlotCapacity` floor (geometric reserve at
  transition pushes). Storage-only: `slotCount` is shape-driven and all
  IC/lookup paths read shape->slotCount, never capacity (Rule 96).

- Tagged values: Undefined, Null, Boolean, Smi (int32), HeapNumber (double),
  String (UTF-16), Object (incl. arrays), BigInt, **Symbol (v0.3)**,
  **Proxy (v0.3)**.
- **v0.3 prototype wiring:** fresh plain objects chain to Object.prototype,
  arrays to Array.prototype, closures' function objects to
  Function.prototype; every function's fresh `.prototype` instance chains
  to Object.prototype. The builtin layer is ordinary, monkey-patchable data
  properties (Rule 70) installed at Isolate construction (ts_builtins.cpp).
- **v0.3 user symbols:** `Symbol(desc)` creates unique-identity symbols;
  symbol keys live in a dedicated SymbolId range (ts_core.h
  `kUserSymbolBase`) and flow through shapes/lookup unchanged;
  `Symbol.for`/`keyFor` keep the registry. typeof symbol, ToString
  (`Symbol(desc)`), and SameValue identity are oracle-verified.
- **v0.3 Proxy:** all 13 internal methods (ES ch. 10.5) route through the
  trap protocol (ts_proxy.cpp) with target-side invariant checks;
  `Proxy(target, handler)` is exposed as a native. Traps absent on the
  handler forward to the target; handler traps must be functions or
  undefined (TypeError otherwise).
- Smi normalization: integral, in [-2^31, 2^31-1], and not negative zero.
  Negative zero is always a HeapNumber (Rule 72: `Object.is(-0, +0) = false`
  must be observable; `SameValue`/`SameValueZero` opcodes cover it).
- Numbers print via shortest round-trip formatting with ECMAScript
  decimal/exponential selection rules.
- `ToPrimitive`, `ToNumber`, `ToString`, `ToPropertyKey` implement the
  ECMAScript algorithms including user-code hooks (valueOf/toString),
  invoked through the call machinery. Well-known symbols (@@toPrimitive,
  @@iterator) are v0.4 (bytecode_spec.md Section 11).
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

### 3.4 Fast-Path Guard Discipline (v0.4)

The dispatch handlers contain in-handler fast lanes (Smi arithmetic,
number-pair arithmetic, dense element access, cached property keys,
branch truthiness, closure call hops). Their contract, enforced by the
differential corpus (`fastpath_smi_edges`, `fastpath_tostring`) and by
the cross-engine bench checksums:

- A fast path is a **guard in front of the semantic helper, never a
  replacement** (Rule 96). Any input the lane does not provably cover
  (overflow, -0, NaN, holes, hooks, uncached keys) executes the unchanged
  helper the oracles verified.
- Lanes preserve the slow path's observable results exactly, including
  -0 vs +0 (Rule 72), Smi-range overflow to HeapNumber, IEEE remainder
  sign rules, and ToInt32 shift semantics.
- Feedback recording is identical on fast and slow lanes (Rule 124:
  deterministic profiles regardless of which lane executed).
- Relational comparison is `x < y` / `x > y` semantics: `Unordered` (NaN)
  is false for ALL four comparison opcodes (v0.4 fix — Le/Ge previously
  returned true for NaN, caught by the fast-path review against node and
  quickjs).

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
  Get/SetElement vs Get/SetProperty parity (Rule 96). The v0.4
  fast-path corpus (`fastpath_smi_edges`, `fastpath_tostring`) pins the
  new in-handler lanes to goldens generated from node AND quickjs
  agreement: Smi overflow lanes, -0 probes through `1/x`, IEEE remainder
  signs (`INT32_MIN % -1` -> -0), ToInt32 shifts, `>>>` above kSmiMax,
  the NaN relational fix, and the doubleToString integer fast path
  (integral doubles < 2^53 vs shortest-round-trip exotics).
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

## 9. Out of Scope for v0.3 (tracked in bytecode_spec.md Section 11)

Generators/async, sloppy mode, GC, well-known symbols, eval. Each is owned
and expiry-dated there per Rule 143 (generators/sloppy moved from v0.3 to
v0.4 with reasons — both need frontend-level opcodes that do not exist in
TSBC yet). Nothing in this list is silently degradable: opcodes or
semantics that depend on them do not exist in v0.3 bytecode.

## 10. Driver CLI (tsrun)

```text
tsrun <file.tsbc> [--verify-only] [--dump] [--dump-feedback] [--stats]
                  [--time] [--no-record] [--check <expected.out>]
```

- `--time` measures the `run()` execution phase only — assembly, verification
  and module load are excluded — and prints `elapsed_ms=<x.xx>` on stderr
  (program output on stdout stays untouched for `--check`).
- `--time` is the timing source for the interpreter-only benchmark suite
  (`tests/bench/`, results in `docs/benchmarks_v0.2.md`): the JS twin
  kernels time `kernel()` in-script, so every engine is compared
  kernel-execution-only. Feedback collection stays ON during timed runs —
  the numbers include the spec-mandated Tier 0 profiling cost
  (Section 5 / Part I Tier 0), and that is deliberate.
- `--no-record` (v0.3) disables feedback recording for the recording-tax
  measurement (benchmarks_v0.3.md Section 4). Measurement toggle only —
  NOT an operating mode: disabling feedback also disables the IC layer,
  which the same measurement shows is net-positive (object_fields).
