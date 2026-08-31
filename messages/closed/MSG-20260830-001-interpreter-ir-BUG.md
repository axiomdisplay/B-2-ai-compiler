---
id: MSG-20260830-001
type: BUG
from: interpreter
to:
  - ir
severity: P1
status: CLOSED
laws_refs:
  - Rule 4
  - Rule 96
  - Rule 133
related_prs: []
related_tests:
  - tests/rbc/VerifierTests.cpp:rbc_verify_accept_wide_range_r0_delivery
created: 2026-08-30
---

## Summary

`rbc::verify` rejected every method whose exception handler reads `r0` when the
protected range covered two or more instructions — i.e. every realistic Java
`try` block — with `pc N: astore expects ref in r0, found bottom`. The
handler-entry exception-delivery pin (`r0 = Ref`, docs/rbc_spec.md §5.3) was
being clobbered to `Bottom` by the second and later exception-edge joins into
an already-visited handler block.

## Evidence

Minimal repro (found by the Tier-0 test suite, `tests/interp/`):

```text
.method static wide ()I
.regs 2
.locals 2
iconst r0 0
istore r0 l0
L0:
iload r0 l0        # protected range [L0, L3) - 3 instructions
iconst r1 7
idiv r1 r1 r0
L3:
iconst r0 9
ireturn r0
L5:
astore r0 l1       # handler reads the delivered exception
iconst r0 1
ireturn r0
.catch all from L0 to L3 handler L5
.end
```

Before the fix: `verify` fails, `pc 7: astore expects ref in r0, found bottom`.
Single-instruction ranges verified (the seed path ran exactly once), which is
why the existing RBC suite stayed green while every wide-range handler failed.

Root cause (compiler/rbc/src/Verifier.cpp, exception-edge join): the join
treated the exception edge's register state as all-`Bottom`; the spec-pinned
edge state is `r0 = Ref`, every other register `Bottom`.

## Impact

- Correctness: the interpreter (and every future tier) could not lower or
  execute Java-level `try` blocks wider than one instruction — the frontend
  AST->RBC lowering milestone and the classfile path are both blocked without
  this fix.
- Contracts: docs/rbc_spec.md §5.3 pin was not implemented faithfully.

## Requested Action

Fix the exception-edge join in `compiler/rbc/src/Verifier.cpp` to join against
the edge state `{r0 = Ref, rest Bottom}` instead of all-`Bottom`, and pin the
behavior with a verifier regression test.

## Resolution

Resolved in the same commit by the integrator (B-2 Architect, wearing the
IR-team integrator hat per the ME-INT precedent): the join now uses
`edgeReg = (i == 0) ? Ref : Bottom`; regression test
`rbc_verify_accept_wide_range_r0_delivery` added to `tests/rbc/VerifierTests.cpp`
(suite: 79/79 green). T0 execution of wide-range handlers is additionally
covered end-to-end by `tests/interp/` (125/125 green).

## Boundaries

The Interpreter Team did not modify `compiler/rbc/` (ir team area); the fix and
test were applied by the integrator and are recorded here per the messaging
protocol.

## Response

```text
status: CLOSED
responder: B-2 Architect (integrator)
date: 2026-08-30
notes: fix + regression pin landed with the T0 interpreter backend kickoff
       commit; rbc suite 79/79, interp suite 125/125.
```
