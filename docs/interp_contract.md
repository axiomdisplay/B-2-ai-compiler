# B-2 Tier-0 Interpreter State Contract (v1)

Owner: Interpreter Team (T0) (`docs/teams/interpreter-team.md`)

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

Version: 1.1.0
Status: normative for all compiled tiers — `baseline_noir`, `ir`, `passes`, `regalloc`,
`codegen`, and `aot` consume this contract; `docs/deopt_backend.md` Part A (canonical T0 state model) and
`docs/stencils.md` §8 (T0-compatible T1 frames) are built on it.

This is the published T0 exact-state / deoptimization-reconstruction contract (interpreter-team charter
deliverable 8): the prose twin of the frozen headers
`include/b2/interp/{Value,Heap,Runtime,Frame,Interp}.h` (whose block comments pin the dispatch loop, call
protocol, exception algorithm, quickened-opcode pins, and safepoint/profiling model) and of the landed
`compiler/interp/src/{Heap,Runtime,Frame,Interp}.cpp`. Headers and document change together, never
separately (§13); every behavioral claim is pinned by `tests/interp/` (125 tests plus a 14-program golden
corpus with exact-stdout fixtures). T0 is the baseline engine, the universal correctness fallback (Rule
96), the reference implementation against the Java SE/JVM oracle (Rules 67, 133), and the deopt target of
every compiled tier (Rules 4, 75; Amendment B.1); a defect here corrupts every tier above it, so the state
model, deopt entry, and fixture format are versioned and change-controlled (§13).

---

## 1. The T0 frame

The T0 frame is the deoptimization unit of the whole system (Rules 4, 75, 96; Amendment B.1;
`docs/deopt_backend.md` Part A §1/§3): a plain, copyable, dumpable value (`Frame.h`) — which is what keeps
"deopt = memcpy into a new T0 frame" true (`docs/rbc_spec.md` §1.4).

```cpp
struct Frame {
  const rbc::Method* method;      // owning method (Program is const in a run)
  std::uint32_t pc;               // RBC instruction index
  std::vector<Value> locals;      // numLocals slots, parameters first
  std::vector<Value> regs;        // numRegs slots, Bottom at entry
  std::vector<ObjRef> monitors;   // entered by this frame, LIFO order
  ObjRef pendingException;        // in-flight exception, else invalid
  std::uint32_t callerPc;         // pc of the invoke that pushed this frame
};
```

- **method** — `numLocals`/`numRegs` fix the locals/regs sizes; deopt materializes exactly those sizes
  (`resume()` Bottom-pads short vectors defensively; no tier may rely on that).
- **pc** — an RBC **instruction index** into `Method::code`, never a byte offset (`docs/rbc_spec.md`
  §2.2); fall-through is always `pc + 1`. A deopt pc must be an instruction boundary in `[0,
  code.size())`; anything else is corrupt and traps `java/lang/InternalError` ("resume frame does not
  match the program") instead of reading OOB.
- **locals** — one slot per value, parameters first: instance methods put the receiver at `l0` then
  parameters in order; static methods put the parameters at `l0..`. `long`/`double` occupy **one** slot
  each — the documented divergence from the JVM's two-slot category-2 locals (`docs/rbc_spec.md` §1.2),
  chosen so deopt reconstruction is a flat memcpy and T1 frames are trivially T0-compatible
  (`docs/stencils.md` §8).
- **regs** — expression temporaries `r0..rN-1`, all `Bottom` at frame entry. Registers and locals
  **survive calls** (frame residency, `docs/rbc_spec.md` §1.3): an `invoke*` writes only its `dst`;
  argument registers are read, never clobbered.
- **monitors** — the objects this frame entered (`monitorenter` plus synchronized method entry), recorded
  **most-recent-first**; return and unwind release them front-to-back, i.e. LIFO (JVMS
  monitorexit-on-throw). This record IS the held-monitor state deopt must reproduce
  (`docs/deopt_backend.md` Part A §15.5).
- **pendingException** — set only between "trap occurred" and "handler entry or unwind step"; always unset
  at any safepoint poll. The exception algorithm runs synchronously trap-to-handler, so the only producer
  of a non-null `pendingException` is an externally constructed exception-deopt frame (§3).
- **callerPc** — the pc of the invoke that pushed this frame; the caller's pc is **not advanced until the
  callee returns normally** (§5.1); informational in a `resume()` frame. Deopt must produce a `Frame`
  whose `method`, `pc`, `locals`, `regs`, `monitors`, and (exception deopt) `pendingException` match
  exactly the state T0 would have at that instruction boundary; `resume()` accepts precisely that frame.

## 2. The value model

`Value.h` is a published contract, not an implementation detail. Every slot — local, register, array
element, instance field, method result — holds one 16-byte `Value` (`static_assert(sizeof(Value) == 16)`):
`{ RType type; union { i32, i64, f32, f64, u32 obj } as; }`.

- **The tag IS the verifier's RType** (`docs/rbc_spec.md` §4): slots hold exactly the type the verifier
  proved at that program point, so on a verified stream the runtime tag and verified type can never
  disagree — which is what makes state dumps usable as deopt fixtures (§10) and why no slot may leak
  `Bottom` into typed code (§6.13).
- **Null-tag invariant:** `Null` is its own tag; `type == Ref` implies `as.obj != 0` (null is never
  Ref-with-zero). One representation for null keeps `if_acmp*` (`Value::sameObject`: null == null, same id
  == same object, null != any object) and NPE checks branch-free.
- **References are stable object ids** (Rule 15 discipline): ids start at 1, 0 is the invalid sentinel,
  and ids never change for the lifetime of the owning `Runtime` — storage may move on heap growth, ids
  never do, so deopt frames copy `ObjRef`s freely. **Deopt reconstruction is a memcpy:** `Value` is
  assert-guaranteed trivially copyable; materializing T0 state never retags, revalidates, or allocates per
  slot.
- **GC visibility:** the tag says which slots hold references — `Ref` slots live references, `Null` slots
  null references, everything else opaque bits; a future GC derives reference maps from tags + layout
  (`docs/stencils.md` §8.2). v0 has no GC and never reuses object ids.
- **NaN boxing is a future, flag-gated representation change** (Part XVIII): disabled by default,
  requiring a global kill switch, per-target flags, verifier/GC-map/deopt checks, interpreter round-trip
  tests, baseline round-trip tests, T2 lowering tests, AOT artifact versioning, and this team's approval
  (the interpreter is an affected party). It is a representation optimization only and must not observably
  change this contract — no change to Java-visible primitive values, object identity, GC reference
  tracking, deopt reconstruction, or float/double bit-level behavior.

## 3. The deopt entry contract

`Interpreter::resume(Frame)` is the single deopt entry point (Rule 4; `docs/deopt_backend.md` Part A).
After materializing T0 state, every tier's deopt stub — for any Part A §2 class (eager guard §2.1,
uncommon trap §2.2, exception deopt §2.3, OSR exit §2.4, lazy invalidation §2.5) —

