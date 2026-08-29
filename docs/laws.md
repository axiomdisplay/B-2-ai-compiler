# B-2 Java Runtime Compiler Laws & Architecture Specification

**Status:** Stable, as amended
**Owner:** B-2 Systems Dev Team
**Last Updated:** 2026-08-30
**Amendments:** Amendment A — No-IR Baseline Tier · Amendment B — Tier Model Redesign · Special Pass Laws (SWLP, PEA, NaN Boxing) — see Parts XVII and XVIII
**Target Runtime:** Java / JVM bytecode semantics, Java SE class-library behavior as approved by the compatibility oracle
**Implementation Language:** C++26
**Related Sections:** `ir_spec.md`, `effect_system.md`, `abi.md`, `java_semantics.md`, `jni_ffi.md`, `teams/README.md`

This document is the authoritative, uncompressed transcription of the laws that govern the **B-2** Java runtime and its compilers. **Every commit to the `compiler/`, `runtime/`, `tools/`, and `tests/` trees must comply. CI verifies them. There are no exceptions.**

---

## Part I: Architectural Overview

The B-2 execution model for Java/JVM bytecode consists of four distinct, seamlessly integrated execution tiers. Transitions between tiers are governed by profile data and static guarantees, never by arbitrary timeouts.

1.  **Tier 0: Direct-Threaded Register Interpreter**

    The baseline execution engine. Uses direct threading and a register-based internal bytecode representation derived from quickened JVM bytecode. JVM bytecode is stack-based, but Tier 0 executes an internal register-bytecode form to minimize dispatch and stack-memory traffic.

    Where the C++ toolchain supports labels-as-values / computed goto, Tier 0 uses computed goto dispatch. Where that is unavailable, Tier 0 must use generated assembly or an equivalent direct-threaded token dispatch model. The architectural requirement is direct threading with minimal dispatch overhead, not a particular compiler extension.

    Tier 0 provides fast startup, minimal memory footprint, and is the ultimate fallback for all executable Java methods.

2.  **Tier 1: Baseline JIT**

    Triggered by low-level heat such as method invocation counts and loop backedges. Compiles in linear or near-linear time. Performs basic type specialization, copy propagation, simple inlining, null-check folding, basic field-offset specialization, and lightweight guard emission.

    Compilation time is strictly bounded to prevent mutator stalls. Tier 1 must always be safe to abandon or fall back from.

3.  **Tier 2: Optimizing JIT**

    Triggered by sustained heat and rich PGO data. Employs the full B-2 optimization pipeline, including the sea-of-nodes optimizer and Passes 1–51. Performs aggressive speculative optimizations such as partial escape analysis, scalar replacement, lock elision, range-check elimination, inlining, devirtualization, loop optimizations, SLP vectorization, and speculative effect reordering.

    Requires full `FrameState`, stack maps, GC maps, dependency tracking, and deoptimization infrastructure.

4.  **Tier 3: AOT / Static JIT**

    Applied to code with provable static guarantees. Examples include closed-world analysis, final classes, sealed hierarchies, no reflection, no dynamic class loading, no unsafe native escape, no JVMTI mutation, verified module boundaries, and fully analyzable call graphs.

    Tier 3 bypasses speculation and deoptimization overhead only where static proofs mechanically validate the optimization. Maximum optimization, zero guard checks where proven, direct native code emission.

---

## Part II: The Unified Pipeline & Speculation Laws

**Rule 1 — One Pipeline, Multiple Inputs**

The IR, pass interfaces, verifier constraints, and correctness constraints are **unified** across Tier 1, Tier 2, and Tier 3. Tiers differ only by budgets, enabled speculation policies, available proofs, and telemetry requirements.

-   **Tier 1 Input:** Static IR + minimal heuristics, budget-constrained.
-   **Tier 2 Input:** Static IR + PGO data, aggressive, guard-emitting.
-   **Tier 3 Input:** Static IR + static proofs, no guards where proven, maximum optimization.

There is no “JIT-only” pass list. Forcing the exact same pass sequence can waste compile time in Tier 1 or prevent legitimate tier-specific lowering. If a pass exists, it must handle all modes via a unified interface, but the pipeline may apply tier-specific filters.

---

**Rule 2 — PGO is a Force Multiplier, Not New Logic**

PGO data does not change *what* the compiler does; it changes *how aggressively* it does it.

Example: Pass 49, Speculative Effect Reordering, runs in all tiers conceptually. In Tier 3, it requires a static effect-order proof. In Tier 2, it accepts PGO-proven probability above a documented confidence threshold and inserts a guard. In Tier 1, it is skipped due to budget.

---

**Rule 3 — Every PGO-Driven Decision Requires a Guard**

If a pass makes a decision based on PGO, it **must** emit a validated guard mechanism.

Examples of PGO-driven Java assumptions include:

-   a virtual call is monomorphic or bimorphic;
-   an interface call has a stable target;
-   a class hierarchy has not changed;
-   a field layout is stable;
-   an array type is exact;
-   a value is never null;
-   an array index is always in bounds;
-   an exception edge is never taken;
-   an invokedynamic call site remains linked to a stable target;
-   a method handle combinator remains stable;
-   a static field is not modified after initialization;
-   a class initializer has already run and will not rerun.

Guard mechanisms may include runtime checks, class-version checks, shape/version guards, method-version guards, patchpoints, traps, dependency invalidation, or hardware checks. Not all speculation is best expressed as a hardware branch. Guard success executes the optimized path; guard failure triggers deoptimization.

---

**Rule 4 — Deoptimization Must Reconstruct Tier 0 State**

When a JIT guard fails, the runtime must deoptimize to the **exact same state** the Tier 0 direct-threaded register interpreter would have been in at that instruction position.

This includes restoring:

-   local variable array or its register-bytecode equivalent;
-   operand stack/register file values;
-   bytecode index, including OSR entry position where applicable;
-   pending Java exception object, if any;
-   monitor state, including held locks and monitor stack;
-   line-number and stack-trace-relevant state;
-   JVMTI/debug/profiling state where active;
-   virtual-thread or continuation suspension state where applicable;
-   GC reference state and oop-map-visible references.

Speculative execution must not perform irreversible Java-visible side effects before the last guard protecting that speculation. If side effects are moved, they must be either proven non-observable, deferred until committed, or supported by a verified compensation mechanism.

“Rolling back memory writes” is dangerous if Java-visible effects already happened. Field stores, array stores, static-field stores, volatile stores, monitor actions, class-initialization effects, JNI side effects, and exception-object publication are not freely rollback-able unless proven safe.

---

**Rule 5 — FrameState is Mandatory for All Guards**

Every node that introduces a speculative assumption must have a `FrameState` attachment.

This snapshot allows the deoptimizer to rebuild the Java execution world at that point, including local variables, operand-stack/register state, monitor state, exception state, bytecode index, line-number state, and any materialized objects required for correct stack traces or debugging.

---

## Part III: Compilation Pipeline & Memory Laws

**Rule 6 — NO EXCEPTIONS ON THE HOT PATH**

Native C++ exceptions are forbidden on compiler/runtime hot paths. The JIT compiler, AOT compiler, and runtime deoptimization engine **MUST** be compiled with `-fno-exceptions`.

Zero `throw` statements are allowed in any code path executed during compilation or runtime specialization. All fallible operations **MUST** use `std::expected<T, Diagnostic>` or `Result<T, Error>`.

Java exceptions remain first-class runtime values and must be modeled explicitly. Do not confuse Java exceptions with C++ exceptions. If a JIT compilation fails, it returns an `Error` variant, causing the system to silently fall back to Tier 0 or Tier 1. No stack unwinding. No catch blocks. No overhead.

---

**Rule 7 — Zero-Allocation Hot Path**

Both AOT and JIT compilers must use `std::pmr::monotonic_buffer_resource` or an equivalent arena allocator for IR allocation. Bulk-free after compilation. No `malloc`/`free` in the compiler hot path.

“Hot path” means compiler pass execution, guard execution, inline-cache fast paths, allocation fast paths, read/write barrier fast paths, safepoint polling fast paths, and deopt entry trampolines.

Deopt materialization may allocate only through a controlled runtime path with explicit budgets. Deopt may legitimately need to materialize objects, rebuild interpreter frames, allocate exception-related metadata, or reconstruct stack-trace-visible state.

---

**Rule 8 — No RTTI**

Both pipelines are compiled with `-fno-rtti`. Use `enum class NodeKind` for type switching. RTTI is forbidden in the IR and backend to ensure maximum devirtualization and cache locality.

---

**Rule 9 — No `std::shared_ptr` / `std::function` in Hot IR Code**

They allocate and incur atomic or indirect-call overhead. Use raw pointers plus stable `NodeId`s inside passes. Ownership must be explicit and arena-scoped.

---

**Rule 10 — Every Pass Must Be Idempotent and Monotonic**

Running the same pass twice must produce the identical IR. A pass either reduces node count or moves the IR closer to a normal form.

If a pass can grow the IR, such as loop unrolling, SLP vectorization, speculative cloning, or partial escape materialization, it must run inside a guarded fixpoint with a strict budget.

---

**Rule 11 — Mutator Threads Never Block on JIT**

If a method becomes hot and triggers JIT compilation, the mutator thread continues executing the current tier. The JIT runs asynchronously on a background compiler thread.

Once ready, installation must be atomic and safe against concurrent execution. Old code must remain valid until quiescence. A safe-point patch is not enough if publication is torn or old code is freed too early.

---

**Rule 12 — Thread-Local Allocation for Mutators**

Mutator threads use thread-local allocation buffers, or TLABs, with bump-pointer allocation for their own runtime allocations. Global synchronization happens only at explicit yield points, safepoints, GC transitions, or slow-path allocation refill.

---

**Rule 13 — Compiler Threads Never Block on Mutator State**

The compiler works on a frozen snapshot of the IR, bytecode, metadata, and PGO data. Mutator updates after the snapshot are picked up by the next compilation.

If dependencies change during compilation, the compiled artifact must be discarded or invalidated, not partially patched into an inconsistent state.

---

**Rule 14 — Epoch-Based Reclamation**

Old JIT code, deopt metadata, IR nodes, and dependency records are reclaimed using epoch-based reclamation or an equivalent RCU-like mechanism.

When the optimizer replaces a `Node`, the old node is tagged with an epoch. Once all relevant threads advance past that epoch, the memory may be bulk-freed. Generated machine code must not be reclaimed until no thread can be executing it or depending on its deopt metadata.

IR nodes and machine code require different reclamation guarantees.

---

## Part IV: The Numbered Rules

### Data Structures & IR Design

**Rule 15 — Index-Based Graph (No Raw Pointers in the IR)**

Never use raw pointers (`Node*`) for edges in the sea-of-nodes IR. All node references must use a 32-bit integer index:

