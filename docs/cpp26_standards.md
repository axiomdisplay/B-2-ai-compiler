# B-2 C++26 Code Standards and Testing Strategy (v0)

Owner: all teams — repo-wide engineering standard, enforced through CI and review

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

**Java owns the semantics. C++ owns the implementation.** They are separate
domains with a strict contract between them. Java tests validate the *contract*.
C++ code standards ensure the *implementation* is correct, maintainable, and
auditable — but they do not pretend to be Java.

The compiler and runtime are C++26 programs. They have their own engineering
concerns independent of Java. But every C++ decision must ultimately be
traceable to a requirement: either a Java semantic requirement, a performance
requirement, or a platform/safety requirement.

---

# Part A — B-2 C++26 Code Standards

## 1. Core Philosophy

> **Why this standard exists:** We are building a high-performance systems
> program in C++26 that happens to execute Java. The C++ codebase must be
> excellent *as C++* — safe, readable, maintainable, performant — while never
> losing sight of the fact that its purpose is defined externally by Java
> semantics. Neither domain subsumes the other.

### Rule CS-1: Every Construct Has a Documented Rationale
Every non-trivial class, template, function, macro, or design pattern must have
a `// WHY:` comment explaining:
- What requirement it serves (Java semantic, performance, safety, or platform)
- Why simpler alternatives were rejected
- What the trade-offs are

```cpp
// WHY: Index-based NodeId instead of Node* pointers.
// Requirement: IR must be serializable for deterministic replay (Law 124).
// Rejected: Raw pointers break serialization and arena reallocation.
// Trade-off: Extra indirection; mitigated by SoA layout (Rule 20).
using NodeId = uint32_t;
```

No "obvious" abstractions. No undocumented cleverness. If a reviewer cannot
understand the rationale in under 30 seconds, it is wrong.

### Rule CS-2: C++ Idioms Are Preferred Over Reinvention
Use standard C++26 facilities when they meet our constraints. Do not reinvent
`std::expected`, `std::span`, `std::pmr`, `enum class`, etc. Custom abstractions
require stronger justification than standard ones.

> **Why:** Standard idioms are reviewed by millions of eyes. Custom abstractions
> are reviewed only by us. Prefer the former unless measurement or correctness
> proves insufficiency.

### Rule CS-3: Naming Encodes Intent, Not Mechanism
Bad: `processNode`, `handleData`, `tempBuffer`
Good: `eliminateRedundantNullCheck`, `materializeEscapedObjectAtDeopt`, `StencilPatchTable`

> **Why:** Implementation names rot. Intent names survive refactoring and
> communicate across team boundaries.

---

## 2. Safety & Correctness

### Rule CS-4: Invariants Are Checked, Not Assumed
- Debug builds: `B2_ASSERT(condition, "message")` with full context
- Release builds: `[[assume(condition)]]` only after CI soak tests prove zero failures
- Never use bare `assert()` from `<cassert>`
- Assertions document contracts; they are not optional debugging aids

> **Why:** Assertions are executable documentation. Bare asserts provide no
> diagnostics and vanish in release. Promoting to `[[assume]]` without
> empirical proof is UB.

### Rule CS-5: Error Handling Is Explicit and Zero-Cost
- `std::expected<T, Diagnostic>` for all fallible operations
- `[[nodiscard]]` on all result types (Law 48)
- No C++ exceptions on hot paths (Law 6); cold paths may use exceptions if documented
- Monadic chaining (`and_then`, `transform`) preferred over verbose if-chains
- Approved `B2_TRY` macro exempt from Law 49 for error propagation only

> **Why:** Silent error handling causes miscompilation. `std::expected` makes
> failure visible in the type system. Hot-path exception freedom is
> non-negotiable for JIT performance. Cold-path exceptions are acceptable where
> they simplify non-performance-critical code (e.g., file I/O, manifest parsing).

### Rule CS-6: Ownership Is Always Visible
- No raw owning pointers
- Arena-scoped objects use typed wrappers with explicit lifetime docs
- Cross-arena references use stable indices (Law 15)
- No `std::shared_ptr` / `std::function` in hot paths (Law 9)
- Every allocation documents owning arena and deallocation strategy

> **Why:** Memory bugs in a compiler/runtime are catastrophic. Visible
> ownership makes lifetime reasoning local and auditable.