1. materializes `locals` and `regs` as `Value`s **with correct tags** — the verified type of each slot at
   that program point (§2);
2. materializes `pc` at an RBC instruction boundary (§1);
3. materializes `monitors` = the **held** monitors, most-recent-first; for a synchronized method this
   includes the entry acquire;
4. for exception deopt only (Part A §2.3), materializes `pendingException` = the in-flight Java exception
   object (exceptions are values, Rule 74);
5. calls `resume(frame)` — the frame is copied; locals/regs sizes must match the method (callers built
   them from this contract).

`resume()` semantics (implementation- and test-pinned):

- **Hard gate first, like `run()`:** the whole program is verified; any diagnostic returns
  `RunStatus::VerifyFailed` — T0 never executes unverified RBC. **Total defensive validation:** the method
  pointer must lie in the program's method table and `pc` in code range; hostile frames yield
  `RunStatus::Threw` with `java/lang/InternalError` ("resume frame does not match the program") — never
  UB, never a crash.
- **Exception deopt enters THE EXCEPTION ALGORITHM at `frame.pc`** (Part A §2.3): a set `pendingException`
  starts dispatch inside the §5.2 algorithm with that exception at the given pc — handler search,
  unwinding, and uncaught `Threw` behave exactly as if the trap had just fired there.
- **Frame-stack scope (v1.0.0 pin):** `resume()` executes exactly the one reconstructed (innermost) frame
  to completion — the frame stack is cleared first, `Runtime` state (heap, statics, ICs) persists across
  `run()`/`resume()`, and the frame's return is reported as `RunStatus::Returned`; the deopt runtime owns
  stitching the caller chain (further `resume()` calls or re-entry) per Part A §13. No invocation counter
  is bumped: the resumed frame's entry was counted by whoever started it.

The observational-equivalence golden rule (`docs/deopt_backend.md` Part A §1, quoted normatively):

> After guard failure, execution must resume in a state observationally
> indistinguishable from the state T0 would have reached at that RBC instruction
> boundary.

`run(name, descriptor, args)` is the normal entry: verify-all hard gate; entry resolution (missing ->
`Threw` `java/lang/NoSuchMethodError` "<name><descriptor>"); entry frame with args as locals (receiver
first for instance entries; insufficient args -> `InternalError`); synchronized entry acquires the
receiver's or class object's monitor before execution (a null receiver traps at the call site).
`RunResult` carries `status`, `result` (Bottom tag when void), `exception`, `verifyDiags`, `stats`, and
`safepointTrace` (§9, §10).

## 4. Dispatch model

- **v0 ships the portable token-threaded switch core:** fetch `op` -> `switch (ins.opcode())` -> handler
  -> next fetch. The switch is exhaustive over all 150 opcodes with **no default clause** (a missing
  handler is a `-Wswitch` compile error, not a runtime surprise; the `Op::_Count` pseudo-case is an
  unreachable-guard raising `InternalError`); the compiler lowers the dense switch to a jump table — the
  portable direct-threaded form. The architectural requirement (Amendment B.1) is **minimal dispatch
  overhead, not a particular compiler extension**.
- **The computed-goto upgrade is LANDED (v1.1.0, MSG-20260830-005):** `B2_INTERP_COMPUTED_GOTO`
  (CMake option, default ON for GNU/Clang, self-disabling elsewhere) compiles `Interp.cpp` as
  `gnu++26` and dispatches through a designated-initializer label table with the indirect branch
  replicated at EVERY handler tail (`B2_NEXT` -> `B2_DISPATCH`), so the branch predictor learns
  per-opcode transitions instead of sharing one jump site with the whole opcode mix. Semantics are
  identical by construction — one shared set of handler bodies (`B2_TARGET`/`B2_NEXT` macros), same
  fetch/probe/stats order, same redispatch-on-catch. The differential proof is
  **`interp_portable_tests`**: the same sources compiled strict-c++26 (switch core) running the same
  corpus against the same goldens (Law 36). The exhaustiveness guarantee stays with the portable
  build's `-Wswitch`; the computed-goto build adds a designated-initializer duplicate check (compile
  error) and a null-scan refusal (Rule 47) for missing table entries. **One loop over an explicit frame
  stack:** Java calls never recurse in C++ (`StackOverflowError` must be a Java-visible trap at
  `maxFrames`, and deopt needs materializable frames — the frame stack IS the state).