```cpp
using NodeId = uint32_t;
```

This cuts memory footprint, improves cache capacity, makes the IR trivially serializable, and avoids pointer invalidation during arena reallocation.

---

**Rule 16 — Interned Symbols (No Strings in the Hot Path)**

Never pass, compare, or store `std::string` or `std::string_view` in the IR or passes.

All identifiers, including class names, method names, field names, signatures, module names, and descriptor names, must be interned into a global or scoped `SymbolTable` at the frontend. The IR must only use a `SymbolId`:

```cpp
using SymbolId = uint32_t;
```

---

**Rule 17 — Cache-Friendly Hash Maps (Ban `std::unordered_map`)**

`std::unordered_map` and `std::map` are forbidden in the compiler hot path.

For Global Value Numbering, hash-consing, dependency lookup, inline-cache metadata, and any pass requiring a hash table, use a cache-friendly, open-addressing hash map such as SwissTable, `flat_hash_map`, or a custom Robin Hood / quadratic-probing table.

---

**Rule 18 — Sparse Sets and BitVectors for Pass Data**

Ban `std::set`, `std::unordered_set`, and `std::vector<bool>` for dataflow analysis.

Passes tracking sets of `NodeId`s, such as liveness, dominance, visited sets, effect sets, and loop membership, must use sparse sets for small dense sets or bitvectors for large sparse sets.

---

**Rule 19 — Small Buffer Optimization (SBO) for Variable-Length Data**

Ban `std::vector` for data that usually has 1 to 4 elements.

For use-def chains, instruction operands, basic-block predecessors/successors, guard dependency lists, and small edge lists, use `SmallVector<T, N>` where `N` is usually 2, 3, or 4.

---

**Rule 20 — Structure of Arrays (SoA) for Bulk Pass Processing**

When a pass needs to process a specific field of millions of nodes, do not iterate over full `Node` structs in AoS layout.

Extract that attribute into a contiguous `std::pmr::vector` or equivalent SoA layout to allow prefetching and SIMD-friendly compiler passes where applicable.

---

### Performance & Hardware

**Rule 21 — Exploit C++26 Compiler Hints**

Use `[[likely]]` and `[[unlikely]]` on PGO-driven branches and deoptimization traps.

Use C++26 or supported C++23/26 assumptions, such as `[[assume(condition)]]` or an approved `std::assume`-like helper, to tell the compiler about invariants. Example:

```cpp
[[assume(node_id < graph.size())]];
```

These hints must never replace runtime correctness checks on non-internal boundaries. They may eliminate internal bounds checks only when the invariant is already proven or debug-verified.

---

**Rule 22 — Zero-Cost Error Propagation**

Do not use verbose `if (err)` chains that ruin branch prediction and readability.

Use `std::expected<T, Error>` and monadic operations such as `and_then`, `transform`, and `or_else`, or approved inline helpers. If an approved `B2_TRY` macro is used, it must be the only sanctioned control-flow macro, must be audited, and must compile down to a single branch.

---

**Rule 23 — No Hard-Coded Constants in Optimization Logic**

Magic numbers are forbidden.

Every threshold, budget, limit, and heuristic constant used in any optimization pass MUST be defined as a named, documented `constexpr` constant or configuration parameter.

CI static analysis fails if numeric literals greater than 2 appear in pass logic without a named constant reference.

---

**Rule 24 — No Target-Specific Hacks in Generic Passes**

Mid-level and research passes such as GVN, LICM, SLP, range-check elimination, alias analysis, and effect reordering MUST NOT contain `#ifdef X86`, `#ifdef AARCH64`, or other target-specific conditionals.

All target knowledge must be abstracted behind the `Target` interface and queried via cost models or capability flags.

---

**Rule 25 — No Heuristics Without Empirical Validation**

Every heuristic MUST be backed by benchmark data showing measurable improvement, a mechanism to override/tune it, and documentation explaining why the value was chosen.

---

**Rule 26 — No Silent Fallbacks Without Telemetry**

When the JIT falls back to a lower tier, when a speculative guard fails, when a compilation budget is exceeded, or when register allocation spills excessively, the event MUST be recorded in telemetry/profile data.

Silent fallbacks hide performance problems.

---

**Rule 27 — No Assumption of Stable Hardware**

No pass may assume fixed cache-line sizes, SIMD widths, branch costs, or memory-latency ratios.

All hardware parameters MUST be queried at runtime for JIT or build-time for AOT via the `Target` interface.

---

**Rule 28 — No Optimization Without Measurable Win**

Every optimization pass added to the pipeline MUST demonstrate a ≥1% geometric mean improvement across the benchmark suite, OR enable a correctness/safety property that cannot be achieved otherwise.

Underperforming passes are removed.

---

### Correctness & Java/JVM Semantics

**Rule 29 — No JNI/FFI Optimization Without ABI Proof**

JNI and native FFI optimizations must prove calling-convention correctness, stack alignment, register clobbering, exception state handling, JNI local/global reference handling, monitor state preservation, and memory ownership transfer.

JNI transitions may involve pending Java exceptions, class loading, Java callbacks, GC triggers, and monitor state changes. None of these may be ignored.

---

**Rule 30 — No Vectorization Without Dependence Proof**

Vectorization, whether SLP or loop-based, must prove no harmful aliasing or use versioned checks, bounds safety, alignment, correct overflow semantics, and a correct scalar fallback.

Java array accesses require bounds checks unless elimination is proven. `Unsafe`-style accesses, VarHandles, JNI writes, and reflective accesses must be treated as potential aliasing hazards unless proven otherwise.

---

**Rule 31 — No Persistent State Without Versioning**

Profile caches, code caches, AOT artifacts, and class metadata caches must be versioned.

A change in IR format, JVM bytecode version, class-file version, pass order, dependency schema, GC object layout, or runtime ABI invalidates the cache.

---

**Rule 32 — All Orthogonal Boolean State Must Be Bitmasked**

Any set of independent boolean properties on a hot-path data structure, such as `NodeFlags`, `EffectTags`, `GuardKinds`, `BarrierKinds`, or `SafepointKinds`, must be represented as a bitmask with type-safe `Flags<E>` wrappers.

Raw integers are forbidden for flag-like state.

---

**Rule 33 — No Implicit Conversions or Coercions in IR**

The B-2 IR MUST NOT perform implicit type conversions, integer promotions, or pointer coercions.

All conversions must be explicit nodes, including but not limited to:

-   `SignExtend`
-   `ZeroExtend`
-   `Truncate`
-   `BitCast`
-   `IntToPtr`
-   `PtrToInt`
-   `RefToNative`
-   `NativeToRef`
-   `CompressedRefDecode`
-   `CompressedRefEncode`
-   `PrimitiveToObject` where boxing is modeled
-   `ObjectToPrimitive` where unboxing is modeled

The frontend lowering pass inserts these explicitly.

---

### Testing & Verification

**Rule 34 — Five Regression Tests Per Bug Fix**

Every bug fix must include at least five regression tests:

1.  Minimal reproducer.
2.  Variant trigger with a different code pattern but the same root cause.
3.  Boundary/negative test ensuring the fix does not over-correct.
4.  Integration/contextual test placing the bug in realistic surrounding code.
5.  Deopt/state reconstruction test verifying that if the JIT speculates wrongly, deopt to Tier 0 produces the exact same Java-visible state.

CI fails if a PR labeled `bugfix` has fewer than five new test cases.

---

**Rule 35 — Golden Tests for Every Pass**

Every optimization pass must have at least ten golden IR tests.

Checked-in `.in.b2` / `.expected.ir` file pairs are required. Tests must run in both Static Mode, meaning AOT/Tier 3, and Profile Mode, meaning Tier 2.

---

**Rule 36 — Differential Testing is Mandatory in CI**

Tier 0 interpreter, Tier 2 JIT apex, and Tier 3 AOT comparisons run on every PR.

Tier outputs must be observationally equivalent according to the Java semantic oracle. Any permitted differences must be explicitly listed, versioned, and tested.

Memory layout is tested separately only where it is a supported guarantee. Moving GC, ASLR, compressed-oop bases, identity hash generation, and heap layout nondeterminism must not create false positives unless a guarantee is documented.

Divergence blocks merge.

---

**Rule 37 — Deopt Paths Must Be Fuzzed Weekly**

Scheduled CI job. Results triaged within 24 hours. Untriaged deopt fuzz failures block releases.

---

**Rule 38 — Replay Logs Retained for All CI Failures**

Failed test runs automatically save full compile replay artifacts:

-   IR snapshot;
-   PGO profile;
-   compiler options;
-   target description;
-   RNG seed;
-   dependency versions;
-   failure location.

Debugging starts from replay, not reproduction.

---

**Rule 39 — Performance Regressions Require Explicit Waiver**

If a benchmark regresses more than 5%, the PR must include root-cause analysis, justification, a tracking issue, and approval.

No silent performance degradation.

---

**Rule 40 — Graph Verifier Runs in Debug Builds After Every Pass**

The verifier checks:

-   no dangling `NodeId`s;
-   effect-chain continuity;
-   control dominance;
-   use-def consistency;
-   valid region/memory edges;
-   valid exception edges;
-   `FrameState` attached to every PGO-driven guard;
-   valid dependency metadata;
-   valid GC-reference metadata where present.

---

**Rule 41 — Test Names Encode the Bug/Feature They Cover**

Bad:

```text
test_pea_3
```

Good:

```text
pea_non_escaping_point_object_with_deopt_materializes_correctly
```

Test names must be searchable and self-documenting.

---

### Speculation & Guarding

**Rule 42 — No Assumption Without Invalidation**

Every PGO-driven assumption must have:

-   a registry entry, the Watchdog;
-   an invalidation path, the Trip;
-   a fallback to static proof or lower-tier execution.

---

**Rule 43 — No Specialization Without Fallback**

Every specialized clone, such as a bounds-check-eliminated array loop or a devirtualized interface call, must have:

-   a generic fallback;
-   a deopt path;
-   a budget limit.

---

**Rule 44 — No Profile Data Without Confidence**

Profile data must include:

-   sample count;
-   stability;
-   age;
-   decay;
-   variance;
-   deopt correlation.

Low-confidence data must not trigger aggressive speculation.

---

**Rule 45 — No Aggressive Pass Without a Cost Model**

Inlining, cloning, unrolling, SLP vectorization, lock elision, escape-analysis materialization, and range-check elimination must all use a strict cost model.

The cost model is based on target hardware latencies, Java object overhead, GC barrier costs, safepoint costs, and deopt probability.

---

## Part V: Code Quality & Developer Velocity Laws

These rules are designed to be maximally strict on correctness and maintainability while actively protecting developer speed.

**Rule 46 — Local Pre-Commit Checks Must Complete in < 2 Seconds**

Strictness must not impede velocity.

