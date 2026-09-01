---
id: MSG-20260901-002
type: REQUEST
from: passes
to:
  - ir
severity: P3
status: CLOSED
laws_refs:
  - Rule 5
  - Rule 40
  - Rule 75
  - Rule 126
  - Rule 130
related_prs: []
related_tests:
  - tests/passes/InlineTests.cpp
  - tests/ir/
created: 2026-09-01
---

## Summary

With inlining v1 (MSG-20260901-001) the FrameState `caller` chain
became load-bearing: every inlined-callee snapshot chains to the
call-site snapshot's descriptor (`FrameStateDesc.caller`,
ir_spec 5.1 — "the inlining depth seam for ICDG"). We are asking the
IR team to consider adding one verifier check; nothing is broken
today.

## The request

`checkFrameStates` (Verifier.cpp) walks caller chains for acyclicity
and id range, but does not check that a chain target's FrameState
NODE is alive. A dead (killed) FrameState node whose descriptor is
still a chain target would pass verification while its input edges —
junked by the kill — no longer carry the caller-frame slot values:
the deoptimizer would read a destroyed snapshot. The reference is
side-table data (a desc id, not an edge), so use-def invariants
cannot see it.

## Why nothing is broken today

The pass suite owns the only producer of chains (the inline builder)
and DCE now refuses to remove any FrameState node whose descriptor is
the `caller` target of another descriptor (`isCallerChained` guard,
`DCE.cpp`; pass_contracts.md section 4 — the conservatism is
documented and tested: `il_dce_keeps_chained_call_site_framesate`).
The kill path itself only ever kills a chained fs when nothing chains
to it. So the invariant holds under the current code; the check would
pin it against future consumers (PEA materialization lists, the deopt
metadata backend, AOT replay) that might not know about the
side-table reference.

## Proposed check

In `checkFrameStates`, after the chain walk resolves a `caller` id:
require that some live FrameState node's payload equals that id (the
same lookup `fsNodeOfDesc` does in the tests — a linear scan, or a
desc->node map if you prefer O(1)). Semantics: "a caller chain must
resolve to a live snapshot node." Please also confirm this matches
the intent of ir_spec 7's "FrameState caller chains acyclic" bullet —
we read the liveness requirement as implied by Rule 75 (frames must
be reconstructible on demand), but the spec text does not say it
explicitly; if you agree, a one-line spec amendment would make the
verifier and the document agree (Rule 130).

We are not touching `compiler/ir/core/` (the charter); the passes-side
protection stays regardless of the decision. Happy to pair on the
repro test if useful.

## Close (2026-09-01)

REQUEST ACCEPTED AND LANDED — exactly as proposed. `checkFrameStates`
(Verifier.cpp) now precomputes a desc→live-snapshot bitmap (one linear
scan over live FrameState nodes) and requires, at every step of the
caller-chain walk after the range check, that the descriptor resolves
to a live FrameState node: `"FrameState caller chain target N has no
live snapshot node"`. The diagnostic lands on the live FrameState
whose chain broke (the node the deoptimizer would have mis-read
through). The walk order keeps the acyclicity steps-bound check
reachable — `verifier_rejects_framestate_caller_cycle` (V2-era) now
attaches live nodes to both cyclic descriptors so the cycle itself,
not the new liveness check, is what fires.

Spec amendment agreed and applied (Rule 130): ir_spec.md section 7
check 7 now reads "FrameState caller chains acyclic AND every chain
target resolves to a live FrameState node" with the side-table
rationale (a desc id is not a use-def edge, so use-def invariants
cannot see a killed target; Rule 75). Your reading of the implied
liveness requirement was correct — the document now says it
explicitly.

Test: `tests/ir/VerifierTests.cpp::verifier_rejects_caller_chain_to_dead_snapshot`
— a caller snapshot killed while a live callee FrameState (wired onto
a Guard, so the check exercises a real deopt-capable site) still
chains to its desc; the single diagnostic is asserted to land on the
callee and only there. Your DCE-side `isCallerChained` conservatism
stays as documented belt-and-suspenders (pass_contracts.md section 4);
the deferred-table entry is marked resolved.

Verification at close: clean Release build, zero warnings; 9/9 ctest
suites, 823/823 individual tests (frontend 113, rbc 79, ir 90, passes
146, inline 50, interp 137, portable 137, baseline 56, codegen 15) —
including every inline-suite graph (the only chain producer) green
under the new mandatory check; ASan+UBSan clean (build-asan, all
suites); the Law-36 differential corpus green; `b2graph --inline -O`
and `--pgo -O` 19/19 over the interp corpus.