- **`fr` is a fetch-time binding, not a function-scope reference** (v1.1.0): the frame pointer is
  re-acquired at every fetch because `frames_` mutates on calls, returns, and unwinds. In source form
  `fr` is a macro over the frame pointer; the discipline it makes load-bearing: every `frames_`
  mutation site breaks or returns immediately (the return handler re-binds its own callee/caller
  locals), so no handler observes a frame through a stale binding.
- **Hot-path law compliance (Rules 6, 7, 8, 9, 16, 118):** the dispatch switch and its fast paths contain
  no C++ exceptions, no allocation, no RTTI, no `std::function`/`shared_ptr`, no string work, no locks.
  Sanctioned allocation sites, each documented at its definition in `Interp.cpp`: **frame pushes** (every
  `invoke*` and `<clinit>` push); **trap paths** (exception objects, JVM message strings, dotted class
  names — Rule 74); **inline-cache misses** (lazily sized IC tables plus resolution/`classId` interning,
  §8); **lazy heap growth** (first touch of a field resolved after its object was allocated, §11);
  **name-bearing opcodes** (`checkcast`/`instanceof`/`new`/`ldc`/`anewarray`/`getstatic`/`aastore` reach
  `classId(name)` per execution through the frozen `Runtime` API — a reported v0 seam; interning is
  memoized, the call itself is not).
- **Verifier guarantees are not re-checked on the fast path** (bounds, types, termination);
  defense-in-depth probes convert impossible states into `java/lang/InternalError` results — total, never
  a crash, never silent.

## 5. Execution semantics pins

### 5.1 The call protocol

For every `invoke*` (`docs/rbc_spec.md` §3.15): arguments occupy consecutive registers `a..a+b-1`
(receiver = `a[0]` for virtual/special/interface; none for static); `dst` receives the result iff the
callee's return descriptor is non-void — **void calls write no dst**. Arguments are read, never clobbered.

- **The caller's pc stays AT the invoke; it advances only on normal return** (`callerPc + 1`). WHY:
  exception coverage is checked in the caller at the call site — the call site is the throwing location
  (JVMS) — so a trap in the callee unwinds with the caller's pc pointing at the invoke and the caller's
  protected ranges apply exactly as in Java.
- **Frame depth limit:** pushing past `maxFrames` (default 8192, the `-Xss` analogue; per `InterpConfig`)
  traps `java/lang/StackOverflowError` — checked BEFORE the push, at the call, so it is a Java-visible
  trap, never a C++ container failure; the frame stack is empty on `Threw`.
- **Resolution order:** virtual/interface calls probe the inline cache first (site key = caller MethodId +
  call pc, §8); on miss, the builtin table (receiver class, §11), else resolve the MethodRef and fill the
  IC. The receiver's NPE precedes everything, including the IC (an IC hit must not bypass the null check).
  Special/static calls resolve directly (static target, no IC); `invoke*_quick` carry the resolved id in
  `imm` (§7); `invokedynamic` raises `java/lang/BootstrapMethodError` (v0 is verifier-only). Abstract or
  native targets raise `java/lang/NoSuchMethodError` "<class>.<name><descriptor>" (documented v0
  divergence: the JVM's `IncompatibleClassChangeError` needs a class model that does not exist yet).
- **Normal return:** pop the frame; release the callee frame's remaining monitors LIFO (best-effort —
  unbalanced streams become well-defined, §6.10); write the returned `Value` into the caller's `dst` iff
  non-void; advance the caller to `callerPc + 1`.

### 5.2 The exception algorithm (Rule 74)

```text
throwException(obj):               [obj non-null; athrow null traps NPE]
  loop over the frame stack, innermost first:
    fr = top; for each ExceptionHandler h of fr.method, IN TABLE ORDER,
      where h.start <= fr.pc < h.end   (the invoke's pc for a caller):
        if h.catchType < 0 (catch-all) or
           isAssignableFrom(catchClass, classOf(obj)):       CAUGHT ->
          fr.pc = h.handler; fr.regs = Bottom x numRegs (ALL die)
          fr.regs[0] = Ref(obj)            (exception delivery)
          pending exception cleared; CONTINUE DISPATCH
    not caught: release fr's monitors LIFO (best-effort; failures are
      swallowed - the original exception wins), pop fr
  stack empty -> RunStatus::Threw with obj (uncaught)
```

