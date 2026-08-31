# Baseline No-IR Team Charter

T1 — No-IR Baseline JIT

```text
Normative reference: docs/laws.md
(B-2 Java Runtime Compiler Laws & Architecture Specification, as amended by
Amendment A, Amendment B, and the Special Pass Laws in Part XVIII.)
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

Team size: 2

Roles:

- `BASELINE-IMPL` — Implementer. Produces all Tier 1 template/copy-and-patch compiler code, patch-site infrastructure, tests, and the T1 contract inside the team's owned write paths.
- `BASELINE-REV` — Reviewer. Checks every change for correctness bugs, law compliance, boundary violations, missing tests, missing documentation, and missing message references. Does not rewrite code unless explicitly reassigned.

---

## Mission

The Baseline No-IR Team owns Tier 1 (T1), the no-IR baseline JIT: a very fast compiler that does NOT build an IR graph. Permitted implementation forms are a template JIT, a copy-and-patch JIT, a linear bytecode-to-native expansion, or a compiler working directly from RBC to machine code (Amendment A; Amendment B.1). T1 exists to remove interpreter dispatch overhead and deliver startup/warmup speed without paying IR construction cost. Compilation is linear or near-linear, strictly budget-bounded, and always safe to abandon or fall back from; deopt direction is T1 to T0 on an unsupported trap (Amendment B.3).

T1's value is speed of compilation, not optimization of the compiled code. What T1 does: patch locals/register slots, field offsets, constant pool entries, call targets, inline-cache stubs, and deopt metadata at patchable sites; emit simple stack maps and basic safepoints; and use lightweight inline caches. What T1 must NEVER do:

- No sea-of-nodes — no IR graph is constructed, ever.
- No global dataflow — no analysis that requires whole-method dataflow.
- No aggressive optimization — no inlining policy, no devirtualization beyond inline caches, no lock elision, no range-check elimination.
- No speculative effect reordering — Java-visible effect order is preserved exactly.
- No loop transformations — no unrolling, no peeling, no interchange.
- No vectorization — not even local SIMD packing.
- No escape analysis — objects are allocated exactly as RBC specifies.

This is a hard architectural boundary, not a preference: T2 is the first IR tier and owns everything optimization-shaped (Amendment B.1, B.4). A T1 that quietly grows an optimizer is a law violation, not an enhancement.

Not owned by this team:

- The interpreter (`compiler/interp/`) and the T0 state contract it publishes — consumed, never modified.
- IR construction (`compiler/ir/core/`), optimization passes (`compiler/passes/`), register allocation (`compiler/regalloc/`), machine codegen infrastructure (`compiler/codegen/`), AOT (`compiler/aot/`).
- The runtime (`runtime/`): GC, tier management, code cache policy, class loading.
- Tier-promotion policy: T1 consumes heat signals, it does not decide them.

---

## Owned Write Paths

```text
compiler/baseline/
tests/baseline/
docs/baseline_contract.md
```

---

## Forbidden Write Paths

```text
compiler/interp/
compiler/ir/core/
compiler/passes/
compiler/regalloc/
compiler/codegen/
compiler/aot/
runtime/
```

The team never writes outside its owned paths, even for one-line fixes. Patch-site ABIs, stack map formats, and installation protocols that touch codegen or runtime are coordinated by message under `messages/` per `docs/teams/messaging.md`; the owning team resolves them.

---

## Key Laws

- Amendment A — No-IR Baseline Tier: the charter law for this team. T1 is exempt from IR-specific rules ONLY where those rules require an IR graph; T1 is NOT exempt from correctness, deoptimization, GC, safepoint, Java semantic, security, or performance laws. T1 must provide:
  1. deterministic compilation;
  2. linear state mapping to T0;
  3. deopt support;
  4. stack maps / GC maps;
  5. safepoint polling;
  6. fallback to T0;
  7. telemetry on fallback.
- Amendment B — Tier Model Redesign: T1 is the no-IR tier; deopt direction is T1 to T0 on unsupported trap; T0 is always the final fallback; the full pass suite binds T2/T3 only.
- Rule 4 — Deoptimization Must Reconstruct Tier 0 State: every T1 deopt point reconstructs the exact T0 state via the linear state map.
- Rule 6 — NO EXCEPTIONS ON THE HOT PATH: compiled-code fast paths and compile paths contain zero C++ `throw`; failures return `Error` variants and fall back to T0.
- Rule 7 — Zero-Allocation Hot Path: compile scratch uses arena allocation; generated fast paths do not allocate.
- Rule 8 — No RTTI: template selection and patch-site handling use `enum class` kinds.
- Rule 9 — No `std::shared_ptr` / `std::function` in Hot IR Code: same discipline for template descriptors and patch records.
- Rule 26 — No Silent Fallbacks Without Telemetry: every T1-to-T0 fallback, budget overrun, and unsupported construct is recorded.
- Rule 86 — All GC References in JIT Code Must Be Tracked: every live reference across a T1 safepoint or call is in a stack map.
- Rule 87 — Read and Write Barriers Must Be Correct: every reference store in T1 code executes the required barrier.
- Rule 88 — Generated Code Must Poll Safepoints: polls at loop backedges, calls, allocation sites, OSR and tier-transition points.
- Rule 89 — All JIT Frames Must Be Walkable: T1 frames carry frame size, return-address location, saved registers, stack map, deopt info.
- Rule 90 — Stack Overflow and Recursion Limits Must Be Checked: checks before T1 frame entry, producing `StackOverflowError`, not a crash.
- Rule 96 — Tier 0 Is the Universal Correctness Fallback: fallback to T0 is always available for any method T1 cannot handle.
- Rule 97 — Executable Memory Must Be W^X: template staging and executable pages are never simultaneously writable and executable.
- Rule 98 — Code Publication Must Be Atomic: entry points, trampolines, and metadata pointers publish with release semantics.
- Rule 99 — Runtime Patching Must Be Safe Against Concurrent Execution: patch sites are aligned and architecturally safe; critical for copy-and-patch.
- Rule 112 — Compilation Latency and Memory Budgets Must Be Defined: the T1 compile-latency budget is strictly bounded; violations trigger fallback, never mutator stalls.
- Rule 114 — Hotness Counters Must Be Robust: the heat signals T1 consumes are saturating and overflow-safe.
- Rule 119 — Tier Transitions Must Be Observable: T0-to-T1 and T1-to-T0 transitions are recorded with reasons and counters.
- Rule 124 — Compilation Must Be Deterministic and Replayable: same RBC, flags, and target produce identical T1 code; nondeterminism is logged.
- Part XVIII, NaN Boxing Rule — the baseline is an affected party to the cross-team representation contract: NaN-tagged values in T1 generic slots must remain GC-visible, deopt-reconstructible to exact T0 state, and must not change Java floating-point semantics; the feature is disabled by default and requires this team's approval plus baseline round-trip tests before it can be enabled.

---

## Deliverables

1. Template / copy-and-patch code emission: precompiled template bodies and their relocation records, per target architecture.
2. Linear RBC-to-native compiler path: opcode-by-opcode expansion with no IR graph and no optimization passes.
3. Patchable-site infrastructure: aligned, architecturally safe patch sites for locals/register slots, field offsets, constant pool entries, call targets, and inline-cache stubs (Rule 99).
4. Lightweight inline caches, including their patch/update path under concurrent execution.
5. Simple stack maps / GC maps sufficient for Rule 86 and Rule 89 frame walking.
6. Basic safepoint polls at the Rule 88 sites.
7. Deopt metadata expressed as a linear state map to T0 — the FrameState-equivalent required by Amendment A, sufficient for Rule 4 reconstruction.
8. Fallback-to-T0 trap path for any construct T1 cannot compile or execute.
9. Compile budget enforcement: latency and memory ceilings with cancellation, not stalls (Rule 112).
10. Telemetry on every fallback, budget overrun, and unsupported-construct trap (Rule 26, Rule 119).
11. Whole-tier kill switch: a runtime switch that disables T1 entirely and runs T0, verified by tests.

---

## Interface Contract

### Consumes

- Quickened RBC and the RBC opcode semantics from the interpreter team.
- The T0 exact-state contract (`docs/interp_contract.md`) that defines the deopt reconstruction target.
- Heat signals from tier management (invocation and backedge counters) triggering T1 compilation.
- Runtime services for installation, code cache, GC, and resolution — used, never modified.

### Outputs

- Installable native code with stack maps, safepoint polls, deopt metadata, and patch sites, published atomically (Rule 98).
- The T1 contract (`docs/baseline_contract.md`): patch-site ABI, stack map format, deopt metadata shape, fallback behavior.
- Fallback and tier-transition telemetry events for observability (Rule 26, Rule 119).

### Promises

- No optimization beyond interpreter-dispatch removal — the MUST-NOT list in the Mission is enforced.
- Deterministic, replayable compilation (Rule 124).
- Strictly bounded compile latency and memory (Rule 112).
- Exact T0-deopt equivalence: forced deopt from any T1 frame yields state identical to continued T0 execution (Rule 4).
- W^X compliance in all code and patch handling (Rule 97).
- Telemetry emitted on every fallback (Rule 26).

---

## Testing Responsibilities

```text
tests/baseline/
```

- Determinism and replay: identical inputs produce identical code; nondeterminism sources logged (Rule 124).
- Differential equivalence vs T0 over the Rule 133 matrix: normal programs, exceptions, dynamic class loading, reflection, invokedynamic, MethodHandles, GC stress, recursion limits, and numeric edge cases including NaN, negative zero, overflow, and division by zero.
- Stack map correctness: every live GC reference across every safepoint and call is mapped (Rule 86, Rule 89).
- Safepoint placement: polls at all Rule 88 sites with bounded latency (Rule 111).
- Patch safety under concurrent execution: patch-under-load and atomicity tests (Rule 98, Rule 99).
- W^X compliance: memory-protection tests for staging and executable pages (Rule 97).
- Fallback telemetry: every fallback path emits its event with reason (Rule 26).
- Deopt round-trip: forced-deopt tests proving T1-to-T0 state equality at every deopt point.
- Budget enforcement: latency and memory ceilings trigger fallback or cancellation, never stalls (Rule 112).
- Kill switch: T1 disabled end-to-end leaves the system fully functional on T0 (Rule 96).

---

## When To Send Messages

- The patch-site or code-installation ABI needs codegen or runtime coordination — RFC to codegen and runtime owners.
- The stack map format changes in a way that affects GC — ADVISORY to GC/runtime owners before landing.
- The deopt metadata shape changes — ADVISORY to the interpreter team and all deopt consumers.
- The T0 state contract is insufficient for a linear state mapping — RFC to the interpreter team with the specific gap.
- The NaN boxing representation contract is proposed or changed — this team participates in the cross-team approval required by Part XVIII and must verify T1 round-trip and deopt-to-T0 behavior before any enablement.
- A T1 bug may affect T2 entry correctness or the correctness of deopt into T0 — ADVISORY to ir and interpreter teams.
- A defect is found in another team's area — BUG message filed against that team; never patched here.

Bugs found in the baseline compiler's own area are owned and fixed by this team directly. If the fix affects other teams, an ADVISORY accompanies it. The team never writes into another team's paths, regardless of severity.

---

## Reviewer Checklist

- [ ] Truly no-IR: no IR graph is built, no optimization passes run, no global dataflow is computed; the Mission's MUST-NOT list is intact.
- [ ] All seven Amendment A provisions are present and tested: determinism, linear state mapping to T0, deopt support, stack maps/GC maps, safepoint polling, fallback to T0, fallback telemetry.
- [ ] Hot-path discipline holds in generated code and compile paths: no allocation on fast paths, no C++ exceptions, no RTTI, no `shared_ptr`/`function` (Rules 6-9).
- [ ] W^X is respected: no page is simultaneously writable and executable (Rule 97).
- [ ] Patch sites are aligned and architecturally safe; no invalid intermediate instruction sequences; I-cache coherence handled (Rule 99).
- [ ] Code and metadata publication is atomic with release/acquire semantics (Rule 98).
- [ ] Telemetry is emitted on every fallback, budget overrun, and unsupported-construct trap (Rule 26, Rule 119).
- [ ] The whole-tier kill switch works and is covered by tests (Rule 96).
- [ ] Differential tests vs T0 are green across the Rule 133 matrix, including exceptions and numeric edge cases.
- [ ] Forced-deopt tests prove exact T1-to-T0 state equality at every deopt point (Rule 4).
- [ ] Compile latency and memory budgets are enforced with fallback, not stalls (Rule 112).
- [ ] No file outside `compiler/baseline/`, `tests/baseline/`, and `docs/baseline_contract.md` is modified.
- [ ] Messages are referenced in the commit/PR wherever cross-team impact exists, and required RFCs/advisories were sent.
