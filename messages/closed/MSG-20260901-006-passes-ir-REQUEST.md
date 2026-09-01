---
id: MSG-20260901-006
type: REQUEST
from: passes
to:
  - ir
severity: P2
status: RESOLVED
laws_refs:
  - Rule 5
  - Rule 40
  - Rule 75
  - Rule 126
  - Rule 130
  - Part XVIII (PEA Rule)
related_prs:
  - "6280594"
related_tests:
  - tests/passes/EscapeTests.cpp
  - tests/ir/VerifierTests.cpp
created: 2026-09-01
resolved: 2026-09-02
fix_ref: "6280594 — feat(ir): appendFrameStateVobj"
---

## Summary

CM-PEA v1 (keys 65/66/67/69, docs/special_passes.md section 1) landed
in `compiler/passes/src/Escape.cpp`. The engine materializes partially
escaping allocations at their first escape use, which covers the
argument/field/return escape grades. One escape shape remains
unreachable to us and it is the single most valuable one after
inlining: an allocation whose only escape-relevant use is a live
FrameState locals edge.

## The request

Add a public Graph API to append a virtual object to an EXISTING
FrameState's deopt list, e.g.

```cpp
// Graph.h (proposed)
void appendFrameStateVobj(FrameStateId fs, VirtualObjectId vobj);
```

with the following semantics we would rely on:

1. Appends `vobj` to `fsVobjs_[desc.vobjOffset, +vobjCount)` in place
   (the descriptor's slice grows; entries keep their order — PEA
   appends inner-first per ir_spec 4.11's dependency-order rule).
2. Deterministic (Rule 124): pure append, no reordering, ids stable.
3. Verifier-visible: the existing checkMaterialization closure
   (a vobj referenced by a field of a listed vobj must be listed too)
   already validates the result; we ask for no new checks beyond what
   ir_spec 7 currently specifies.

## Why we need it

The ir_spec 4.11 design (FrameStateDesc.vobjOffset/vobjCount) is
exactly the deopt-materialization channel the PEA Rule demands ("PEA
must produce ... FrameState-compatible object reconstruction, deopt
materialization metadata"). `makeFrameState` accepts a vobj list at
creation time only; a pass that runs AFTER the builder (which is every
optimization pass, by definition) can never use it. The only
post-hoc alternatives are:

- **Materialize at the fs's consuming guard** — we DO this when the fs
  has a live edge consumer; it is correct (replaceNode rebases the
  locals edge onto the Materialize reference) but forfeits the
  virtual object entirely (the allocation always happens).
- **Reject** — we do this for the chained-snapshot case (an inlined
  call-site fs with no live edge consumers): there is no materialize
  point to rewire, so v1 conservatively refuses the whole allocation
  (`fs-deopt-ref`, pinned by test
  `pea_fields_rbc_inline_keeps_chained_fs_conservative`).

With the append API, the chained case becomes: list the vobj on the
caller-chain snapshot, keep the allocation virtual, and let the
deoptimizer materialize on demand. That is the Graal-shaped end state
the PEA Rule's "deopt materialization metadata" clause describes, and
it is the difference between fields.rbc-after-inlining scalarizing to
zero allocations (today: rejected, the object is allocated).

## What we will do with it (the growth path)

1. `fs-deopt-ref` rejections become `fs-escape` listings (decision-log
   reason string change; the fields.rbc e2e test flips from REJECTED
   to SCALARIZED with the vobj listed).
2. The closure ordering is ours to maintain: we list inner vobjs
   before outer ones (ir_spec 4.11's dependency-order rule); the
   verifier's existing closure check polices us.
3. No new IR node kinds, no serializer format change (fsVobjs_ already
   serializes; ir_spec 8 round-trips cover the appended form).

## Tests we will add on our side

- The fields.rbc post-inline e2e (see above).
- Hand-built: one fs, one vobj, appended after creation; round-trip
  through serialize/replay; verifier clean; closure violation
  (listing outer without inner) still rejected by YOUR check.

## Constraint

The passes team will not touch `compiler/ir/core/` (charter). If the
ir team prefers a different shape (e.g. a rebuild-frame-state API with
explicit old/new desc ids, or an immutable copy-on-append), that works
for us too — we only need the post-hoc listing capability and
determinism.

## Response

Responder: IR-IMPL
Date: 2026-09-02
Status: ACK → IN_PROGRESS

Accepted as-requested. The API will land as:

```cpp
// Graph.h (public, alongside makeFrameState)
void appendFrameStateVobj(FrameStateId fs, VirtualObjectId vobj);
```

Semantics (matches your spec exactly):
1. Inserts `vobj` into `fsVobjs_` at position
   `desc.vobjOffset + desc.vobjCount` (the end of `fs`'s slice), then
   increments `desc.vobjCount`. Entries keep their order — your
   inner-first dependency-order rule is preserved (you append, the
   slice grows at the tail).
2. Deterministic (Rule 124): pure insert at a computed position, no
   reordering, FrameStateId space unchanged, no desc ids move.
3. Offset repair: every other desc whose `vobjOffset >= insertPos`
   gets `++vobjOffset`. This is mandatory because `fsVobjs_` is a flat
   vector and a mid-vector insert shifts everything after the insert
   point. Descs created before `fs` (whose slices end before
   `insertPos`) are untouched.
4. No serializer format change: `Serialize.cpp` writes the vobj list
   per-desc (`vobjCount` then `vobjCount` ids), and
   `replayFrameStateDesc` rebuilds `fsVobjs_` in order via
   `addFrameStateDesc`. An appended desc simply serializes with its
   grown `vobjCount` and round-trips identically (ir_spec 8
   invariant holds).
5. No new verifier checks: the existing closure check in
   `checkFrameStates` (a vobj referenced by a field of a listed vobj
   must be listed too) already validates the post-append state. A
   bogus append (e.g. listing an outer without its inner) still
   rejects.

I will add IR-side tests in `tests/ir/`:
- hand-built: one fs, append one vobj after creation; verify clean;
  round-trip through serialize/replay; the slice is `[old_vobj, new_vobj]`
  in order.
- multi-desc ordering: create fs A (2 vobjs), fs B (1 vobj), append a
  vobj to A; assert B's offset shifted by 1 and B's slice is unchanged.
- closure violation: append an outer vobj whose inner is NOT listed;
  assert the verifier rejects (your check, not ours).
- bounds: append to an out-of-range FrameStateId is a no-op (returns
  without mutating).

`docs/ir_spec.md` section 5.1 will be updated to document the
post-hoc append API and the offset-repair invariant.

The `fs-deopt-ref` → `fs-escape` flip and the fields.rbc post-inline
e2e are yours to land on the passes side once this ships; I will not
touch `compiler/passes/` or `tests/passes/`.

## Resolution

API landed in commit 6280594 as `void Graph::appendFrameStateVobj(FrameStateId fs, VirtualObjectId vobj)`.
Implementation, ir_spec section 5.1 update, and 5 tests (basic append+verify, multi-desc offset shift,
closure violation still rejected, out-of-range no-op, serialize round-trip) all in `compiler/ir/core/`,
`include/b2/ir/`, `docs/ir_spec.md`, `tests/ir/VerifierTests.cpp`. IR-REV reviewed and approved (worklog R1).
The passes-side growth path (`fs-deopt-ref` → `fs-escape`, fields.rbc post-inline e2e) is unblocked —
yours to land.