- Innermost frame first; within a frame, **table order** — the first matching handler in the table wins.
  Coverage is `[start, end)` at the **current** pc (the invoke's pc for a caller, §5.1).
- Handler entry state is the `docs/rbc_spec.md` §5.3 pin: locals keep their stable types, all registers
  die to `Bottom`, and **r0 carries the caught exception, typed `Ref`** (the RBC equivalent of the JVM
  pushing the exception onto the operand stack). The verifier seeds exception edges with exactly this
  state — wide protected ranges included (a verifier join bug that reset r0 to `Bottom` on
  multi-instruction ranges was found by the test suite and fixed).
- Every trap (division by zero, null dereference, array bounds, negative array size, checkcast, aastore,
  monitor misuse, athrow null, missing method, invokedynamic, StackOverflow) allocates the exception via
  `Runtime::makeException` with the JVM's message shapes (§11) and enters the same algorithm.
  `stats.exceptions` counts at **raise time**, rethrows included.

### 5.3 Class initialization (JVMS 5.5)

- **First-use triggers:** `new`, `getstatic`, `putstatic`, `invokestatic`, `ldc`-of-Class — plus
  `invokestatic_quick` (§7), which carries no MethodRef to name another class. On a not-yet-initialized
  class the interpreter pushes the class's `<clinit>()V` frame **without advancing the current pc**, after
  marking the class initialized; **mark-before-run** is why recursive first use terminates (JVMS 5.5).
- **Re-execution mechanism:** the `<clinit>` frame's `callerPc` is the trigger's pc and the return handler
  does NOT advance it — when `<clinit>` returns (recognized by name + descriptor `"()V"`), the original
  instruction re-executes with the class initialized.
- **v0 gating pin:** `needsInit` is true iff the class is the program class, has an RBC `<clinit>()V`, and
  has not been started — in the one-class v0 world only the program's class can own RBC methods, so no
  other class can have a `<clinit>` (the guard prevents double execution under foreign classes'
  initialized flags).
- Known edge (documented verifier gap, never a crash): an explicit `invokestatic` of `<clinit>` from
  bytecode re-executes forever, because the return handler treats any `()V` `<clinit>` frame as a trigger
  return. Producers must not emit it; the v0 verifier does not reject it.

### 5.4 Synchronized methods (JVMS 8.4.3.6)

Instance synchronized methods acquire the **receiver's** monitor on entry; static synchronized methods
acquire the **class object's** (`Runtime::classObject`, one `java/lang/Class` instance per class per
Runtime — also what `ldc`-of-Class materializes). Entry acquire happens before the frame executes (a null
receiver traps at the call site); release happens on **both** normal return **and** unwind — the entry
acquire is simply the first entry of the frame's monitor record, so return/unwind release it like any
`monitorenter`.

## 6. v0 semantic pins and divergences (the honest list)

Every item is implemented, code-commented, and test-pinned; each is a deliberate v0 decision. Divergences
from Java are correctness debt tracked for the real runtime (Rule 67 requires divergence to be documented
— this section is that documentation).

1. **`guard_non_null`/`guard_class` are no-ops in T0:** T0 holds no speculative state, so the guards'
   failure conditions cannot arise; both advance `pc + 1` (not path terminators, so the fall-through
   exists by verification). No v0 tier emits them.
2. **`deopt_trap` raises `java/lang/InternalError`** ("deopt_trap executed in T0 without deopt metadata
   (v0)"): a path terminator whose resume point lives in deopt metadata no tier provides yet — honest
   refusal, never silent.
3. **Narrowing is store-side:** `bastore`/`castore`/`sastore` truncate the value on store
   (int8/uint16/int16, zero-extended back to Int); `baload`/`caload`/`saload` are plain loads (well-typed
   stores keep sub-int arrays in range, so a raw load is already extended).
4. **`ldc` of MethodType/MethodHandle raises `InternalError`** — no method-handle runtime in v0.
   **`invokedynamic` raises `java/lang/BootstrapMethodError`** ("invokedynamic not supported in v0") — v0
   is verifier-only for indy.
5. **Array assignability is exact-descriptor-only:** no covariance (`[Sub` is not `[Super`), not even
   array-to-Object (`[I` is not `java/lang/Object`; `checkcast`/`instanceof` against Object on an array
   throws in v0). Non-array classes walk the superclass chain.
6. **`println`/`print` builtins format by RUNTIME VALUE TAG** (the frozen `execBuiltin(Builtin, span<const
   Value>)` signature cannot carry the call-site descriptor): Int/Long decimal; Float/Double via
   `javaFloatString`/`javaDoubleString`; Null "null"; a Ref prints the String payload for Strings, else
   `DottedClassName@<decimal objid>` (no hashCode — the stable object id stands in, deterministic under
   Rule 124). So `(Z)V` prints "0"/"1" (Java: "true"/"false"), `(C)V` prints the code point decimal (Java:
   the character), `(Object)V` prints a decimal id (Java: hex hashCode). **Output-parity corpus and tests
   must use `(I)(J)(F)(D)(Ljava/lang/String;)` descriptors.** v1 plan: pass the descriptor to
   `execBuiltin`.
7. **Trailing `multianewarray` dims are length-0 arrays**, not Java's nulls: the Class constant names the
   full array class, the leading-`[` count is the total nest depth, unspecified trailing levels default to
   length 0. Dimension count caps at 255 (JVMS 4.4.1); hostile counts trap `InternalError`.
8. **Empty exception messages are empty Strings, not null `detailMessage`:** `makeException` stores an
   interned `""` so `exceptionMessage` is total and message identity stable (no v0 observable difference;
   flagged for the v1 message model).
9. **Unknown exception classes catch by exact name only:** a class outside the 24-class builtin hierarchy
   (§11) has no superclass chain (`isAssignableFrom` is reflexive for it), so `catch Throwable` misses it.
   The builtin hierarchy itself DOES catch-chain (`catch Exception` catches `ArithmeticException`,
   tested).
10. **Monitor release on unbalanced streams is best-effort and well-defined:** normal return releases the
    frame's remaining monitors LIFO; unwind-release failures are swallowed (the original exception wins).
    Monitor balancing is not verified (`docs/rbc_spec.md` §10.2).
11. **Subnormal float extremes may print differently from the JDK's renderer** (digit selection differs on
    a few subnormals; e.g. double `MIN_VALUE` renders "5E-324" vs the JDK's "4.9E-324"). Both round-trip;
    the `to_chars`-based digits are the pinned reference.
12. **Exceptions are counted at raise time** (rethrows included, §5.2).
13. **Unwritten fields read as their type's zero** (JLS 4.12.5): the heap's `Bottom` "unset" marker is
    coerced to the declared type's zero on read, so no register ever leaks `Bottom` (the §2 slot-type
    contract). Quickened corner: `getfield_quick` of a never-written field raises `InternalError` — the
    descriptor is gone after in-place quickening, so the default value is unknowable. Write fields before
    quickening their reads.