The local pre-commit hook, including formatting, basic linting, and copyright headers, must execute in under two seconds. Heavy checks such as full test suites and differential testing are deferred to asynchronous CI.

---

**Rule 47 — Actionable Compiler Diagnostics**

The compiler must never output opaque errors such as:

```text
Error: something went wrong
```

All `Diagnostic` objects must include:

-   exact source location where available;
-   bytecode/class/method location where applicable;
-   a clear human-readable message;
-   expected versus actual state;
-   a suggested fix or recovery action.

Use `std::source_location` where appropriate.

---

**Rule 48 — `[[nodiscard]]` on All Result Types**

All functions returning `std::expected`, `Result`, or `Error` must be marked `[[nodiscard]]`.

Ignoring an error is a compilation failure. This forces developers to handle edge cases explicitly without requiring verbose performance-killing `if` chains.

---

**Rule 49 — No `#define` Macros for Logic**

C-style macros for control flow or logic are forbidden.

Use `constexpr` functions, `inline` functions, templates, or C++26 facilities. Macros are exempt only for:

-   header guards;
-   trivial token pasting;
-   one approved error-propagation macro, if explicitly registered and audited.

This ensures debugger visibility and compiler optimization quality.

---

**Rule 50 — Fast Incremental Builds via Modular CMake**

The build system must allow sub-second incremental builds for single-file changes where practical.

Heavy dependencies, such as testing frameworks or optional backend components, must be isolated. Developers must not wait minutes to test a single IR pass modification.

---

**Rule 51 — Automated Refactoring Tools Over Manual Edits**

When a structural change is required, such as renaming a `NodeKind` or adding a field to `FrameState`, a scripted refactoring tool must be provided and run as part of the PR.

Manual, error-prone find-and-replace across many files is forbidden.

---

**Rule 52 — Self-Contained, Reproducible Test Cases**

Every test must be fully self-contained.

It must not rely on external network calls, specific local directory structures, or non-deterministic system state. Tests must run identically on a developer laptop, a Linux CI runner, or another supported workstation.

---

## Part VI: Anti-Slop & Robustness Laws

**Rule 53 — No “Small Bug” or “Minor Edge Case” Rationalization**

The phrases “small bug,” “minor edge case,” “rarely happens,” “only affects cold paths,” and “good enough for now” are banned.

In a systems compiler/runtime, small bugs cause silent data corruption, security holes, or catastrophic performance cliffs. All bugs must be triaged with explicit severity.

---

**Rule 54 — No Workarounds for Compiler/Runtime Bugs**

Adding code to work around a bug in the compiler, runtime, or class library is forbidden. The underlying defect MUST be fixed.

Temporary mitigations require:

-   a tracking issue;
-   a removal deadline of two weeks or less;
-   explicit approval from the tech lead.

---

**Rule 55 — No Implicit Knowledge Transfer**

All design decisions, trade-offs, historical context, and operational knowledge MUST be captured in persistent, searchable documentation.

Valid documentation includes code comments, ADRs, specs, and design docs. Oral tradition and chat messages are not valid knowledge stores.

---

**Rule 56 — No Premature Simplification**

Do not simplify, abstract, or generalize code until the full problem space is understood and at least two concrete use cases exist.

Premature simplification creates leaky abstractions that fail under real-world Java conditions.

---

**Rule 57 — No Copy-Paste Code or Structural Duplication**

If two code blocks share structure, extract a helper, template, or data-driven approach.

ABI definitions, register lists, opcode tables, pass boilerplate, and barrier stubs must use generators, `constexpr` helpers, or declarative tables.

---

**Rule 58 — No Silent Fallbacks or Default Returns**

Switch statements on closed enums must be exhaustive.

Non-exhaustive switches require `[[assume(false)]]` plus `B2_UNREACHABLE()` or equivalent. Functions must not return arbitrary default values such as `return 0;` or `return nullptr;` when input is invalid.

---

**Rule 59 — No Lazy Data Structures or Algorithms**

Use the right tool, not the convenient tool.

Linear search is forbidden where O(1) lookup is feasible. String comparison is forbidden where symbol IDs suffice.

---

**Rule 60 — No Untested or Unverified Code Paths**

Every branch, edge case, and error path must have explicit test coverage.

“It compiles” is not verification. Only automated, reproducible tests count.

---

**Rule 61 — No Performance-Agnostic Implementation**

Hot-path code must avoid allocations, exceptions, RTTI, virtual dispatch, and cache-unfriendly patterns.

Performance is a feature. Ignoring it in implementation guarantees degradation.

---

**Rule 62 — No Deletion-by-Avoidance (“Too Hard” Is Not a Valid Reason)**

Deleting, disabling, commenting out, or stubbing functionality because it is “too hard” or “too complex” is strictly forbidden.

When encountering difficult problems: decompose, research, prototype, document, and escalate.

---

**Rule 63 — No Fragile Implementations**

All implementations MUST be resilient to malformed input, concurrent access, resource exhaustion, and platform/hardware variation.

Fragile patterns are forbidden, including implicit ordering dependencies, global mutable state, and unchecked pointer arithmetic.

---

**Rule 64 — No Documentation Debt**

Every public API, internal helper, IR node, pass, configuration knob, and non-obvious algorithm MUST have documentation at the point of definition covering:

-   purpose;
-   invariants;
-   rationale;
-   edge cases;
-   cross-references.

Stale documentation is treated as a bug with the same severity as stale code.

---

**Rule 65 — No Easy Fixes — Only Correctness-Preserving Performance Fixes**

When fixing a bug, you must implement the fix that simultaneously preserves performance and correctness.

“Easy” fixes that sacrifice either property are forbidden unless explicitly documented as temporary mitigations with tracking issues and removal deadlines.

---

**Rule 66 — Slop Detection Checklist (For Code Review)**

Every PR reviewer must verify:

-   [ ] No unnamed numeric constants in logic.
-   [ ] No duplicated code blocks or copy-paste patterns.
-   [ ] No silent fallbacks or unsafe default returns.
-   [ ] No prohibited containers such as `std::unordered_map` or heap-backed `std::vector` for small collections in hot paths.
-   [ ] All invariants documented and validated.
-   [ ] No premature abstractions without at least two consumers.
-   [ ] No untracked workarounds or `HACK` comments.
-   [ ] No target-specific logic outside `backend/`.
-   [ ] All new code paths have test coverage.
-   [ ] Hot-path changes justified with profiling/benchmarks.
-   [ ] Every new guard has a `FrameState` attachment.
-   [ ] Every speculative node carries metadata: speculation kind, PGO source, confidence, guard plan, deopt target, invalidation dependency.
-   [ ] Every GC reference in generated code across a safepoint has a stack map.
-   [ ] Every deopt point is reachable and has complete deopt metadata.
-   [ ] No raw object references held across safepoints without GC map entries.
-   [ ] No `getenv()` or mutex-locking calls in dispatch loops or hot paths.
-   [ ] No atomic RMW in per-instruction or per-backedge hot paths unless explicitly justified.
-   [ ] Every memory store of an object reference executes the correct write barrier if the GC requires one.
-   [ ] Every relevant object load executes the required read/load barrier if the GC requires one.
-   [ ] W^X is maintained: no page is simultaneously writable and executable.
-   [ ] Code publication is atomic with release semantics; consumers acquire.
-   [ ] Pending Java exceptions are not silently dropped across runtime or JNI transitions.
-   [ ] Monitor state is preserved across deopt, safepoints, and runtime transitions.
-   [ ] Class-initialization dependencies are recorded where relevant.
-   [ ] invokedynamic and MethodHandle dependencies are recorded and invalidatable.

Failure on any item blocks merge. No exceptions. No “small slop.” No “we’ll fix it later.” Slop is banned.

---

## Part VII: Java Semantic Fidelity Laws

**Rule 67 — Java SE/JVM Is the Semantic Oracle**

All executable Java behavior must match the supported Java SE/JVM reference semantics unless a divergence is explicitly documented, justified, versioned, and approved.

Observable behavior includes at least:

-   program output;
-   exceptions and stack traces;
-   side effects;
-   object mutation;
-   array mutation;
-   static-field mutation;
-   class-initialization behavior;
-   monitor and synchronization behavior;
-   Java Memory Model behavior;
-   weak/soft/phantom reference behavior;
-   finalizer or Cleaner behavior where supported;
-   reflection and introspection;
-   dynamic class loading;
-   invokedynamic behavior;
-   MethodHandle behavior;
-   debugging and JVMTI events where supported;
-   `System.identityHashCode` and reference identity semantics where supported;
-   module access behavior;
-   documented standard-library behavior.

Any unapproved divergence is a correctness bug.

Enforcement: compatibility suite, differential Tier 0/1/2/3 runs, Java-specific semantic tests.

---

**Rule 68 — Observable Effects Must Not Be Reordered, Duplicated, or Deleted**

No optimization may delete, duplicate, hoist, sink, merge, or reorder Java-visible effects unless the effect system proves semantic equivalence.

Java-visible effects include:

-   field reads/writes;
-   static-field reads/writes;
-   array reads/writes;
-   volatile reads/writes;
-   CAS and VarHandle effects;
-   fence effects;
-   monitor enter/exit;
-   class-initialization side effects;
-   exception raises;
-   object allocation side effects where visible;
-   invokedynamic bootstrap/linkage effects;
-   MethodHandle invocation effects;
-   JNI/native side effects;
-   I/O effects;
-   thread start/interrupt effects;
-   monitoring, profiling, and debugging events;
-   changes observable through reflection or introspection.

Enforcement: effect-chain verifier, golden IR tests, differential semantic tests.

---

**Rule 69 — Only Provably Pure Expressions May Be Constant-Folded**

Constant folding may only apply to expressions whose result is independent of:

-   runtime state;
-   class-initialization state;
-   static-field state;
-   object identity;
-   identity hash codes;
-   hash randomization where relevant;
-   environment variables;
-   time;
-   randomness;
-   locale;
-   filesystem state;
-   class-loading state;
-   module state;
-   thread state;
-   GC state;
-   dynamic constants;
-   invokedynamic linkage state.

No call with possible side effects may be constant-folded.

Enforcement: verifier rule, pure-node annotations, negative tests.

---

**Rule 70 — Dynamic Java Features Are First-Class Correctness Requirements**

The JIT must correctly handle or safely fall back for:

-   reflection;
-   dynamic class loading;
-   hidden classes;
-   dynamic proxies;
-   `invokedynamic`;
-   MethodHandles;
-   VarHandles;
-   class redefinition/instrumentation where supported;
-   JVMTI;
-   debugging;
-   profiling;
-   stack walking;
-   serialization where supported;
-   unsafe-style operations if supported;
-   native linkage and JNI callbacks;
-   module access changes where supported.

If a dynamic feature cannot be optimized safely, the system must deoptimize or disable JIT for the affected scope. It must never silently produce wrong introspection or semantics.

