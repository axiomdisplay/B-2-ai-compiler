# Codegen Team Charter

```text
Normative reference: docs/laws.md
(B-2 Java Runtime Compiler Laws & Architecture Specification, as amended by
Amendment A, Amendment B, and the Special Pass Laws in Part XVIII.)
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

Team size: 2

Roles:

- `CODEGEN-IMPL` — Implementer. Produces lowering, instruction selection, target backends, safepoint/deopt/exception emission, stack map finalization, machine code serialization, and code metadata inside the team's owned write paths.
- `CODEGEN-REV` — Reviewer. Checks every change for semantics-preservation bugs, GC/deopt/safepoint discipline, security compliance, ABI compliance, boundary violations, and missing tests/docs. Does not rewrite code unless explicitly reassigned.

---

## Mission

The Codegen Team owns lowering and final machine code emission for the optimizing tiers. It serves T2, the optimizing JIT, and shares its lowering with the T3 AOT pipeline (Amendment B: AOT reuses the T2 pipeline offline, so lowering has a runtime-detected JIT mode and a build-time-validated AOT mode for target features — Rule 129). T1's template/copy-patch emission belongs to the baseline_noir team, NOT here: nothing in `compiler/codegen/` is wired into the no-IR baseline, and this team adds no baseline carve-outs (Amendment A).

In scope:

- Lowering optimized IR to the machine-level representation (MIR) consumed by regalloc.
- Instruction selection and target-specific lowering — the ONLY place target-specific logic lives (Rule 24: generic passes stay target-agnostic; codegen owns the target backends behind the `Target` interface).
- ABI compliance: calling conventions, stack alignment, callee/caller-saved discipline on every runtime and Java-to-Java transition (Rule 92).
- Deopt stub emission and deopt metadata production (Rule 81).
- Exception table emission.
- Safepoint poll emission at every location Rule 88 requires.
- Stack map assembly: consuming regalloc's contribution data and finalizing GC reference maps (Rules 86, 128).
- Machine code serialization into the code cache with patch-safe code layout and W^X-compatible output design (Rules 97-99).
- Code metadata emission: dependencies, target features, publication data (Rules 101, 129, 98).

Codegen must support what the Part XVIII first-class passes require: SIMD/vector instruction emission for SWLP — preserving exact Java numeric semantics, no fast-math, feature-gated per Rule 129 — and tagged-value move/conversion primitives for the NaN boxing lowering if that cross-team contract is ever enabled (disabled by default; global kill switch; approval from all affected teams required per Part XVIII).

Not owned: the IR and pass suite, register allocation, the interpreter, the baseline JIT, the AOT driver, the runtime, and the code cache runtime machinery itself.

---

## Owned Write Paths

```text
compiler/codegen/
tests/codegen/
docs/codegen_contract.md
```

---

## Forbidden Write Paths

```text
compiler/interp/
compiler/baseline/
compiler/ir/core/
compiler/passes/
compiler/regalloc/
compiler/aot/
runtime/
```

The team never writes outside its owned paths, even for one-line fixes. If the IR lacks metadata lowering needs, or regalloc output is incomplete, send a message under `messages/` per `docs/teams/messaging.md`; the owning team resolves it. `docs/codegen_contract.md` (MIR format produced for regalloc, metadata formats emitted, target backend interface) is this team's published cross-team contract — changes are announced, never imposed by editing other teams' trees.

---

## Key Laws

- Rule 24 — No Target-Specific Hacks in Generic Passes: all target knowledge lives here behind the `Target` interface; codegen is the single sanctioned home of target-specific logic, and must not leak it upward.
- Rule 81 — Deoptimization Metadata Is a Required Compilation Output: deopt points, FrameState snapshots, stack maps, GC reference maps, live-range info, materialization plan, interpreter re-entry, dependencies, guard, exception-state, and monitor-state reconstruction info; if it cannot be generated, the compilation fails and falls back.
- Rule 86 — All GC References in JIT Code Must Be Tracked: no raw references in registers, stack slots, or constants across safepoints unless recorded in GC maps; embedded references use handles or tracked forms.
- Rule 88 — Generated Code Must Poll Safepoints: polls at loop backedges, calls, allocation sites, long native transitions, OSR entry/exit, tier transitions, invalidation points, and monitor wait/park points; bounded latency.
- Rule 89 — All JIT Frames Must Be Walkable: frame metadata carries size, return address location, saved registers, stack map, deopt info, callee-saved locations, Java frame association, and transition markers.
- Rule 92 — Runtime Call Transitions Must Preserve ABI and Runtime State: calling convention, stack alignment, callee-saved registers, GC state, pending exception state, thread state, JNI environment, monitor state.
- Rule 97 — Executable Memory Must Be W^X: never simultaneously writable and executable; staging pages, protection changes, or platform-approved mechanisms.
- Rule 98 — Code Publication Must Be Atomic: entry points, OSR entries, trampolines, and metadata pointers publish atomically with release semantics; consumers acquire; no partially initialized code or missing GC maps/deopt info is observable.
- Rule 99 — Runtime Patching Must Be Safe Against Concurrent Execution: aligned, architecturally safe patch sites; no invalid intermediate instructions; instruction-cache coherence handled.
- Rule 102 — Generated Code Must Be Constrained: approved runtime entrypoints only; no arbitrary syscalls, out-of-frame writes, or sandbox bypass.
- Rule 103 — Platform Exploit Mitigations Must Be Enabled Where Available: non-executable stack/heap, CFI, shadow stacks, PAC/BTI, CET, ASLR-safe codegen; unavailable mitigations documented and configurable.
- Rule 104 — JIT Spraying Defenses Are Required: constant blinding, controlled immediate embedding, separated code and data, limited constant islands, validated codegen inputs.
- Rule 127 — Backend Lowering Must Preserve IR Semantics: effect order, exception semantics, numeric semantics, overflow behavior, JMM fence/volatile semantics, GC reference liveness, safepoint placement, deopt point mapping, stack layout constraints, monitor state.
- Rule 128 — Register Allocation Must Be GC-Reference Safe: codegen supplies the reference tags, clobber info, and safepoint locations that make the allocator's job possible, and finalizes the resulting maps with it.
- Rule 129 — Target Features Must Be Gated and Recorded: runtime-detected for JIT, build-time validated for AOT, recorded in code metadata, feature-guarded; no unsupported instructions ever execute.
- Part XVIII — SWLP Rule: SIMD emission must preserve Java numeric semantics exactly (NaN, negative zero, division-by-zero, exact conversions); no fast-math; masked/partial vectors and scalar fallbacks emitted as the pass demands. NaN Boxing Rule: tagged move/conversion primitives must not break GC reference tracking, stack maps, deopt reconstruction, or bit-level FP behavior; disabled by default behind the cross-team contract.

---

## Deliverables

1. IR-to-MachineIR lowering — semantics-preserving translation of optimized IR into the MIR consumed by regalloc (Rule 127).
2. Instruction selector — pattern selection per target, driven by target cost models.
3. Target backend interface — the `Target`-rooted backend abstraction (register files, conventions, encoders) that keeps target specifics inside this tree (Rule 24).
4. Safepoint emitter — polls at every Rule 88 location, with bounded latency.
5. Deopt stub emitter — deopt points, stubs, and re-entry into T0 state reconstruction (Rules 4, 81).
6. Exception table emitter — handler tables matching IR exception edges exactly.
7. Stack map finalizer — merges regalloc contribution data into final GC reference maps (Rules 86, 128).
8. Machine code serializer — W^X-compatible, patch-safe code layout into the code cache (Rules 97-99).
9. Code metadata emitter — dependency records, target feature records, publication descriptors (Rules 101, 129, 98).
10. Codegen golden tests — checked-in lowering/emission fixtures under `tests/codegen/`.

---

## Interface Contract

### Consumes

- Optimized IR from passes: guards, `FrameState`, speculative and dependency metadata, vector nodes from SWLP, materialization metadata from PEA, tagged nodes from NaN boxing lowering when enabled.
- IR contracts and public APIs from ir (`include/b2/ir/`, `docs/ir_spec.md`, `docs/effect_system.md`).
- Register allocation results from regalloc: physical assignments, spill slots, spill/reload edits, GC reference location maps, stack map contribution data.
- Target descriptions via the `Target` interface: features, register files, conventions, exploit mitigation availability (Rules 24, 27, 103).

### Outputs

- Machine code artifact, laid out patch-safe and W^X-compatible.
- Stack maps and GC reference maps.
- Deopt metadata (complete Rule 81 set) and deopt stubs.
- Exception tables.
- Safepoint metadata.
- Dependency metadata for the invalidation watchdog (Rule 101) and target feature metadata (Rule 129).
- The MIR handed to regalloc, per `docs/regalloc_contract.md`, and the published `docs/codegen_contract.md`.

### Promises

- Lowering preserves IR semantics in full (Rule 127) — any necessary deviation is a bug or a documented law-approved contract change, never a silent one.
- Output is W^X-compatible and atomically publishable with release semantics (Rules 97, 98).
- Deopt points and metadata are emitted wherever the IR requires them; a compilation that cannot produce them fails (Rule 81).
- GC references are visible to runtime metadata at every safepoint (Rules 86, 128).
- Target features are gated and recorded; unsupported instructions never execute (Rule 129).
- Generated code calls only approved runtime entrypoints (Rule 102).

---

## Testing Responsibilities

```text
tests/codegen/
```

- Lowering golden tests: IR in, MIR/machine code out, covering effect order, exception edges, numeric edge cases (NaN, negative zero, overflow, division by zero), JMM fences, monitor operations (Rule 127).
- ABI tests: call frames, stack alignment, callee-saved restoration, runtime helper transitions (Rule 92).
- Stack map tests: finalized maps match allocated code at every safepoint; reference locations correct (Rules 86, 128).
- Deopt stub tests: every deopt point reconstructs exact T0 state (Rules 4, 81).
- Safepoint placement tests: polls at all Rule 88 locations; bounded latency.
- Exception table tests: every IR exception edge has a correct handler entry.
- Target-feature gating tests: features runtime-detected (JIT) / build-validated (AOT), recorded, guarded; mismatch refuses to run (Rule 129).
- Patch-safe layout tests: patch sites aligned and architecturally safe; patch-under-load stress (Rule 99).
- W^X compatibility tests: no page simultaneously writable and executable (Rule 97).
- SIMD emission correctness for SWLP shapes: lane ops, masks, reductions, gathers/scatters, tails — Java numeric semantics exact (Part XVIII).
- Tagged-move tests, gated behind the NaN boxing flag: tag/untag moves and conversions preserve bit-level FP behavior and GC visibility (Part XVIII).

Test names are self-documenting (Rule 41); every bug fix adds five regression tests (Rule 34).

---

## When To Send Messages

- The IR lacks metadata needed for lowering (a node kind or effect/guard/FrameState information without a lowering contract) — RFC to ir/passes.
- Passes emit incomplete FrameState or deopt-relevant metadata — RFC to passes.
- Regalloc output lacks stack map information or violates the MIR edit contract — RFC to regalloc.
- Runtime ABI expectations are unclear (helper signatures, transition state) — RFC to the runtime owner.
- A target backend requires a new or changed contract (new architecture, new feature gates, new mitigation) — announce and coordinate.
- Generated code cannot preserve Java semantics without a change elsewhere (IR, passes, regalloc, runtime) — escalate; never paper over with a local semantics hack.
- SIMD or NaN-boxing emission contracts change — cross-team; NaN boxing requires approval from all affected teams per Part XVIII.
- Code metadata or publication formats change — aot and runtime consumers must be advised.

Bugs in codegen are owned and fixed by this team regardless of who reports them; if a fix affects other teams, an ADVISORY accompanies it. The team never writes into another team's paths, regardless of severity.

---

## Reviewer Checklist

- [ ] Lowering preserves effect order and every item of the Rule 127 list; no silent semantics change.
- [ ] Java exception semantics preserved: every exception edge lowered to a correct handler entry and exception-state reconstruction.
- [ ] GC references tracked across safepoints; finalized stack maps complete and correct (Rules 86, 128, 89).
- [ ] Safepoints emitted at every Rule 88 location, with bounded latency.
- [ ] Deopt metadata complete per Rule 81; deopt stubs reconstruct exact T0 state; failures fail the compilation rather than emit broken code.
- [ ] Target features gated, guarded, and recorded; no ungated or unrecorded feature use (Rule 129).
- [ ] No W^X-violating design: staging/protection strategy documented for every emission path (Rule 97); publication is atomic with release/acquire (Rule 98); patch sites are concurrency-safe (Rule 99).
- [ ] Generated code constrained to approved entrypoints; exploit mitigations and JIT-spraying defenses present or documented-and-configurable (Rules 102-104).
- [ ] ABI preserved on all transitions, including runtime helpers (Rule 92).
- [ ] No target-specific logic leaked into generic code owned by others; all target knowledge stays behind the `Target` interface (Rule 24).
- [ ] Golden tests updated for every lowering/emission change; bug fixes carry five regression tests (Rules 34, 41).
- [ ] SIMD emission preserves exact Java numeric semantics; tagged moves stay behind the NaN boxing flag (Part XVIII).
- [ ] No file outside the owned write paths is modified; cross-team requests went through `messages/`.