14. **Quickened `getfield_quick`/`putfield_quick` still require `imm < cp.size()` in the verifier** —
    their `Sig` still looks cp-shaped, so the structural cp bounds check applies even though `imm` is a
    byte offset. Inconsistency against `docs/rbc_spec.md` §6.2 (which says the cp check is skipped — only
    `invoke*_quick` actually skip). Hand-built quickened streams must pad the pool; in the text format
    `imm` IS a pool index, so text streams are consistent by construction.
15. **Hand-written quickened streams must PRE-TYPE the dst of `getfield_quick`/`invoke*_quick`:**
    `OpInfo`'s result type for quickened ops is `Bottom` (no descriptor source after quickening), so the
    verifier cannot infer the dst type — a typed use must be preceded by a `*store` of the result.
16. **Statics persist across `run()` calls and ICs persist with the `Runtime`** ("a JVM that stays up");
    `putstatic` to the `System.out`/`System.err` singletons is refused (`InternalError` "store to builtin
    static") — the singletons are final builtins whose identity every `getstatic` depends on.
17. **Numeric semantics are Java's exactly** (Rule 72): int math computed in `int64_t` then narrowed (C++
    signed overflow is UB; Java wraps); long math in the unsigned domain; shift counts masked to 5/6 bits
    (JLS 15.19); float->int conversions clamp by comparison in the value's own domain BEFORE the cast (NaN
    -> 0, saturation, JLS 5.1.3); `frem`/`drem` are `std::fmod` (JLS 15.17.3 truncated quotient), NOT the
    IEEE remainder — `docs/rbc_spec.md` §3.7/§3.8's "IEEE remainder" wording is a spec typo (reported).

## 7. Quickened-opcode interim pins

