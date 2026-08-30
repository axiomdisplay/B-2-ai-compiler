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
- Backend kickoff (T0 interpreter v0) complete: direct-threaded register interpreter executing all 150 RBC opcodes, the v0 reference runtime (object model, monitors, exceptions with JVM messages, builtin println with JDK-exact float formatting, string interning, lazy class init), inline caches, safepoint polls, saturating profile counters, the deopt entry point (`resume`), the `b2run` driver, and `tests/interp/` (125 tests + 14-program runnable corpus, all green). The published T0 state contract is `docs/interp_contract.md` (v1.0.0) — normative for every compiled tier's deopt path.
- T1 baseline (plan stage v0) complete: frozen `include/b2/baseline/` contract (stencil identity/categories/patch holes, `StencilPlan` records + 7 invariants, the v0 stencil-set manifest, the `compilePlan` API with budget/refusal semantics), the plan builder (linear no-IR scan with greedy superinstruction fusion, patch-value computation split plan-computable vs runtime-pending, pc map, stack maps, deopt points, translated exception edges, `verifyPlan` auditor), the deterministic golden dump, the `b2plan` driver, and `tests/baseline/` (50 tests, all green; corpus sweep: 5 methods plan, the rest refuse `no-stencil-for-op` per the documented v0 availability map). Instantiation (copy/patch/link, W^X publish) lands with the codegen team's stencil archive.
- Frontend (v1) complete: the v0 lexer/parser/AST/diagnostics stage PLUS the v0 AST-to-RBC lowering (`include/b2/frontend/Lower.h`, RFC `messages/closed/MSG-20260830-003-frontend-ir-RFC.md`): one top-level class, fields/methods/constructors, `<clinit>` + default-ctor synthesis, the full statement set (try/catch/finally, switch both forms, labeled loops, enhanced-for over arrays), JLS numeric promotions, NaN-correct comparisons, short-circuit booleans, casts, compound assignment, arrays, objects, calls (own methods + System.out println/print). Every emitted method passes `rbc::verify` by construction; 67 new tests + an 8-program executed corpus close source -> RBC -> T0 execution (`b2parse --emit-rbc X.java` + `b2run`). Contract: `docs/frontend_contract.md` (v1).
- Next: T1 stencil instantiation + code cache (codegen team stencil archive); T2 IR construction kickoff (CM-PEA designs per `docs/special_passes.md`; ICDG per `docs/icdg.md` - the T0 interpreter's profile counters are its Phase 1 dispatch profiler); GCR Phase 1 (region allocator, TLAB, young scavenge) per `docs/gc.md`; string-concatenation builtins RFC (interpreter) to lift the lowering's top refusal.

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
