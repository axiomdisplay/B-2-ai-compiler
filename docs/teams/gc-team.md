# GC Team Charter

```text
Normative reference: docs/laws.md
(B-2 Java Runtime Compiler Laws & Architecture Specification, as amended by
Amendment A, Amendment B, and the Special Pass Laws in Part XVIII.)
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
Design contract: docs/gc.md (GCR — Generational Concurrent Region-Based
Collector). If this charter conflicts with `docs/gc.md` on GC engineering
matters, `docs/gc.md` wins; law conflicts always resolve to `docs/laws.md`.
```

Team size: 2

Roles:

- `GC-IMPL` — Implementer. Produces the GCR collector (regions, TLABs, barriers, young scavenge, concurrent mark-evacuate, stack scanning support, telemetry and stress hooks) inside the team's owned write paths.
- `GC-REV` — Reviewer. Checks every change for Java-semantics fidelity (identity, reachability, finalization ordering), race-freedom (Rule 109), pause-budget discipline, missing Java-visible tests, and contract drift. Does not rewrite code unless explicitly reassigned.

---

## Mission

The GC Team owns the B-2 heap and the GCR collector. Per Part VIII of the
laws, GC is a first-class compilation contract, not a runtime afterthought:
every compiled tier must track references (Rule 86), emit correct barriers
(Rule 87), poll safepoints (Rule 88), and keep frames walkable (Rule 89);
the GC team owns the other side of each of those contracts.

The GC design is `docs/gc.md` (GCR): generational because Java is
generational, region-based because absurd scale demands it, concurrent
because latency is non-negotiable, load-barriered because a moving collector
requires it, observable because B-2 validates correctness exclusively
through Java tests.

In scope:

- Region allocator, heap layout, young/old/humongous region pools, NUMA-local pools.
- TLAB allocation (lock-free fast path, CAS refill), humongous allocation.
- Write and read barriers (semantics + slow paths; tier teams own the lowering/stenciling of these into their code).
- Young parallel scavenge; old concurrent mark-evacuate; remark; cleanup.
- Remembered sets, card tables, SATB buffers, refinement.
- Stack scanning contracts with each tier: RBC-metadata scanning for T0, stencil slot maps for T1, generated stack maps for T2/T3 (the GC team defines the required metadata shape; each tier team produces it).
- CM-PEA materialization allocation context (dedicated buffer, handle scope, rollback) — jointly specified with the passes team.
- Weak references, finalizers, cleaners processing order.
- Identity hash stability across relocation.
- GC telemetry and the Java-visible stress hooks (`RuntimeHooks`, `GCTelemetry` per `docs/gc.md` §10).
- Epoch-based reclamation of heap memory; coordination with code-cache reclamation (which stays owned by codegen/integrator per Part IX).

Out of scope:

- Code cache collection (Part IX owners).
- Barrier optimization passes (passes team consumes GC barrier semantics).
- Barrier stenciling in T1 (baseline team) and barrier lowering in T2/T3 (codegen team).
- Object model/class space semantics owned by the interpreter/runtime v0 contracts (joint changes go through messages).

---

## Standing Constraints

- All pause and overhead targets in `docs/gc.md` §11 are named constants, not aspirations; performance gates must measure more than throughput (Rule 138).
- Every optimization-dependent GC mode (moving/non-moving, barrier forms) has a kill switch (Rule 132).
- GC and deopt stress tests are first-class CI citizens (Rule 136); the Java stress/differential/fuzz harness is the validation mechanism — no C++ GC unit tests as validation.
- Shared state with compiler/runtime is race-free (Rule 109); no global locks on hot paths (Rule 118); safepoint latency stays bounded (Rule 111).
- NaN-boxed reference scanning correctness is differential-tested against unboxed execution.
- A moving-GC design never breaks T0 reconstruction: deopt materialization allocates through the dedicated context only.

---

## Interfaces To Other Teams

Requests inbound (via `messages/`):

- Tier teams request barrier/st stack-metadata shape changes.
- Passes team requests materialization-context extensions for CM-PEA.

Requests outbound (via `messages/`):

- Barrier semantics or metadata shape changes → all tier teams (ADVISORY, all must approve per the shared-contract rule).
- Object header/layout changes → interpreter, codegen, passes, regalloc, aot.
- GC compatibility manifest version bumps → aot.

Bugs in GC code are owned and fixed by this team regardless of who reports
them; if a fix affects other teams, an ADVISORY accompanies it. The team
never writes into another team's paths, regardless of severity.

---

## Reviewer Checklist

- [ ] Java semantics preserved: identity hash stable across relocation, weak/finalizer/cleaner ordering spec-compliant (Rules 78, 94).
- [ ] Every reference in every tier's frames is reachable by the scanner for that tier (Rules 86, 89).
- [ ] Barrier semantics documented and versioned; changes ADVISORIED to all consuming teams (Rule 87).
- [ ] No new global lock on a mutator or collector hot path (Rules 109, 118).
- [ ] Pause budgets enforced and measured; telemetry added for every new phase (Rules 111, 138, 139).
- [ ] Stress hooks exposed for every new behavior; Java stress/differential tests added (Rule 136).
- [ ] Materialization context safety maintained for CM-PEA deopt paths.
- [ ] Epoch/quiescence discipline respected before freeing anything (Rule 100 analog for heap).
- [ ] No file outside the owned write paths is modified; cross-team requests went through `messages/`.
