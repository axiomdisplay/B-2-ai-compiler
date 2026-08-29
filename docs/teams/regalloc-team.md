# RegAlloc Team Charter

```text
Normative reference: docs/laws.md
(B-2 Java Runtime Compiler Laws & Architecture Specification, as amended by
Amendment A, Amendment B, and the Special Pass Laws in Part XVIII.)
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

Team size: 2

Roles:

- `REGALLOC-IMPL` — Implementer. Produces liveness analysis, live intervals, register assignment, spill logic, GC-reference location tracking, stack map contribution data, pressure reporting, and allocation diagnostics inside the team's owned write paths.
- `REGALLOC-REV` — Reviewer. Checks every change for correctness bugs, GC-reference safety, law compliance, determinism, boundary violations, and missing tests/docs. Does not rewrite code unless explicitly reassigned.

---

## Mission

The RegAlloc Team owns register allocation for the B-2 optimizing backend. It serves T2, the optimizing JIT, and — because T3 AOT reuses the same T2 pipeline offline (Amendment B.5: "AOT uses the T2 pipeline — not a separate optimizer") — every allocator decision must behave identically in JIT mode (PGO, guards, deopt) and offline AOT mode (static proofs, build-time target descriptions). Per Amendment A, T1 is a no-IR template/copy-patch baseline that patches its own simple code under fixed conventions; the RegAlloc Team does NOT serve T1 and adds no allocator machinery or carve-outs for it.

In scope:

- Liveness analysis over the machine-level representation produced by codegen lowering.
- Live interval construction (and interference-derived structures) for virtual registers.
- Register assignment: physical register selection per register class, respecting operand constraints and fixed conventions.
- Spill slot allocation, including slot reuse and frame-size contribution.
- Spill/reload insertion, emitted as edits under the MIR contract — the allocator never rewrites instruction semantics.
- GC-reference liveness tracking: which references are live in which register or spill slot at every safepoint.
- Stack map contribution data: per-safepoint reference locations, handed to codegen's stack map finalizer.
- Register pressure reporting, which feeds the late pass "register pressure estimation" (suite item 118, owned by passes) and allocation diagnostics.
- Allocation determinism and replayability (Rule 124).

If the NaN boxing representation contract (Part XVIII) is ever enabled, the allocator must additionally handle tagged-value register classes: values that may be a reference or a primitive carried in one tagged slot must remain visible to GC maps at safepoints. This is a cross-team contract requiring approval from all affected teams (interpreter, baseline_noir, ir, passes, regalloc, codegen, aot, GC/runtime); it is disabled by default with a global kill switch, and tagged-class support stays behind that flag until the contract is approved.

Not owned: lowering and machine code emission (codegen), the IR and pass suite, the interpreter, the baseline JIT, the AOT driver, and the runtime/GC itself.

---

## Owned Write Paths

```text
compiler/regalloc/
tests/regalloc/
docs/regalloc_contract.md
```

---

## Forbidden Write Paths

```text
compiler/interp/
compiler/baseline/
compiler/ir/core/
compiler/passes/
compiler/codegen/
compiler/aot/
runtime/
```

The team never writes outside its owned paths, even for one-line fixes. If the MIR arriving from codegen lacks information the allocator needs, send a message under `messages/` per `docs/teams/messaging.md`; the owning team resolves it. `docs/regalloc_contract.md` (MIR input format, spill/reload edit format, stack map contribution schema) is this team's published cross-team contract — changes to it are announced, never imposed by editing other teams' trees.

---

## Key Laws

- Rule 26 — No Silent Fallbacks Without Telemetry: the rule names register allocation explicitly — when allocation spills excessively or falls back, the event MUST be recorded in telemetry/profile data.
- Rule 27 — No Assumption of Stable Hardware: register files, register class sizes, and calling conventions are queried via the `Target` interface at runtime (JIT) or build time (AOT); no hard-coded register assumptions.
- Rule 38 — Replay Logs Retained for All CI Failures: allocation failures must be replayable from the retained artifact (IR snapshot, options, target description, RNG seed, dependency versions).
- Rule 45 — No Aggressive Pass Without a Cost Model: spill vs. keep decisions, slot reuse, and split heuristics are justified by a cost model built from target latencies, memory costs, and safepoint/deopt costs — never by unmeasured intuition.
- Rule 61 — No Performance-Agnostic Implementation: the allocator is hot-path compiler code — no per-function allocations, exceptions, RTTI, or virtual dispatch in allocation loops.
- Rule 86 — All GC References in JIT Code Must Be Tracked: every reference register across a call/safepoint must be in a stack map; every reference spill must be visible to GC; references must not hide in untracked integer registers.
- Rule 89 — All JIT Frames Must Be Walkable: the allocator contributes frame size, saved-register locations, callee-saved register locations, and stack map data so GC/deoptimizer/profiler/debugger can walk every frame it allocated.
- Rule 124 — Compilation Must Be Deterministic and Replayable: identical input (same MIR, version, flags, target description, seed) produces identical allocation; nondeterminism sources are logged.
- Rule 125 — No Hidden Global Mutable State: allocator state is per-compilation; only immutable configuration and read-only target descriptions are global.
- Rule 128 — Register Allocation Must Be GC-Reference Safe: GC references are not lost across calls/safepoints; reference spills are tracked; register maps are generated; callee-saved/caller-saved conventions are respected; reference registers do not alias untracked integer registers unless allowed by the object representation.
- Part XVIII — NaN Boxing Rule: tagged representation must not break GC reference tracking, stack maps, or deopt reconstruction; tagged register classes exist only behind the disabled-by-default contract with a global kill switch, and require approval from all affected teams.

---

## Deliverables

1. Liveness analyzer — computes live-in/live-out and reference liveness over the machine-level input.
2. Live interval builder — intervals (or equivalent structures) per virtual register, including intervals for reference-tagged values.
3. Register allocator — physical register assignment per register class, honoring operand constraints and the Target-provided register file.
4. Spill logic — spill slot allocation, slot reuse, and spill/reload edit generation under the MIR contract, driven by the Rule 45 cost model.
5. GC reference spill tracking — every spilled reference recorded so GC can find it (Rules 86, 128).
6. Stack map contribution API — per-safepoint reference location data handed to codegen's stack map finalizer (Rules 86, 89).
7. Allocation diagnostics — spill counts, pressure curves, fallback reasons; excessive spilling is telemetered (Rule 26).
8. Deterministic allocation mode — bit-identical allocation for identical inputs, with logged nondeterminism exceptions (Rule 124) and replay support (Rule 38).
9. RegAlloc golden tests — checked-in input/expected allocation fixtures under `tests/regalloc/`.

---

## Interface Contract

### Consumes

- Machine-level input from codegen (per `docs/regalloc_contract.md`): virtual registers, register classes, operand constraints, call clobber information, safepoint locations, GC reference tags, and FrameState-associated references.
- Target register descriptions via the `Target` interface: physical registers per class, callee/caller-saved split, calling conventions, reserved registers (Rule 27).
- Compilation budgets and pressure-query hooks from the pass pipeline (feeds pass 118, register pressure estimation).

### Outputs

- Physical register assignment for every virtual register.
- Spill slot assignment and frame-size contribution.
- Spill/reload edits, expressed strictly as MIR-contract edits — no ad-hoc instruction rewriting.
- GC reference location maps: register or slot, per safepoint.
- Stack map contribution data for codegen's finalizer.
- Allocation diagnostics and register pressure reports (consumed by the passes team's pressure-estimation pass and by telemetry).
- The published contract `docs/regalloc_contract.md`.

### Promises

- No semantic changes to instructions: allocation is a reassignment of storage, never a reordering or rewriting of effects (Rule 127 discipline).
- No GC reference is lost across calls or safepoints; every live reference is mapped (Rules 86, 128).
- Deterministic allocation for identical inputs (Rule 124); nondeterminism, if any, is documented and logged.
- No hidden global mutable state (Rule 125).

---

## Testing Responsibilities

```text
tests/regalloc/
```

- Liveness correctness: expected live sets on hand-built MIR fixtures, including loops, branches, and exception edges.
- GC reference survival across calls: every reference live across a call/safepoint appears in the stack map contribution; none hidden in untracked integer registers (Rules 86, 128).
- Spill correctness: values identical whether spilled or kept in registers; slot reuse never aliases live values.
- Stack map contribution correctness: per-safepoint location data matches the allocated code.
- Register class constraints: fixed-constraint operands, tied operands, and class-restricted values are assigned legal registers.
- Clobber correctness: values live across calls respect caller-saved clobbering; callee-saved conventions respected.
- Deterministic replay: identical inputs produce identical allocation, verified in CI (Rule 124).
- Allocation failure handling: unallocatable inputs fail the compilation with telemetry, never miscompile (Rules 26, 81 discipline: compilation fails and falls back).
- Tagged-register-class tests, gated behind the NaN boxing flag (Part XVIII) — skipped, not skipped-silently, when the contract is disabled.

Test names are self-documenting (Rule 41); every bug fix adds five regression tests (Rule 34).

---

## When To Send Messages

- Codegen's MIR lacks liveness-relevant information (missing defs/uses, unconstrained operands) — RFC to codegen.
- GC reference tags are missing or inconsistent on values that reach safepoints — RFC to codegen/passes.
- Call clobber information is incomplete for a call shape the allocator must keep values across.
- Safepoint metadata is missing at a location the frame walk requires (Rules 88, 89).
- FrameState-associated references are not representable in the stack map contribution schema.
- Tagged-value register classes are needed for the NaN boxing representation — cross-team contract; requires approval from all affected teams per Part XVIII before any non-flagged work.
- Allocation diagnostics or pressure report schema changes — the passes team's pressure-estimation pass and telemetry consumers must be advised.

Bugs in allocator code are owned and fixed by this team regardless of who reports them; if a fix affects other teams, an ADVISORY accompanies it. The team never writes into another team's paths, regardless of severity.

---

## Reviewer Checklist

- [ ] No GC reference is lost across any call or safepoint; every live reference appears in the stack map contribution (Rules 86, 128).
- [ ] Every reference spill is tracked and visible to GC; no reference hides in an untracked integer register (Rules 86, 128).
- [ ] Stack map contribution data is generated for every safepoint the frame walk needs (Rules 89, 128).
- [ ] The allocator is deterministic for identical inputs; nondeterminism is logged (Rule 124).
- [ ] No instruction is semantically changed: spill/reload and assignment are pure storage decisions; effect order, exception edges, and numeric semantics are untouched (Rule 127 discipline).
- [ ] Spill decisions trace to the cost model, not intuition; thresholds are named, tunable constants (Rules 23, 45).
- [ ] Excessive spilling and allocation fallbacks emit telemetry — no silent degradation (Rule 26).
- [ ] No hidden global mutable state; per-compilation state only (Rule 125).
- [ ] Register assumptions come from the `Target` interface, never hard-coded (Rule 27).
- [ ] Allocation failure paths are tested: fail-with-telemetry, never miscompile.
- [ ] Golden tests updated for every behavior change; bug fixes carry five regression tests (Rules 34, 41).
- [ ] Tagged-class changes stay behind the NaN boxing flag with a global kill switch and cross-team approval (Part XVIII).
- [ ] No file outside the owned write paths is modified; cross-team requests went through `messages/`.
