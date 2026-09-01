id: MSG-20260902-001
type: INFO
from: passes
to:
  - all
severity: P3
status: OPEN
laws_refs:
  - Rule 14
  - Rule 26
  - Rule 35
  - Rule 45
  - Rule 124
  - Rule 126
related_prs: []
related_tests:
  - tests/passes/EscapeTests.cpp
created: 2026-09-02
---

## Summary

CM-PEA v1.1 landed on the passes side: the **fs-escape listing** - the
consumption of the ir team's `appendFrameStateVobj` (MSG-20260901-006,
landed in 6280594) and the DFS step-belt fix (MSG-20260901-007, landed
in 5582043). The `fs-deopt-ref` rejection is gone: a NoEscape allocation
referenced by deopt snapshots now SCALARIZES with per-instant
VirtualObjectStates listed on the referencing FrameStates. Contract:
`docs/pass_contracts.md` section 12 (the fs-escape listing block);
`docs/special_passes.md` section 1 status block updated to v1.1.

## What landed

- `compiler/passes/src/Escape.cpp` (the engine, keys 65/66/67/69): the
  NoEscape + FrameState-references case executes the fs-escape listing
  instead of rejecting. Every referencing fs resolves ONE observation
  instant (a live fs: the call's mem input or the guard's ctrl
  predecessor state; every consumer must agree); a
  `VirtualObjectState` carries the field state visible there (a
  backward chain lookup - NOT id order, because inlined-body stores
  carry higher ids than the call-site fss they follow in program
  order); the fs's slot edges rebase onto the vobj and the desc lists
  it via `appendFrameStateVobj` (the deopt-materialization channel).
- **IDENTITY is the soundness core**: every fs of the allocation inside
  one live deopt chain is reconstructed at the same instant - the same
  object in several frames - so they share ONE vobj (union-find over
  the fss co-occurring in a live caller chain, with the
  serving-deopt-points agreement check; a disagreement rejects with
  the new `fs-multi-deopt` reason). Deopt-UNREACHABLE userless
  snapshots (the post-inline call-site fss after nullfold+DCE) are
  dead metadata: they share one final-state vobj (never reconstructed,
  any deterministic sound state). Unknown fs consumers reject
  (`fs-unknown-consumer`); unresolvable states reject
  (`snapshot-merge`).
- The MSG-20260901-007 consumption is test-side: the instance-field
  cost-gate test is restored to its real shape (17 chained stores,
  `too-many-fields`) - the v1 array-shape workaround is retired and
  the array shape stays as the companion gate (`too-many-elems`).
- A latent passes-side hole fixed on the way: `memStateBefore` now
  walks branch projections through their PARENT slot (a guard inside a
  branch previously resolved the pre-branch state as Start - the
  initial state - which would have produced default-valued snapshots;
  the materialization fallback path had the same exposure).

## The end state this reaches

The fields.rbc post-inline e2e (`main { a = new Main(); a.bump(); a.bump();
a.bump(); println(a.count) }` with all three bump() sites inlined)
scalarizes to **ZERO allocations**: the inlined bodies' loads forward
through the spliced virtual stores, and the call-site snapshots list
final-state vobjs instead of forcing the allocation to stay real. That
is the inline + PEA combination the ICDG engine and the Part XVIII PEA
Rule were designed to reach; the deoptimizer (the future T2 codegen
team's path) materializes the listed vobjs on demand at deopt.

## Boundary notes

- **No interp, ir, baseline, or codegen code changed** - the engine is
  passes-local; the ir API is consumed read-mostly (the append mutates
  only the passes-built fs descs' vobj lists).
- The vobj field-mapping convention matches the existing materialization
  path exactly (fieldIds-sorted subset, values parallel, dense arrays
  with defaults, unlisted slots default at materialization) - when the
  T2 deopt materializer is specified, both PEA paths feed it the same
  shape. The per-fs vobj ordering on a desc is visit order (fs id
  order; Rule 124).
- New refusal reasons in the decision log: `fs-multi-deopt`,
  `fs-unknown-consumer` (the catalog in pass_contracts.md 12).

## Verification

- 9 new tests in `tests/passes/EscapeTests.cpp` (49 total): per-instant
  values, instant-disagreement refusal, the chained identity-sharing
  vobj, the array snapshot, unknown-consumer refusal, the kill switch,
  analysis-only, idempotency+determinism, and the elems cost-gate
  companion.
- Full matrix 10/10 suites, 878 individual tests; ASan+UBSan clean over
  the suites and the corpus; `b2graph --inline --pea -O` over the
  19-program corpus 19/19; the fields.rbc standalone `--pea` decision
  log now reads `scalarized (fs-escape; ...)` where it read
  `rejected (fs-deopt-ref)` before the pipeline's nullfold even runs.
- Law-36 differential semantics unchanged (PEA is a pure T2 pass).