### Rule CS-7: Concurrency Is Typed and Justified
- No bare `std::atomic<T>` without memory order justification
- Typed wrappers: `ReleasePublish<T>`, `AcquireLoad<T>`, `RelaxedCounter<T>`
- Every atomic operation documents: protected invariant, ordering rationale, benign races
- Global locks require written approval and budget (Law 118)

> **Why:** Memory ordering bugs are invisible in testing. Typed wrappers make
> misuse a compile error. Documentation forces JMM/C++ memory model
> correspondence reasoning.

---

## 3. Performance Discipline

### Rule CS-8: Hot Path Optimizations Are Measured
Every hot-path optimization references:
- Benchmark name and metric
- Before/after numbers
- CI validation job

No theoretical optimizations. No premature optimization. No micro-optimizations
without macro impact.

> **Why:** Performance intuition is unreliable. Measurement prevents cargo-cult
> optimization.

### Rule CS-9: Data Layout Serves Access Patterns
- SoA for bulk processing (Law 20)
- Cache-line alignment for hot structures
- Bit-packed flags (Law 32)
- `SmallVector<T, N>` for bounded-small collections (Law 19)
- No inheritance in hot data structures

> **Why:** Modern CPUs are memory-bound. Layout determines performance more
> than algorithm choice.

### Rule CS-10: Allocation Is Budgeted
- Hot paths use PMR monotonic arenas (Law 7)
- Every allocation documents expected size/frequency
- Deopt materialization has explicit budgets
- Allocation failure paths are tested

> **Why:** Unbounded allocation causes latency spikes. Budgets make resource
> consumption predictable.

---

## 4. Documentation Requirements

### Rule CS-11: File Purpose Headers
```cpp
/**
 * @file stencil_patcher.cpp
 * @brief Applies patch values to precompiled stencil code buffers.
 *
 * WHY THIS FILE EXISTS:
 * T1 baseline JIT composes methods from precompiled stencils. Patching
 * is the only runtime code generation step. It must be fast (<1µs/method)
 * and safe (no W^X violations, no torn patches).
 *
 * KEY INVARIANTS:
 * - All patch sites described in StencilHeader; no arbitrary mutation
 * - Patching occurs in writable staging buffer before publication
 * - Branch targets validated for range before patching
 *
 * CROSS-REFERENCES:
 * - Law 97: W^X compliance
 * - Law 99: Safe concurrent patching
 * - docs/stencils.md §6
 */
```

### Rule CS-12: Public API Contracts
Preconditions, postconditions, thread-safety, allocation behavior, failure
modes. No "see implementation."

### Rule CS-13: Complex Algorithms Have Design Docs
Linked design doc with: problem statement, alternatives, decision rationale,
limitations, test strategy.

---

## 5. Forbidden Patterns

| Pattern | Why Forbidden | Replacement |
| :--- | :--- | :--- |
| `std::unordered_map` in hot path | Cache-hostile (Law 17) | SwissTable / flat_hash_map |
| `std::string` in IR/hot path | Heap alloc, slow compare (Law 16) | Interned SymbolId |
| Virtual dispatch in hot path | Indirect call barrier (Law 8) | enum class + switch |
| C-style macros for logic | Undebuggable (Law 49) | constexpr / templates |
| Global mutable state | Hidden deps, races (Law 125) | Explicit context |
| Raw pointer arithmetic | UB risk (Law 63) | span / checked iterators |
| `reinterpret_cast` without proof | Type punning UB | std::bit_cast + static_assert |
| Magic numbers | Unauditable (Law 23) | Named constexpr with doc |

---

# Part B — Testing Strategy

## The Contract Model

```text
┌─────────────────────┐         ┌─────────────────────┐
│   C++ Compiler/RT   │◄───────►│   Java Semantics    │
│                     │ CONTRACT│                     │
│ • Owns impl details │         │ • Owns behavior     │
│ • C++ code standards│         │ • Java test suite   │
│ • Internal quality  │         │ • External oracle   │
└─────────────────────┘         └─────────────────────┘
        ▲                               ▲
        │ Validates                     │ Validates
   C++ quality                     Java correctness
   (static analysis,               (differential,
    sanitizers,                     stress, golden)
    internal invariants)
```

**Java does not own the compiler. The compiler does not own Java semantics.**
They meet at a well-defined contract. Both sides have their own validation
responsibilities.

---

## Layer 1: Java Semantic Tests (Primary Validation)

These validate that the C++ implementation correctly executes Java programs.
This is the **primary** correctness criterion.

### Differential Tests
Same program, all tiers, outputs must match.

```java
@DifferentialTest(tiers = {T0, T1, T2, T3})
void swlpProducesIdenticalResults() { ... }
```

