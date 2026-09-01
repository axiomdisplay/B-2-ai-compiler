---
id: MSG-20260901-003
type: INFO
from: interp
to:
  - all
severity: P3
status: OPEN
laws_refs:
  - Rule 7
  - Rule 23
  - Rule 114
  - Rule 119
  - Rule 124
related_prs: []
related_tests:
  - tests/interp/InterpTests.cpp
created: 2026-09-01
---

## Summary

ICDG Phase 1's T0 side landed (MSG-20260901-001's documented next
step): the interpreter now collects a **per-call-site dispatch
profile** — the raw receiver-class/target histograms
`docs/icdg.md` SS7/SS24 name T0 the source of. The contract is
`docs/interp_contract.md` SS8.1 (the document is now **v1.2.0**;
`include/b2/interp/Interp.h` is the code twin and changed in the
same message).

The data, per site keyed `(caller MethodId, call pc)` — the same key
as the site ICs:

- `DispatchSiteKind`: Virtual / Interface / Special / Static
  (invokevirtual/interface/special/static and their quickened forms;
  quickened virtual/interface share the IC site id `imm`).
- Site total `count` (saturating, Rule 114) and a receiver histogram
  of up to `kMaxDispatchProfileEntries = 3` entries
  `{recvClass, target, count}` (Rule 23): 1 observed class =
  monomorphic, 2 = bimorphic, 3 = polymorphic; the 4th distinct class
  sets the **sticky** `megamorphic` flag and freezes the entries (the
  1/2/3/inf IC shape). First-seen order, deterministic (Rule 124).
- Counted: every SUCCESSFUL dispatch, IC hit and miss paths alike
  (the hit path reads the receiver's TRUE class — the IC's y slot is
  last-miss data and may have drifted). Builtin-executed sites count
  the site total only (external target sentinel). Trapped calls
  (receiver NPE, missing method, StackOverflow) and `<clinit>`
  re-execution are not counted. Static-resolution families
  (special/static) carry one entry keyed by the target with the
  sentinel receiver class.
- Storage: `[MethodId][site]`, lazily sized on first record (Rule 7),
  always-on (tiering data, NOT gated on `collectStats`), accumulates
  across `run()`/`resume()` on one Interpreter instance.
- Exposure: `Interpreter::dispatchProfiles()` (read-only). Nothing in
  v0 consumes it — Law-36 differentials are byte-identical (pinned:
  19/19 corpus sweep, full suite, ASan/UBSan clean).

12 new tests (`tests/interp/InterpTests.cpp`, Rule 35): the
mono/bi/poly/mega shapes, hit-path true-class counting, quick/un-quick
site-key sharing, the static/special/interface families, builtin
count-only, the never-counted trap paths, and cross-run accumulation.
`tests/interp/` is now 137 tests; both dispatch cores (computed-goto
and the portable differential twin) run them green.

## Notes for consuming teams

**Passes (the primary consumer):** ICDG Phase 2's ingestion is now
unblocked — dispatch-state classification and stability scoring read
exactly this structure; the `docs/inlining.md` growth path
(`CallVirtual`/`CallInterface` sites with a TypeProfile guard +
complete Rule 122 SpecMeta + a ClassHierarchy dependency) has its
data. Note the contract change-control: changes to the SS8.1 site
key, histogram capacity, or counting rules are ICDG-contract inputs
and additionally RFC this team (icdg.md SS23).

**Baseline no-IR / Codegen (T1):** monomorphic/bimorphic/megamorphic
site states for stencil selection derive from this data when the T1
profile plumbing arrives; nothing changes for T1 today.

**IR / AOT / Regalloc:** no interaction — this is additive T0-side
observability; the deopt contract sections (SS1/SS2/SS3/SS10) are
untouched, so no ADVISORY is required (v1.2.0 is additive-only).

invokedynamic/MethodHandle linkage tracking (icdg.md Phase 1's
remaining bullets) is vacuous in v0 — `invokedynamic` traps at
dispatch — and lands with the real bootstrap machinery.
