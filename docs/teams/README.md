# B-2 Team Organization

This document defines how the B-2 compiler is split into isolated AI teams.
There are nine teams: the eight compilation teams plus the GC Team
(`docs/teams/gc-team.md`, design `docs/gc.md`).

The normative rules are in:

```text
docs/laws.md
```

If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.

---

## Tier Model

The B-2 execution model, per Amendment B in `docs/laws.md`:

```text
Java classfiles
      |
      v
Loader / Verifier / Quickener
      |
      v
Register Bytecode (RBC)
      |
      +--------------------+----------------------+
      |                    |                      |
      v                    v                      v
   T0 Interpreter      T1 No-IR Baseline      T2 Optimizing JIT
   direct-threaded     template/copy-patch    sea-of-nodes IR
      |                    |                      |
      +--------------------+----------+-----------+
                                    |
                                    v
                                  T3 AOT
                         offline T2-style pipeline
```

Tier transitions (upward):

```text
T0 -> T1 on heat
T1 -> T2 on sustained heat + useful profile
T2 -> T3 via offline / static proofs / PGO
```

Deopt direction:

```text
T3 -> T2 or T0 if dynamic invalidation
T2 -> T1 or T0 on guard failure
T1 -> T0 on unsupported trap
T0 is always final fallback
```

Tier-to-team mapping:

- T0 belongs to the **Interpreter Team**.
- T1 belongs to the **Baseline No-IR Team** (no IR graph — Amendment A).
- T2 belongs to the **IR, Passes, RegAlloc, and Codegen Teams** jointly, each inside its owned area.
- T3 belongs to the **AOT Team** as driver; it reuses the same T2 optimizer pipeline (Amendment B).
- The source path (Java source -> AST -> RBC) belongs to the **Frontend Team**. It is tier-independent: it lowers source into RBC, the same level the classfile Loader/Verifier/Quickener path delivers to, and the tiers take over from there.

---

## Macro Structure

The user-facing build order for B-2, in three macro stages:

```text
Java source (.java)
      |
      v
Frontend (source -> AST: lexer, parser, diagnostics)
      |
      v  (future: AST -> RBC lowering)
Middle End (RBC, IR Team, Passes Team, Interpreter Team,
            Baseline No-IR Team; tiers T0/T1/T2)
      |
      v
Backend (RegAlloc Team, Codegen Team; AOT Team drives T3 offline)
```

Java classfiles enter the same pipeline at the Register Bytecode (RBC) level via the Loader/Verifier/Quickener path (see the Tier Model above). The source path joins them there once AST-to-RBC lowering lands.

---

## Current Status