### Forced Behavior Tests
Runtime hooks force specific C++ behaviors, validated through Java observables.

```java
@Test
void deoptPreservesMonitorReentry() {
    RuntimeHooks.enableForcedDeopt("test_method", 3);
    // ... validate via Java monitor introspection
}
```

### Stress Tests
GC stress, deopt stress, allocation failure — driven from Java, validating C++
resilience.

### Negative Tests
Programs that must fail in specific Java-defined ways.

### Performance Regression Tests
Java benchmarks with statistical thresholds.

---

## Layer 2: C++ Quality Validation (Internal Engineering)

These validate that the C++ implementation is *well-engineered*, independent of
Java semantics. **They do not replace Java tests.** They ensure the C++ codebase
doesn't rot internally.

### Static Analysis
- Clang-tidy with custom checks enforcing CS-1 through CS-13
- Compile-time enforcement of Law 8 (no RTTI), Law 6 (no exceptions in hot path), Law 9 (no shared_ptr/function)
- Include-what-you-use for dependency hygiene

### Sanitizer Matrix (CI)
- ASan: memory safety
- UBSan: undefined behavior
- TSan: data races
- MSan: uninitialized memory (where supported)

These run against **Java test harnesses**, not C++ unit tests. The Java tests
drive the C++ code through sanitizer-instrumented builds.

### Internal Invariant Checks
`B2_ASSERT` statements throughout C++ code validate internal engineering
contracts:

```cpp
// Internal invariant: patch site offset within stencil bounds
B2_ASSERT(patch.offset < header.code_size,
          "Patch site {} exceeds stencil code size {}",
          patch.offset, header.code_size);
```

These are **not tests**. They are runtime assertions that catch C++
implementation bugs during development. They fire during Java test execution.
They do not exist as standalone C++ test files.

### Fuzzing
C++ fuzzers target **internal interfaces** that cannot be adequately exercised
through Java:

- IR parser fuzzer (malformed serialized IR)
- Stencil patcher fuzzer (malformed patch tables)
- GC map decoder fuzzer (corrupt stack maps)
- Deopt metadata fuzzer (invalid FrameState)

These fuzzers validate **defensive coding against malformed inputs**, not Java
semantics. They are infrastructure/security tests, not semantic tests. Their
outputs are validated by "does not crash/UB," not "produces correct Java
result."

### Golden IR Tests
IR snapshots validated as text diffs. These validate **compiler intermediate
representation stability**, not Java behavior. They catch unintended IR changes
that might not yet manifest as Java-visible bugs but indicate regressions in
compiler internals.

```text
tests/golden/ir/pea_scalar_replace_basic.in.b2
tests/golden/ir/pea_scalar_replace_basic.expected.ir
```

These are C++ engineering artifacts. They exist because IR stability matters
for compiler maintainability, not because they validate Java.

---

## What Does NOT Exist

- ❌ C++ unit tests that duplicate Java semantic tests
- ❌ C++ mock frameworks simulating Java behavior
- ❌ C++ test harnesses that reimplement Java semantics
- ❌ GTest/Catch2/doctest suites for business logic

These create a parallel validation universe that drifts from actual
requirements.

---

## Test Ownership

| Test Type | Language | Owner | Purpose |
| :--- | :--- | :--- | :--- |
| Semantic correctness | Java | All teams | Validate Java behavior |
| Differential | Java | All teams | Tier equivalence |
| Stress | Java | GC/Runtime teams | Resilience |
| Performance | Java | All teams | Regression detection |
| Static analysis | C++ tooling | CI | Code quality |
| Sanitizers | C++ instrumentation | CI | Memory/thread safety |
| Fuzzing | C++ | Security/Runtime | Defensive coding |
| Golden IR | Text/IR format | Compiler teams | IR stability |
| Internal asserts | C++ inline | All teams | Development-time bug detection |

---

## Enforcement

- PR template requires: Java tests added/modified + C++ quality justification
- Reviewer checklist: "Is Java behavior validated?" AND "Are C++ standards followed?"
- CI blocks merge if: Java differential tests fail OR sanitizer finds issues OR static analysis reports violations
- Coverage metrics: Java semantic coverage is primary; C++ line coverage is secondary engineering metric, not a gate

---

This model respects both domains: **C++ is engineered rigorously as C++, and
Java semantics are validated exclusively as Java.** Neither pretends to be the
other. The contract between them is the only thing that must be perfectly
correct.
