# Interpreter Team Charter

```text
Normative reference: docs/laws.md
(B-2 Java Runtime Compiler Laws & Architecture Specification, as amended by
Amendment A, Amendment B, and the Special Pass Laws in Part XVIII.)
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

Team size: 2

Roles:

- `INTERP-IMPL` — Implementer. Produces all Tier 0 interpreter code, tests, and the T0 state contract inside the team's owned write paths.
- `INTERP-REV` — Reviewer. Checks every change for correctness bugs, law compliance, boundary violations, missing tests, missing documentation, and missing message references. Does not rewrite code unless explicitly reassigned.

---

## Mission

The Interpreter Team owns Tier 0 (T0), the direct-threaded register interpreter: the baseline execution engine, the universal correctness fallback, and the state-reconstruction target for the entire compiler stack. T0 executes quickened Register Bytecode (RBC), never raw JVM stack bytecode. Dispatch is direct-threaded — computed goto where the C++ toolchain supports labels-as-values, generated assembly or an equivalent direct-threaded token dispatch model otherwise; the architectural requirement is minimal dispatch overhead, not a particular compiler extension (Part I; Amendment B.1). The team takes the fastest possible interpreter path and performs no heavy optimization: inline caches, profiling counters, superinstructions, and safepoint polls are in scope; optimization of Java program behavior is not.

T0 carries a second, system-wide responsibility: it is THE deoptimization target. This team owns and publishes the exact T0 state model — locals/register file, operand state, bytecode index, pending exception, monitor state, JVMTI/profiling state, GC reference visibility — that every compiled tier (T1/T2/T3) must reconstruct on deopt (Rule 4, Rule 96; Amendment A "linear state mapping to T0"). T0 is also the reference implementation and the standing differential-testing baseline against the Java SE/JVM semantic oracle (Rule 67, Rule 133). A defect in T0 semantics or in the published state contract corrupts every tier above it; this is why the contract is versioned, tested, and change-controlled by message.

In scope:

- RBC execution semantics and the RBC opcode set definition (encoding, operands, semantics).
- The direct-threaded dispatch core, with computed-goto fast path and portable fallback.
- Quickener cooperation: consuming JVM stack bytecode translated by the loader/verifier/quickener into quickened RBC (Amendment B.2 pipeline shape).
- Interpreter inline caches and their update paths.
- Profiling counters that feed tier-promotion decisions, plus profile export for T2 PGO.
- Superinstruction formation and execution, each proven semantically identical to its unquickened sequence.
- Safepoint polls in the dispatch loop (loop backedges, calls, and the other Rule 88 sites).
- The published T0 exact-state / deopt reconstruction contract, `docs/interp_contract.md`.

Not owned by this team:

- Any JIT tier: T1 baseline (`compiler/baseline/`), T2 optimizing IR and passes (`compiler/ir/core/`, `compiler/passes/`), register allocation (`compiler/regalloc/`), codegen (`compiler/codegen/`), AOT (`compiler/aot/`).
- The runtime (`runtime/`): GC, class loading, verification, quickener implementation, monitors, JNI, tier-management policy.
- Any optimization decision that consumes interpreter profiles — the team produces the data, not the policy.

---

## Owned Write Paths

```text
compiler/interp/
tests/interp/
docs/interp_contract.md
```

---

## Forbidden Write Paths

```text
compiler/baseline/
compiler/ir/core/
compiler/passes/
compiler/regalloc/
compiler/codegen/
compiler/aot/
runtime/
```

The team never writes outside its owned paths, even for one-line fixes. Any issue that lives in another team's area — or any change to a cross-team contract this team publishes — is raised as a message under `messages/` per `docs/teams/messaging.md`; the owning team resolves it.

---

## Key Laws

- Rule 4 — Deoptimization Must Reconstruct Tier 0 State: T0 defines the state that JIT deopt must reproduce exactly; this team is the authority on that state.
- Rule 6 — NO EXCEPTIONS ON THE HOT PATH: the dispatch loop is the hottest path in the system; zero C++ `throw`.
- Rule 7 — Zero-Allocation Hot Path: dispatch, inline-cache fast paths, and safepoint poll fast paths must not allocate.
- Rule 8 — No RTTI: type dispatch in the interpreter core uses `enum class` kinds, never dynamic type queries.
- Rule 9 — No `std::shared_ptr` / `std::function` in Hot IR Code: the same discipline applies to the dispatch loop.
- Rule 16 — Interned Symbols (No Strings in the Hot Path): dispatch and inline caches use `SymbolId`, never `std::string`.
- Rule 44 — No Profile Data Without Confidence: interpreter-exported profiles carry sample count, stability, age, decay, variance, and deopt correlation.
- Rule 67 — Java SE/JVM Is the Semantic Oracle: T0 is the reference implementation; any divergence is a correctness bug.
- Rule 72 — Java Numeric Semantics Must Be Preserved Exactly: overflow wrapping, division-by-zero, NaN, negative zero, conversion rules.
- Rule 74 — Java Exceptions Are Control-Flow Values, Not Native Exceptions: exception dispatch in T0 is value-based control flow.
- Rule 75 — Frames Must Be Reconstructible on Demand: the T0 frame layout is the canonical reconstructible frame.
- Rule 88 — Generated Code Must Poll Safepoints: the poll-site list (loop backedges, calls, allocation sites, OSR points, ...) applies to the dispatch loop.
- Rule 96 — Tier 0 Is the Universal Correctness Fallback: every executable method must run in T0; nothing is JIT-only.
- Rule 111 — Safepoint Latency Must Be Bounded: interpreter polls cooperate with the bounded-latency suspension protocol.
- Rule 114 — Hotness Counters Must Be Robust: saturating, overflow-safe, decaying, racy-with-bounded-error where thread-local.
- Rule 118 — No Global Locks on Hot Runtime Paths: no locks on dispatch, inline-cache updates, or polls.
- Rule 119 — Tier Transitions Must Be Observable: T0 promotion events are recorded with reasons and counters.
- Rule 133 — Differential Oracle Testing Must Run Continuously: T0 is one side of every tier comparison in the CI matrix.
- Amendment A — No-IR Baseline Tier: T1's "linear state mapping to T0" consumes this team's published state contract.
- Amendment B — Tier Model Redesign: tier model, T0 definition, deopt directions (T0 is always final fallback).
- Part XVIII, NaN Boxing Rule — the interpreter is an affected party to the cross-team representation contract: NaN-tagged generic slots must not change T0 state reconstruction, GC visibility of references, or Java floating-point semantics; the feature is disabled by default and requires this team's approval plus interpreter round-trip tests before it can be enabled.

---

## Deliverables

1. RBC executor with the complete opcode semantics table for every quickened and unquickened RBC instruction.
2. Direct-threaded dispatch core: computed goto where supported, portable direct-threaded fallback where not, with identical semantics.
3. Quickener integration: consume and execute quickened RBC produced by the loader/verifier/quickener from JVM stack bytecode.
4. Interpreter inline caches, including racy-but-bounded update paths.
5. Profiling counters (invocation, backedge, branch, call target) feeding tier-promotion decisions, with documented decay and saturation behavior.
6. Superinstructions, each accompanied by a proof-of-equivalence test against its expansion.
7. Safepoint poll implementation at all Rule 88 sites, with bounded latency under Rule 111.
8. The published T0 exact-state / deopt reconstruction contract, `docs/interp_contract.md`, versioned and consumed by baseline_noir, ir, passes, regalloc, codegen, and aot.
9. Profiling data export for T2 PGO, with confidence metadata per Rule 44.

---

## Interface Contract

### Consumes

- Verified classfiles translated to quickened RBC by the loader/verifier/quickener.
- Runtime services (allocation, monitors, resolution, JNI) as call-outs — used, never modified.
- Tier-management thresholds and feedback (what to count, when to report heat).

### Outputs

- The T0 exact-state contract and RBC semantics, published in `docs/interp_contract.md`, for baseline_noir, ir, passes, regalloc, codegen, and aot.
- RBC opcode set and encoding definition for all compiling tiers.
- Profile data (counters, call-target histograms, branch data) to tier management and T2 PGO consumers.
- Observable tier-transition and fallback events from the interpreter side.

### Promises

- Exact Java semantics: T0 is the differential oracle baseline (Rule 67, Rule 133).
- Bounded safepoint latency from any executing interpreter frame (Rule 111).
- No hot-path allocation, C++ exceptions, RTTI, or locks in the dispatch loop (Rules 6, 7, 8, 9, 118).
- Deterministic profiling counters with documented error bounds and Rule 44 confidence metadata.
- Every executable method runs correctly in T0 (Rule 96).

---

## Testing Responsibilities

```text
tests/interp/
```

- Dispatch correctness: every RBC opcode executed against its semantics table.
- Superinstruction semantics: equivalence to the unquickened instruction sequence.
- Safepoint poll behavior: polls taken at all Rule 88 sites, latency bounded under Rule 111.
- Profiling counter behavior: saturation, decay, overflow safety, thread-safety or documented bounded raciness (Rule 114).
- T0 state contract conformance: state-reconstruction fixtures generated from T0 and reused by all other tiers' deopt tests.
- Quickening correctness: differential JVM bytecode vs quickened RBC execution.
- Numeric edge cases: NaN, negative zero, integer overflow, division by zero, conversions (Rule 72, Rule 133).
- Fallback completeness: every method executable in T0 with all higher tiers disabled (Rule 96).

---

## When To Send Messages

- The T0 state contract changes in any way — ADVISORY to all teams; every compiled tier's deopt path is affected.
- The RBC opcode set or encoding changes — RFC/ADVISORY to baseline_noir, ir, passes, codegen, and aot.
- The quickener input expectations or RBC format produced by the pipeline change — coordinate with the loader/verifier/quickener owners.
- The profiling data format changes — ADVISORY to tier management and T2 PGO consumers.
- The NaN boxing representation contract is proposed or changed — this team participates in the cross-team approval required by Part XVIII and must verify T0 round-trip behavior before any enablement.
- An interpreter bug is suspected to affect deopt correctness of T1/T2/T3 — ADVISORY with reproduction data.
- A defect is found in another team's area — BUG message filed against that team; never patched here.

Bugs found in the interpreter's own area are owned and fixed by this team directly. If the fix affects other teams, an ADVISORY accompanies it. The team never writes into another team's paths, regardless of severity.

---

## Reviewer Checklist

- [ ] Hot-path discipline holds: no allocation, C++ exceptions, RTTI, or locks in the dispatch loop or fast paths (Rules 6-9, 118).
- [ ] Every executable method still runs correctly in T0 with all JIT tiers disabled (Rule 96).
- [ ] The T0 state contract is updated, versioned, and covered by reconstruction tests before any semantic change lands.
- [ ] Profiling counters are saturating, overflow-safe, and decaying as documented (Rule 114).
- [ ] Every superinstruction has a passing semantic-equivalence test against its expansion.
- [ ] Safepoint polls appear at all required sites and latency is bounded (Rule 88, Rule 111).
- [ ] Only interned symbols are used in dispatch and inline caches; no strings in the hot path (Rule 16).
- [ ] Numeric edge cases (NaN, negative zero, overflow, division by zero) are tested and pass (Rule 72, Rule 133).
- [ ] No file outside `compiler/interp/`, `tests/interp/`, and `docs/interp_contract.md` is modified.
- [ ] Messages are referenced in the commit/PR wherever cross-team impact exists, and required advisories were sent.
- [ ] Golden and differential test fixtures (including state-reconstruction fixtures used by other tiers) were regenerated and updated.
- [ ] New or changed RBC opcodes are reflected in the semantics table, the contract, and consumer teams' advisories.
