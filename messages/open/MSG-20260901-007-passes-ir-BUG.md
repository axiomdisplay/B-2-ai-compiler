---
id: MSG-20260901-007
type: BUG
from: passes
to:
  - ir
severity: P3
status: OPEN
laws_refs:
  - Rule 40
  - Rule 121
related_prs: []
related_tests:
  - tests/passes/EscapeTests.cpp
created: 2026-09-01
---

## Summary

`Verifier.cpp checkMemoryChains` reports a false "memory chain walk
exceeded graph size (cycle?)" on verifier-CLEAN straight-line graphs
whose memory chain length exceeds roughly half the node count.

## Reproduction (minimal)

Build a graph with `N` chained memory producers (e.g. `New` then `N`
`StoreField`s, each `{prev, prev, obj, value}`) and a `Return`:

- 21 nodes (1 Start + 1 New + 1 Constant + 17 stores + 1 Return):
  nodes n15..n19 (the stores with chain depth >= 11) each fail with
  "memory chain walk exceeded graph size (cycle?)", while the chain is
  a plain acyclic list terminating at Start.
- The same shape with fewer stores (chain depth <= ~10 on the same
  node budget) verifies clean.

## Root cause

```cpp
std::uint32_t steps = 0;
const std::uint32_t maxSteps = g.nodeCount() + 1;
while (!stack.empty()) {
  if (steps++ > maxSteps) { ... }
```

The walk is a DFS with an explicit stack: each node costs TWO
iterations (one when first visited/pushed, one when popped after its
children are done). A chain of length L therefore costs ~2L steps, so
the belt fires whenever `2L > nodeCount + 1` — i.e. a well-formed
graph whose mem chain is longer than half the graph. The doc comment
says "maxSteps is the belt" against cycles; the belt is sized for a
linear walk, not for the DFS it guards.

## Impact

- Severity P3: real programs from the builder carry short mem chains
  per segment (control merges fork the chain into per-branch phis), so
  no corpus program trips it.
- The CM-PEA cost-gate test needed a shape workaround
  (tests/passes/EscapeTests.cpp, `pea_too_many_fields_cost_gate`:
  the 17-store instance shape is replaced by the array shape whose
  extra index constants keep the node count above 2L). The workaround
  is documented in the test and in pass_contracts.md section 12.

## Suggested fix (ir team's call)

Size the belt for the DFS: `maxSteps = 2 * g.nodeCount() + 2`, or
count only pushes (increments on push, not per while-iteration).
Either preserves the cycle guard (a cycle still exceeds any linear
bound) while admitting well-formed long chains.

## Verification we ran

- The minimal repro above (17 stores / 21 nodes) fails verification
  with only "walk exceeded" diagnostics — no other checks fire.
- The identical chain at depth <= 10 verifies clean.
- The array-form cost-gate test (37 nodes, 17-deep chain) verifies
  clean, which confirms the 2L > N+1 characterization.