Enforcement: dynamic-feature test matrix, deopt tests, compatibility API tests.

---

**Rule 71 — Specialization Requires Versioned, Invalidatable Dependencies**

Every specialization assumption must record a dependency on a versioned entity.

Examples:

-   class identity/version;
-   class hierarchy version;
-   field layout/version;
-   method version;
-   vtable/itable version;
-   interface dispatch table version;
-   invokedynamic call-site version;
-   MethodHandle target version;
-   class-initialization state;
-   module/access state;
-   array element type version;
-   static-field stability version;
-   code object/version;
-   profile version.

If any dependency changes, all dependent compiled code must be invalidated or guarded.

Enforcement: dependency graph tests, invalidation fuzzing, inline-cache tests.

---

**Rule 72 — Java Numeric Semantics Must Be Preserved Exactly**

Numeric specializations must preserve Java semantics, including:

-   two’s-complement integer overflow wrapping for `int`/`long` where specified;
-   exact division-by-zero exceptions for integer division/modulo;
-   floating-point IEEE behavior required by the target Java specification;
-   NaN behavior;
-   negative zero;
-   float/double conversion rules;
-   primitive widening/narrowing rules;
-   boxing/unboxing semantics where modeled;
-   `Math` intrinsics only when exact equivalence is proven;
-   no fast-math-style optimization unless explicitly scoped, proven safe, and disabled by default.

Enforcement: numeric differential tests, overflow tests, NaN tests, boxing/unboxing tests.

---

**Rule 73 — Escape Analysis Must Not Eliminate Observable Objects**

Objects may be scalarized or eliminated only if they cannot be observed by:

-   reference identity comparison where observable;
-   `System.identityHashCode`;
-   weak/soft/phantom references;
-   finalizers where supported;
-   Cleaners where supported;
-   GC introspection where supported;
-   exception tracebacks;
-   reflection;
-   debugging or JVMTI;
-   profiling hooks;
-   JNI escape;
-   `Unsafe`-style address escape if supported;
-   serialization where supported;
-   monitor usage where observable or shared.

If any escape path exists, the object must be materialized.

Enforcement: escape-analysis verifier, materialization tests, deopt tests.

---

**Rule 74 — Java Exceptions Are Control-Flow Values, Not Native Exceptions**

Java exceptions must be represented as runtime object values and control-flow edges.

Native C++ exceptions must not be used to implement Java exception propagation.

The JIT must preserve:

-   exception type;
-   exception message;
-   cause;
-   suppressed exceptions;
-   stack trace;
-   line numbers;
-   frame association;
-   finally-block semantics;
-   try-with-resources cleanup semantics;
-   exception-edge ordering where specified.

Enforcement: exception semantic tests, stack-trace golden tests.

---

**Rule 75 — Frames Must Be Reconstructible on Demand**

If the JIT inlines, merges, elides, or optimizes frames, it must be able to materialize a semantically correct Java frame when required by:

-   stack traces;
-   exceptions;
-   debuggers;
-   JVMTI;
-   profilers;
-   deoptimization;
-   OSR entry/exit;
-   monitor audits;
-   thread dumps.

`FrameState` must be sufficient to reconstruct:

-   bytecode index;
-   line number;
-   local variables;
-   operand-stack/register state;
-   monitor stack;
-   held monitors;
-   pending exception;
-   JVMTI state where active;
-   profiling/tracing state;
-   virtual-thread or continuation state where applicable.

Enforcement: frame materialization tests, deopt tests, debugger tests.

---

**Rule 76 — Threads, Virtual Threads, and Suspension Must Be JIT-Safe**

If the runtime supports virtual threads, continuations, fibers, or other suspension mechanisms, suspension points are semantic boundaries.

The JIT must correctly handle:

-   park/unpark;
-   monitor wait/notify;
-   sleep;
-   virtual-thread mount/unmount where supported;
-   continuation suspend/resume where supported;
-   exception propagation across suspension;
-   local state after resume;
-   safepoint behavior during suspension;
-   JNI blocking transitions where applicable.

Suspension points must be valid deopt/safepoint candidates.

Enforcement: virtual-thread stress tests, suspension deopt tests, monitor stress tests.

---

**Rule 77 — Debugging, Tracing, Profiling, and Monitoring Must Remain Correct**

The JIT must not break Java tooling.

Supported tooling includes at least:

-   JVMTI where supported;
-   debuggers;
-   breakpoints where supported;
-   method entry/exit events where supported;
-   exception events;
-   line-number events;
-   profilers;
-   flight-recorder-style monitoring where supported.

When tooling is active, the JIT must either:

-   emit correct events with correct semantics;
-   run unoptimized lower-tier code;
-   fall back to Tier 0.

Missing events, duplicate events, wrong line numbers, or wrong exception events are correctness bugs.

Enforcement: tracing/profiling differential tests.

---

**Rule 78 — Object Identity and GC Semantics Must Be Explicit**

Object identity semantics for reference equality and `System.identityHashCode` must remain correct.

If using a moving GC:

-   moving objects must not expose unstable addresses to Java;
-   identity hash codes must remain stable;
-   pinned objects must be used where address identity is observable;
-   JNI local/global references must remain valid;
-   handles or stable identity mechanisms must be provided.

No optimization may assume object addresses are stable unless the object model explicitly pins the object.

If `Unsafe`-style raw address exposure is supported, the runtime must either pin the object, use explicit handles, or document the escape and invalidate unsafe assumptions across GC.

Enforcement: identity tests, weakref tests, GC tests, JNI reference tests.

---

**Rule 79 — No Assumptions About Hashes, Randomness, or Addresses**

The compiler must not persist or bake assumptions about:

-   hash values;
-   identity hash codes;
-   `HashMap`/`HashSet` iteration order beyond language guarantees;
-   ASLR addresses;
-   object addresses;
-   code addresses;
-   compressed-oop bases;
-   heap region addresses;
-   randomized runtime values;
-   nondeterministic allocation order.

Persistent artifacts must not contain address-dependent assumptions unless explicitly relocated/validated at load time.

Enforcement: hash-randomization CI where relevant, ASLR replay tests, persistent artifact validation.

---

**Rule 80 — Static Typing Is Not Runtime Proof Unless Certified**

Java static types do not by themselves justify unsafe optimization. Runtime behavior may still be affected by:

-   dynamic class loading;
-   reflection;
-   hidden classes;
-   proxies;
-   instrumentation;
-   JNI;
-   `Unsafe`-style operations;
-   invokedynamic;
-   MethodHandles;
-   module/access changes;
-   class redefinition where supported.

Static proofs must be based on:

-   final/sealed guarantees;
-   closed-world assumptions;
-   module isolation;
-   absence of reflection;
-   absence of dynamic class loading;
-   verified JNI boundaries;
-   verified absence of unsafe escapes;
-   mechanically checked proof artifacts.

If static proof cannot be maintained, the code must use guards or fall back.

Enforcement: static-mode verifier, negative mutation tests.

---

## Part VIII: Deoptimization and GC Integration Laws

**Rule 81 — Deoptimization Metadata Is a Required Compilation Output**

A compilation is not complete until it has produced:

-   deopt points;
-   `FrameState` snapshots;
-   stack maps;
-   GC reference maps;
-   live-range information;
-   materialization plan;
-   interpreter re-entry information;
-   dependency list;
-   guard metadata;
-   exception-state reconstruction info;
-   monitor-state reconstruction info.

If deopt metadata cannot be generated, the compilation must fail and fall back.

Enforcement: compiler verifier, missing-deopt compile-fail tests.

---

**Rule 82 — FrameState Must Be Complete and Machine-Checkable**

`FrameState` must describe enough information to reconstruct the exact lower-tier state.

It must include:

-   bytecode index/instruction position;
-   register-to-interpreter slot mapping;
-   local variable state;
-   operand-stack/register state;
-   object references;
-   primitive values;
-   constants;
-   monitor stack;
-   held monitors;
-   pending exception;
-   line-number state;
-   class-initialization context where relevant;
-   JVMTI/debug/profiling state;
-   virtual-thread or continuation state where relevant;
-   materialized object graph;
-   GC-handle/reference state.

The verifier must reject incomplete `FrameState`.

Enforcement: `FrameState` verifier, forced-deopt tests.

---

**Rule 83 — Guard Failure Must Produce Exact Lower-Tier State**

When a guard fails, the runtime must resume in a state observationally indistinguishable from the state the lower tier would have reached at that point.

This includes:

-   local variables;
-   operand stack/register state;
-   exception state;
-   side effects already committed;
-   monitor state;
-   GC-equivalent reference state;
-   frame visibility;
-   line number;
-   profiling/tracing state;
-   object materialization state.

If exact reconstruction is impossible, the speculation must be rejected at compile time.

Enforcement: differential deopt tests, forced-guard-failure fuzzing.

---

**Rule 84 — Deopt Loops Must Be Detected and Throttled**

Repeated deoptimization at the same site is a performance and correctness hazard.

The runtime must track:

-   deopt count per site;
-   deopt count per method;
-   deopt reason;
-   time window;
-   tier history.

If thresholds are exceeded, the system must:

-   disable the failing speculation;
-   recompile with weaker assumptions;
-   downgrade tier;
-   blacklist the method temporarily or permanently;
-   emit telemetry.

Enforcement: deopt-loop regression tests, telemetry validation.

---

**Rule 85 — Speculative Side Effects Must Be Reversible or Deferred**

Speculative optimization must not commit irreversible Java-visible side effects before the speculation is proven.

If an effect cannot be proven safe:

-   defer it;
-   guard before it;
-   materialize fallback state;
-   or do not perform the optimization.

Field stores, array stores, static-field stores, volatile stores, monitor actions, JNI effects, exception publication, and class-initialization effects are not freely rollback-able.

Enforcement: effect-system audit, speculative-store tests.

---

**Rule 86 — All GC References in JIT Code Must Be Tracked**

Generated code must not hold raw object references in registers, stack slots, or embedded constants across safepoints unless those references are recorded in GC maps.

Rules:

-   every reference register across a call/safepoint must be in a stack map;
-   every reference spill must be visible to GC;
-   embedded object references must use handles or be otherwise tracked;
-   object references must not be hidden in untracked integer registers unless explicitly tagged and supported.

Enforcement: GC map verifier, GC stress tests.

---

**Rule 87 — Read and Write Barriers Must Be Correct**

If the GC requires write barriers, every store of an object reference in generated code must execute the correct barrier.

If the GC requires read barriers, load barriers, or forwarding checks, every relevant reference load must execute them.

Missing barriers are blocker bugs.

Enforcement: barrier verifier, GC stress, moving-GC tests.

---

**Rule 88 — Generated Code Must Poll Safepoints**

JIT code must include safepoint polls at:

