---
id: MSG-20260901-005
type: INFO
from: passes
to:
  - all
severity: P3
status: OPEN
laws_refs:
  - Rule 3
  - Rule 42
  - Rule 44
  - Rule 122
  - Rule 124
related_prs: []
related_tests:
  - tests/passes/InlineTests.cpp
created: 2026-09-01
---

## Summary

ICDG Phase 2 landed on the T2 side (the documented next step after
Phase 1, MSG-20260901-003): the **dispatch-profile ingestion +
GuardInline** - profile-driven devirtualized inlining of monomorphic
virtual/interface call sites. Contract: `docs/inlining.md` section 9
(the document is now the v2 review artifact). `docs/icdg.md` Phase 2
carries the LANDED note.

## What landed

- `passes::DispatchProfile` (`include/b2/passes/Inline.h`): the T0
  dispatch-profile snapshot - the raw per-site histograms keyed
  `(caller MethodId, call pc)` (the same key as the site ICs), with
  receiver CLASS IDS resolved to internal NAMES (the name bridges the
  runtime ClassId space and the resolver's ir TypeId space). The
  snapshot is a compilation input consumed read-only; the
  classification policy lives with the consumer (interp_contract.md
  SS8.1's boundary, unchanged - **no interp code changed**).
- The shared converter `compiler/passes/tools/DispatchProfileSnapshot.h`
  (b2graph and the tests are the interp-linking integrators; the
  `b2_passes` LIBRARY stays interp-free).
- `b2graph --pgo` (implies `--inline`): T0 training run -> snapshot ->
  engine; decision lines carry the profile numbers
  (`profile=recv/site`).
- The engine: monomorphic `CallVirtual`/`CallInterface` sites inline
  behind a `TypeProfile` guard (`InstanceOf(recv, profiledClass)`
  condition; deopt at the call pc re-executes the invoke - the guard
  runs before any body effect), with complete Rule 122 SpecMeta
  (TypeMonomorphic / PGO / confidence / cost) and a `ClassHierarchy`
  invalidation dependency (Rule 42 - the C2 shape). Bimorphic /
  polymorphic / megamorphic / low-confidence / Object-profiled sites
  refuse with structured reasons (the full refusal catalog is
  `docs/inlining.md` section 7).

## The zero-regression property (for every consumer)

With NO profile attached (`InlineConfig::profile == nullptr`, the
default), virtual/interface sites are not even considered: graphs,
telemetry, decision logs, and serialized bytes are identical to v1.
With a profile attached, every refusal still changes nothing in the
graph. Pinned by tests over all 19 corpus programs.

## Notes per team

- **ir**: no core changes and none requested for this landing - the
  existing SpecMeta/Dependency/Guard kinds were consumed exactly as
  specified (the verifier's Rule 122 completeness checks pass by
  construction). Separately filed: MSG-20260901-004 (BUG, P1) - the
  memory-chain walk's false positive on any memory producer inside a
  loop; it blocks call-in-loop graphs, the most common real-world
  inline shape, and is the highest-value verifier fix right now.
- **baseline_noir**: the T1 stencil dispatch selection can now consume
  the same snapshot shape (`DispatchProfile` + the converter header)
  for its icdg.md Phase 2 "T1 stencil dispatch" work whenever you
  start it - the site key and the histogram semantics are pinned by
  tests on both sides of the boundary.
- **aot**: the offline driver will want to persist this snapshot
  format (Phase 4's "persistent dispatch profiles"); the type is
  plain data, no interp dependency needed to read a persisted form.
- **codegen**: when GuardInline'd graphs reach lowering, the
  TypeProfile guard lowers like any other guard (deopt through the
  standard machinery); the SpecMeta side table is where deopt
  metadata/dependency records will be read from.

## Verification

9/9 ctest suites (812/812 individual tests; the inline suite is now
49); Law-36 differential sweep 19/19 (T0/T1 untouched); `b2graph
--pgo --inline -O` over all 19 corpus programs; the end-to-end demo
(T0 training run -> guard-inline at depth 2 with profile numbers in
the decision log); ASan+UBSan clean over everything.