- Frontend (v0) complete: lexer, recursive-descent parser, lossless position-annotated AST, diagnostics with error recovery, the `b2parse` driver, and `tests/frontend/` (46 unit tests + 24-file corpus, all green). See `docs/teams/frontend-team.md` and `docs/frontend_contract.md`.
- Middle end (v0) complete: RBC specification, 150-opcode core, builder, structural/type verifier, deterministic text format, the `b2rbc` driver, and `tests/rbc/` (79 tests + corpus, all green). See `docs/rbc_spec.md`.
- Backend kickoff (T0 interpreter v0) complete: direct-threaded register interpreter executing all 150 RBC opcodes, the v0 reference runtime (object model, monitors, exceptions with JVM messages, builtin println with JDK-exact float formatting, string interning, lazy class init), inline caches, safepoint polls, saturating profile counters, the deopt entry point (`resume`), the `b2run` driver, the ICDG Phase 1 per-site dispatch profile (receiver-class/target histograms, sticky megamorphic — `Interpreter::dispatchProfiles()`, MSG-20260901-003), and `tests/interp/` (137 tests + 19-program runnable corpus, all green). The published T0 state contract is `docs/interp_contract.md` (v1.2.0) — normative for every compiled tier's deopt path.
- T1 baseline (plan stage v0) complete: frozen `include/b2/baseline/` contract (stencil identity/categories/patch holes, `StencilPlan` records + 7 invariants, the v0 stencil-set manifest, the `compilePlan` API with budget/refusal semantics), the plan builder (linear no-IR scan with greedy superinstruction fusion, patch-value computation split plan-computable vs runtime-pending, pc map, stack maps, deopt points, translated exception edges, `verifyPlan` auditor), the deterministic golden dump, the `b2plan` driver, and `tests/baseline/` (50 tests, all green; corpus sweep: 5 methods plan, the rest refuse `no-stencil-for-op` per the documented v0 availability map). Instantiation (copy/patch/link, W^X publish) lands with the codegen team's stencil archive.
- Frontend (v1) complete: the v0 lexer/parser/AST/diagnostics stage PLUS the v0 AST-to-RBC lowering (`include/b2/frontend/Lower.h`, RFC `messages/closed/MSG-20260830-003-frontend-ir-RFC.md`): one top-level class, fields/methods/constructors, `<clinit>` + default-ctor synthesis, the full statement set (try/catch/finally, switch both forms, labeled loops, enhanced-for over arrays), JLS numeric promotions, NaN-correct comparisons, short-circuit booleans, casts, compound assignment, arrays, objects, calls (own methods + System.out println/print). Every emitted method passes `rbc::verify` by construction; 67 new tests + an 8-program executed corpus close source -> RBC -> T0 execution (`b2parse --emit-rbc X.java` + `b2run`). Contract: `docs/frontend_contract.md` (v1).
- T1 instantiation (machine code v1) complete: the build-time stencil archive (`tools/stencilgen` emits one x86-64 body per manifest stencil + the internal entry/deopt-thunk/deopt-exit/switch-case templates; deterministic, golden-tested), the copy-and-patch instantiator (`compiler/codegen/` - declared-hole-only patching per Stencil Rule 3, true re-layout, per-DeoptPoint thunks, handler entry stubs, W^X publish with the RW-before-free discipline), the Tier-1 execution engine (code cache, the 16-op runtime-helper seam, calls through the engine with builtin probe + resolution, uniform engine-side exception dispatch that RE-ENTERS compiled code at caught handlers, deopt-to-T0 with full frame reconstruction), the `b2jit` driver, and `tests/codegen/` (15 tests; the differential law: the 14-program interp corpus + the 8-program frontend corpus execute byte-identical to T0, ASan/UBSan clean; fib(28) runs ~1.3x faster than T0). The availability flip (set v2, `MSG-20260830-004`): the un-quickened `invoke*` and `ldc` are Available (the helper seam executes them); `multianewarray` is the honest v1 gap. Contract: `docs/codegen_contract.md` (v1).
- T2 kickoff (IR core v1) complete: the sea-of-nodes representation library `compiler/ir/core/` + `include/b2/ir/` - arena-backed Graph with NodeId edges and SmallVector use-def chains (Rules 7/15/19), epoch-tagged replacement with a log (Rule 14), the full NodeKind taxonomy (typed arithmetic, explicit conversions per Rule 33, calls with mandatory FrameState, guards with the Rule 122 SpecMeta set, SWLP vector nodes with lane/mask well-formedness, PEA VirtualObjectState/Materialize with fields-as-edges, NaN-boxing tag/untag/RepTransitionGuard), the 12-class effect model with the 144-entry reorder table and construction rules R1-R9, the total structural+metadata verifier (Rules 40/126), the deterministic printer, and versioned serialization with verified replay (Rules 31/124). 69 tests + ASan/UBSan clean. Contract: `docs/ir_spec.md` + `docs/effect_system.md`.
- IR core v2 (MSG-20260831-007) + the RBC-to-IR graph builder (passes team, suite items 1-6) landed: `compiler/passes/` + `include/b2/passes/` + the `b2graph` tool. Locals/registers lower to SSA values (loads/stores/moves alias, no nodes), loop/merge phis with the loop-invariant self-input, guards gate control in front of every trap (null/bounds/zero), FrameStates at calls/guards/loop-exits carry all slots (Undef = T0's Bottom), every exceptional continuation deopts (classes 2.1/2.2/2.3; uncaught unwinds), class-init triggers mirror T0's pins, quickened ops fully supported. v2 additions to the IR: boolean test kinds, Undef, memory Phis, fixed-node control chaining, Phi self-inputs, serialization v2 (v1-compatible). 19 builder tests + 12 IR v2 tests; the 19-program corpus sweep builds and IR-verifies all 24 methods, determinism pinned. Contract: `docs/graph_builder.md`; next: the first T2 passes (GVN, CM-PEA) and ICDG Phase 1 on the existing profile counters.
- T2 optimization pass suite v1 (early cleanup + GVN, suite items 11-20 & 35) landed: `compiler/passes/src/` + `include/b2/passes/Passes.h` + `b2graph -O`. Ten registry keys with Rule 123 contracts, per-pass kill switches (Rules 132/144), rewrite budgets, and Rule 26 telemetry; one fixpoint pipeline (Rule 10) with the IR verifier between passes (Rule 40). The rewrite catalogs: exact-bit constant folding (Java wrap/JLS 5.1.3/IEEE NaN policy), identity removal, strength reduction (mul to shl), canonicalization, trivial-phi collapse + region repair, null-check folding on never-null values, constant-condition branch/guard folding (always-false guards become Deopts preserving DeoptId+FrameState), the unreachable sweep, and structural GVN (identical (kind,payload,inputs) merge by lowest id; FrameState snapshots auto-update per Rule 14). The tombstone-law protocol (junk sinks) keeps every intermediate graph verifier-clean under the IR verifier's dead-node input checks. 126 new tests (>=10 per pass, Rule 35); the 19-program corpus runs the full pipeline verified, deterministic, idempotent; ASan/UBSan clean. Two builder bugs found and fixed by the suite (advisory in MSG-008): the arithmetic kind-table OOB (Iand/Ior/Ixor read past; shifts mapped to bitwise kinds) and the long zero-guard operand-type violation. IR-core type defects reported to the IR team (MSG-009): resultTypeOf classifies I2L as Double- and CmpL/CmpFl/CmpFg/CmpDl/CmpDg as FP-producing although Node.h defines int results. Contract: `docs/pass_contracts.md`.
- T2 inlining v1 (the ICDG direct-inline engine) landed: `compiler/passes/src/Inline.cpp` + `include/b2/passes/Inline.h` + `b2graph --inline`. Direct (`CallStatic`) sites inline by RE-RUNNING the RBC->IR builder over the callee inside the caller's graph (Graal-style - trial build per callee, argument-wired entry, exits instead of Return terminals, exit Region/memory-Phi/value-Phi merges, role-based rewiring, tombstone-law kills, verify after every site). Every callee FrameState chains to the call-site snapshot (Rule 75 - the deoptimizer reconstructs the inlined frame stack); exception escapes reuse the existing deopt conventions (covered sites deopt through the callee frame; uncovered sites keep the Unwind); quickened payloads resolve through the unified id space (ProgramCalleeSource IS the resolver: program methods = table indices, externals above). The cost model is refusal caps + budgets with a structured decision log per site (icdg.md 19), kill switch, and Rule 26 telemetry; DCE gained the chained-FrameState protection (caller-chain references are side-table data - the snapshots are deopt metadata even with zero edge users). 29 tests + the 19-program corpus through build -> inline -> pipeline (verified, deterministic, idempotent); ASan/UBSan clean; Law-36 sweep 19/19 (T0/T1 untouched). Contract: `docs/inlining.md`.
- ICDG Phase 2 landed (the T2 side: ingestion + GuardInline; MSG-20260901-005): `passes::DispatchProfile` (the T0 dispatch-profile snapshot, site key `(caller MethodId, call pc)` recovered from the call's FrameState; runtime class ids bridge to the resolver TypeId space by NAME) + the shared converter `compiler/passes/tools/DispatchProfileSnapshot.h` + `b2graph --pgo` (T0 training run -> snapshot -> engine, decision lines carry the profile numbers). A monomorphic `CallVirtual`/`CallInterface` site inlines behind a `TypeProfile` guard (`InstanceOf` condition, deopt at the call pc re-executes the invoke - the guard runs before any body effect), with complete Rule 122 SpecMeta (TypeMonomorphic/PGO/confidence/cost) + a `ClassHierarchy` invalidation dependency (Rule 42; the C2 shape). Bimorphic/polymorphic/megamorphic/low-confidence/Object-profiled sites refuse with structured reasons; the passes LIBRARY stays interp-free (the tool and the test harness are the interp-linking integrators). Zero-regression pinned: no profile attached = virtual sites not considered, v1-identical bytes. 20 new tests (suite 49) + a profile-attached corpus-sweep variant; full matrix green (812/812 individual tests, Law-36 19/19, `--pgo -O` corpus 19/19, ASan/UBSan clean). Found on the way: the IR verifier's memory-chain walk false-positives on any memory producer inside a loop (MSG-20260901-004, blocks call-in-loop shapes until the ir team lands the fix) and a passes-side builder defect on interface-call type probing (fixed in-tree). Contract: `docs/inlining.md` section 9.
- Next: the ir team's verifier/type batch (MSG-009 type-classification fixes, MSG-20260901-004 loop memory-chain false positive - unblocks the most common real-world inline shape, MSG-20260901-002 chain-target liveness check); the GuardInline growth (bimorphic two-target guard, charter item 26; CHA subclass proofs once hierarchy analysis exists); CM-PEA per `docs/special_passes.md`; the mid-suite scalar passes (CSE/PRE/SCCP) once MSG-009 lands; GCR Phase 1 (region allocator, TLAB, young scavenge) per `docs/gc.md` - the T1 activation record is the GC's frame-scanning contract (every slot is a tagged Value); string-concatenation builtins RFC (interpreter) to lift the lowering's top refusal.

---

## Shared Cross-Team Contracts

Two design contracts are shared across teams and require RFC + consumer
approval to change:

- `docs/icdg.md` — ICDG (Inline Call/Dispatch Graph), primary owner: passes; consumers: baseline_noir, codegen, aot, ir.
- `docs/gc.md` — GCR (GC design), primary owner: gc; consumers: ir, passes, baseline_noir, codegen, interpreter, aot.

---

## Core Principle

Each team owns a small subsystem.

Each team has exactly two AI roles:

- `IMPLEMENTER`
- `REVIEWER`

The implementer produces the output.

The reviewer reviews the output for:

- correctness bugs
- law compliance
- boundary violations
- missing tests
- missing documentation
- missing message references

The reviewer does **not** rewrite the implementation unless explicitly reassigned.

---

## Teams

| Team | Implementer | Reviewer | Tier | Primary Ownership |
|---|---|---|---|---|
| Frontend Team | `FRONTEND-IMPL` | `FRONTEND-REV` | source path | Java-source frontend: lexer, parser, lossless AST, diagnostics, `b2parse`; future AST-to-RBC lowering |
| Interpreter Team | `INTERP-IMPL` | `INTERP-REV` | T0 | Direct-threaded register interpreter, RBC execution, profiling, T0 state contract |
| Baseline No-IR Team | `BASELINE-IMPL` | `BASELINE-REV` | T1 | Template/copy-and-patch no-IR baseline JIT |
| IR Team | `IR-IMPL` | `IR-REV` | T2/T3 | Sea-of-nodes IR core, node kinds, effect model, verifier data structures |
| Passes Team | `PASS-IMPL` | `PASS-REV` | T2/T3 | Optimization passes (incl. SWLP, PEA, NaN boxing lowering), pass pipeline, pass contracts |
| RegAlloc Team | `REGALLOC-IMPL` | `REGALLOC-REV` | T2/T3 | Liveness, register allocation, spills, GC reference tracking |
| Codegen Team | `CODEGEN-IMPL` | `CODEGEN-REV` | T2/T3 | Lowering, machine code emission, safepoints, deopt stubs, ABI output |
| AOT Team | `AOT-IMPL` | `AOT-REV` | T3 | AOT driver, manifests, artifact validation |
| GC Team | `GC-IMPL` | `GC-REV` | runtime | GCR collector: regions, TLABs, barriers, concurrent mark-evacuate, stack-scanning contracts, stress hooks |

Nine teams of two.

If fewer teams are desired initially, AOT can be merged into Passes/Codegen — but for a real compiler experiment, a separate AOT team is preferred.

---

## Area Isolation Rule

Every team may read the entire repository.

Every team may write only to its owned paths.

If a team needs a change outside its owned paths, it must send a message.

Forbidden examples:

- Passes Team modifying `compiler/ir/core/`
- Codegen Team modifying pass heuristics
- RegAlloc Team changing IR node semantics
- IR Team changing optimization budgets
- Baseline No-IR Team building any IR graph or running any optimization pass (Amendment A)
- AOT Team forking the optimizer instead of driving the shared T2 pipeline (Amendment B)
- Frontend Team constructing IR graphs or emitting machine code (the frontend stops at the AST until AST-to-RBC lowering is contracted)
- Interpreter Team changing the RBC encoding without an advisory to all compiled tiers

Cross-area work without an approved message is a rule violation.

The machine-readable ownership map is:

```text
docs/teams/ownership.yaml
```

CI must fail any PR that touches paths outside the submitting team's `write` list.

---

## Pull Request Rules

Every PR must contain:

```text
B2-Team: <team>
B2-Implementer: <agent-id>
B2-Reviewer: <agent-id>
B2-Laws-Version: <hash or version>
B2-Messages: <message ids or NONE>
```

Every PR must be approved by the team reviewer.

The reviewer must check:

- [ ] only owned paths were modified
- [ ] required tests exist
- [ ] no banned containers or patterns were introduced
- [ ] all relevant laws from `docs/laws.md` were respected
- [ ] any cross-team impact has an associated message

If a PR touches a shared contract or references another team, CI must require a non-empty `B2-Messages` field.

---

## Testing Ownership

Each team tests its own side.

Test ownership:

| Team | Test Path |
|---|---|
| Frontend Team | `tests/frontend/` |
| Interpreter Team | `tests/interp/` |
| Baseline No-IR Team | `tests/baseline/` |
| IR Team | `tests/ir/` |
| Passes Team | `tests/passes/` |
| RegAlloc Team | `tests/regalloc/` |
| Codegen Team | `tests/codegen/` |
| AOT Team | `tests/aot/` |

If a team finds a bug in its own subsystem, it fixes it.

If the bug may affect another team, it must send an advisory message.

If a team finds a bug in another subsystem, it must not patch that subsystem. It must send a bug message.

Differential testing across tiers (Rule 36, Rule 133) is owned by CI, not by any single team.

---

## Escalation

If two teams disagree:

1. Both reviewers write their position in the message thread.
2. The message is marked `BLOCKED`.
3. A human integrator or designated arbitration process resolves it.

No team may unilaterally change another team's contract.

---

## Required Team Documents

Each team has its own charter:

- `docs/teams/frontend-team.md`
- `docs/teams/interpreter-team.md`
- `docs/teams/baseline-noir-team.md`
- `docs/teams/ir-team.md`
- `docs/teams/passes-team.md`
- `docs/teams/regalloc-team.md`
- `docs/teams/codegen-team.md`
- `docs/teams/aot-team.md`

The message system is defined in:

- `docs/teams/messaging.md`

The ownership map is:

- `docs/teams/ownership.yaml`

Message templates live in:

- `docs/templates/message.md`
- `docs/templates/own-bug-advisory.md`
- `docs/templates/review-checklist.md`

---

NOTE TO ALL AI TEAMS:

If your team finds a bug while testing its own side, your team owns the fix.

You must not ask another team to fix your bug unless the bug is actually in their area.

If your bug may affect another team, send an ADVISORY message.

If your bug is in another team's area, send a BUG message and do not patch their code.

No team is allowed to touch anything outside its owned area.

Cross-team changes happen only through messages.
