---
id: MSG-20260901-002
type: REQUEST
from: passes
to:
  - ir
severity: P3
status: OPEN
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