-   loop backedges;
-   method calls;
-   allocation sites;
-   long native transitions where specified;
-   OSR entry/exit points;
-   tier-transition points;
-   invalidation points where required;
-   monitor wait/park suspension points where applicable.

Safepoint latency must be bounded.

Enforcement: safepoint stress tests, GC pause tests.

---

**Rule 89 — All JIT Frames Must Be Walkable**

Every JIT frame must be walkable by:

-   GC;
-   deoptimizer;
-   profiler;
-   debugger;
-   exception unwinder;
-   stack-overflow checks;
-   diagnostic tools.

Frame metadata must include:

-   frame size;
-   return address location;
-   saved registers;
-   stack map;
-   deopt info;
-   callee-saved register locations;
-   Java frame association;
-   native/managed transition markers.

Enforcement: stack-walking tests, GC/deopt/profiler integration tests.

---

**Rule 90 — Stack Overflow and Recursion Limits Must Be Checked**

JIT code must respect Java stack limits and native stack limits.

Checks must occur:

-   before entering JIT frames;
-   before inlined calls;
-   before recursive calls;
-   before OSR entry where applicable;
-   before native transitions where stack usage changes;
-   before virtual-thread or continuation stack switches where applicable.

Failure must produce the correct Java exception, usually `StackOverflowError`, not a crash.

Enforcement: recursion-limit tests, stack-overflow tests.

---

**Rule 91 — Allocation Fast Paths Must Handle Failure Safely**

Allocation fast paths may optimize the common case, but slow paths must handle:

-   heap exhaustion;
-   memory allocation failure;
-   GC pressure;
-   finalizer/Cleaner interaction where relevant;
-   allocation callbacks where specified;
-   Java `OutOfMemoryError` semantics.

Generated code must not abort the VM on allocation failure unless the VM is in an unrecoverable state defined by the runtime spec.

Enforcement: low-memory tests, allocation-failure injection.

---

**Rule 92 — Runtime Call Transitions Must Preserve ABI and Runtime State**

Calls from JIT code into runtime helpers must preserve:

-   calling convention;
-   stack alignment;
-   callee-saved registers;
-   GC state;
-   pending Java exception state;
-   thread state;
-   JNI environment state where relevant;
-   monitor state;
-   floating-point/vector register state as required.

Runtime helpers must not assume JIT register contents beyond ABI.

Enforcement: ABI tests, register-clobber tests.

---

**Rule 93 — JNI/Native/FFI Is Opaque Unless Proven**

Calls into JNI or native code are opaque barriers unless a formal ABI/effect proof exists.

Assume native calls may:

-   mutate arbitrary Java state;
-   call back into Java;
-   allocate;
-   throw Java exceptions;
-   load classes;
-   change class hierarchies;
-   modify static fields;
-   invalidate specialization assumptions;
-   acquire/release monitors;
-   trigger GC;
-   observe object layout;
-   use JNI local/global references;
-   enter critical regions;
-   corrupt assumptions if misused.

Optimizations across JNI/native boundaries require explicit proof and invalidation rules.

Enforcement: JNI barrier tests, native callback mutation tests.

---

**Rule 94 — Weak References, Finalizers, Cleaners, and GC Callbacks Must See Valid State**

JIT code must not leave weak references, finalizers, Cleaners, reference queues, or GC callbacks in states where they observe:

-   partially initialized objects;
-   invalid forwarding pointers;
-   untracked references;
-   missing barriers;
-   stale object headers;
-   inconsistent GC state;
-   objects that should have been materialized but were not.

Enforcement: weakref/finalizer/Cleaner stress tests.

---

**Rule 95 — Class/Shape Mutation Must Invalidate Specialized Code**

Any change to assumptions used by inline caches or specialization must invalidate dependent code.

This includes:

-   loading of new subclasses;
-   changes to class hierarchy;
-   changes to interfaces implemented;
-   method redefinition where supported;
-   field layout changes where supported;
-   hidden-class creation;
-   dynamic proxy generation;
-   invokedynamic relinking;
-   MethodHandle target changes;
-   static-field stability changes;
-   module/access changes where supported;
-   instrumentation/redefinition where supported.

Enforcement: mutation-after-JIT tests.

---

**Rule 96 — Tier 0 Is the Universal Correctness Fallback**

Every executable method must be runnable in Tier 0.

No feature may be “JIT-only” unless explicitly part of a documented static mode. If Tier 1/2/3 cannot compile, patch, deopt, or execute code correctly, execution must fall back to Tier 0.

Enforcement: fallback tests, JIT-disable tests.

---

## Part IX: Code Cache, Patching, and Security Laws

**Rule 97 — Executable Memory Must Be W^X**

JIT memory pages must never be simultaneously writable and executable.

Code generation and patching must use one of:

-   write-then-execute with protection changes;
-   separate staging and executable pages;
-   atomic patching of existing executable locations where safe;
-   platform-approved JIT memory mechanisms.

Enforcement: memory-protection tests, OS-specific audits.

---

**Rule 98 — Code Publication Must Be Atomic**

Function entry points, OSR entry points, trampolines, and metadata pointers must be published atomically.

No thread may observe:

-   partially initialized code;
-   uninitialized metadata;
-   missing deopt info;
-   missing GC maps;
-   half-patched jump tables.

Publication must use release semantics; consumers must use acquire semantics.

Enforcement: TSAN tests, concurrent-install stress tests.

---

**Rule 99 — Runtime Patching Must Be Safe Against Concurrent Execution**

Patching running code must be safe.

Requirements:

-   patch sites must be aligned and architecturally safe;
-   instruction sequences must not create invalid intermediate instructions;
-   instruction-cache coherence must be handled where required;
-   concurrent threads must never execute corrupted instructions;
-   patching must either use safepoints or architecture-safe atomic sequences.

Enforcement: patch-under-load tests, architecture-specific tests.

---

**Rule 100 — Old Code May Be Freed Only After Quiescence**

Old compiled code, deopt metadata, and dependency records must not be reclaimed until no thread can be executing or depending on them.

Use:

-   epoch-based reclamation;
-   RCU-like quiescence;
-   safepoint-based retirement;
-   reference counting for code objects where appropriate.

Code reclamation must be distinct from IR node reclamation.

Enforcement: concurrent code retirement tests.

---

**Rule 101 — Every Compiled Artifact Must Record Dependencies**

Every compiled method must record dependencies sufficient for invalidation.

Dependency examples:

-   method identity/version;
-   class identity/version;
-   class hierarchy version;
-   field layout version;
-   interface table version;
-   invokedynamic call-site version;
-   MethodHandle target version;
-   profile version;
-   IR version;
-   compiler version;
-   target feature set;
-   ABI version;
-   runtime configuration.

Enforcement: dependency graph verifier.

---

**Rule 102 — Generated Code Must Be Constrained**

Generated code must only call approved runtime entrypoints and must not directly:

-   perform arbitrary syscalls unless mediated by the runtime;
-   write outside its own frame/runtime-approved memory;
-   execute arbitrary user-provided machine code;
-   load arbitrary dynamic libraries unless approved;
-   bypass sandbox/security policy.

Enforcement: codegen allowlist, backend audit, security tests.

---

**Rule 103 — Platform Exploit Mitigations Must Be Enabled Where Available**

JIT must integrate with platform security features where available:

-   non-executable stack;
-   non-executable heap;
-   CFI;
-   shadow stacks;
-   PAC/BTI on ARM64;
-   pointer authentication where supported;
-   CET where supported;
-   ASLR-safe code generation;
-   code signing where required;
-   sandbox compatibility.

If a mitigation is unavailable, the risk must be documented and configurable.

Enforcement: platform security matrix.

---

**Rule 104 — JIT Spraying Defenses Are Required**

The JIT must not turn attacker-controlled data into executable instruction streams without mitigation.

Mitigations may include:

-   constant blinding;
-   avoiding embedding uncontrolled immediate sequences;
-   separating executable code from embedded data;
-   limiting executable constant islands;
-   code-cache entropy/randomization where appropriate;
-   validating inputs that influence codegen.

Enforcement: security review, exploit PoC tests.

---

**Rule 105 — Profiles, Bytecode, and AOT Artifacts Are Untrusted**

Profile data, serialized IR, AOT artifacts, caches, and class-file inputs must be validated before use.

Malformed inputs must not cause:

-   undefined behavior;
-   memory corruption;
-   arbitrary code execution;
-   VM crashes;
-   silent miscompilation.

Invalid artifacts must be rejected or ignored with telemetry.

Enforcement: artifact fuzzing, schema validation.

---

**Rule 106 — Code Cache Pressure Must Be Managed**

The code cache must have explicit budgets and eviction policies.

The system must monitor:

-   total code size;
-   metadata size;
-   dependency graph size;
-   number of live compiled methods;
-   number of invalidated methods;
-   patchpoint count;
-   deopt metadata size.

When pressure exceeds budgets, the system must throttle compilation, evict cold code, or fall back.

Enforcement: code-cache stress tests.

---

**Rule 107 — AOT Artifacts Must Include a Compatibility Manifest**

AOT artifacts must include:

-   Java version;
-   class-file version;
-   module compatibility data where relevant;
-   IR version/hash;
-   compiler version;
-   pass pipeline hash;
-   target architecture;
-   target feature set;
-   ABI hash;
-   runtime configuration hash;
-   dependency fingerprints;
-   security policy version;
-   creation metadata.

Enforcement: manifest validation tests.

---

**Rule 108 — AOT Artifacts Must Be Verified Before Loading**

AOT loading must verify:

-   manifest compatibility;
-   integrity checksum/signature where required;
-   dependency validity;
-   target feature support;
-   ABI compatibility;
-   security policy compatibility.

On mismatch, the artifact must be rejected. Silent loading of incompatible artifacts is forbidden.

Enforcement: stale/corrupt AOT artifact tests.

---

## Part X: Concurrency, Compilation Scheduling, and Tiering Laws

**Rule 109 — Compiler, Runtime, and GC Shared State Must Be Race-Free**

All shared state accessed by mutator threads, compiler threads, GC threads, JVMTI/debug services, and background services must be synchronized using documented atomic/locking protocols.

TSAN-clean is mandatory for supported concurrent tests.

Enforcement: TSAN CI, concurrency stress tests.

---

**Rule 110 — Function Pointer Swaps Must Be Safe and Reversible**

Installing new code must:

-   use atomic publication;
-   preserve old code until safe;
-   avoid torn calls;
-   avoid invalidating metadata still needed by running threads;
-   support rollback where possible.

Function installation must be testable independently of compilation.

Enforcement: concurrent install/uninstall tests.

---

**Rule 111 — Safepoint Latency Must Be Bounded**

Threads must be able to reach a safepoint within a documented bounded time.

Long-running generated loops must contain polls. Native helpers that run for long durations must cooperate with suspension protocols.

Enforcement: GC pause tests, suspension stress tests.

