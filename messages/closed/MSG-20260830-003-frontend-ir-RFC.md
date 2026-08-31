---
id: MSG-20260830-003
type: RFC
from: frontend
to:
  - ir
  - interpreter
  - passes
  - baseline_noir
severity: P1
status: CLOSED
laws_refs:
  - Rule 1
  - Rule 47
  - Rule 67
  - Rule 72
  - Rule 74
  - Rule 80
  - Rule 96
  - Rule 124
  - Amendment B
related_prs: []
related_tests:
  - tests/frontend/LowerTests.cpp
  - tests/frontend/LowerCorpus.cpp
  - tests/frontend/lower_corpus/
created: 2026-08-30
---

# RFC: AST -> RBC lowering lands (source path closes source -> execution)

## Summary

The frontend's AST-to-RBC lowering stage has landed (commit to follow this
message): `include/b2/frontend/Lower.h` (`lowerUnit`) + the implementation
under `compiler/frontend/src/Lower*.cpp`, driven by `b2parse --emit-rbc`.
Java source now compiles to the same Register Bytecode the classfile path
will deliver, and `b2run` executes it: **source -> RBC -> T0 is closed**.

This is the contracted RFC the ownership map required ("AST -> RBC lowering
will be contracted via messages before it lands").

## What consumers need to know

1. **RBC consumption is read-only.** The lowering emits exclusively through
   the frozen middle-end contracts: `rbc::RbcBuilder` factories,
   `rbc::Method`/`rbc::Program` shapes, and the canonical text format. No
   middle-end file was touched.

2. **Every emitted method passes `rbc::verify`.** This is a hard invariant
   of the stage (pinned by 67 new tests + the executed corpus). Two
   lowering-side disciplines make it hold by construction:
   - Every local slot is default-initialized in the method prologue to its
     fixed family, so no store inside a protected range ever changes a
     slot's verification type (rbc_spec SS5.3).
   - Ref-like slots are initialized with a Ref-typed constant (`ldc ""`),
     not `aconst_null`: a Null-typed slot whose first real store lands in a
     protected range would change type mid-range.

3. **Safepoint polls are emitted on every loop backedge target**
   (`safepoint_poll` at each loop head). T1's plan builder will now find
   polls where the hand-written corpus had none; the baseline team should
   expect `MissingBackedgePoll` refusals to largely disappear for
   source-lowered programs.

4. **Catch variables are locals, not registers.** Handler entries emit
   `astore r0 lN`; the catch body reads the local. This survives nested
   protected regions (registers die at handler entries; locals do not).

5. **Default `<init>` synthesis + `<clinit>` synthesis** follow JLS 8.8.9 /
   12.4.2: static initializers and static blocks in declaration order;
   instance field initializers prepended to every constructor (skipped when
   the first statement is `this(...)`; `super()` is a v0 no-op).

## Documented v0 refusals (diagnostics, never silent miscompiles)

Lambdas / method references / switch expressions / patterns (invokedynamic
RFC), string concatenation (runtime builtins RFC), enums / interfaces /
records / nested / local / anonymous classes (class model), try-with-
resources, assert, varargs call sites, multi-dimensional array creation,
String/Throwable method calls, boxed arithmetic (binding stage),
cross-class statics and construction (loader).

## Documented divergences (until the named stage lands)

- No definite-assignment analysis: reading an unassigned local yields the
  prologue default (0 / 0.0 / "" — the Ref default is the interned empty
  string, never `null`). Lands with the binding stage.
- Missing return statements synthesize a default return (javac rejects).
- Reference-subtype assignments are unchecked (no hierarchy knowledge).
- Output parity follows the interpreter's frozen println pins (Z prints
  0/1, C prints code points) — the lowering emits the correct `(Z)V` /
  `(C)V` descriptors.

## Integration decision

Landed by the integrator as frontend-team work; consumers reviewed the RBC
shapes above. Closes with this commit.