The quickener does not exist yet (not this team's to build), but T0 already executes quickened streams
(charter deliverables 1 and 3). v0 pins the interim `imm` encodings (Interp.h "INTERIM QUICKENED-OPCODE
PINS"):

| Opcode | `imm` carries | v0 encoding | T0 execution |
|---|---|---|---|
| `getfield_quick` | resolved field **byte offset** | `slot * sizeof(Value)` = `slot * 16` | `slot = imm / 16`; no cp, no IC; NPE + unwritten-field refusal |
| `putfield_quick` | resolved field **byte offset** | `slot * 16` | `slot = imm / 16`; no cp, no IC |
| `invokespecial_quick` | **MethodId** | program method-table index | direct push; id validated against table size (out of range -> `InternalError`) |
| `invokestatic_quick` | **MethodId** | program method-table index | direct push; still honors the JVMS 5.5 first-use trigger |
| `invokevirtual_quick` | **IC site id** | the pc of the call instruction itself (unique within its method; with the executing method it identifies the site) | IC lookup at `[MethodId][imm]`; cold cache resolves via `cp[imm]` |
| `invokeinterface_quick` | **IC site id** | the pc of the call site | same as `invokevirtual_quick` |

- **Virtual/interface site ids are call-pc-keyed in v0**; quick and unquickened forms of one site share IC
  state when `imm == pc`. Site ids are bounded by the method's code size; larger ids are hostile
  (`InternalError`).
- **A cold quickened virtual/interface cache must still resolve** ("full unquickened-style resolve"), but
  in-place quickening repurposed `imm`, so the only lawful v0 resolution source is the method pool reached
  through `imm`: a cold cache whose `imm` is not a MethodRef/InterfaceMethodRef (or is out of pool range)
  raises `InternalError` ("quickened virtual call with cold inline cache (v0)"). Every v0 quickened stream
  is hand-written and carries the cp index in `imm`; a future quickener writing site ids into `imm` makes
  cold misses unresolvable — that changes with global site ids.
- **The real quickener will allocate global field offsets, MethodIds, and IC site ids.** Those changes RFC
  through this team and bump this contract (§13). **Quickened streams verify with the same type rules**
  (`docs/rbc_spec.md` §6.2), with the §6.14-15 verifier caveats. **T0 executes both forms and never
  quickens at runtime itself.**

## 8. Inline caches and profiling

Per-call-site inline caches (charter deliverable 4) live in the `Interpreter` (`siteICs_`, indexed
`[MethodId][site]`, lazily sized per method on first miss only — miss-path allocation only, Rule 7). The
site key is **(caller MethodId, call pc)**.

| Site family | `x` caches | `y` caches | Hit path consults |
|---|---|---|---|
| field sites (`getfield`/`putfield`) | resolved slot index | declaring ClassId | `x` only |
| call sites (`invokevirtual`/`invokeinterface`) | target MethodId | observed receiver ClassId | `x` only |

- **Monomorphic v0:** the hit path deliberately does not consult `y` — v0 resolution is site-static; the
  `y` slot is where polymorphic broadcast goes when receiver-sensitive dispatch exists.
- **Miss path:** count `icMisses`, resolve (field: `resolveField`; call: builtin probe first, then
  `resolveMethod`), fill `(x, y)`; the hit path counts `icHits`. Both are per-run `InterpStats`.
- **Builtin call sites are permanent (cheap) misses:** the cache stores RBC MethodIds and `execBuiltin`
  has none to store, so builtin sites never fill the IC — they re-probe every execution and the profile
  still counts them (one miss per execution).
- **Quickened virtual/interface calls** look the IC up at `[MethodId][imm]` (the site id); a cold cache
  resolves through `cp[imm]` (§7).
- **Profiling (Rule 114):** `Runtime::profiles()` exposes per-method `MethodProfile { invocations,
  backedges }`, both **saturating** adds (never overflow UB). Entries bump on method entry; backedges bump
  on taken backward branches (`goto`/`if*`/switch target `< pc`). v0 is single-threaded; the future
  concurrent form is the sanctioned racy-but-bounded one — relaxed atomic increments with bounded
  lost-update error (Rule 114: "thread-safe or explicitly racy-with-bounded-error").
- **Nothing in v0 acts on heat** (tiering arrives with T1; Rule 119 observability is via `stats` and
  `profiles()`). Rule 44's confidence metadata (sample count, stability, age, decay, variance, deopt
  correlation) attaches to profile EXPORT (charter deliverable 9), which does not exist yet; raw v0
  counters carry counts only.

## 9. Safepoints

- **Poll sites (Rule 88):** the `safepoint_poll` opcode (placed on loop backedges by lowering convention),
  every `invoke*`, and every allocation site; OSR entry/exit and tier-transition points join when those
  tiers exist.
- **The poll is a counter + global request-flag check.** The flag is a function-local
  `std::atomic<std::uint8_t>` (no static-init order dependence; lock-free on every supported ABI — no
  locks on the poll path, Rule 118), loaded RELAXED on purpose: v0 is single-threaded and the flag carries
  no data publication. The multi-threaded protocol (release-store + park handshake) lands with the real
  runtime; the relaxed load already gives bounded latency.
- **Bounded latency (Rule 111):** every poll site checks the flag and loops carry `safepoint_poll` on
  backedges by lowering convention, so there is no unbounded instruction sequence without a poll.
- **v0 single-threaded reality:** a set flag makes every poll "park" — which in v0 means finishing the
  current instruction and recording the request; the run completes normally. The flag is observable only
  through `Interpreter::safepointRequested()`/`requestSafepoint()`/`clearSafepointRequest()` (tests pin
  that a set flag never interrupts execution).
- **Counting pin:** `stats.polls` counts explicit `safepoint_poll` retirements; the other Rule 88 poll
  sites (invoke/alloc/backedge) reduce in v0 to the profile bumps they already perform.
  `stats.instructions` counts opcodes at FETCH.
- **`traceSafepoints`** (config, off by default) emits the §10 state dump of the whole frame stack at
  every `safepoint_poll` retirement — the deopt-fixture stream, deterministic (Rule 124).

## 10. The state-dump fixture format

`dumpFrame`/`dumpFrames` (`Frame.h`, `Frame.cpp`) produce the pinned, byte-deterministic (Rule 124) text
format — **the golden format every tier's deopt tests consume**. Grammar (v1 pin; `dumpFrames` emits
frames innermost first, depth 0 = innermost, exactly one blank line between frames, none trailing):

```text
dump      := frame { blank-line frame }
frame     := header local-line reg-line monitors-line
header    := "frame " depth " method=" name descriptor " pc=" pc
local-line:= "  local" { " l" index ":" kind "=" payload }
reg-line  := "  reg"   { " r" index ":" kind "=" payload }
monitors  := "  monitors=" count { "," objid }
```

- **`kind=payload` for EVERY slot** — including empty and null slots, which print `bot=bot` and
  `null=null`. Kind spellings: `int`, `long`, `float`, `double`, `null`, `ref`, and **`bot`** for `Bottom`
  (the dump spelling wins over `typeName`'s "bottom" — a documented spelling divergence between the two
  frozen forms).
- Payloads: Int/Long decimal; Float/Double via `javaFloatString`/`javaDoubleString` (§11); Null the
  literal `null`; Bottom the literal `bot`; Ref `<internal-class-name>@<id>` — the stable object id (Rule
  15) — with String refs appending `" str=<payload>"` (raw UTF-8, no quotes, no escapes; keeps fixtures
  grep-able and byte-stable).
- The local and reg lines are **always emitted**, even for zero-slot frames (the label word alone); slots
  print in index order, space-separated. The method name and descriptor concatenate without a space
  (`method=main()I`); a methodless frame (defensive only) prints the `<none>` placeholder rather than
  crashing.
- `monitors=` prints the count then the object ids **most-recent-first** (the Frame.h LIFO pin; unwinding
  releases in exactly this order).
- Determinism: same state, same bytes — dumping twice is identical, and two runs of a traced program
  produce identical traces (tested).

Worked example — an innermost frame at a loop head under a caller parked AT the invoke pc (never advanced
during the call, §5.1); `System.out` is object id 1 and `System.err` id 2 (§11):

