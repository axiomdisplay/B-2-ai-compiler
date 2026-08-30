# Passes Team Charter

```text
Normative reference: docs/laws.md
(B-2 Java Runtime Compiler Laws & Architecture Specification, as amended by
Amendment A, Amendment B, and the Special Pass Laws in Part XVIII.)
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

Team size: 2

Roles:

- `PASS-IMPL` — Implementer. Produces the T2/T3 optimization pass suite, pass pipeline, pass contracts, budgets, telemetry, kill switches, and golden tests inside the team's owned write paths.
- `PASS-REV` — Reviewer. Checks every change for correctness bugs, law compliance, contract adherence, guard/`FrameState` discipline, missing tests, and boundary violations. Does not rewrite code unless explicitly reassigned.

---

## Mission

The Passes Team owns the optimization pass suite and the pass pipeline for T2 and T3. Per Amendment B.4, the full pass suite — including the named first-class passes SWLP (Superword-Level Parallelism), PEA (Partial Escape Analysis), and NaN boxing lowering — applies to T2 and T3 ONLY; T1 runs no passes and nothing in this team's tree may be wired into it. Per Amendment B, T2 is the first IR tier (input: RBC, PGO, class hierarchy, dependency, and profile data) and T3 is the offline reuse of the same optimizer with static proofs; there is one optimizer with two modes, not two optimizers (Rule 1, as amended). Explicitly owned subdirectories under `compiler/passes/`:

```text
compiler/passes/swlp/  compiler/passes/pea/  compiler/passes/nanbox_lowering/  compiler/passes/vector/
compiler/passes/escape/  compiler/passes/loop/  compiler/passes/scalar/  compiler/passes/memory/  compiler/passes/inline/
```

Must deliver: the SWLP pass, the PEA pass, the NaN boxing lowering pass, and the full standard pass suite (see appendix); plus pass contracts, pass budgets, pass telemetry, kill switches, golden tests, and deterministic replay. The engineering designs for the three special passes are fixed in `docs/special_passes.md` (SMT-free: CM-PEA height-6 escape lattice + inline summary tables; speculative effect reordering via the finite effect-kind lookup table and construction rules; adaptive value representation via profile-guided threshold classification) — implementations must follow those designs and their complexity budgets. The team is additionally the **primary owner of the ICDG contract** (`docs/icdg.md`, Inline Call/Dispatch Graph): the central call/dispatch decision engine that decides patch / devirtualize / guard-inline / inline / outline / trap / keep-indirect per call site from dispatch stability + downstream unlock value + cost/budget + deopt safety + invalidation risk. ICDG changes require RFC messages and approval from all consuming teams (baseline_noir, codegen, aot, ir). Not owned: core IR data structures (request via RFC to the IR team), machine lowering, register allocation, codegen, the interpreter, the baseline JIT, the AOT driver, the GC (GC Team, `docs/gc.md`).

---

## Owned Write Paths

```text
compiler/passes/
compiler/pipeline/
tests/passes/
docs/pass_contracts.md
docs/special_passes.md
docs/icdg.md
```

---

## Forbidden Write Paths

```text
compiler/interp/
compiler/baseline/
compiler/ir/core/
compiler/regalloc/
compiler/codegen/
compiler/aot/
runtime/
```

The team never writes outside its owned paths, even for one-line fixes. If the IR is missing a node kind or API, send an RFC to the IR team; do not patch `compiler/ir/core/`. Any issue in another team's area is raised as a message under `messages/` per `docs/teams/messaging.md`; the owning team resolves it.

---

## Key Laws

- Rule 1 — One Pipeline, Multiple Inputs: as amended by Amendments A/B, the unified IR pipeline binds T2/T3; tiers differ by budgets, speculation policy, proofs, and telemetry — not by pass list forks.
- Rule 2 — PGO is a Force Multiplier, Not New Logic: the same pass handles PGO and static-proof modes.
- Rule 3 — Every PGO-Driven Decision Requires a Guard: runtime check, class/shape/version guard, patchpoint, trap, or dependency invalidation.
- Rule 4 — Deoptimization Must Reconstruct Tier 0 State: no irreversible Java-visible side effects before the last guard protecting a speculation.
- Rule 5 — FrameState is Mandatory for All Guards: every speculative node carries one.
- Rule 10 — Every Pass Must Be Idempotent and Monotonic: growing passes (unrolling, vectorization, speculative cloning, PEA materialization) run in guarded fixpoint with strict budget.
- Rule 23 — No Hard-Coded Constants in Optimization Logic: every threshold/budget/limit is a named, documented `constexpr` or configuration parameter.
- Rule 24 — No Target-Specific Hacks in Generic Passes: target knowledge only via the `Target` interface and cost models.
- Rule 25 — No Heuristics Without Empirical Validation: benchmark data, override/tuning hook, and written justification.
- Rule 26 — No Silent Fallbacks Without Telemetry: fallbacks, guard failures, budget overruns, and excessive spills are recorded.
- Rule 28 — No Optimization Without Measurable Win: ≥1% geomean across the suite, or an enabling correctness/safety property.
- Rule 30 — No Vectorization Without Dependence Proof: aliasing, bounds, alignment, overflow semantics, and a correct scalar fallback.
- Rule 35 — Golden Tests for Every Pass: at least ten per pass, checked-in `.in.b2`/`.expected.ir` pairs, run in Static Mode (T3) and Profile Mode (T2).
- Rule 40 — Graph Verifier Runs in Debug Builds After Every Pass.
- Rule 42 — No Assumption Without Invalidation: registry entry (Watchdog), invalidation path (Trip), fallback.
- Rule 43 — No Specialization Without Fallback: generic fallback, deopt path, budget limit.
- Rule 44 — No Profile Data Without Confidence: sample count, stability, age, decay, variance, deopt correlation.
- Rule 45 — No Aggressive Pass Without a Cost Model: inlining, cloning, unrolling, SLP, lock elision, PEA materialization, range-check elimination.
- Rule 72 — Java Numeric Semantics Must Be Preserved Exactly: overflow wrapping, integer division-by-zero, IEEE behavior, NaN, negative zero, conversion and boxing rules.
- Rule 73 — Escape Analysis Must Not Eliminate Observable Objects: identity comparison, `System.identityHashCode`, weak/soft/phantom references, finalizers, Cleaners, GC introspection, tracebacks, reflection, JVMTI, serialization, shared monitors.
- Rule 122 — Speculative Nodes Must Carry Metadata: kind, source, confidence, guard plan, `FrameState`, deopt target, cost, invalidation dependency, rollback plan.
- Rule 123 — Passes Must Declare Contracts: required/produced IR properties, invalidated analyses, supported tiers, budget, determinism, target dependencies, verifier checks, telemetry hooks.
- Rules 124/125 — Deterministic, Replayable, No Hidden Global State: same bytecode, version, flags, profile snapshot, target, seed produce identical IR; no hidden singletons in pass logic.
- Rules 132/144 — Kill Switches and Feature Gates: every optimization is independently disableable and feature-gated for bisection and emergency response.
- Part XVIII — Special Pass Laws bind T2 and T3. SWLP Rule: no vectorization unless dependencies are proven safe, versioned checks are emitted, or a scalar fallback is guaranteed; exact Java numeric semantics, no fast-math, no reordering of Java-visible effects; must run after inlining, GVN/CSE, range analysis, alias analysis, loop normalization, and type specialization. PEA Rule: every escape point is proven non-observable, guarded, materialized, or rejected; materialization at calls, safepoints, deopt points, exception edges, escaping stores, monitor enter, JNI, and reflection boundaries. NaN Boxing Rule: representation-only optimization, disabled by default with a global kill switch until all cross-team contracts are approved; the representation contract requires approval from interpreter, baseline_noir, ir, passes, regalloc, codegen, aot, and GC/runtime — the lowering pass lives here, but the contract is shared.

---

## Deliverables

1. Pass registry — stable keys for every pass (appendix numbering is normative), metadata, feature-flag bindings.
2. Pass contract declarations per Rule 123, checked by contract tests.
3. Pipeline scheduler — T2 (Profile Mode) and T3 (Static Mode) orderings of the same suite, with tier-specific filters per Rule 1.
4. Budget enforcement — per-pass and whole-compilation budgets, guarded fixpoints for growing passes (Rule 10).
5. Pass telemetry — fallbacks, guard failures, budget overruns, per-pass outcomes (Rule 26).
6. Golden tests for each pass — ten per pass, both modes (Rule 35).
7. Kill switches — per-optimization and global (Rules 132, 144).
8. Deterministic pass execution — same input snapshot and seed produce identical IR (Rule 124).
9. Replayable pass pipelines — replay artifacts per Rule 38.
10. SWLP pass — superword-level parallelism satisfying the Part XVIII SWLP Rule.
11. PEA pass — partial escape analysis, scalar replacement, and materialization satisfying the Part XVIII PEA Rule.
12. NaN boxing lowering pass — tagged-value lowering satisfying the Part XVIII NaN Boxing Rule, gated on shared-contract approval.
13. Full standard pass suite — all 120 passes of the appendix.

---

## Interface Contract

### Consumes

- IR graphs and the IR Team's public APIs: node creation, effect-chain construction, guard attachment, `FrameState` attachment, read-only traversal, verifier hooks (`include/b2/ir/`).
- PGO profile data with Rule 44 confidence fields, plus mechanically checkable static proof artifacts for T3 (Rule 131).
- Target cost models and capability flags via the `Target` interface (Rules 24, 27).

### Outputs

- Optimized IR graphs with guard nodes, `FrameState`, deopt targets, and dependency metadata for the invalidation watchdog (Rule 42), consumed by regalloc and codegen.
- Pass logs, telemetry, and replay artifacts.
- Golden IR baselines under `tests/passes/`; pass contracts in `docs/pass_contracts.md`.

### Promises

- No mutation of core IR structures: `compiler/ir/core/` is never edited; graphs are transformed only through IR public APIs.
- Deterministic behavior for the same input snapshot (bytecode, profile, target, flags, seed) and explicit guard insertion for every PGO decision (Rules 3, 124).
- Fallback on budget violation, always with telemetry (Rules 10, 26).
- No target-specific logic in generic passes (Rule 24); NaN boxing stays disabled by default until cross-team approval (Part XVIII).

---

## Testing Responsibilities

```text
tests/passes/
```

- Golden IR tests per pass: ten per pass, Static (T3) and Profile (T2) modes; self-documenting names; bug fixes add five regression tests (Rules 34, 35, 41).
- Pass contract tests: declared required/produced properties and invalidated analyses hold (Rule 123).
- Budget and kill-switch tests: overruns abort with telemetry, guarded fixpoints terminate, every optimization disables cleanly and leaves valid IR (Rules 10, 26, 132, 144).
- Guard emission and `FrameState` attachment tests: every PGO decision emits a validated guard mechanism; every guard and speculative node carries `FrameState` (Rules 3, 5).
- Deopt metadata preservation tests: deopt reconstructs exact T0 state (Rule 4).
- SWLP dependence-proof and tail/masking tests: aliasing hazards, bounds, alignment, tail iterations, masked/partial vectors, division-by-zero, NaN and negative-zero semantics (Rule 30, Part XVIII).
- PEA materialization tests, including weakref/finalizer negative tests (Rule 73, Part XVIII).
- NaN boxing round-trip and kill-switch tests: T2 lowering tests live here; interpreter/baseline round-trips are coordinated cross-team (Part XVIII).
- Differential T2 behavior against the T0 oracle (Rule 36).

---

## When To Send Messages

- The IR lacks a node kind or API required by a pass — RFC to the IR team.
- The IR verifier rejects valid optimized IR — verifier bug or contract drift; the IR team owns the verifier core.
- Codegen cannot consume pass output — lowering contract mismatch.
- RegAlloc needs additional liveness or reference metadata.
- A pass bug may affect downstream teams (regalloc, codegen, aot) or already-shipped golden baselines.
- Golden IR baselines change — other teams pin tests against them.
- The NaN boxing contract changes — requires all affected teams' approval per Part XVIII.

Bugs in pass code are owned and fixed by this team regardless of who reports them; if a fix affects other teams, an ADVISORY accompanies it. The team never writes into another team's paths, regardless of severity.

---

## Reviewer Checklist

- [ ] Every pass has a declared contract and a registry entry (Rule 123).
- [ ] Every pass has ten golden tests in both Static and Profile modes (Rule 35).
- [ ] No magic constants; every heuristic is validated, tunable, and documented (Rules 23, 25).
- [ ] No target-specific conditionals in generic passes (Rule 24).
- [ ] Every PGO assumption has a guard with an invalidation path (Rules 3, 42).
- [ ] Every guard has `FrameState` (Rule 5).
- [ ] Speculative nodes carry the full Rule 122 metadata set.
- [ ] Fallbacks and budget overruns emit telemetry, and budgets are enforced with guarded fixpoints for growing passes (Rules 10, 26).
- [ ] SWLP, PEA, and NaN boxing satisfy their Part XVIII rules: dependence proofs, materialization at all required points, shared-contract approval, kill switches.
- [ ] Passes are idempotent/monotonic (Rule 10), deterministic and replayable (Rule 124), and free of hidden global mutable state (Rule 125).
- [ ] No file outside the owned write paths is modified; IR requests went through `messages/`.

---

## Appendix: Required T2/T3 Pass Suite

The full pass suite below applies to T2 and T3 (Amendment B.4). Numbering is stable and normative for pass registry keys.

### Frontend / graph building

1. RBC parsing
2. bytecode type stabilization
3. inline-cache profile import
4. method entry graph construction
5. exception edge construction
6. FrameState seeding
7. OSR entry construction
8. invokedynamic call-site modeling
9. MethodHandle lowering
10. static field dependency import

### Early cleanup

11. dead code elimination
12. trivial block fusion
13. canonicalization
14. constant folding
15. strength reduction
16. identity operation removal
17. redundant cast removal where proven
18. null-check folding where safe
19. branch normalization
20. control-flow simplification

### Inlining and call optimization

21. CHA-based devirtualization
22. PGO-based devirtualization
23. inline cache specialization
24. monomorphic call specialization
25. bimorphic call specialization
26. interface call specialization
27. invokedynamic target specialization
28. MethodHandle inlining
29. recursive inlining control
30. caller/callee graph budgeting
31. inlining cost modeling
32. partial inlining
33. guard-based inlining
34. exception-edge-aware inlining

### Scalar optimization

35. GVN
36. CSE
37. CPROP
38. SCCP
39. conditional constant propagation
40. branch folding
41. jump threading
42. block duplication for hot edges
43. reassociation where Java-safe
44. algebraic simplification
45. narrow integer type optimization
46. sign/zero extension elimination
47. redundant conversion elimination
48. range propagation
49. value range analysis
50. overflow-safe arithmetic optimization

### Memory optimization

51. alias analysis
52. load forwarding
53. store-to-load forwarding
54. common subexpression memory folding
55. PRE where safe
56. load hoisting
57. store sinking
58. field access canonicalization
59. array access canonicalization
60. volatile/ordering preservation
61. fence placement validation
62. barrier insertion
63. write barrier optimization
64. read barrier optimization

### Object optimization

65. escape analysis
66. partial escape analysis
67. scalar replacement
68. allocation sinking
69. object materialization planning
70. lock elision
71. monitor coarsening
72. monitor elimination where provably safe
73. identity hash preservation checks
74. weakref/finalizer escape checks
75. JNI escape checks

### Loop optimization

76. loop detection
77. loop canonicalization
78. loop rotation
79. loop peeling
80. loop unrolling
81. loop unswitching where profitable
82. loop-invariant code motion
83. loop range-check elimination
84. loop null-check elimination
85. loop type-check elimination
86. induction variable analysis
87. affine analysis
88. trip count analysis
89. safepoint placement in long loops
90. OSR loop entry optimization

### Parallelism / vectorization

91. dependency graph construction
92. memory dependence analysis
93. loop vectorization
94. SLP vectorization
95. Superword-Level Parallelism
96. vector cost modeling
97. scalar fallback generation
98. tail masking
99. alignment handling
100. vector barrier correctness

### Representation lowering

101. compressed oop lowering
102. object header lowering
103. array header lowering
104. reference tagging lowering
105. NaN boxing lowering
106. primitive boxing/unboxing lowering
107. stack slot lowering
108. FrameState lowering
109. GC map lowering
110. safepoint lowering

### Late optimization

111. block layout
112. branch layout
113. cold/hot splitting
114. exception-path outlining
115. uncommon trap placement
116. deopt duplication reduction
117. code size budgeting
118. register pressure estimation
119. late GC barrier cleanup
120. final effect-chain verification
