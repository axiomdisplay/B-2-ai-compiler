# AOT Team Charter

```text
Normative reference: docs/laws.md
(B-2 Java Runtime Compiler Laws & Architecture Specification, as amended by
Amendment A, Amendment B, and the Special Pass Laws in Part XVIII.)
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

Team size: 2

Roles:

- `AOT-IMPL` — Implementer. Produces the offline compilation driver, pipeline orchestration, proof/PGO/closed-world input handling, artifact writing and validation, manifests, versioning, and AOT golden tests inside the team's owned write paths.
- `AOT-REV` — Reviewer. Checks every change for unproven assumptions, manifest/validation completeness, law compliance, boundary violations, and missing tests/docs. Does not rewrite code unless explicitly reassigned.

---

## Mission

The AOT Team owns the T3 AOT / Static JIT driver and its artifacts. Per Amendment B.1: T3 "uses the same T2 optimizer pipeline, offline. Input: classfiles, static proofs, PGO if available, and closed-world assumptions if available. Output: native code, stack maps, deopt metadata or static no-deopt metadata, a dependency manifest, and a compatibility manifest." The AOT team does NOT build a separate optimizer: it drives the shared pipeline owned by passes (with codegen lowering and regalloc underneath) and concentrates on what is unique to T3 — offline orchestration, static inputs, and deployable artifacts.

CRITICAL RULE from Amendment B: AOT cannot assume dynamic Java behavior is impossible unless mechanically proven. If proof fails, AOT must either emit guarded code or leave the method for T0/T1/T2 (Rule 80 restates the discipline; Rule 131 forbids human-only proof). Deopt direction on dynamic invalidation is T3 → T2 or T0 (Amendment B.3); T0 is always final fallback.

In scope:

- The AOT driver and offline compilation orchestration (batching, scheduling, budgets, determinism of builds).
- Input handling: classfiles, mechanically checkable static proof artifacts, PGO profiles if available, closed-world assumptions if available — all untrusted (Rule 105).
- Static proof consumption and checking integration: proofs are verified mechanically before any optimization relies on them (Rule 131).
- Closed-world analysis integration.
- Artifact generation: native code, stack maps, deopt metadata or static no-deopt metadata, dependency manifest, compatibility manifest (Rules 101, 107).
- Artifact validation on load (Rule 108) and artifact versioning/invalidation (Rule 31).
- Target handling in offline mode: features are build-time validated and recorded (Rule 129), never runtime-detected.

Not owned: the optimizer pipeline and pass suite (passes), lowering and code emission (codegen), register allocation (regalloc), the IR, the interpreter, the baseline JIT, and the runtime that loads artifacts.

---

## Owned Write Paths

```text
compiler/aot/
tests/aot/
docs/aot_contract.md
```

---

## Forbidden Write Paths

```text
compiler/interp/
compiler/baseline/
compiler/ir/core/
compiler/passes/
compiler/regalloc/
compiler/codegen/
runtime/
```

The team never writes outside its owned paths, even for one-line fixes. The shared T2 pipeline, lowering, and allocation are driven, never edited: changes there go through `messages/` per `docs/teams/messaging.md` as RFCs to the owning teams. `docs/aot_contract.md` (artifact format, manifest schemas, driver inputs, validation rules) is this team's published cross-team contract — changes are announced, never imposed by editing other teams' trees.

---

## Key Laws

- Rule 31 — No Persistent State Without Versioning: AOT artifacts are versioned caches; a change in IR format, bytecode or class-file version, pass order, dependency schema, GC object layout, or runtime ABI invalidates them.
- Rule 79 — No Assumptions About Hashes, Randomness, or Addresses: artifacts must not bake identity hashes, iteration order, ASLR/object/code addresses, compressed-oop bases, or randomized runtime values; address-dependent content must be relocated or validated at load.
- Rule 80 — Static Typing Is Not Runtime Proof Unless Certified: static proofs may rely only on final/sealed guarantees, closed-world assumptions, module isolation, absence of reflection and dynamic loading, verified JNI boundaries, verified absence of unsafe escapes, and mechanically checked artifacts; otherwise guards or fallback.
- Rule 105 — Profiles, Bytecode, and AOT Artifacts Are Untrusted: all inputs validated before use; malformed inputs must not cause UB, memory corruption, arbitrary execution, crashes, or silent miscompilation; invalid artifacts rejected with telemetry.
- Rule 107 — AOT Artifacts Must Include a Compatibility Manifest: Java version, class-file version, module compatibility data, IR version/hash, compiler version, pass pipeline hash, target architecture, target feature set, ABI hash, runtime configuration hash, dependency fingerprints, security policy version, creation metadata.
- Rule 108 — AOT Artifacts Must Be Verified Before Loading: manifest compatibility, integrity checksum/signature, dependency validity, target feature support, ABI compatibility, security policy compatibility; on mismatch, reject — silent loading of incompatible artifacts is forbidden.
- Rule 129 — Target Features Must Be Gated and Recorded: build-time validated for AOT and recorded in code metadata; generated code must not execute unsupported instructions.
- Rule 131 — Static Proofs Must Be Mechanically Checked: T3 static optimizations never rely on human-only proof; proofs are machine-checkable artifacts or verifier constraints, else the optimization uses guards or is disabled.
- Rule 133 — Differential Oracle Testing Must Run Continuously: CI compares T3/AOT against Tier 0, Tier 1, Tier 2, and the approved Java reference oracle, across exceptions, JNI, dynamic class loading, reflection, invokedynamic, class mutation, GC stress, and numeric edge cases.
- Amendment B — Tier Model Redesign: T3 is the offline reuse of the T2 pipeline, not a separate optimizer (B.1, B.5); AOT emits guarded code or leaves methods for T0/T1/T2 when proof fails; deopt direction on dynamic invalidation is T3 → T2 or T0 (B.3); the full pass suite binds T2 and T3 only (B.4).
- Part XVIII — NaN Boxing Rule: if NaN boxing is ever enabled, AOT artifacts require versioning for the tagged representation; the contract is disabled by default with a global kill switch and needs approval from all affected teams.

---

## Deliverables

1. AOT driver — the offline T3 entry point: input discovery, batch orchestration, error reporting, deterministic builds.
2. Offline pipeline orchestration — drives the shared T2 pipeline in Static Mode (passes team's scheduler) with AOT budgets and proof-aware filters.
3. Static proof consumption and checking integration — loads proof artifacts and mechanically verifies them before any optimization may rely on them (Rule 131).
4. Closed-world analysis integration — consumes closed-world assumptions, tracks exactly what was assumed, and records it in the dependency manifest.
5. PGO import — validated profile ingestion for offline mode (Rule 105), with confidence fields honored.
6. Artifact writer — emits native code, stack maps, deopt or static no-deopt metadata, and manifests into a single versioned artifact.
7. Dependency manifest generator — per-method dependency records sufficient for invalidation (Rule 101 discipline), including closed-world and proof dependencies.
8. Compatibility manifest generator — the complete Rule 107 field set.
9. Artifact reader/validator — load-time verification per Rule 108: manifest compat, integrity, dependencies, target features, ABI, security policy; rejection with telemetry, never silent acceptance.
10. No-deopt metadata or guarded deopt metadata — static no-deopt metadata only when mechanically proven; otherwise guarded deopt metadata or method-level fallback to T0/T1/T2.
11. Artifact versioning — format versioning and invalidation on IR/pass/ABI/GC-layout/feature changes (Rule 31).
12. AOT golden tests — checked-in artifact fixtures under `tests/aot/`.

---

## Interface Contract

### Consumes

- Java classfiles, static proof artifacts, PGO profiles, and closed-world assumptions — all untrusted inputs, validated before use (Rule 105).
- The shared T2 optimizer pipeline (owned by passes) driven in Static Mode, with codegen lowering and regalloc underneath; AOT supplies inputs, budgets, and target descriptions, and consumes the outputs.
- Build-time target descriptions via the `Target` interface: architecture, feature set, ABI (Rules 24, 27, 129).
- The invalidation/deopt machinery contract (runtime-side, via messages): T3 → T2 or T0 on dynamic invalidation (Amendment B.3).

### Outputs

- Deployable native artifacts: native code, stack maps, deopt or static no-deopt metadata, dependency manifest, compatibility manifest.
- Rejection/validation telemetry for malformed or incompatible artifacts (Rules 105, 108).
- The published contract `docs/aot_contract.md`: artifact format, manifest schemas, driver inputs, validation rules.

### Promises

- No unproven assumptions baked in: every static assumption is mechanically proven, guarded, or the method is left for T0/T1/T2 (Rules 80, 131; Amendment B).
- Proofs are mechanically checked — no human-only certification (Rule 131).
- Guarded code or method-level fallback exists for every optimization whose proof fails.
- Artifacts are versioned and invalidated by IR/pass/ABI/GC-layout changes (Rule 31) and validated at load (Rule 108).
- No address-, hash-, or randomness-dependent assumptions persist in artifacts unless relocated/validated at load (Rule 79).
- Offline builds are deterministic and replayable for identical inputs (Rule 124).

---

## Testing Responsibilities

```text
tests/aot/
```

- Artifact round-trip: write then read then execute; byte-stable for identical inputs (Rules 124, 31).
- Manifest completeness: every Rule 107 field present and validated.
- Stale artifact rejection: version/manifest mismatch refuses to load (Rules 31, 108).
- Corrupt artifact rejection: fuzzed and truncated artifacts rejected safely with telemetry — no UB, no crash, no silent miscompilation (Rule 105).
- Target-feature mismatch rejection: artifact features exceed load-time machine — reject (Rules 108, 129).
- Guarded-code fallback behavior: dynamic invalidation of an AOT assumption transitions T3 → T2 or T0 correctly (Amendment B.3).
- Differential T3 vs T0/T2 runs (and the approved Java reference oracle where applicable) per Rule 133: exceptions, JNI, reflection, invokedynamic, class mutation, GC stress, numeric edge cases including NaN, negative zero, overflow, division by zero.
- Versioning invalidation tests: change IR version / pass pipeline hash / ABI hash / GC layout — artifact invalidates (Rule 31).
- Closed-world violation tests: dynamic class loading or reflection after AOT trips the recorded dependency and invalidates or deopts (Rules 80, 101).

Test names are self-documenting (Rule 41); every bug fix adds five regression tests (Rule 34).

---

## When To Send Messages

- The shared pipeline's output shape changes in a way AOT consumes — RFC to passes (and ADVISORY when they change it first).
- Codegen lowering needs AOT-specific modes (build-time feature validation, no-deopt emission, guarded-code layout) — RFC to codegen.
- Manifests need runtime-side validation support at load time — RFC to the runtime owner; Rule 108 verification is a shared contract.
- Proof artifacts are insufficient for a required optimization — escalate rather than assume (Rules 80, 131): the method falls back or carries guards in the meantime.
- NaN boxing artifact versioning changes — cross-team; requires approval from all affected teams per Part XVIII.
- Artifact format or manifest schema changes — downstream consumers (runtime loader, tooling) must be advised.

Bugs in AOT driver/artifact code are owned and fixed by this team regardless of who reports them; if a fix affects other teams, an ADVISORY accompanies it. The team never writes into another team's paths, regardless of severity.

---

## Reviewer Checklist

- [ ] No unproven dynamic-behavior assumptions baked into any artifact: proven, guarded, or left for T0/T1/T2 (Rule 80; Amendment B).
- [ ] Every static proof is mechanically checked — no human-only certification anywhere (Rule 131).
- [ ] Compatibility manifests are complete per the full Rule 107 field list.
- [ ] Load-time validation enforces every Rule 108 check; incompatible artifacts are rejected with telemetry, never silently loaded.
- [ ] A guarded fallback exists for every unproven optimization; T3 → T2/T0 invalidation path is exercised by tests (Amendment B.3).
- [ ] Artifacts are versioned and invalidated by IR/pass/ABI/GC-layout/feature changes (Rule 31).
- [ ] No address-, hash-, or randomness-dependent assumptions persist without load-time relocation/validation (Rule 79).
- [ ] Untrusted inputs (classfiles, proofs, profiles, artifacts) are validated before use; fuzz/corruption tests present (Rule 105).
- [ ] Differential T3 vs T0/T2 (and oracle) tests included and current (Rule 133).
- [ ] Target features build-time validated and recorded (Rule 129); closed-world assumptions recorded in the dependency manifest.
- [ ] Offline builds deterministic; golden artifacts updated with every format change (Rule 124).
- [ ] No file outside the owned write paths is modified; the shared pipeline is driven, not edited; cross-team requests went through `messages/`.