---

**Rule 112 — Compilation Latency and Memory Budgets Must Be Defined**

Each tier must have explicit budgets:

-   Tier 1 compile latency;
-   Tier 2 compile latency;
-   Tier 2 memory usage;
-   Tier 3 AOT compile time where relevant;
-   IR memory usage;
-   pass fixpoint iteration limits;
-   code size limits;
-   deopt metadata limits.

Budget violations must trigger fallback or cancellation, not mutator stalls.

Enforcement: compile-budget benchmarks.

---

**Rule 113 — Compilations Must Be Cancellable**

If a method is invalidated while compiling, the compiler must be able to cancel or discard the result without leaking memory or installing stale code.

Enforcement: invalidation-during-compilation tests.

---

**Rule 114 — Hotness Counters Must Be Robust**

Profiling counters must be:

-   thread-safe or explicitly racy-with-bounded-error;
-   saturating or overflow-safe;
-   decaying where appropriate;
-   resistant to pathological overflow;
-   correlated with deopt feedback.

Undefined behavior from counter overflow is forbidden.

Enforcement: counter fuzzing, long-run soak tests.

---

**Rule 115 — Recompilation Must Be Throttled**

Repeated compilation of the same method must be limited by:

-   maximum recompiles per method;
-   exponential backoff;
-   deopt-history awareness;
-   code-cache pressure awareness;
-   budget awareness.

No method may cause unbounded compile churn.

Enforcement: recompile-thrash tests.

---

**Rule 116 — OSR Entry and Exit Must Be Semantically Exact**

On-stack replacement must preserve exact program state at OSR entry and exit.

OSR must handle:

-   loop induction variables;
-   iterator state;
-   exception state;
-   local variables;
-   operand-stack/register values;
-   monitor state;
-   virtual-thread/continuation state if supported;
-   deopt from OSR code back to interpreter.

Enforcement: OSR state reconstruction tests.

---

**Rule 117 — Invalidation Must Be Ordered and Visible**

Invalidation of dependencies must be visible before new assumptions are relied upon.

The system must avoid:

-   executing stale code after invalidation beyond allowed grace;
-   installing code based on already-invalid dependencies;
-   racing invalidation with installation.

Enforcement: invalidation race tests.

---

**Rule 118 — No Global Locks on Hot Runtime Paths**

Global locks are forbidden in hot runtime paths unless explicitly approved and budgeted.

Hot paths include:

-   inline-cache updates;
-   guard checks;
-   method entry dispatch;
-   allocation fast paths;
-   read/write barriers;
-   safepoint polls;
-   basic object access;
-   monitor fast paths where feasible.

Enforcement: lock profiling, scalability tests.

---

**Rule 119 — Tier Transitions Must Be Observable**

All tier transitions must be recorded:

-   Tier 0→1;
-   Tier 1→2;
-   Tier 2→3 where applicable;
-   deopt to lower tier;
-   code invalidation;
-   blacklist events;
-   fallback events;
-   recompilation events.

Telemetry must include reasons and counters.

Enforcement: telemetry schema tests.

---

**Rule 120 — Compiler Bugs Must Not Crash User Programs**

A compiler failure should degrade performance, not terminate the application.

Compiler/runtime JIT bugs should result in:

-   fallback;
-   disabled optimization;
-   diagnostic log;
-   telemetry;
-   replay artifact where possible.

Process aborts are only acceptable for unrecoverable VM corruption and must be treated as P0 bugs.

Enforcement: fault-injection tests.

---

## Part XI: IR, Passes, and Backend Laws

**Rule 121 — The IR Must Have an Explicit Effect Model**

The IR must explicitly represent effects and ordering.

Effect classes should include at least:

-   pure computation;
-   allocation;
-   instance-field mutation;
-   static-field mutation;
-   array mutation;
-   volatile/CAS/fence effects;
-   monitor enter/exit;
-   class-initialization effects;
-   exception effects;
-   JNI/FFI effects;
-   GC barrier effects;
-   thread/virtual-thread effects;
-   monitoring/tracing effects;
-   deopt/guard effects;
-   memory reads/writes;
-   invokedynamic linkage effects.

Passes must not reorder effects without proof.

Enforcement: effect-chain verifier.

---

**Rule 122 — Speculative Nodes Must Carry Metadata**

Every speculative node must record:

-   speculation kind;
-   PGO/static source;
-   confidence;
-   guard plan;
-   `FrameState`;
-   deopt target;
-   cost;
-   invalidation dependency;
-   rollback/deferred-effect plan.

No implicit speculation is allowed.

Enforcement: IR verifier.

---

**Rule 123 — Passes Must Declare Contracts**

Each pass must declare:

-   required IR properties;
-   produced IR properties;
-   invalidated analyses;
-   supported tiers;
-   budget;
-   determinism requirements;
-   target dependencies;
-   required verifier checks;
-   telemetry hooks.

Passes that cannot satisfy their contract must fail safely.

Enforcement: pass registry, contract tests.

---

**Rule 124 — Compilation Must Be Deterministic and Replayable**

Given the same bytecode, compiler version, flags, profile snapshot, target description, RNG seed, and feature configuration, compilation must produce deterministic IR and code selection, except for explicitly documented nondeterminism.

Nondeterminism sources must be logged.

Enforcement: deterministic replay tests.

---

**Rule 125 — Passes Must Not Use Hidden Global Mutable State**

Hot-path passes must not depend on hidden global mutable state.

Allowed global state:

-   immutable configuration;
-   interned symbol tables with proper synchronization;
-   read-only target descriptions;
-   versioned caches with explicit invalidation.

Hidden singletons in pass logic are forbidden.

Enforcement: static analysis, code review checklist.

---

**Rule 126 — The Verifier Must Check Deopt and GC Metadata**

The graph verifier must check not only IR consistency but also:

-   every guard has `FrameState`;
-   every deopt point is reachable;
-   every GC reference across safepoint has a map;
-   every effect chain is continuous;
-   every speculative node has invalidation info;
-   every materialized object graph is acyclic or properly handled;
-   every monitor-state dependency is recorded where relevant.

Enforcement: debug verifier runs after every pass.

---

**Rule 127 — Backend Lowering Must Preserve IR Semantics**

Lowering from high/mid IR to machine code must preserve:

-   effect order;
-   exception semantics;
-   numeric semantics;
-   overflow behavior;
-   JMM fence/volatile semantics;
-   GC reference liveness;
-   safepoint placement;
-   deopt point mapping;
-   stack layout constraints;
-   monitor state.

Backend optimizations may not silently change IR semantics.

Enforcement: backend golden tests, differential tests.

---

**Rule 128 — Register Allocation Must Be GC-Reference Safe**

The register allocator must ensure:

-   GC references are not lost across calls/safepoints;
-   spills of references are tracked;
-   register maps are generated;
-   callee-saved/caller-saved conventions are respected;
-   reference registers do not alias untracked integer registers unless allowed by object representation.

Enforcement: register-map verifier, GC stress tests.

---

**Rule 129 — Target Features Must Be Gated and Recorded**

Use of CPU features must be:

-   runtime-detected for JIT;
-   build-time validated for AOT;
-   recorded in code metadata;
-   protected by feature guards where needed.

Generated code must not execute unsupported instructions.

Enforcement: target-feature mismatch tests.

---

**Rule 130 — Every IR Node and Trampoline Must Have a Specification**

No IR node, runtime stub, or trampoline may exist without documentation covering:

-   semantics;
-   effects;
-   tier behavior;
-   lowering;
-   verifier constraints;
-   deopt behavior;
-   GC behavior;
-   tests.

Enforcement: documentation lint, IR node registry.

---

**Rule 131 — Static Proofs Must Be Mechanically Checked**

Tier 3 static optimizations may not rely on human-only proof.

Static proofs must be represented as machine-checkable artifacts or verifier constraints. If proof cannot be checked automatically, the optimization must use guards or be disabled.

Enforcement: proof-verifier tests.

---

**Rule 132 — Every Optimization Must Have a Kill Switch**

Every nontrivial optimization should be disableable by:

-   compiler flag;
-   environment variable;
-   configuration knob;
-   runtime feature gate;
-   per-method annotation where appropriate.

This enables bisection and incident response.

Enforcement: feature-flag matrix.

---

## Part XII: Testing, Observability, and Governance Laws

**Rule 133 — Differential Oracle Testing Must Run Continuously**

CI must compare behavior across:

-   Tier 0;
-   Tier 1;
-   Tier 2;
-   Tier 3/AOT where available;
-   approved Java reference oracle where applicable.

Tests must include:

-   normal programs;
-   exceptions;
-   virtual threads/suspension where supported;
-   JNI/native interactions;
-   dynamic class loading;
-   reflection;
-   invokedynamic;
-   MethodHandles;
-   class mutation;
-   tracing/profiling enabled;
-   GC stress;
-   low-memory stress;
-   recursion limits;
-   numeric edge cases including NaN, negative zero, overflow, and division by zero.

Enforcement: CI differential matrix.

---

**Rule 134 — Fuzzing Must Cover Bytecode, IR, Profiles, and Artifacts**

Fuzzing must target:

-   Java class-file inputs;
-   bytecode inputs;
-   IR inputs;
-   serialized profiles;
-   AOT artifacts;
-   code-cache metadata;
-   patching sequences;
-   deopt metadata;
-   GC barrier sequences;
-   JNI boundaries;
-   invokedynamic call sites;
-   class/class-hierarchy mutation schedules.

Untriaged fuzz failures block release.

Enforcement: scheduled fuzz jobs.

---

**Rule 135 — Sanitizer Matrix Is Mandatory**

CI must run supported configurations with:

-   ASan;
-   UBSan;
-   TSan where concurrency exists;
-   MSan where supported;
-   debug asserts;
-   release builds;
-   interpreter-only mode;
-   JIT-enabled mode;
-   AOT mode where applicable.

Enforcement: CI matrix.

---

**Rule 136 — GC and Deopt Stress Tests Must Be First-Class**

Dedicated stress modes must:

-   force frequent GC;
-   force moving GC where applicable;
-   force allocation failure;
-   force guard failure;
-   force deopt at every supported point;
-   force weak/soft/phantom reference activity;
-   force finalizer/Cleaner activity where supported;
-   force code invalidation under load.

Enforcement: nightly stress, release gating.

---

**Rule 137 — Code Installation and Patching Must Be Concurrency-Tested**

CI must test:

-   installing code while executing old code;
-   invalidating code while running;
-   patching under load;
-   retiring code under load;
-   OSR entry during invalidation;
-   deopt during patching.

Enforcement: concurrency stress tests.

---

**Rule 138 — Performance Gates Must Measure More Than Throughput**

Performance CI must measure:

