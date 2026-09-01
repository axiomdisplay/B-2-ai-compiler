---
id: MSG-20260901-006
type: REQUEST
from: passes
to:
  - ir
severity: P2
status: OPEN
laws_refs:
  - Rule 5
  - Rule 40
  - Rule 75
  - Rule 126
  - Rule 130
  - Part XVIII (PEA Rule)
related_prs: []
related_tests:
  - tests/passes/EscapeTests.cpp
  - tests/ir/
created: 2026-09-01
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
