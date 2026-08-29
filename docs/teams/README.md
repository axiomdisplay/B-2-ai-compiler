# B-2 Team Organization

This document defines how the B-2 compiler is split into isolated AI teams.

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
| Interpreter Team | `INTERP-IMPL` | `INTERP-REV` | T0 | Direct-threaded register interpreter, RBC execution, profiling, T0 state contract |
| Baseline No-IR Team | `BASELINE-IMPL` | `BASELINE-REV` | T1 | Template/copy-and-patch no-IR baseline JIT |
| IR Team | `IR-IMPL` | `IR-REV` | T2/T3 | Sea-of-nodes IR core, node kinds, effect model, verifier data structures |
| Passes Team | `PASS-IMPL` | `PASS-REV` | T2/T3 | Optimization passes (incl. SWLP, PEA, NaN boxing lowering), pass pipeline, pass contracts |
| RegAlloc Team | `REGALLOC-IMPL` | `REGALLOC-REV` | T2/T3 | Liveness, register allocation, spills, GC reference tracking |
| Codegen Team | `CODEGEN-IMPL` | `CODEGEN-REV` | T2/T3 | Lowering, machine code emission, safepoints, deopt stubs, ABI output |
| AOT Team | `AOT-IMPL` | `AOT-REV` | T3 | AOT driver, manifests, artifact validation |

Seven teams of two.

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