-   startup time;
-   warmup time;
-   peak throughput;
-   tail latency;
-   compile latency p50/p99;
-   deopt rate;
-   guard overhead;
-   code size;
-   memory usage;
-   GC pause impact;
-   compile CPU cost;
-   memory pressure;
-   tier-transition counts.

A regression in any critical dimension requires waiver.

Enforcement: benchmark suite with thresholds.

---

**Rule 139 — Telemetry Must Be Structured, Stable, and Privacy-Safe**

Telemetry must record:

-   compile attempts;
-   compile failures;
-   fallback reasons;
-   guard failures;
-   deopt reasons;
-   invalidations;
-   code-cache pressure;
-   budget violations;
-   blacklist events;
-   performance counters.

Telemetry must not include source code, user data, or secrets unless explicitly opted in.

Enforcement: schema validation, privacy review.

---

**Rule 140 — Replay Artifacts Must Be Sufficient for Debugging**

A failed compilation or deopt event should be replayable from:

-   class-file/bytecode hash;
-   IR snapshot;
-   pass pipeline state;
-   profile snapshot;
-   compiler flags;
-   target description;
-   RNG seed;
-   runtime config;
-   dependency versions;
-   failure location.

Debugging should start from replay, not anecdote.

Enforcement: replay artifact tests.

---

**Rule 141 — ABI and JNI Must Have Dedicated Tests**

Dedicated tests must cover:

-   Java→native calls;
-   native→Java callbacks;
-   register clobbering;
-   stack alignment;
-   exception propagation through JNI;
-   JNI local/global reference behavior;
-   monitor interactions across JNI;
-   GC interactions across JNI;
-   critical-region behavior where supported;
-   varargs/keyword-equivalent native conventions where supported;
-   error return conventions.

Enforcement: ABI/JNI test suite.

---

**Rule 142 — Security Tests Must Be Part of CI**

Security checks should include:

-   W^X scans;
-   executable memory accounting;
-   JIT spraying PoCs;
-   malformed artifact loading;
-   code-cache exhaustion;
-   patch race attempts;
-   sandbox escape tests where applicable;
-   dependency vulnerability scans.

Enforcement: security CI lane.

---

**Rule 143 — Java Compatibility Must Be Tracked Explicitly**

The project must maintain:

-   supported Java SE version range;
-   supported class-library subset;
-   known divergences;
-   unsupported features;
-   compatibility-suite pass requirements;
-   allowed failure list with owners and expiry dates.

Enforcement: compatibility dashboard.

---

**Rule 144 — All Major Optimizations Must Be Feature-Gated**

Every major optimization must be capable of being disabled independently for bisection and emergency response.

Examples:

-   inlining;
-   partial escape analysis;
-   scalar replacement;
-   lock elision;
-   SLP;
-   LICM;
-   GVN;
-   effect reordering;
-   inline-cache specialization;
-   type specialization;
-   range-check elimination;
-   unrolling;
-   OSR;
-   Tier 2 compilation;
-   Tier 3 AOT loading.

Enforcement: flag matrix test.

---

**Rule 145 — Exceptions to Rules Require an Exception Register**

No rule may be silently bypassed.

Exceptions must include:

-   rule ID;
-   reason;
-   owner;
-   risk assessment;
-   mitigation;
-   telemetry;
-   expiry date;
-   tech lead approval.

Expired exceptions automatically become release blockers.

Enforcement: exception register.

---

**Rule 146 — Every Rule Must Have Enforcement Metadata**

Each rule in this document must specify:

-   enforcement mechanism;
-   owner;
-   severity;
-   test coverage;
-   waiver policy.

Rules without enforcement should be moved to guidelines or given an enforcement plan.

Enforcement: rule metadata lint.

---

**Rule 147 — Maintain a Compliance Matrix**

The repository must maintain a mapping from each rule to:

-   CI check;
-   test suite;
-   verifier;
-   review checklist;
-   documentation;
-   owner.

This matrix must be reviewed each release.

Enforcement: release checklist.

---

**Rule 148 — Architectural Decisions Require ADRs**

Any significant compiler/runtime decision must have an Architecture Decision Record.

ADRs must cover:

-   context;
-   options considered;
-   decision;
-   consequences;
-   performance impact;
-   correctness impact;
-   security impact;
-   rollback plan.

Enforcement: PR template requirement.

---

**Rule 149 — Builds Must Be Hermetic and Dependencies Must Be Pinned**

Compiler/runtime builds must be reproducible.

Requirements:

-   pinned dependencies;
-   locked toolchains where practical;
-   no network access during tests;
-   reproducible artifact hashes;
-   supply-chain review for new dependencies.

Enforcement: build reproducibility tests.

---

**Rule 150 — Stale Documentation Is a Defect**

Documentation must be updated in the same PR as behavior changes.

This includes:

-   rules doc;
-   IR spec;
-   effect system spec;
-   ABI spec;
-   pass documentation;
-   runtime documentation;
-   telemetry schema;
-   compatibility matrix.

Stale docs are treated like stale code.

Enforcement: docs lint, PR checklist.

---

## Part XIII: Definitions

**Hot path:** Compiler pass execution, guard execution, inline-cache fast paths, allocation fast paths, barrier fast paths, deopt entry trampolines, and dispatch loops. Not deopt materialization, which may allocate under budget.

**Guard:** A runtime mechanism that validates a speculative assumption. May be a hardware branch, class/version check, method-version check, field-layout check, patchpoint, trap, dependency invalidation, or hardware check.

**FrameState:** A snapshot attached to a speculative node that allows the deoptimizer to reconstruct the exact lower-tier Java execution state.

**Deopt:** The process of transferring execution from a higher tier to a lower tier, typically Tier 0, while preserving observable Java program state.

**Safe point:** A point in generated code where the thread can safely pause for GC, deopt, thread suspension, or invalidation. Must have bounded latency.

**Observable behavior:** Program output, exceptions, stack traces, side effects, object mutation, monitor behavior, JMM-visible behavior, weak-reference behavior, finalization/Cleaner behavior, reflection, debugging/JVMTI events, and class-initialization behavior.

**Effect:** A Java-visible operation that cannot be freely reordered, deleted, or duplicated without semantic proof.

**Dependency:** A versioned entity that a specialization relies on. If the dependency changes, the specialization must be invalidated or guarded.

**Code installation:** The atomic publication of a compiled method’s entry point, metadata, deopt info, GC maps, and dependency records.

**Quiescence:** A state where no thread is executing old code or depending on old metadata, allowing safe reclamation.

**Speculation:** An optimization that assumes a runtime property holds, guarded by a mechanism that triggers deopt on failure.

**Static proof:** A mechanically-checked artifact demonstrating that a property holds without runtime guards.

**Profile confidence:** A metric combining sample count, stability, age, decay, variance, and deopt correlation. Low-confidence data must not trigger aggressive speculation.

**Tier transition:** A change in execution tier, including Tier 0→1→2→3 or deopt to a lower tier. Must be observable and recorded.

**Fallback:** Graceful degradation to a lower tier or disabled optimization when compilation or speculation fails.

**Kill switch:** A mechanism to disable a specific optimization for bisection and incident response.

---

## Part XIV: Normative References

-   Java Language Specification for the supported Java version.
-   Java Virtual Machine Specification for the supported class-file and bytecode version.
-   Approved Java compatibility oracle / compatibility kit behavior.
-   `docs/deopt_backend.md` — deopt system and backend design: T0 canonical state, FrameState, deopt metadata, MIR pipeline, W^X code publication.
-   `docs/stencils.md` — precompiled stencil system: stencil format, categories, patching model, and Stencil Rules 1-10 (Amendment C).
-   `docs/cpp26_standards.md` — B-2 C++26 code standards (CS-1 through CS-13) and the Java/C++ two-domain testing contract.
-   `docs/ir_spec.md` — IR node specifications.
-   `docs/effect_system.md` — effect model and effect-chain rules.
-   `docs/abi.md` — calling conventions and native linkage.
-   `docs/java_semantics.md` — Java-specific runtime behavior.
-   `docs/jni_ffi.md` — JNI/native boundary rules.
-   B-2 telemetry schema.
-   B-2 compatibility matrix.
-   Target platform ABI documents, such as SysV x86-64 and AAPCS64.
-   Security policy document.

---

## Part XV: Rule Severity and Waiver Process

**Severity levels:**

-   **P0 (Blocker):** Silent data corruption, security vulnerabilities, crashes on valid input. Blocks release.
-   **P1 (Critical):** Wrong results, missing deopt metadata, GC unsafety. Blocks merge to main.
-   **P2 (Major):** Performance regressions greater than 5%, missing tests, documentation debt. Requires waiver with expiry.
-   **P3 (Minor):** Style, naming, minor optimizations. Tracked but non-blocking.

**Waiver process:**

1.  File an exception in the exception register per Rule 145.
2.  Include rule ID, reason, owner, risk assessment, mitigation, telemetry, expiry date, and tech lead approval.
3.  Waivers auto-expire. Expired waivers become release blockers.
4.  No rule may be silently bypassed. Silent bypass is itself a Rule 145 violation.

---

## Part XVI: Compliance Matrix

The full compliance matrix, mapping Rule → CI check, test suite, and owner, is maintained in `docs/compliance_matrix.md` and reviewed each release.

Example entries:

| Rule | Enforcement | Test Suite | Owner | Notes |
| :--- | :--- | :--- | :--- | :--- |
| 3 | IR verifier | guard_tests | compiler team | Every PGO decision needs a guard |
| 67 | differential CI | java_compat | runtime team | Java SE/JVM is the oracle |
| 86 | GC map verifier | gc_stress | GC team | No untracked refs across safepoints |
| 87 | barrier verifier | gc_barrier_stress | GC team | Read/write barriers must be exact |
| 93 | JNI tests | jni_abi | runtime team | JNI is opaque unless proven |
| 97 | OS memory tests | security_ci | security team | W^X mandatory |

---

## Part XVII: Law Amendments

Amendments modify or clarify the laws in Parts I–XVI. **Where an amendment conflicts with an earlier rule or Part, the amendment wins.** Amendments carry the same severity, waiver, exception-register, and enforcement obligations as all other laws (Rule 145, Rule 146).

Current amendments:

- **Amendment A — No-IR Baseline Tier**
- **Amendment B — Tier Model Redesign**
- **Amendment C — Precompiled Stencil Subsystem**

---

## Amendment A — No-IR Baseline Tier

**Status:** Accepted 2026-08-30
**Amends:** Rule 1 (One Pipeline, Multiple Inputs) and all tier language implying Tier 1 is IR-based.

The unified IR pipeline requirement applies to T2 and T3.

T1 may be implemented as a no-IR baseline JIT.

T1 is exempt from IR-specific rules only where those rules require an IR graph.
T1 is not exempt from correctness, deoptimization, GC, safepoint, Java semantic,
security, or performance laws.

T1 must provide:

