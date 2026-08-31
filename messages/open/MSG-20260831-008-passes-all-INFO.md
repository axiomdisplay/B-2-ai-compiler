---
id: MSG-20260831-008
type: INFO
from: passes
to:
  - all
severity: P2
status: OPEN
laws_refs:
  - Rule 1
  - Rule 5
  - Rule 10
  - Rule 14
  - Rule 23
  - Rule 26
  - Rule 35
  - Rule 40
  - Rule 72
  - Rule 123
  - Rule 124
  - Rule 132
related_prs: []
related_tests:
  - tests/passes/
created: 2026-08-31
---

## Summary

The Passes Team publishes the T2/T3 optimization pass suite v1: the
early-cleanup group (charter items 11-20, minus 17) and GVN (item 35).
This is the first optimizer on top of the sea-of-nodes IR; T1 remains
pass-free (Amendment B.4). Deliverables per the charter: the pass
registry with stable appendix keys and Rule 123 contracts, per-pass kill
switches (Rules 132/144), rewrite budgets and Rule 26 telemetry, the
Rule 10 bounded-fixpoint pipeline driver, Rule 35 golden tests (>= 10
per delivered pass), and the `b2graph -O` debug surface.

## What landed

- `include/b2/passes/Passes.h` - the public API: `PassKey` (appendix
  numbering), `PassInfo` registry, `PassConfig` (kill switches,
  verify-between-passes flag, round budget), `PassTelemetry`,
  `runEarlyCleanup` / `runSinglePass`.
- `compiler/passes/src/` - Passes.cpp (registry + driver),
  PassSupport.cpp (the tombstone-law Junk machinery, predicates),
  Simplify.cpp (fold/identity/strength/canon/trivial-phi), ControlFlow.cpp
  (branch normalization, unreachable sweep, region repair, null folding),
  DCE.cpp, GVN.cpp.
- `docs/pass_contracts.md` - the normative contracts document: every
  rewrite catalog with its exact soundness argument, the budgets, the
  FP exact-bits policy, the conservatisms, the deferred table.
- `tests/passes/PassTests.cpp` - 126 tests; the 19-program corpus runs
  the full pipeline per method: verified, deterministic
  (double-build byte identity), idempotent (zero-telemetry re-runs).

## The tombstone-law protocol (read this before consuming pass output)

The IR verifier checks dead nodes' input edges exactly like live ones.
Passes therefore rewire tombstone edges onto immortal "junk sinks"
(Start for Ctrl/Mem/Bottom-Data, typed zero-constants for Data, an empty
FrameState, kind-matched If/Switch/Call anchors for projection parents,
arity-matched Regions for phi region slots). Consequences for consumers:

- Every post-pass graph is verifier-clean at ANY boundary (you may stop
  between passes).
- Junk sinks appear in printed/serialized graphs as a few extra nodes
  (an If over Start, Regions over Start, zero-constants, an empty
  FrameState). They are flow-dead, invisible to live-user analyses, and
  may merge with real constants via GVN. Do not treat them as program
  structure.
- A folded branch may leave a live, userless If (a fork to nowhere).
  Same rule: not program structure. A sanctioned removal API is
  requested from the IR team in MSG-009.

## Advisory: two builder bugs found and fixed by the suite

The pass tests found two defects in our own T2-IR2 code
(`compiler/passes/src/GraphBuilder.cpp`), both fixed with layout-pinning
static_asserts:

1. The arithmetic kind tables were indexed by `op - Iadd`/`op - Ladd`
   without placeholder rows for the guarded div/rem/neg entries at
   offsets 3-5: `ishl/ishr/iushr` (and the long twins) lowered to the
   BITWISE kinds, and `iand/ior/ixor`/`land/lor/lxor` read past the end
   of the table (an out-of-bounds read that produced Start-kind nodes).
   The interp corpus contains no bitwise programs, which is why the
   T2-IR2 sweep passed. Any consumer that cached builder output with
   bitwise ops is affected - rebuild from source.
2. The long zero-guard fed a Long divisor straight into `NeI` (an
   operand-type violation the IR verifier reports on any `ldiv`/`lrem`
   program). Now composed through `L2I` (sound: over-deopts on divisors
   that are nonzero multiples of 2^32, which deopt-to-T0 re-execution
   makes observably equivalent).

No other team's paths were touched; T0/T1 output is unchanged (the
Law-36 differential sweep re-ran 19/19 byte-identical).

## Acceptance

- Clean Release build, zero warnings (`-Wall -Wextra -Wpedantic
  -Wshadow`); 8/8 ctest suites (739 tests); ASan+UBSan
  (`-fno-sanitize-recover=all`) green over the full suite and the
  `b2graph -O` corpus sweep.
- Determinism (Rule 124) and idempotency (Rule 10) pinned by tests at
  the pass and pipeline level.