```text
frame 0 method=sum(I)I pc=4
  local l0:int=10 l1:int=30 l2:int=4
  reg r0:bot=bot r1:bot=bot r2:bot=bot r3:bot=bot
  monitors=0

frame 1 method=main()V pc=6
  local l0:ref=java/lang/String@3 str=hi l1:long=-9 l2:null=null
  reg r0:float=1.5 r1:double=0.25 r2:bot=bot
  monitors=1,7
```

Sources of truth: `interp_safepoint_trace_golden` (the trace stream, byte-for-byte, twice),
`interp_dumpframes_pinned_format` (every kind spelling, the String append, most-recent-first monitors, no
trailing blank line), and the corpus `.expected` fixtures (exact stdout).

## 11. The v0 reference runtime boundary

- **The seam.** The dispatch core (`Interp.cpp`) talks only to `Runtime.h`, which fronts the reference
  object model (`Heap.h`) plus resolution, statics, builtins, trap construction, and profiling. When the
  real `runtime/` lands (class loading, GC, JNI, real monitors), it replaces the implementation **behind
  the same seam, not the seam** — the dispatch core does not change (charter: runtime services are "used,
  never modified"). Resolution and string work happen on the miss path only; the hot path consumes integer
  ids (Rules 7, 16).
- **Builtin method table (the v0 native-call seam).** `getstatic java/lang/System out|err :
  Ljava/io/PrintStream;` returns the eagerly allocated singleton PrintStream objects (one per Runtime;
  `putstatic` to them is refused). Virtual `java/io/PrintStream` `println`/`print` are recognized for nine
  descriptors: `()V (I)V (J)V (F)V (D)V (Z)V (C)V (Ljava/lang/String;)V (Ljava/lang/Object;)V`. The sink
  is `stderr` for the `System.err` singleton, `stdout` otherwise. Arity is receiver + at most one value
  argument (mismatches are impossible on verified streams). Formatting is by value tag (§6.6).
- **The 24-class builtin hierarchy** (registration order IS id order — `java/lang/Object` is `ClassId{0}`
  — part of the observable surface):

```text
java/lang/Object
+-- java/lang/String
+-- java/lang/Class
+-- java/io/PrintStream
+-- java/lang/System
+-- java/lang/Throwable                                   (exception)
    +-- java/lang/Exception
        +-- java/lang/RuntimeException
            +-- java/lang/ArithmeticException
            +-- java/lang/NullPointerException
            +-- java/lang/ArrayStoreException
            +-- java/lang/ClassCastException
            +-- java/lang/IllegalMonitorStateException
            +-- java/lang/IndexOutOfBoundsException
            |   +-- java/lang/ArrayIndexOutOfBoundsException
            +-- java/lang/NegativeArraySizeException
            +-- java/lang/UnsupportedOperationException
    +-- java/lang/Error
        +-- java/lang/VirtualMachineError
            +-- java/lang/InternalError
            +-- java/lang/StackOverflowError
        +-- java/lang/LinkageError
            +-- java/lang/NoSuchMethodError
            +-- java/lang/BootstrapMethodError
```

Catch semantics: `isAssignableFrom` walks this chain (plus reflexive exact matches and null-passes-cast);
user classes have no mid-hierarchy supers (§6.9); array classes match by exact descriptor only.
- **Lazy field layout (grow-on-access):** a `(class, name)` pair gets a slot the first time it is resolved
  (idempotently); instances allocated earlier grow to fit on first access. Statics are keyed by `FieldId`
  and default to Bottom ("unset") — coerced to the type's zero on read (§6.13). The real runtime computes
  layouts at class-load time.
- **String interning (JVMS 5.1):** equal string constants are one object per Runtime — `if_acmpeq`
  identity depends on it. `ldc` materializes interned Strings; String contents live out of band in the v0
  heap.
- **Exception-message shapes (16).** Every exception T0 raises, with its pinned message shape (JVM-exact
  unless noted):

| # | Class | Message shape |
|---|---|---|
| 1 | `java/lang/ArithmeticException` | `/ by zero` |
| 2 | `java/lang/NullPointerException` | (empty; helpful NPEs need bytecode context) |
| 3 | `java/lang/ArrayIndexOutOfBoundsException` | `Index N out of bounds for length L` |
| 4 | `java/lang/NegativeArraySizeException` | `N` (the decimal size) |
| 5 | `java/lang/ClassCastException` | `class X cannot be cast to class Y` (dotted) |
| 6 | `java/lang/ArrayStoreException` | `class X` (dotted value class) |
| 7 | `java/lang/IllegalMonitorStateException` | (empty; JVM: no message) |
| 8 | `java/lang/StackOverflowError` | (empty) |
| 9 | `java/lang/NoSuchMethodError` | `C.n(D)` (call-site resolution failure) |
| 10 | `java/lang/NoSuchMethodError` | `<name><descriptor>` (missing `run()` entry) |
| 11 | `java/lang/BootstrapMethodError` | `invokedynamic not supported in v0` |
| 12 | `java/lang/InternalError` | `deopt_trap executed in T0 without deopt metadata (v0)` |
| 13 | `java/lang/InternalError` | `ldc of methodtype/methodhandle not supported in v0` (and `ldc of unsupported constant kind`) |
| 14 | `java/lang/InternalError` | `unresolved field` / `malformed field descriptor` |
| 15 | `java/lang/InternalError` | `store to builtin static` / `builtin arity mismatch` |
| 16 | `java/lang/InternalError` | quickened refusals: `quickened getfield of unwritten field (v0)`, `quickened virtual call with cold inline cache (v0)`, `quickened method id out of range`, `quickened call site id out of range` |

Rows 12-16 are contract refusals (honest `InternalError`s), not JVM semantics. A further family of
defensive `InternalError` messages guards impossible-on-verified-streams invariants
(non-array/non-instance access, corrupt pc, unreachable opcode, multianewarray misuse, entry-argument and
resume-frame validation); their exact wording is a total-behavior guarantee, not a fixture-stable surface.
- **Numeric formatting.** `javaDoubleString`/`javaFloatString` implement the JDK
  `Double.toString`/`Float.toString` algorithm: specials first (`NaN`, `Infinity`, `-Infinity`, signed
  zeros), then the shortest round-tripping decimal, plain notation iff `10^-3 <= |v| < 10^7`, else
  scientific `d.dddE<x>` (one digit before the point, always >= 1 fraction digit, no `+`, no leading
  exponent zeros). Subnormal-extreme caveat: §6.11.

## 12. Known v0 limitations

Consolidated honest list (each is tracked; none is a silent gap):

- **No user class hierarchy** — user classes walk to `Object` only; catch outside the 24-class builtin
  tree is exact-name (§6.9). **No array covariance** — exact-descriptor-only assignability (§6.5).
- **Single-threaded** — monitors are single-owner, the safepoint flag is record-only, ICs and counters are
  unsynchronized (§8, §9). **No GC** — object ids are never reused; `stats.allocations` is the cumulative
  heap object count.
- **No quickener at runtime** — T0 executes quickened streams but never produces them; v0 quickened
  streams are hand-written (§7). **No OSR** — on-stack replacement in either direction does not exist
  (`docs/deopt_backend.md` Part A §16 is design-only).
- **No line tables** — `Method` carries no BCI/line mapping; stack-trace line info needs a side table
  keyed by pc (`docs/rbc_spec.md` §10). **Uninitialized-`this` tracking deferred** (`docs/rbc_spec.md`
  §5.5).
- **`invokedynamic` verifier-only; `ldc` MethodType/MethodHandle refused** (§6.4). **Monitor balancing not
  verified** (`docs/rbc_spec.md` §10.2); release on unbalanced streams is best-effort (§6.10).
- **`execBuiltin` descriptor gap** — formatting is value-tag-based; the v1 signature change (pass the
  descriptor) is planned (§6.6).
- **No `const Heap&` accessor** — `Runtime::heap()` lacks a const overload, so
  `dumpFrame`/`exceptionMessage` bridge via a documented `const_cast` that only calls const members (v1
  fix: add the overload).
- **Frozen-header residuals** (worked around in code, reported for v1): `Heap::classOf`'s dead-ref result
  collides with Object's id; `ClassInfo::superclass` `v==0` is ambiguous between "unset" and "Object" (v0
  pins the Java-correct reading); the field-name index is encoded into the class registry; `findClinit`'s
  "invalid" MethodId is expressed via `needsInit` because `MethodId{0}` is valid; `typeName` says "bottom"
  where the dump pins "bot".
