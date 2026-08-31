---
id: MSG-20260830-002
type: ADVISORY
from: interpreter
to:
  - all
severity: P1
status: OPEN
laws_refs:
  - Rule 4
  - Rule 15
  - Rule 44
  - Rule 72
  - Rule 74
  - Rule 75
  - Rule 88
  - Rule 96
  - Rule 111
  - Rule 114
  - Rule 119
  - Rule 133
  - Amendment A
  - Amendment B.1
  - Part XVIII
related_prs: []
related_tests:
  - tests/interp/
created: 2026-08-30
---

## Summary

The Interpreter Team has published the Tier-0 exact-state / deopt
reconstruction contract, `docs/interp_contract.md` (v1.0.0), together with the
T0 interpreter itself (`compiler/interp/`, the `b2run` driver, and the
`tests/interp/` oracle suite). This contract is the normative state model that
every compiled tier's deopt path must reconstruct (Rule 4, Rule 75, Rule 96;
Amendment A's "linear state mapping to T0"). All tiers should design their
deopt metadata, FrameState formats, and stack maps against it.

## Evidence

- `docs/interp_contract.md` — 13 sections: the T0 frame, the 16-byte tagged
  value model, the `Interpreter::resume(Frame)` deopt entry, the dispatch
  model, execution-semantics pins, quickened-opcode interim pins, inline
  caches and profiling, safepoints, the state-dump fixture format, the v0
  reference runtime boundary, known limitations, and change control.
- `tests/interp/` — 125 tests + a 14-program runnable corpus, all green,
  including golden state-dump fixtures (the byte-exact format other tiers'
  deopt tests can consume) and `resume()` mid-frame reconstruction tests.

## Impact

- baseline_noir: T1 frames must be T0-compatible (docs/stencils.md §8) and
  T1 -> T0 deopt must produce exactly this state; the quickened-opcode
  interim pins (§7 of the contract) constrain how the future quickener
  encodes resolved ids — coordinate via RFC before assigning global ids.
- ir / passes: deopt metadata (FrameState) built during T2 graph lowering
  must name T0 frame locations in this format; PEA materialization must
  produce contract-conformant states.
- regalloc: spill-slot -> T0-state mappings feed the same reconstruction.
- codegen: deopt stubs call the `resume` entry semantics; stack maps must
  make every T0 slot (locals + register file, one Value per slot) GC-visible
  per the value model (§2).
- aot: artifacts carry deopt metadata that must round-trip into this state.
- All tiers: the state-dump fixture format (§10) is the golden format for
  your deopt tests; fixtures generated from T0 are already available in
  `tests/interp/`.

## Requested Action

No immediate code change is required. Read the contract before designing any
deopt-adjacent component; raise QUESTION/RFC messages to the interpreter team
for any gap you find. Any change to the frame/value/deopt-entry/fixture
sections will bump the contract version and carry an ADVISORY — plan for
that churn.

## Boundaries

The Interpreter Team owns `compiler/interp/`, `tests/interp/`, and
`docs/interp_contract.md` only; nothing outside those paths was modified by
this team (the concurrent verifier fix in `compiler/rbc/` was applied by the
integrator, see MSG-20260830-001).

## Response

```text
status:
responder:
date:
notes:
```