- deterministic compilation
- linear state mapping to T0
- deopt support
- stack maps / GC maps
- safepoint polling
- fallback to T0
- telemetry on fallback

---

## Amendment B — Tier Model Redesign

**Status:** Accepted 2026-08-30
**Amends:** Part I (Architectural Overview), Rule 1, Rule 2, and Rule 112, where they conflict with the redesigned tier model.

### B.1 — Tier Model

The B-2 execution model consists of four tiers:

1. **T0 — Direct-Threaded Register Interpreter.** The correctness fallback. It executes quickened register bytecode (RBC), not raw JVM stack bytecode, using direct-threaded dispatch — computed goto where the C++ toolchain supports labels-as-values, generated assembly or an equivalent direct-threaded token dispatch model otherwise. T0 provides inline caches, profiling, superinstructions, and safepoint polls. It takes the fastest possible interpreter path, performs no heavy optimization, is the exact Java state reconstruction target for deoptimization, and every compiled tier must be able to deopt back to T0 state.

2. **T1 — No-IR Baseline JIT.** A very fast compiler that does not build an IR graph. It may be a template JIT, a copy-and-patch JIT, or a linear bytecode-to-native expansion, compiling directly from RBC to machine code. It removes interpreter dispatch overhead. It patches locals/register slots, field offsets, constant pool entries, call targets, inline-cache stubs, and deopt metadata; emits simple stack maps and basic safepoints; and may use lightweight inline caches. It performs no sea-of-nodes, no global dataflow, no aggressive optimization, no speculative effect reordering, no loop transformations, no vectorization, and no escape analysis. This tier exists to give startup/warmup speed without paying IR compile cost. T1 is governed by Amendment A and must obey all correctness, deoptimization, GC, safepoint, Java semantic, security, and performance laws.

3. **T2 — Optimizing JIT.** The main peak compiler and the first IR tier. Input: RBC, PGO, class hierarchy information, dependency information, and interpreter/T1 profile data. Core IR: sea-of-nodes, index-based `NodeId`, explicit effects, guards, `FrameState`, dependencies, and deopt metadata. Required capabilities: inlining, devirtualization, guard insertion, escape analysis, loop optimizations, vectorization/SWLP, range-check elimination, lock elision, NaN boxing lowering, backend lowering, register allocation integration, and codegen integration.

4. **T3 — AOT / Static JIT.** Uses the same T2 optimizer pipeline, offline. Input: classfiles, static proofs, PGO if available, and closed-world assumptions if available. Output: native code, stack maps, deopt metadata or static no-deopt metadata, a dependency manifest, and a compatibility manifest. AOT cannot assume dynamic Java behavior is impossible unless mechanically proven. If proof fails, AOT must either emit guarded code or leave the method for T0/T1/T2.

### B.2 — Pipeline Shape

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

### B.3 — Tier Transitions

Upward:

```text
T0 interpreter
   |
   | heat
   v
T1 no-IR baseline
   |
   | sustained heat + useful profile
   v
T2 optimizing JIT
   |
   | offline / static proofs / PGO
   v
T3 AOT
```

Deopt direction:

```text
T3 -> T2 or T0 if dynamic invalidation
T2 -> T1 or T0 on guard failure
T1 -> T0 on unsupported trap
T0 is always final fallback
```

All tier transitions remain subject to Rule 119 (observable and recorded).

### B.4 — Pass Suite Scope

The full optimization pass suite — including the named first-class passes SWLP (Superword-Level Parallelism), PEA (Partial Escape Analysis), and NaN boxing lowering — applies to **T2 and T3 only**. T1 performs no optimization passes. The special pass laws in Part XVIII bind T2 and T3.

### B.5 — What Changes From the Pre-Amendment Laws

1. **T1 is now no-IR** — previously the baseline tier might have used a lightweight IR; now it must be template/copy-patch/linear.
2. **T2 becomes the first IR tier** — sea-of-nodes starts here.
3. **AOT uses the T2 pipeline** — not a separate optimizer.
4. **SWLP becomes a first-class pass** — not just loop vectorization.
5. **Partial Escape Analysis becomes a named required pass** — not just full escape analysis.
6. **NaN boxing becomes a cross-team representation lowering feature** — not just a normal optimization pass.

---

## Amendment C — Precompiled Stencil Subsystem

**Status:** Accepted 2026-08-30
**Amends:** Amendment A (No-IR Baseline Tier) and Amendment B (Tier Model Redesign), where they describe the T1 implementation strategy and runtime code generation.
**Normative spec:** `docs/stencils.md` (§14 restates these rules).

T1 is implemented as a **stencil compositor**: it selects precompiled,
relocatable machine-code stencils for an RBC method, copies them into a
writable staging buffer, patches the declared holes, and publishes the code to
executable memory atomically under W^X. T2/T3 use precompiled stencils for
stubs and runtime support: deopt stubs, uncommon-trap stubs, IC stubs,
barrier stubs, safepoint-poll stubs, materialization helpers, and vector
scalar-fallback fragments. Stencil generation happens at build time via the
stencil toolchain (`tools/stencilgen/`). The runtime never generates stencil
bodies.

**T1 frame model:** T1 stencil frames are T0-compatible — the T0 register
file lives in the frame — so T1 deopt is a pc-map lookup plus a register flush
into the T0 interpreter, without IR state reconstruction. This does not weaken
any deopt law in Part VIII.

**Stencil Rules (binding on every team that emits, patches, or consumes
stencils):**

1. **Stencils are immutable.** The runtime must not generate new stencil
   bodies. It may only copy existing stencils, patch described holes, and
   compose them into code buffers. New stencil bodies are built by the
   toolchain, never by the runtime.
2. **Every stencil must have metadata:** patch table, clobber info, stack map
   info, safepoint info, deopt info where applicable, exception behavior,
   target feature requirements, and effect tags. No stencil may exist
   without them.
3. **Only described holes may be patched.** Arbitrary instruction mutation is
   forbidden.
4. **Stencils must be versioned.** Artifacts carry the stencil format
   version, target arch, target feature hash, ABI hash, compiler hash, and
   runtime config hash. A stencil that fails validation is rejected.
5. **Stencil instantiation must be W^X safe.** Copying and patching happen in
   writable staging memory; publication to executable memory preserves W^X.
6. **Stencil deopt metadata is mandatory.** Any stencil that can trap, call,
   safepoint, or deopt must carry enough metadata to reconstruct T0 state.
7. **Stencils must not hold untracked GC references.** Any GC reference live
   across a call, safepoint, runtime helper, deopt stub, or allocation must be
   tracked in stack maps or handles.
8. **Stencil fallback is mandatory.** If no valid stencil exists for an RBC
   sequence, T1 falls back to T0. No silent miscompile.
9. **Stencil cache pressure must be managed.** The runtime tracks stencil
   code size, patch table size, metadata size, IC stub count, and retired
   stencil instances; under pressure it throttles T1 compilation or evicts
   cold code.
10. **Stencil changes require a cross-team message.** Any stencil format
    change requires an RFC message to affected teams (Baseline No-IR,
    Codegen, Interpreter, IR, Passes, RegAlloc, AOT, GC/Runtime).

---

## Part XVIII: Special Pass Laws

These laws govern the three first-class passes introduced by the tier redesign. They bind T2 and T3. They are enforced like all other laws: violations follow the severity model in Part XV and the exception process in Rule 145.

---

**SWLP Rule:**

The Superword-Level Parallelism pass must not vectorize any operation unless:

1. dependencies are proven safe, or
2. versioned runtime checks are emitted, or
3. a scalar fallback is guaranteed.

SWLP must preserve Java numeric semantics exactly.
Fast-math is forbidden.
Vectorization must not reorder Java-visible effects.

SWLP must prove or guard: no harmful aliasing; known array types; known array element sizes; safe or guarded bounds; safe or handled alignment; safe loop bounds; preserved floating-point semantics; preserved integer overflow semantics; existence of a scalar fallback; handled tail iterations; and masked or scalarized partial vectors.

SWLP must preserve integer division-by-zero behavior, arithmetic exception behavior, the IEEE float/double behavior required by Java, NaN behavior, negative zero behavior, and exact primitive conversion rules.

SWLP must run after inlining, GVN/CSE, range analysis, alias analysis, loop normalization, and type specialization.

SWLP must have a cost model, a kill switch, golden tests, a fallback path, and telemetry.

---

**PEA Rule:**

Partial Escape Analysis may scalar-replace an object only if every escape
point is either:

1. proven non-observable,
2. guarded,
3. materialized, or
4. rejected.

PEA must not eliminate objects observable by identity, weak references,
finalizers, Cleaners, JNI, reflection, debugging, JVMTI, serialization,
or shared monitors.

PEA must produce scalar replacement nodes, materialization plans, FrameState-compatible object reconstruction, deopt materialization metadata, and GC-safe reference handling.

PEA must materialize the object at: calls that may observe it; safepoints where required; deopt points; exception edges; stores to escaping locations; monitor enter where identity or shared locking matters; JNI transitions; and reflection/interface boundaries.

PEA must have a budget, a kill switch, golden tests, deopt materialization tests, and weakref/finalizer negative tests.

---

**NaN Boxing Rule:**

NaN boxing is a representation optimization only.

It must not change Java-visible primitive values, object identity, GC
reference tracking, deopt reconstruction, or floating-point bit-level
behavior unless explicitly proven and documented.

NaN boxing must be disabled by default until all cross-team contracts are
approved.

NaN boxing must have a global kill switch.

NaN boxing must additionally have: per-target enable flags; verifier checks; GC map checks; deopt tests; interpreter round-trip tests; baseline round-trip tests; T2 lowering tests; and AOT artifact versioning.

**Safe design rule for B-2:** floating-point computation uses real FP registers. NaN boxing is used only for tagged value storage where the type is known or where canonicalization is proven safe. Reference values may be NaN-tagged in generic slots; real Java floating-point values are kept unboxed in FP slots/registers. Arbitrary Java `double` NaN payloads must not collide with object reference tags unless the runtime controls and documents the canonicalization behavior.

NaN boxing must not break: Java primitive semantics; float/double NaN behavior; `Double.doubleToLongBits`; `Double.doubleToRawLongBits` where supported; `Float.floatToIntBits`; `Float.floatToRawIntBits` where supported; object identity; GC reference tracking; write barriers; stack maps; deopt reconstruction; JNI reference rules; compressed oop decoding; or `System.identityHashCode`.

**Ownership:** NaN boxing is a cross-team representation contract. The lowering pass lives with the Passes Team, but the representation contract must be approved by all affected teams: Interpreter, Baseline No-IR, IR, Passes, RegAlloc, Codegen, AOT, and GC/Runtime where present.

---

*End of B-2 Java Runtime Compiler Laws & Architecture Specification.*
*Compliance is not optional. It is the foundation of B-2.*