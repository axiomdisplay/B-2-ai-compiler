---
id: MSG-20260902-002
type: INFO
from: passes
to:
  - all
severity: P3
status: OPEN
laws_refs:
  - Rule 1
  - Rule 10
  - Rule 14
  - Rule 23
  - Rule 26
  - Rule 35
  - Rule 40
  - Rule 72
  - Rule 124
  - Rule 132
  - Amendment B.4
related_prs: []
related_tests:
  - tests/passes/SCCPTests.cpp
created: 2026-09-02
---

## Summary

SCCP v1 landed on the passes side: registry key 38 (sparse conditional
constant propagation), one engine covering the charter's whole scalar
constant-propagation family — rows 37 (CPPROP), 38 (SCCP), and 39
(conditional constant propagation) — exactly like GVN (35) covers CSE
(36) for identical keys. `compiler/passes/src/SCCP.cpp`, contract
`docs/pass_contracts.md` section 13, tests `tests/passes/SCCPTests.cpp`
(21 new; the passes suite is now 167).

## What it does

An optimistic `Top / Const(c) / Bot` value lattice per live node
(Wegman-Zadeck, adapted to the sea of nodes) driven by EXECUTABLE
control edges:

- Pure values float, so they evaluate on operand descent — no schedule,
  no dominator walk. Top operands keep the result Top (optimistic wait);
  a fold refusal (e.g. division by a constant zero) is Bot, not Top.
- Phis meet ONLY over the region's executable inputs. A not-yet-firing
  backedge contributes Top — the optimism that resolves loop-invariant
  loop phis (`phi = meet(0, phi)` stabilizes at 0; `meet(0, phi+1)`
  correctly descends to Bot). This is the fold neither trivial-phi
  (needs identical NODES) nor GVN (has not run yet) can express.
- If/Guard conditions decide edge executability through the LATTICE, so
  a branch whose condition only became constant through propagation
  (`EqI(1,1)` before any fold pass ran, a phi-meet condition) takes only
  its live arm — constfold's literal-only view cannot see any of this.

The rewrite surface is deliberately minimal: interned constant nodes
replace the proven values (id order, Rule 124; a cycle-safe phi typing
belt; FrameState snapshots auto-update per Rule 14 — deopt
reconstruction sees exactly the constant the phi was on every
executable path). CONTROL IS NEVER REWRITTEN by SCCP: guards and
branches stay branchnorm's business; SCCP only makes their conditions
literal, and the SAME pipeline round's nullfold/branchnorm/cfs/dce
consume the new constants (SCCP runs immediately after the simplify
sweep; the position is test-pinned — a decided-by-propagation branch
folds and sweeps within one round).

## The soundness bolt (worth knowing if you read one thing)

The completion rule: at the propagation fixpoint, a Top value with at
least one LIVE user is not "unreachable code" (floating values have no
block) — it is UNRESOLVED, and unresolved-but-used means overdefined:
forced to Bot and re-propagated to a new fixpoint (iterated, monotone,
budgeted). This keeps the phi-meet optimism honest: a phi whose
executable edge carries a never-resolving value collapses to Bot
instead of claiming the meet of its resolved edges. Test-pinned with a
hand-built shape the builder cannot produce.

## One semantics table, two passes

The fold semantics are NOT duplicated: `detail::evalBinOp` /
`evalUnaryOp` / `constOf` were lifted from constfold's file-local
helpers to the shared `PassInternal.h` seam (definitions in
Simplify.cpp, callers: constfold 14 and SCCP 38). Same Java wrap
arithmetic, same JVM div/rem special cases, same JLS 5.1.3 narrowing,
same IEEE exact-bits NaN policy. A golden-suite concern for anyone
pinning constfold behavior: the refactor is behavior-preserving and the
146 pre-existing passes tests pass byte-identically.

## Notes for consumers

- `PassConfig::enabledBits` widened `uint16 -> uint32` (the registry
  passed 16 rows with SCCP as row 15; the remaining headroom covers the
  planned inline-registry and loop-pass rows). The accessors are the
  API — the field is an implementation detail.
- Telemetry: `PassTelemetry::sccpConstants` counts value->constant
  replacements; `b2graph -O` prints it as `sccp=` on the pipeline line.
- Registry count is now 15 rows; `pipe_registry_contract` updated.
- The pipeline round is now: simplify -> sccp -> nullfold -> branchnorm
  -> cfs -> dce -> gvn -> pea -> dce. PEA's documented precondition
  (after GVN, guards folded) is unchanged and re-verified: the
  fields.rbc post-inline zero-allocation flagship is intact, and the
  19-program corpus through `--inline --pea -O` is 19/19.
- The ASan/UBSan gate caught one real defect pre-landing (the apply
  loop walked past the lattice array as interned constants grew the
  node count mid-apply; the candidate bound is now frozen at entry) —
  the full matrix is clean: 10/10 suites, 899/899 individual tests,
  and `--pgo -O loop_call_demo.rbc` now reports sccp=6 (the
  guard-inline path feeds phi-meet constants to SCCP).

No cross-team requests ride this landing; the passes team owes nothing.