- **Not yet built (charter, later deliverables):** superinstructions (deliverable 6, T1-side; the
  T0-side quickening seam is already executed — §7), profile export with
  Rule 44 confidence metadata (deliverable 9). Computed-goto dispatch (§4) LANDED in v1.1.0.

## 13. Change control

- This contract is versioned (this document is 1.1.0; v1.0.0 was the initial publication, v1.1.0 lands
  the §4 computed-goto dispatch milestone — MSG-20260830-005). **Any change to §1 (the T0 frame), §2
  (the value model), §3 (the deopt entry contract), or §10 (the state-dump fixture format) requires a
  version bump and an ADVISORY to all consuming teams** (`baseline_noir`, `ir`, `passes`, `regalloc`,
  `codegen`, `aot`) under the message system (`docs/teams/messaging.md`, team key `all`): those
  sections define the state every tier must reconstruct and the fixture format every tier's deopt
  tests pin against. §4 (dispatch model) changes additionally require the portable differential test
  to pass unchanged (the two dispatchers must stay observationally identical — the §4 upgrade itself
  is exactly such a change).
- **Quickened-id changes (§7) — global field offsets, MethodIds, or IC site ids from the real quickener —
  go through an RFC to this team** and bump the contract: the interim v0 encodings are not permanent.
- `include/b2/interp/Interp.h` (and `Frame.h`'s dump-format block) are the code twins of this document;
  the two change together in the same message. A header change without a contract bump (or vice versa) is
  a review defect.
- This contract is law-bound: Rules 4, 75, and 96 make T0 state the reconstruction target; Amendment A's
  "linear state mapping to T0" makes it T1's frame model; Amendment B.1 fixes T0's role in the tier model.
  Where any law and this document conflict, `docs/laws.md` wins — and the conflict is a bug in one of
  them, to be raised as a message, not silently resolved.
- Consumers: `baseline_noir` (T0-compatible T1 frames, stencil patch sites), `ir` (RBC consumption,
  FrameState mapping), `passes` (deopt metadata production), `regalloc` (spill-slot state mapping),
  `codegen` (deopt stubs, safepoint sites), `aot` (artifact-embedded deopt metadata). Cross-team consumers
  pin tests against this contract at their own risk until a contract message lands; after one lands, the
  versioned sections above are stable until the next message.


