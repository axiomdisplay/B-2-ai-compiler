# TurboScript Compiler Laws & Architecture Specification

**Status:** Stable
**Owner:** TurboScript Systems Dev Team
**Last Updated:** 2026-08-25
**Target:** High-performance JavaScript engine via TurboScript Bytecode and the JavaScript Semantic Profile
**Related Sections:** `ir_spec.md`, `effect_system.md`, `abi.md`, `bytecode_spec.md`, `javascript_semantic_profile.md`, `runtime_contracts.md`

This document is the authoritative, uncompressed transcription of the laws that govern the TurboScript compiler and runtime. Every commit to the `compiler/`, `runtime/`, `tools/`, and `tests/` trees must comply. CI verifies them. There are no exceptions.

---

## Part 0: JavaScript Semantic Contract

TurboScript is a **high-performance JavaScript engine** designed to execute ECMAScript-compliant code through a tiered compiler and runtime infrastructure built on a Sea of Nodes intermediate representation.

TurboScript is not a generic virtual machine. It is purpose-built for JavaScript. However, the compiler and runtime must never hardcode JavaScript semantic assumptions directly into core optimization passes, IR invariants, backend lowering, or runtime fast paths unless those semantics are explicitly exposed through the JavaScript Semantic Profile.

All JavaScript code must flow through TurboScript via the following contract:

1. **TurboScript Bytecode**
   A typed, register-based, effect-carrying bytecode format that can be validated, profiled, interpreted, baseline-compiled via Copy-and-Patch stencils, and optimized through the full Sea of Nodes pipeline.

2. **JavaScript Semantic Profile**
   A versioned description of the ECMAScript semantics, object model, memory model, exception model, dynamic features, native interop rules, and observability requirements that TurboScript must preserve.

3. **ECMAScript Semantic Oracle**
   The ECMAScript specification (Test262 suite) and the host environment API contracts (Web APIs, Node.js APIs) against which TurboScript can be compared for semantic fidelity.

4. **Runtime Capability Declarations**
   Explicit declarations of which dynamic, reflective, numeric, concurrency, debugging, and memory-management features the JavaScript environment supports.

The TurboScript core compiler must remain semantically disciplined. All JavaScript-specific semantics must be expressed through:

- The JavaScript Semantic Profile
- capability flags
- effect classes
- dependency records
- lowering hooks
- runtime helpers
- verifier constraints
- test matrices

If a JavaScript feature cannot be represented safely in TurboScript IR, the frontend must lower that feature conservatively or mark it opaque. The runtime must then execute it through a lower tier or through approved runtime helpers.

### JavaScript Semantic Profile Requirements

The JavaScript Semantic Profile must define, at minimum:

- ECMAScript version
- dialect/version hash
- bytecode format version
- object header layout
- object shape/hidden class version model
- prototype chain versioning rules
- module/import semantics
- global/module/builtin namespace semantics
- exception model
- stack trace model
- numeric semantics including NaN, negative zero, BigInt, and Number
- operator overload or dispatch-hook semantics including toPrimitive, valueOf, toString
- dynamic member/property access semantics
- reflection/introspection capabilities
- frame introspection capabilities
- debugging/profiling/tracing hooks
- weak-reference model including WeakRef and FinalizationRegistry
- finalization model
- memory management model
- identity semantics
- native/FFI model
- concurrency/threading model including Web Workers and SharedArrayBuffer
- suspension model including generators, async functions, and async generators
- static certification rules if any

### JavaScript Capability Flags

The JavaScript Semantic Profile must declare capability flags such as:

- `HasDynamicCodeEvaluation`
- `HasDynamicCompilation`
- `HasDynamicImports`
- `HasDynamicMemberMutation`
- `HasOpenClasses`
- `HasMonkeyPatching`
- `HasFrameIntrospection`
- `HasWeakReferences`
- `HasFinalizers`
- `HasIdentityAddressObservation`
- `HasReferenceCountIntrospection`
- `HasMovingGC`
- `HasResumableFunctions`
- `HasGenerators`
- `HasCoroutines`
- `HasAsyncFunctions`
- `HasFibers`
- `HasContinuations`
- `HasOperatorOverloading`
- `HasDynamicDispatch`
- `HasModuleReloading`
- `HasFFI`
- `HasGlobalRuntimeLock`
- `HasFreeThreading`
- `HasTracingHooks`
- `HasProfilerHooks`
- `HasDebuggerHooks`
- `HasStaticCertification`
- `HasProxyObjects`
- `HasSymbolSpecies`
- `HasSymbolToPrimitive`
- `HasSymbolIterator`
- `HasSymbolHasInstance`
- `HasPrivateFields`
- `HasPrivateMethods`
- `HasTemporalDeadZone`
- `HasMappedArguments`
- `HasStrictMode`
- `HasSloppyMode`
- `HasEvalScope`
- `HasWithScope`
- `HasSharedArrayBuffer`
- `HasAtomics`

If a capability is absent, the compiler must not assume the feature exists. If a capability is present, the compiler must preserve its observable semantics.

---

## Part I: Architectural Overview

The TurboScript execution model consists of four distinct, seamlessly integrated execution tiers. Transitions between tiers are governed by profile data and static guarantees, never by arbitrary timeouts.

1. **Tier 0: Direct-Threaded Register Interpreter**
   The baseline execution engine. Uses computed gotos and a register-based bytecode representation, not a stack-based representation, to minimize dispatch overhead. Uses 24-bit fixed-width instructions with 256 virtual registers per frame. This tier provides fast startup, minimal memory footprint, full JavaScript semantic fidelity, and the ultimate fallback for all JavaScript code. Collects initial Inline Cache shape feedback, type histograms, branch probabilities, and call target profiles.

2. **Tier 1: Baseline JIT via Copy-and-Patch Stencils**
   Triggered by low-level heat, such as loop backedges or function invocation counts. Compiles in linear time using pre-compiled machine code templates called Super Stencils. Performs basic type specialization, copy propagation, simple inlining, and lightweight guard insertion. No intermediate representation is constructed. Compilation time is strictly bounded to prevent mutator stalls.

3. **Tier 2: Optimizing JIT via Sea of Nodes**
   Triggered by sustained heat and rich PGO data. Constructs the full TurboScript Sea of Nodes IR. Employs the full TurboScript optimization pipeline including all 53 passes. Performs aggressive speculative optimizations such as partial escape analysis, scalar replacement, guard-based devirtualization, SLP vectorization, guard hoisting, guard elimination, range analysis, and speculative effect reordering where legal. Requires full FrameState and deoptimization infrastructure.

4. **Tier 3: AOT / Static JIT**
   Applied to code with provable static guarantees. Examples include sealed types, final declarations, isolated modules, fully annotated code, absence of dynamic introspection, and mechanically verified static constraints. Bypasses speculation and deoptimization overhead entirely where permitted. Maximum optimization, zero guard checks, direct native code emission.

---

## Part II: The Unified Pipeline & Speculation Laws

**Rule 1 — One Pipeline, Multiple Inputs**
The IR, pass interfaces, verifier constraints, and correctness constraints are unified across Tier 1, Tier 2, and Tier 3. Tiers differ only by budgets, enabled speculation policies, available proofs, JavaScript capabilities, and telemetry requirements.

- Tier 1 Input: Static IR + Minimal Heuristics + JavaScript Semantic Profile.
- Tier 2 Input: Static IR + PGO Data + JavaScript Semantic Profile.
- Tier 3 Input: Static IR + Static Proofs + JavaScript Semantic Profile Certification.

There is no "JIT-only" pass list. Forcing the exact same pass sequence can waste compile time in Tier 1 or prevent legitimate tier-specific lowering. If a pass exists, it must handle all modes via a unified interface, but the pipeline may apply tier-specific filters.

**Rule 2 — PGO is a Force Multiplier, Not New Logic**
PGO data does not change what the compiler does; it changes how aggressively it does it.

Example: a speculative effect-reordering pass may be conceptually available in all tiers. In Tier 3, it requires static proof. In Tier 2, it accepts high-confidence PGO and inserts a guard. In Tier 1, it is skipped due to budget.

PGO must never introduce new semantic behavior. It only enables or disables optimizations that are already semantically legal under guard or proof.

**Rule 3 — Every PGO-Driven Decision Requires a Guard**
If a pass makes a decision based on PGO, it must emit a validated guard mechanism: runtime check, shape/version guard, patchpoint, trap, dependency invalidation, hardware check, or another approved validation mechanism.

Examples of PGO-driven decisions include:

- pointers A and B never alias
- this object property lookup is monomorphic
- this call site has one dominant receiver type
- this integer value remains in range
- this object shape is stable
- this branch is almost always taken
- this function is usually called with a specific signature
- this exception path is cold

Guard success executes the optimized path. Guard failure triggers deoptimization or fallback.

**Rule 4 — Deoptimization Must Reconstruct Tier 0 State**
When a JIT guard fails, the runtime must deoptimize to the exact same state the Tier 0 Direct-Threaded Register Interpreter would have been in at that instruction position.

This includes:

- restoring register values
- re-materializing JavaScript frames
- restoring local variables
- restoring closure cells
- restoring exception state
- restoring tracing/monitoring state
- restoring JavaScript-visible memory effects
- restoring reference-management state where observable
- rolling back or compensating speculatively reordered effects only where legal

Speculative execution must not perform irreversible JavaScript-visible side effects before the last guard protecting that speculation. If side effects are moved, they must be either proven non-observable, deferred until committed, or supported by a verified compensation mechanism.

Rolling back memory writes is dangerous if JavaScript-visible effects already happened. Therefore, speculative side effects are governed by Rule 85.

**Rule 5 — FrameState is Mandatory for All Guards**
Every node that introduces a speculative assumption must have a FrameState attachment. This snapshot allows the deoptimizer to rebuild the JavaScript execution world if the speculation fails.

FrameState must include enough information to reconstruct the JavaScript-visible state required by the JavaScript Semantic Profile, including but not limited to:

- bytecode offset
- source position
- local variables
- closure cells
- free variables
- global/module namespace version
- builtin/runtime library version
- exception state
- tracing/profiling state
- suspension state where applicable including generator and async function state
- reference-management state where observable

---

## Part III: Compilation Pipeline & Memory Laws

**Rule 6 — NO EXCEPTIONS ON THE HOT PATH**
Native C++ exceptions are forbidden on compiler/runtime hot paths. The JIT compiler, AOT compiler, and Runtime Deoptimization engine MUST be compiled with `-fno-exceptions`. Zero throw statements are allowed in any code path executed during compilation or runtime specialization.

All fallible operations MUST use `std::expected<T, Diagnostic>` or `Result<T, Error>`.

JavaScript exceptions remain first-class runtime values and must be modeled explicitly in the Sea of Nodes IR. Do not confuse JavaScript exceptions with C++ exceptions. If a JIT compilation fails, it returns an Error variant, causing the system to silently fall back to Tier 0 or Tier 1. No stack unwinding. No catch blocks. No overhead.

**Rule 7 — Zero-Allocation Hot Path**
Both AOT and JIT compilers must use `std::pmr::monotonic_buffer_resource` for IR allocation. Bulk-free after compilation. No malloc or free in the compiler hot path.

Hot path means: compiler pass execution, guard execution, inline-cache fast paths, allocation fast paths, and deopt entry trampolines. Deopt materialization may allocate only through a controlled runtime path with explicit budgets because deopt may legitimately need to materialize objects that were scalarized by Partial Escape Analysis.

**Rule 8 — No RTTI**
Both pipelines are compiled with `-fno-rtti`. Use `enum class NodeKind` for type switching. RTTI is forbidden in the IR and backend to ensure maximum devirtualization and cache locality.

**Rule 9 — No `std::shared_ptr` or `std::function` in Hot IR Code**
They allocate and incur atomic overhead. Use raw pointers plus stable NodeIds inside passes.

**Rule 10 — Every Pass Must Be Idempotent and Monotonic**
Running the same pass twice must produce the identical IR. A pass either reduces node count or moves the IR closer to a normal form. If a pass can grow the IR, such as Loop Unrolling or SLP Vectorization, it must run inside a guarded fixpoint with a strict budget.

**Rule 11 — Mutator Threads Never Block on JIT**
If a function becomes hot and triggers a JIT compilation, the mutator thread continues executing the current tier. The JIT runs asynchronously on a background compiler thread. Once ready, a safe-point patch swaps the function pointer. Function pointer publication must be atomic and safe against concurrent execution. Old code must remain valid until quiescence. A safe-point patch is not enough if publication is torn or old code is freed too early.

**Rule 12 — Thread-Local Allocation for Mutators**
Mutator threads use thread-local bump pointers, lexical regions, or equivalent thread-local allocation mechanisms for their own runtime allocations where the JavaScript memory model permits. Global synchronization happens only at explicit yield points, safepoints, or memory-model boundaries.

**Rule 13 — Compiler Threads Never Block on Mutator State**
The compiler works on a frozen snapshot of the IR and PGO data. Mutator updates after the snapshot are picked up by the next compilation.

**Rule 14 — Epoch-Based Reclamation**
Old JIT code and IR nodes are reclaimed using epoch-based garbage collection. When the optimizer replaces a Node, the old node is tagged with an epoch. Once all threads advance past that epoch, the memory is bulk-freed. This avoids both locks and use-after-free. Generated code must not be reclaimed until no thread can be executing it or depend on its deopt metadata. IR nodes and machine code need different reclamation guarantees.

---

## Part IV: The Numbered Rules

### Data Structures & IR Design

**Rule 15 — Index-Based Graph**
Never use raw pointers (Node*) for edges in the Sea of Nodes. All node references must use a 32-bit integer index:

```cpp
using NodeId = uint32_t;
```

This cuts memory footprint in half, doubles L1/L2 cache capacity, and makes the IR trivially serializable and immune to pointer invalidation during arena reallocation.

**Rule 16 — Interned Symbols**
Never pass, compare, or store `std::string` or `std::string_view` in the IR or passes. All identifiers, variable names, function names, type names, field names, module names, and JavaScript property names must be interned into a global SymbolTable at the frontend. The IR must only use a SymbolId which is a uint32_t.

**Rule 17 — Cache-Friendly Hash Maps**
`std::unordered_map` and `std::map` are forbidden in the compiler hot path. For Global Value Numbering, Hash-Consing, and any pass requiring a hash table, you must use a cache-friendly, open-addressing hash map such as `tsl::robin_map` or `std::flat_hash_map`.

**Rule 18 — Sparse Sets and BitVectors for Pass Data**
Ban `std::set`, `std::unordered_set`, and `std::vector<bool>` for dataflow analysis. Passes tracking sets of NodeIds, such as liveness, dominators, and visited sets, must use Sparse Sets for small dense sets or BitVectors for large sparse sets.

**Rule 19 — Small Buffer Optimization for Variable-Length Data**
Ban `std::vector` for data that usually has 1 to 4 elements. For use-def chains, instruction operands, and basic block predecessors and successors, use a `SmallVector<T, N>`, where N is typically 2, 3, or 4.

**Rule 20 — Structure of Arrays for Bulk Pass Processing**
When a pass needs to process a specific field of millions of nodes, do not iterate over the Node structs. Extract that attribute into contiguous `std::pmr::vector` storage in Structure-of-Arrays layout to allow perfect CPU prefetching and SIMD vectorization on the compiler's own passes.

### Performance & Hardware

**Rule 21 — Exploit C++26 Compiler Hints**
Use `[[likely]]` and `[[unlikely]]` on all PGO-driven branches and deoptimization traps. Use C++26 `[[assume(condition)]]` to tell the compiler about invariants, for example:

```cpp
[[assume(node_id < graph.size())]];
```

This eliminates bounds checks in internal compiler data structures where safe.

**Rule 22 — Zero-Cost Error Propagation**
Do not use verbose `if (err)` chains that ruin branch prediction. Use `std::expected<T, Error>` and monadic operations (and_then, transform) or a custom TRY() macro that compiles down to a single branch, keeping the hot path instruction cache pristine.

**Rule 23 — No Hard-Coded Constants in Optimization Logic**
Magic numbers are forbidden. Every threshold, budget, limit, and heuristic constant used in any optimization pass MUST be defined as a named, documented constexpr constant or configuration parameter. CI static analysis fails if numeric literals greater than 2 appear in pass logic without a named constant reference.

**Rule 24 — No Target-Specific Hacks in Generic Passes**
Mid-level and research passes, such as GVN, LICM, SLP, and alias analysis, MUST NOT contain target-specific conditionals such as `#ifdef X86`.

All target knowledge must be abstracted behind the Target interface and queried via cost models or capability flags.

**Rule 25 — No Heuristics Without Empirical Validation**
Every heuristic MUST be backed by benchmark data showing measurable improvement, a mechanism to override or tune it, and documentation explaining why the value was chosen.

**Rule 26 — No Silent Fallbacks Without Telemetry**
When the JIT falls back to a lower tier, when a speculative guard fails, or when regalloc spills excessively, the event MUST be recorded in telemetry and profile data. Silent fallbacks hide performance problems.

**Rule 27 — No Assumption of Stable Hardware**
No pass may assume fixed cache line sizes, SIMD widths, or memory latency ratios. All hardware parameters MUST be queried at runtime for JIT or build-time for AOT via the Target interface.

**Rule 28 — No Optimization Without Measurable Win**
Every optimization pass added to the pipeline MUST demonstrate a measurable geometric mean improvement across the benchmark suite, OR enable a correctness or safety property that cannot be achieved otherwise. Underperforming passes are removed.

### Correctness & JavaScript Semantics

**Rule 29 — No FFI Optimization Without ABI Proof**
FFI and native interop optimizations must prove:

- calling convention correctness
- stack alignment
- register clobbering
- exception propagation
- memory ownership transfer
- JavaScript runtime state preservation
- reference-management transfer where applicable

**Rule 30 — No Vectorization Without Dependence Proof**
Vectorization, whether SLP or loop-based, must prove no aliasing, or use versioned checks, bounds safety, alignment, and correct scalar fallback.

**Rule 31 — No Persistent State Without Versioning**
Profile caches, code caches, and AOT artifacts must be versioned. A change in the IR format, TurboScript bytecode version, JavaScript Semantic Profile version, pass order, target ABI, or runtime configuration invalidates the cache.

**Rule 32 — All Orthogonal Boolean State Must Be Bitmasked**
Any set of independent boolean properties on a hot-path data structure, such as NodeFlags or EffectTags, must be represented as a bitmask with type-safe `Flags<E>` wrappers. Raw integers are forbidden for flag-like state.

**Rule 33 — No Implicit Conversions or Coercions in IR**
The TurboScript IR MUST NOT perform implicit type conversions, integer promotions, or pointer coercions. All conversions must be explicit nodes. The frontend lowering pass inserts these explicitly.

Examples:

- IntToPtr
- PtrToInt
- SExt
- ZExt
- Trunc
- BitCast
- Box
- Unbox
- TaggedToFloat64
- Float64ToTagged
- TaggedToSmi
- SmiToTagged

### Testing & Verification

**Rule 34 — Five Regression Tests Per Bug Fix**
Every bug fix must include at least 5 regression tests:

1. Minimal reproducer.
2. Variant trigger, meaning a different code pattern with the same root cause.
3. Boundary or negative test, ensuring the fix does not over-correct.
4. Integration or contextual test, meaning the bug in realistic surrounding code.
5. Deopt and State Reconstruction test, verifying that if the JIT speculates wrongly, the deopt to Tier 0 produces the exact same state.

Enforcement: CI fails if a PR labeled bugfix has fewer than 5 new test cases.

**Rule 35 — Golden Tests for Every Pass**
Every optimization pass must have at least 10 golden IR tests. Checked-in input and expected IR file pairs must be maintained. Tests must run in both Static Mode and Profile Mode.

**Rule 36 — Differential Testing is Mandatory in CI**
Tier 0 Interpreter versus Tier 1 JIT versus Tier 2 JIT versus Tier 3 AOT comparisons run on every PR. Tier outputs must be observationally equivalent according to the ECMAScript Semantic Oracle (Test262).

Any permitted differences must be explicitly listed, versioned, and tested. Memory layout is tested separately only where it is a supported guarantee. Divergence blocks merge.

**Rule 37 — Deopt Paths Must Be Fuzzed Weekly**
Scheduled CI job. Results triaged within 24 hours. Untriaged deopt fuzz failures block releases.

**Rule 38 — Replay Logs Retained for All CI Failures**
Failed test runs automatically save full compile replay artifacts:

- IR snapshot
- PGO profile
- compiler options
- RNG seed
- target description
- JavaScript Semantic Profile version
- failure context

Debugging starts from replay, not reproduction.

**Rule 39 — Performance Regressions Require Explicit Waiver**
If a benchmark regresses beyond the approved threshold, the PR must include root cause analysis, justification, a tracking issue, and approval. No silent performance degradation.

**Rule 40 — Graph Verifier Runs in Debug Builds After Every Pass**
The verifier checks:

- no dangling NodeIds
- effect chain continuity
- control dominance
- use-def consistency
- FrameState attached to every PGO-driven guard
- dependency metadata completeness
- JavaScript effect legality

**Rule 41 — Test Names Encode the Bug or Feature They Cover**
Bad: `test_pea_3`.
Good: `pea_non_escaping_region_object_with_deopt_materializes_correctly`.
Searchable, self-documenting.

### Speculation & Guarding

**Rule 42 — No Assumption Without Invalidation**
Every PGO-driven assumption must have:

- a registry entry, called the Watchdog
- an invalidation path, called the Trip
- a fallback to static proof or lower-tier execution

**Rule 43 — No Specialization Without Fallback**
Every specialized clone, for example a bounds-check-eliminated array loop, must have:

- a generic fallback
- a deopt path
- a budget limit

**Rule 44 — No Profile Data Without Confidence**
Profile data must include:

- sample count
- stability
- age
- decay
- variance
- deopt correlation

Low-confidence data must not trigger aggressive speculation.

**Rule 45 — No Aggressive Pass Without a Cost Model**
Inlining, cloning, unrolling, SLP vectorization, and PEA materialization must all use a strict cost model based on target hardware latencies and JavaScript object overhead.

---

## Part V: Code Quality & Developer Velocity Laws

**Rule 46 — Local Pre-Commit Checks Must Complete in Under 2 Seconds**
Strictness must not impede velocity. The local pre-commit hook, including formatting, basic linting, and copyright headers, must execute in under 2 seconds. Heavy checks, including full test suites and differential testing, are deferred to asynchronous CI.

**Rule 47 — Actionable Compiler Diagnostics**
The compiler must never output opaque errors such as "Error: something went wrong". All Diagnostic objects must include:

- the exact source location
- a clear human-readable message
- the expected vs actual state
- a suggested fix

This saves developers hours of debugging.

**Rule 48 — `[[nodiscard]]` on All Result Types**
All functions returning `std::expected`, Result, or Error must be marked `[[nodiscard]]`. Ignoring an error is a compilation failure. This forces developers to handle edge cases explicitly without requiring verbose, performance-killing if chains.

**Rule 49 — No `#define` Macros for Logic**
C-style macros for control flow or logic are forbidden. Use constexpr functions, inline functions, or templates. Macros are exempt only for header guards and trivial token pasting. This ensures the debugger can step through the code and the compiler can inline and optimize it properly.

**Rule 50 — Fast Incremental Builds via Modular CMake**
The build system must be structured to allow sub-second incremental builds for single-file changes. Heavy dependencies, such as testing frameworks or optional backend components, must be isolated. Developers must not wait minutes to test a single IR pass modification.

**Rule 51 — Automated Refactoring Tools Over Manual Edits**
When a structural change is required, such as renaming a NodeKind or adding a field to FrameState, a scripted refactoring tool must be provided and run as part of the PR. Manual, error-prone find-and-replace across many files is forbidden.

**Rule 52 — Self-Contained, Reproducible Test Cases**
Every test must be fully self-contained. It must not rely on external network calls, specific local directory structures, or non-deterministic system state. Tests must run identically on a developer machine, a Linux CI runner, or a macOS workstation.

---

## Part VI: Anti-Slop & Robustness Laws

**Rule 53 — No "Small Bug" or "Minor Edge Case" Rationalization**
The phrases "small bug," "minor edge case," "rarely happens," "only affects cold paths," and "good enough for now" are banned. In a systems compiler, small bugs cause silent data corruption or catastrophic performance cliffs. All bugs must be triaged with explicit severity.

**Rule 54 — No Workarounds for Compiler or Runtime Bugs**
Adding code to work around a bug in the compiler, runtime, or standard library is forbidden. The underlying defect MUST be fixed. Temporary mitigations require a tracking issue, a removal deadline of two weeks or less, and explicit approval from the tech lead.

**Rule 55 — No Implicit Knowledge Transfer**
All design decisions, trade-offs, historical context, and operational knowledge MUST be captured in persistent, searchable documentation. This includes code comments, ADRs, wiki pages, and specs. Oral tradition and chat messages are not valid knowledge stores.

**Rule 56 — No Premature Simplification**
Do not simplify, abstract, or generalize code until the full problem space is understood and at least two concrete use cases exist. Premature simplification creates leaky abstractions that fail under real-world JavaScript conditions.

**Rule 57 — No Copy-Paste Code or Structural Duplication**
If two code blocks share structure, extract a helper, template, or data-driven approach. ABI definitions, register lists, and pass boilerplate must use generators, constexpr helpers, or declarative tables.

**Rule 58 — No Silent Fallbacks or Default Returns**
Switch statements on closed enums must be exhaustive. Non-exhaustive switches require `[[assume(false)]]` plus `TURBOSCRIPT_UNREACHABLE()`. Functions must not return arbitrary default values such as `return 0;` or `return nullptr;` when input is invalid.

**Rule 59 — No Lazy Data Structures or Algorithms**
Use the right tool, not the convenient tool. Linear search is forbidden where O(1) lookup is feasible. String comparison is forbidden where symbol IDs suffice.

**Rule 60 — No Untested or Unverified Code Paths**
Every branch, edge case, and error path must have explicit test coverage. "It compiles" is not verification. Only automated, reproducible tests count.

**Rule 61 — No Performance-Agnostic Implementation**
Hot-path code must avoid allocations, exceptions, RTTI, virtual dispatch, and cache-unfriendly patterns. Performance is a feature. Ignoring it in implementation guarantees degradation.

**Rule 62 — No Deletion-by-Avoidance**
Deleting, disabling, commenting out, or stubbing functionality because it is "too hard" or "too complex" is strictly forbidden. When encountering difficult problems: Decompose, Research, Prototype, Document, and Escalate.

**Rule 63 — No Fragile Implementations**
All implementations MUST be resilient to malformed input, concurrent access, resource exhaustion, and platform or hardware variation. Fragile patterns, such as implicit ordering dependencies, global mutable state, and unchecked pointer arithmetic, are forbidden.

**Rule 64 — No Documentation Debt**
Every public API, internal helper, IR node, pass, configuration knob, and non-obvious algorithm MUST have documentation at the point of definition covering:

- Purpose
- Invariants
- Rationale
- Edge Cases
- Cross-References

Stale documentation is treated as a bug with the same severity as stale code.

**Rule 65 — No Easy Fixes — Only Correctness-Preserving Performance Fixes**
When fixing a bug, you must implement the fix that simultaneously preserves performance and correctness. Easy fixes that sacrifice either property are forbidden unless explicitly documented as temporary mitigations with tracking issues and removal deadlines.

**Rule 66 — Slop Detection Checklist**
Every PR reviewer must verify:

- No unnamed numeric constants in logic
- No duplicated code blocks or copy-paste patterns
- No silent fallbacks or unsafe default returns
- No prohibited containers in hot paths, such as `std::unordered_map` or `std::vector` for small collections
- All invariants documented and validated
- No premature abstractions without at least two consumers
- No untracked workarounds or HACK comments
- No target-specific logic outside the backend directory
- All new code paths have test coverage
- Hot-path changes justified with profiling and benchmarks
- Every new guard has a FrameState attachment
- Every speculative node carries metadata including speculation kind, PGO or static source, confidence, guard plan, deopt target, and invalidation dependency
- Every GC reference in generated code across a safepoint has a stack map
- Every deopt point is reachable and has complete deopt metadata
- No raw object pointers held across safepoints without GC map entries
- No getenv() or mutex-locking calls in dispatch loops or hot paths
- No atomic RMW in per-instruction or per-backedge hot paths unless explicitly justified
- Every memory store of a reference executes the correct write barrier if the GC requires one
- W^X is maintained: no page is simultaneously writable and executable
- Code publication is atomic with release semantics; consumers acquire

Failure on any item blocks merge. No exceptions. No "small slop." No "we will fix it later." Slop is banned.

---

## Part VII: JavaScript Semantic Fidelity Laws

**Rule 67 — The ECMAScript Specification Is the Semantic Oracle**
All executable behavior must match the supported ECMAScript reference semantics unless a divergence is explicitly documented, justified, versioned, and approved.

Observable behavior includes at least:

- program output
- exceptions and stack traces
- side effects
- object mutation
- weak-reference behavior where supported
- finalization behavior where supported
- frame introspection
- debugging and monitoring events
- object identity semantics where supported
- module and import semantics
- documented builtin and runtime library behavior

Any unapproved divergence is a correctness bug.

Enforcement includes the Test262 suite, differential Tier 0/1/2/3 runs, and JavaScript-specific semantic tests.

**Rule 68 — Observable Effects Must Not Be Reordered, Duplicated, or Deleted**
No optimization may delete, duplicate, hoist, sink, merge, or reorder JavaScript-visible effects unless the effect system proves semantic equivalence.

JavaScript-visible effects include:

- field and member reads and writes
- global and module and builtin reads and writes
- operator overload dispatch including toPrimitive, valueOf, toString
- dynamic member access hooks
- Proxy trap invocations
- import and module initialization side effects
- allocation side effects where visible
- exceptions raised
- finalizers and weak-reference callbacks where observable
- tracing, profiling, and monitoring events
- I/O effects
- changes to object identity where visible
- changes to container membership where visible
- prototype chain mutations
- Symbol.species constructor invocations
- Symbol.iterator protocol invocations

Enforcement includes effect-chain verifier rules, golden IR tests, and differential semantic tests.

**Rule 69 — Only Provably Pure Expressions May Be Constant-Folded**
Constant folding may only apply to expressions whose result is independent of:

- runtime state
- object identity
- hash randomization
- environment variables
- time
- randomness
- locale
- filesystem state
- import state
- global and module mutation
- builtin and runtime mutation
- object layout and shape
- GC state
- reference-management state where observable
- thread scheduling
- JavaScript dynamic state including prototype chains and property descriptors

No call with possible side effects may be constant-folded.

Enforcement includes verifier rules, pure-node annotations, and negative tests.

**Rule 70 — Dynamic JavaScript Features Are First-Class Correctness Requirements**
The JIT must correctly handle or safely fall back for all dynamic features declared by the JavaScript Semantic Profile.

Examples include:

- eval and Function constructor
- dynamic imports
- dynamic property get, set, and delete
- Proxy traps for all 13 internal methods
- Object.defineProperty and Object.getOwnPropertyDescriptor
- prototype chain mutation
- monkey patching of builtins
- module reloading where supported
- tracing and profiling hooks
- monitoring hooks
- WeakRef and FinalizationRegistry
- Symbol.toPrimitive, Symbol.iterator, Symbol.species, Symbol.hasInstance
- with scopes
- direct eval scope poisoning
- arguments object aliasing in sloppy mode
- caller and arguments restrictions in strict mode

If a dynamic feature cannot be optimized safely, the system must deoptimize or disable JIT for the affected scope. It must never silently produce wrong introspection or semantics.

Enforcement includes dynamic-feature test matrices, deopt tests, and JavaScript API tests.

**Rule 71 — Specialization Requires Versioned, Invalidatable Dependencies**
Every specialization assumption must record a dependency on a versioned entity.

Examples include:

- object shape and hidden class version
- prototype chain version
- module version
- global dictionary version
- builtin and runtime library version
- function and code object version
- property descriptor version
- elements kind version
- signature version
- import state version
- bytecode version
- JavaScript Semantic Profile version
- profile version

If any dependency changes, all dependent compiled code must be invalidated or guarded.

Enforcement includes dependency graph tests, invalidation fuzzing, and inline cache tests.

**Rule 72 — JavaScript Numeric Semantics Must Be Preserved Exactly**
Numeric specializations must preserve JavaScript numeric semantics exactly.

This includes:

- IEEE 754 double-precision floating point for all Number operations
- NaN propagation rules
- negative zero semantics including Object.is(-0, +0) returning false
- BigInt arbitrary precision integer semantics
- TypeError when mixing Number and BigInt in arithmetic
- integer overflow behavior for bitwise operations (ToInt32, ToUint32)
- division by zero producing Infinity or NaN
- modulo with negative operands
- Math.pow edge cases
- exponentiation operator edge cases

Fast-math-style optimizations are forbidden unless explicitly scoped, proven safe, and disabled by default.

Enforcement includes numeric differential tests, overflow tests, NaN tests, negative zero tests, and BigInt mixing tests.

**Rule 73 — Escape Analysis Must Not Eliminate Observable Objects**
Objects may be scalarized or eliminated only if they cannot be observed by:

- identity operators (===)
- WeakRef and WeakMap and WeakSet
- FinalizationRegistry
- GC introspection
- exception stack traces
- frame locals visible to debugger
- profiling and debugging hooks
- user code escaping the function
- dynamic introspection via Object.keys, Object.getOwnPropertyNames, Reflect.ownKeys
- native and FFI calls
- monitoring events
- Proxy traps

If any escape path exists, the object must be materialized.

Enforcement includes escape-analysis verifier rules, materialization tests, and deopt tests.

**Rule 74 — JavaScript Exceptions Are Control-Flow Values, Not Native Exceptions**
JavaScript exceptions must be represented as runtime values and control-flow edges in the Sea of Nodes IR. Native C++ exceptions must not be used to implement JavaScript exception propagation.

The JIT must preserve:

- exception type (TypeError, ReferenceError, RangeError, SyntaxError, etc.)
- exception value
- stack trace
- exception chaining via cause
- exception context
- source positions
- frame association
- finally-block semantics
- scope-exit cleanup semantics

Enforcement includes exception semantic tests and stack-trace golden tests.

**Rule 75 — Frames Must Be Reconstructible on Demand**
If the JIT inlines, merges, elides, or optimizes frames, it must be able to materialize a semantically correct JavaScript frame when required by:

- stack traces via Error.stack
- exceptions
- debuggers
- frame introspection
- reflection APIs
- locals access
- tracing and profiling
- deoptimization
- monitoring hooks
- user introspection

FrameState must be sufficient to reconstruct:

- bytecode offset
- source position
- locals
- closure cells
- free variables
- global and module version
- builtin and runtime version
- exception state
- tracing state
- monitoring state
- suspension state for generators and async functions

Enforcement includes frame materialization tests, deopt tests, and debugger tests.

**Rule 76 — Generators, Async Functions, and Other Suspension Constructs Must Be JIT-Safe**
Suspension points are semantic boundaries.

The JIT must correctly handle:

- yield and resume for generators
- await and resume for async functions
- throw into resumable functions
- return and close semantics
- completion exceptions
- cancellation exceptions
- exception propagation across suspension
- frame reconstruction after suspension
- local state after resume
- finalization of unresumed objects

Suspension points must be valid deopt and safepoint candidates.

Enforcement includes suspension stress tests and deopt-at-yield or deopt-at-await style tests.

**Rule 77 — Debugging, Tracing, Profiling, and Monitoring Must Remain Correct**
The JIT must not break JavaScript tooling.

Supported tooling may include:

- tracers
- profilers
- debuggers
- breakpoints
- line events
- call events
- return events
- exception events
- instruction-level events
- monitoring hooks

When tooling is active, the JIT must either:

- emit correct events with correct semantics
- run unoptimized lower-tier code
- or fall back to Tier 0

Missing events, duplicate events, wrong source positions, or wrong exception events are correctness bugs.

Enforcement includes tracing and profiling differential tests.

**Rule 78 — Object Identity and Reference-Management Semantics Must Be Explicit**
If JavaScript exposes object identity via ===, WeakMap keys, or WeakRef, TurboScript must preserve those semantics.

If using a moving GC:

- object identity semantics must remain correct
- moving objects must not expose unstable addresses to JavaScript code
- pinned objects must be used where address identity is observable
- handles or stable identity mechanisms must be provided

No optimization may assume that object addresses are stable unless the object model explicitly pins the object.

Enforcement includes identity tests, weak-reference tests, and GC tests.

**Rule 79 — No Assumptions About Hashes, Randomness, or Addresses**
The compiler must not persist or bake assumptions about:

- hash values
- hash seeds
- Map and Set iteration order beyond language guarantees
- ASLR addresses
- object addresses
- code addresses
- randomized runtime values
- nondeterministic allocation order

Persistent artifacts must not contain address-dependent assumptions unless explicitly relocated and validated at load time.

Enforcement includes hash-randomization CI, ASLR replay tests, and persistent artifact validation.

**Rule 80 — Static Typing Is Not Runtime Proof Unless Certified**
TypeScript type annotations, JSDoc comments, or any static type information do not by themselves justify unsafe optimization at runtime.

Static proofs must be based on:

- sealed types
- finality guarantees
- module isolation
- absence of dynamic mutation
- verified ABI constraints
- verified import boundaries
- absence of introspection and monkey patching
- mechanically checked proof artifacts

If static proof cannot be maintained, the code must use guards or fall back.

Enforcement includes static-mode verifier rules and negative mutation tests.

---

## Part VIII: Deoptimization and GC Integration Laws

**Rule 81 — Deoptimization Metadata Is a Required Compilation Output**
A compilation is not complete until it has produced:

- deopt points
- FrameState snapshots
- stack maps
- GC reference maps
- live range information
- materialization plan for VirtualObjects
- interpreter re-entry information
- dependency list
- guard metadata
- exception-state reconstruction info

If deopt metadata cannot be generated, the compilation must fail and fall back.

Enforcement includes compiler verifier rules and missing-deopt compile-fail tests.

**Rule 82 — FrameState Must Be Complete and Machine-Checkable**
FrameState must describe enough information to reconstruct the exact lower-tier state.

It must include, as applicable:

- bytecode offset and instruction position
- register-to-interpreter slot mapping
- stack slot types
- object references
- primitive values
- constants
- closure cells
- free variables
- global and module version
- builtin and runtime version
- exception state
- tracing and monitoring state
- suspension state for generators and async functions
- materialized object graph for VirtualObjects
- reference-management state

The verifier must reject incomplete FrameState.

Enforcement includes FrameState verifier rules and forced-deopt tests.

**Rule 83 — Guard Failure Must Produce Exact Lower-Tier State**
When a guard fails, the runtime must resume in a state observationally indistinguishable from the state the lower tier would have reached at that point.

This includes:

- local variables
- stack state
- exception state
- side effects already committed
- reference-management state
- frame visibility
- source position
- monitoring and tracing state
- object materialization state for VirtualObjects

If exact reconstruction is impossible, the speculation must be rejected at compile time.

Enforcement includes differential deopt tests and forced-guard-failure fuzzing.

**Rule 84 — Deopt Loops Must Be Detected and Throttled**
Repeated deoptimization at the same site is a performance and correctness hazard.

The runtime must track:

- deopt count per site
- deopt count per function
- deopt reason
- time window
- tier history

If thresholds are exceeded, the system must:

- disable the failing speculation
- recompile with weaker assumptions
- downgrade tier
- blacklist the function temporarily or permanently
- emit telemetry

Enforcement includes deopt-loop regression tests and telemetry validation.

**Rule 85 — Speculative Side Effects Must Be Reversible or Deferred**
Speculative optimization must not commit irreversible JavaScript-visible side effects before the speculation is proven.

If an effect cannot be proven safe:

- defer it
- guard before it
- materialize fallback state
- or do not perform the optimization

Memory stores that may be observed by JavaScript code, native code, finalizers, weak references, Proxy traps, or debugging tools are not freely rollback-able.

Enforcement includes effect-system audits and speculative-store tests.

**Rule 86 — All GC References in JIT Code Must Be Tracked**
Generated code must not hold raw object pointers in registers, stack slots, or embedded constants across safepoints unless those references are recorded in GC maps.

Rules:

- every reference register across a call or safepoint must be in a stack map
- every reference spill must be visible to GC
- embedded object pointers must use handles or be otherwise tracked
- object references must not be hidden in untracked integer registers unless explicitly tagged and supported by the object representation

Enforcement includes GC map verifier rules and GC stress tests.

**Rule 87 — Read and Write Barriers Must Be Correct**
If the GC requires write barriers, every store of a reference in generated code must execute the correct barrier.

If the GC requires read barriers, load barriers, or forwarding checks, every relevant reference load must execute them.

Missing barriers are blocker bugs.

Enforcement includes barrier verifier rules, GC stress, and moving-GC tests.

**Rule 88 — Generated Code Must Poll Safepoints**
JIT code must include safepoint polls at:

- loop backedges
- function calls
- allocation sites
- long native transitions where specified
- OSR entry and exit points
- tier transition points
- invalidation points where required

Safepoint latency must be bounded.

Enforcement includes safepoint stress tests and GC pause tests.

**Rule 89 — All JIT Frames Must Be Walkable**
Every JIT frame must be walkable by:

- GC
- deoptimizer
- profiler
- debugger
- exception unwinder
- stack overflow checks
- diagnostic tools

Frame metadata must include:

- frame size
- return address location
- saved registers
- stack map
- deopt info
- callee-saved register locations
- JavaScript frame association
- native and managed transition markers

Enforcement includes stack-walking tests and GC, deopt, and profiler integration tests.

**Rule 90 — Stack Overflow and Recursion Limits Must Be Checked**
JIT code must respect JavaScript recursion limits and native stack limits.

Checks must occur:

- before entering JIT frames
- before inlined calls
- before recursive calls
- before OSR entry where applicable
- before native transitions where stack usage changes

Failure must produce the correct JavaScript RangeError, not a crash.

Enforcement includes recursion-limit tests and stack-overflow tests.

**Rule 91 — Allocation Fast Paths Must Handle Failure Safely**
Allocation fast paths may optimize the common case, but slow paths must handle:

- heap exhaustion
- memory allocation failure
- GC pressure
- object finalization hooks
- allocation callbacks where specified
- JavaScript memory error semantics

Generated code must not abort the VM on allocation failure unless the VM is in an unrecoverable state defined by the runtime spec.

Enforcement includes low-memory tests and allocation-failure injection.

**Rule 92 — Runtime Call Transitions Must Preserve ABI and Runtime State**
Calls from JIT code into runtime helpers must preserve:

- calling convention
- stack alignment
- callee-saved registers
- GC state
- exception state
- thread state
- JavaScript runtime state
- floating-point and vector register state as required

Runtime helpers must not assume JIT register contents beyond ABI.

Enforcement includes ABI tests and register-clobber tests.

**Rule 93 — Native Interop and FFI Are Opaque Unless Proven**
Calls into native extensions or foreign functions are opaque barriers unless a formal ABI and effect proof exists.

Assume native calls may:

- mutate arbitrary JavaScript state
- call back into JavaScript code
- allocate
- raise JavaScript exceptions
- change types, modules, and globals
- invalidate specialization assumptions
- acquire and release runtime locks
- trigger GC
- observe object layout and shape
- corrupt assumptions if misused

Optimizations across FFI boundaries require explicit proof and invalidation rules.

Enforcement includes FFI barrier tests and native callback mutation tests.

**Rule 94 — Weak References, Finalizers, and GC Callbacks Must See Valid State**
JIT code must not leave WeakRef, WeakMap, WeakSet, FinalizationRegistry, or GC callbacks in states where they observe:

- partially initialized objects
- invalid forwarding pointers
- untracked references
- missing barriers
- stale object headers and shapes
- inconsistent reference counts
- objects that should have been materialized but were not due to PEA

Enforcement includes weakref and finalizer stress tests.

**Rule 95 — Object Shape and Type Mutation Must Invalidate Specialized Code**
Any change to assumptions used by inline caches or specialization must invalidate dependent code.

This includes:

- prototype chain changes via Object.setPrototypeOf or __proto__
- property addition or deletion
- property descriptor changes via Object.defineProperty
- Object.preventExtensions, Object.seal, Object.freeze
- method redefinition
- getter and setter installation
- Proxy creation wrapping existing objects
- global rebinding
- module namespace mutation
- builtin shadowing and monkey patching
- Array.prototype and Object.prototype pollution

Enforcement includes mutation-after-JIT tests.

**Rule 96 — Tier 0 Is the Universal Correctness Fallback**
Every executable JavaScript function must be runnable in Tier 0. No feature may be "JIT-only" unless explicitly part of a documented static mode.

If Tier 1, Tier 2, or Tier 3 cannot compile, patch, deopt, or execute code correctly, execution must fall back to Tier 0.

Enforcement includes fallback tests and JIT-disable tests.

---

## Part IX: Code Cache, Patching, and Security Laws

**Rule 97 — Executable Memory Must Be W^X**
JIT memory pages must never be simultaneously writable and executable.

Code generation and patching must use one of:

- write-then-execute with protection changes via mprotect
- separate staging and executable pages
- atomic patching of existing executable locations where safe
- platform-approved JIT memory mechanisms

Enforcement includes memory-protection tests and OS-specific audits.

**Rule 98 — Code Publication Must Be Atomic**
Function entry points, OSR entry points, trampolines, and metadata pointers must be published atomically.

No thread may observe:

- partially initialized code
- uninitialized metadata
- missing deopt info
- missing GC maps
- half-patched jump tables

Publication must use release semantics; consumers must use acquire semantics.

Enforcement includes TSAN tests and concurrent-install stress tests.

**Rule 99 — Runtime Patching Must Be Safe Against Concurrent Execution**
Patching running code must be safe.

Requirements:

- patch sites must be aligned and architecturally safe
- instruction sequences must not create invalid intermediate instructions
- instruction cache coherence must be handled where required
- concurrent threads must never execute corrupted instructions
- patching must either use safepoints or architecture-safe atomic sequences

Enforcement includes patch-under-load tests and architecture-specific tests.

**Rule 100 — Old Code May Be Freed Only After Quiescence**
Old compiled code, deopt metadata, and dependency records must not be reclaimed until no thread can be executing or depending on them.

Use:

- epoch-based reclamation
- RCU-like quiescence
- safepoint-based retirement
- reference counting for code objects where appropriate

Code reclamation must be distinct from IR node reclamation.

Enforcement includes concurrent code retirement tests.

**Rule 101 — Every Compiled Artifact Must Record Dependencies**
Every compiled function must record dependencies sufficient for invalidation.

Dependency examples:

- code object identity and version
- function identity and version
- shape and hidden class versions
- prototype chain versions
- global and builtin versions
- module versions
- profile version
- IR version
- compiler version
- target feature set
- ABI version
- JavaScript Semantic Profile version
- runtime configuration

Enforcement includes dependency graph verifier rules.

**Rule 102 — Generated Code Must Be Constrained**
Generated code must only call approved runtime entrypoints and must not directly:

- perform arbitrary syscalls unless mediated by the runtime
- write outside its own frame and runtime-approved memory
- execute arbitrary user-provided machine code
- load arbitrary dynamic libraries unless approved
- bypass sandbox and security policy

Enforcement includes codegen allowlists, backend audits, and security tests.

**Rule 103 — Platform Exploit Mitigations Must Be Enabled Where Available**
JIT must integrate with platform security features where available:

- non-executable stack
- non-executable heap
- CFI (Control Flow Integrity)
- shadow stacks
- PAC and BTI on ARM64
- pointer authentication where supported
- CET where supported
- ASLR-safe code generation
- code signing where required
- sandbox compatibility

If a mitigation is unavailable, the risk must be documented and configurable.

Enforcement includes platform security matrix tests.

**Rule 104 — JIT Spraying Defenses Are Required**
The JIT must not turn attacker-controlled data into executable instruction streams without mitigation.

Mitigations may include:

- constant blinding
- avoiding embedding uncontrolled immediate sequences
- separating executable code from embedded data
- limiting executable constant islands
- code cache entropy and randomization where appropriate
- validating inputs that influence codegen

Enforcement includes security review and exploit PoC tests.

**Rule 105 — Profiles, Bytecode, and AOT Artifacts Are Untrusted**
Profile data, serialized IR, AOT artifacts, caches, and bytecode inputs must be validated before use.

Malformed inputs must not cause:

- undefined behavior
- memory corruption
- arbitrary code execution
- VM crashes
- silent miscompilation

Invalid artifacts must be rejected or ignored with telemetry.

Enforcement includes artifact fuzzing and schema validation.

**Rule 106 — Code Cache Pressure Must Be Managed**
The code cache must have explicit budgets and eviction policies.

The system must monitor:

- total code size
- metadata size
- dependency graph size
- number of live compiled functions
- number of invalidated functions
- patchpoint count
- deopt metadata size

When pressure exceeds budgets, the system must throttle compilation, evict cold code, or fall back.

Enforcement includes code-cache stress tests.

**Rule 107 — AOT Artifacts Must Include a Compatibility Manifest**
AOT artifacts must include:

- JavaScript Semantic Profile version and hash
- IR version and hash
- compiler version
- pass pipeline hash
- target architecture
- target feature set
- ABI hash
- runtime configuration hash
- dependency fingerprints
- security policy version
- creation metadata

Enforcement includes manifest validation tests.

**Rule 108 — AOT Artifacts Must Be Verified Before Loading**
AOT loading must verify:

- manifest compatibility
- integrity checksum and signature where required
- dependency validity
- target feature support
- ABI compatibility
- security policy compatibility
- JavaScript Semantic Profile compatibility

On mismatch, the artifact must be rejected. Silent loading of incompatible artifacts is forbidden.

Enforcement includes stale and corrupt AOT artifact tests.

---

## Part X: Concurrency, Compilation Scheduling, and Tiering Laws

**Rule 109 — Compiler, Runtime, and GC Shared State Must Be Race-Free**
All shared state accessed by mutator threads, compiler threads, GC threads, and background services must be synchronized using documented atomic and locking protocols. TSAN-clean is mandatory for supported concurrent tests.

Enforcement includes TSAN CI and concurrency stress tests.

**Rule 110 — Function Pointer Swaps Must Be Safe and Reversible**
Installing new code must:

- use atomic publication
- preserve old code until safe
- avoid torn calls
- avoid invalidating metadata still needed by running threads
- support rollback where possible

Function installation must be testable independently of compilation.

Enforcement includes concurrent install and uninstall tests.

**Rule 111 — Safepoint Latency Must Be Bounded**
Threads must be able to reach a safepoint within a documented bounded time. Long-running generated loops must contain polls. Native helpers that run for long durations must cooperate with suspension protocols.

Enforcement includes GC pause tests and suspension stress tests.

**Rule 112 — Compilation Latency and Memory Budgets Must Be Defined**
Each tier must have explicit budgets:

- Tier 1 compile latency (strictly under 1 millisecond)
- Tier 2 compile latency (10 to 30 milliseconds)
- Tier 2 memory usage
- Tier 3 AOT compile time where relevant
- IR memory usage
- pass fixpoint iteration limits
- code size limits
- deopt metadata limits

Budget violations must trigger fallback or cancellation, not mutator stalls.

Enforcement includes compile-budget benchmarks.

**Rule 113 — Compilations Must Be Cancellable**
If a function is invalidated while compiling, the compiler must be able to cancel or discard the result without leaking memory or installing stale code.

Enforcement includes invalidation-during-compilation tests.

**Rule 114 — Hotness Counters Must Be Robust**
Profiling counters must be:

- thread-safe or explicitly racy with bounded error
- saturating or overflow-safe
- decaying where appropriate
- resistant to pathological overflow
- correlated with deopt feedback

Undefined behavior from counter overflow is forbidden.

Enforcement includes counter fuzzing and long-run soak tests.

**Rule 115 — Recompilation Must Be Throttled**
Repeated compilation of the same function must be limited by:

- maximum recompiles per function
- exponential backoff
- deopt-history awareness
- code-cache pressure awareness
- budget awareness

No function may cause unbounded compile churn.

Enforcement includes recompile-thrash tests.

**Rule 116 — OSR Entry and Exit Must Be Semantically Exact**
On-stack replacement must preserve exact JavaScript program state at OSR entry and exit.

OSR must handle:

- loop induction variables
- iterator state
- exception state
- closure cells
- locals
- stack values
- suspension state for generators and async functions
- deopt from OSR code back to interpreter

Enforcement includes OSR state reconstruction tests.

**Rule 117 — Invalidation Must Be Ordered and Visible**
Invalidation of dependencies must be visible before new assumptions are relied upon.

The system must avoid:

- executing stale code after invalidation beyond allowed grace
- installing code based on already-invalid dependencies
- racing invalidation with installation

Enforcement includes invalidation race tests.

**Rule 118 — No Global Locks on Hot Runtime Paths**
Global locks are forbidden in hot runtime paths unless explicitly approved and budgeted.

Hot paths include:

- inline-cache updates
- guard checks
- function entry dispatch
- allocation fast paths
- read and write barriers
- safepoint polls
- basic object property access

Enforcement includes lock profiling and scalability tests.

**Rule 119 — Tier Transitions Must Be Observable**
All tier transitions must be recorded:

- Tier 0 to Tier 1
- Tier 1 to Tier 2
- Tier 2 to Tier 3 where applicable
- deopt to lower tier
- code invalidation
- blacklist events
- fallback events
- recompilation events

Telemetry must include reasons and counters.

Enforcement includes telemetry schema tests.

**Rule 120 — Compiler Bugs Must Not Crash User Programs**
A compiler failure should degrade performance, not terminate the application.

Compiler and runtime JIT bugs should result in:

- fallback to Tier 0
- disabled optimization
- diagnostic log
- telemetry
- replay artifact where possible

Process aborts are only acceptable for unrecoverable VM corruption and must be treated as P0 bugs.

Enforcement includes fault-injection tests.

---

## Part XI: IR, Passes, and Backend Laws

**Rule 121 — The IR Must Have an Explicit Effect Model**
The TurboScript Sea of Nodes IR must explicitly represent effects and ordering.

Effect classes should include at least:

- pure computation
- allocation
- JavaScript object mutation
- global and module and builtin mutation
- import effects
- exception effects
- I/O effects
- FFI effects
- GC effects
- monitoring and tracing effects
- deopt and guard effects
- memory reads and writes
- reference-management barrier effects
- Proxy trap effects
- prototype chain observation effects
- Symbol method invocation effects

Passes must not reorder effects without proof.

Enforcement includes effect-chain verifier rules.

**Rule 122 — Speculative Nodes Must Carry Metadata**
Every speculative node must record:

- speculation kind
- PGO or static source
- confidence
- guard plan
- FrameState
- deopt target
- cost
- invalidation dependency
- rollback and deferred-effect plan

No implicit speculation is allowed.

Enforcement includes IR verifier rules.

**Rule 123 — Passes Must Declare Contracts**
Each pass must declare:

- required IR properties
- produced IR properties
- invalidated analyses
- supported tiers
- supported JavaScript capabilities
- budget
- determinism requirements
- target dependencies
- required verifier checks
- telemetry hooks

Passes that cannot satisfy their contract must fail safely.

Enforcement includes pass registry and contract tests.

**Rule 124 — Compilation Must Be Deterministic and Replayable**
Given the same source and bytecode, compiler version, flags, profile snapshot, target description, RNG seed, JavaScript Semantic Profile, and feature configuration, compilation must produce deterministic IR and code selection, except for explicitly documented nondeterminism.

Nondeterminism sources must be logged.

Enforcement includes deterministic replay tests.

**Rule 125 — Passes Must Not Use Hidden Global Mutable State**
Hot-path passes must not depend on hidden global mutable state.

Allowed global state:

- immutable configuration
- interned symbol tables with proper synchronization
- read-only target descriptions
- versioned caches with explicit invalidation

Hidden singletons in pass logic are forbidden.

Enforcement includes static analysis and code review checklist.

**Rule 126 — The Verifier Must Check Deopt and GC Metadata**
The graph verifier must check not only IR consistency but also:

- every guard has FrameState
- every deopt point is reachable
- every GC reference across safepoint has a map
- every effect chain is continuous
- every speculative node has invalidation info
- every materialized object graph is acyclic or properly handled

Enforcement includes debug verifier runs after every pass.

**Rule 127 — Backend Lowering Must Preserve IR Semantics**
Lowering from high and mid IR to machine code must preserve:

- effect order
- exception semantics
- numeric semantics including NaN and negative zero
- overflow behavior
- GC reference liveness
- safepoint placement
- deopt point mapping
- stack layout constraints
- JavaScript observable behavior

Backend optimizations may not silently change IR semantics.

Enforcement includes backend golden tests and differential tests.

**Rule 128 — Register Allocation Must Be GC-Reference Safe**
The register allocator must ensure:

- GC references are not lost across calls and safepoints
- spills of references are tracked
- register maps are generated
- callee-saved and caller-saved conventions are respected
- reference registers do not alias untracked integer registers unless allowed by the object representation

Enforcement includes register-map verifier rules and GC stress tests.

**Rule 129 — Target Features Must Be Gated and Recorded**
Use of CPU features must be:

- runtime-detected for JIT
- build-time validated for AOT
- recorded in code metadata
- protected by feature guards where needed

Generated code must not execute unsupported instructions.

Enforcement includes target-feature mismatch tests.

**Rule 130 — Every IR Node and Trampoline Must Have a Specification**
No IR node, runtime stub, or trampoline may exist without documentation covering:

- semantics
- effects
- tier behavior
- lowering
- verifier constraints
- deopt behavior
- GC behavior
- tests

Enforcement includes documentation lint and IR node registry.

**Rule 131 — Static Proofs Must Be Mechanically Checked**
Tier 3 static optimizations may not rely on human-only proof. Static proofs must be represented as machine-checkable artifacts or verifier constraints.

If proof cannot be checked automatically, the optimization must use guards or be disabled.

Enforcement includes proof-verifier tests.

**Rule 132 — Every Optimization Must Have a Kill Switch**
Every nontrivial optimization should be disableable by:

- compiler flag
- environment variable
- configuration knob
- runtime feature gate
- per-function annotation where appropriate

This enables bisection and incident response.

Enforcement includes feature-flag matrix tests.

---

## Part XII: Testing, Observability, and Governance Laws

**Rule 133 — Differential Oracle Testing Must Run Continuously**
CI must compare behavior across:

- Tier 0
- Tier 1
- Tier 2
- Tier 3 and AOT where available
- ECMAScript reference implementation where applicable

Tests must include:

- normal programs
- exceptions
- async functions, generators, and async generators
- native interop interactions
- dynamic prototype and shape mutation
- tracing and profiling enabled
- GC stress
- low-memory stress
- recursion limits
- large integers, floats, NaN, negative zero, and BigInt
- JavaScript-specific edge cases including Proxy, eval, with, arguments, and private fields

Enforcement includes CI differential matrix tests.

**Rule 134 — Fuzzing Must Cover Bytecode, IR, Profiles, and Artifacts**
Fuzzing must target:

- JavaScript source and bytecode inputs
- IR inputs
- serialized profiles
- AOT artifacts
- code cache metadata
- patching sequences
- deopt metadata
- GC barrier sequences
- FFI boundaries
- type and shape mutation schedules

Untriaged fuzz failures block release.

Enforcement includes scheduled fuzz jobs.

**Rule 135 — Sanitizer Matrix Is Mandatory**
CI must run supported configurations with:

- ASan
- UBSan
- TSan where concurrency exists
- MSan where supported
- debug asserts
- release builds
- interpreter-only mode
- JIT-enabled mode
- AOT mode where applicable

Enforcement includes CI matrix tests.

**Rule 136 — GC and Deopt Stress Tests Must Be First-Class**
Dedicated stress modes must:

- force frequent GC
- force moving GC where applicable
- force allocation failure
- force guard failure
- force deopt at every supported point
- force weakref and finalizer activity where supported
- force code invalidation under load
- force prototype chain mutation under load

Enforcement includes nightly stress and release gating.

**Rule 137 — Code Installation and Patching Must Be Concurrency-Tested**
CI must test:

- installing code while executing old code
- invalidating code while running
- patching under load
- retiring code under load
- OSR entry during invalidation
- deopt during patching

Enforcement includes concurrency stress tests.

**Rule 138 — Performance Gates Must Measure More Than Throughput**
Performance CI must measure:

- startup time
- warmup time
- peak throughput
- tail latency
- compile latency p50 and p99
- deopt rate
- guard overhead
- code size
- memory usage
- GC pause impact
- compile CPU cost
- memory pressure
- tier transition counts

A regression in any critical dimension requires waiver.

Enforcement includes benchmark suite with thresholds.

**Rule 139 — Telemetry Must Be Structured, Stable, and Privacy-Safe**
Telemetry must record:

- compile attempts
- compile failures
- fallback reasons
- guard failures
- deopt reasons
- invalidations
- code cache pressure
- budget violations
- blacklist events
- performance counters

Telemetry must not include source code, user data, or secrets unless explicitly opted in.

Enforcement includes schema validation and privacy review.

**Rule 140 — Replay Artifacts Must Be Sufficient for Debugging**
A failed compilation or deopt event should be replayable from:

- source and bytecode hash
- IR snapshot
- pass pipeline state
- profile snapshot
- compiler flags
- target description
- RNG seed
- runtime config
- JavaScript Semantic Profile version
- dependency versions
- failure location

Debugging should start from replay, not anecdote.

Enforcement includes replay artifact tests.

**Rule 141 — ABI and FFI Must Have Dedicated Tests**
Dedicated tests must cover:

- JavaScript to native calls
- native to JavaScript callbacks
- register clobbering
- stack alignment
- exception propagation through FFI
- global runtime lock or free-threading interactions where relevant
- reference ownership transfer
- struct passing where supported
- varargs conventions where supported
- error return conventions

Enforcement includes ABI test suite.

**Rule 142 — Security Tests Must Be Part of CI**
Security checks should include:

- W^X scans
- executable memory accounting
- JIT spraying PoCs
- malformed artifact loading
- code-cache exhaustion
- patch race attempts
- sandbox escape tests where applicable
- dependency vulnerability scans

Enforcement includes security CI lane.

**Rule 143 — JavaScript Compatibility Must Be Tracked Explicitly**
The project must maintain:

- supported ECMAScript version range
- supported standard-library subset
- known divergences
- unsupported features
- Test262 pass requirements
- allowed failure list with owners and expiry dates

Enforcement includes compatibility dashboard.

**Rule 144 — All Major Optimizations Must Be Feature-Gated**
Every major optimization must be capable of being disabled independently for bisection and emergency response.

Examples:

- inlining
- PEA
- SLP
- LICM
- GVN
- effect reordering
- inline-cache specialization
- type specialization
- unrolling
- OSR
- guard hoisting
- guard elimination
- range analysis
- Tier 2 compilation
- Tier 3 AOT loading

Enforcement includes flag matrix test.

**Rule 145 — Exceptions to Rules Require an Exception Register**
No rule may be silently bypassed. Exceptions must include:

- rule ID
- reason
- owner
- risk assessment
- mitigation
- telemetry
- expiry date
- tech lead approval

Expired exceptions automatically become release blockers.

Enforcement includes exception register.

**Rule 146 — Every Rule Must Have Enforcement Metadata**
Each rule in this document must specify:

- enforcement mechanism
- owner
- severity
- test coverage
- waiver policy

Rules without enforcement should be moved to guidelines or given an enforcement plan.

Enforcement includes rule metadata lint.

**Rule 147 — Maintain a Compliance Matrix**
The repository must maintain a mapping from each rule to:

- CI check
- test suite
- verifier
- review checklist
- documentation
- owner

This matrix must be reviewed each release.

Enforcement includes release checklist.

**Rule 148 — Architectural Decisions Require ADRs**
Any significant compiler and runtime decision must have an Architecture Decision Record. ADRs must cover:

- context
- options considered
- decision
- consequences
- performance impact
- correctness impact
- security impact
- rollback plan

Enforcement includes PR template requirement.

**Rule 149 — Builds Must Be Hermetic and Dependencies Must Be Pinned**
Compiler and runtime builds must be reproducible.

Requirements:

- pinned dependencies
- locked toolchains where practical
- no network access during tests
- reproducible artifact hashes
- supply-chain review for new dependencies

Enforcement includes build reproducibility tests.

**Rule 150 — Stale Documentation Is a Defect**
Documentation must be updated in the same PR as behavior changes.

This includes:

- rules doc
- IR spec
- effect system spec
- ABI spec
- bytecode spec
- JavaScript Semantic Profile docs
- pass documentation
- runtime documentation
- telemetry schema
- compatibility matrix

Stale docs are treated like stale code.

Enforcement includes docs lint and PR checklist.

---

## Part XIII: Definitions

**TurboScript:**
High-performance JavaScript engine. The compiler and runtime system governed by this specification.

**JavaScript Semantic Profile:**
A versioned description of the ECMAScript semantics, capabilities, object model, memory model, exception model, and runtime contracts that TurboScript must preserve.

**ECMAScript Semantic Oracle:**
The ECMAScript specification (Test262 suite) and host environment API contracts used to validate JavaScript semantic fidelity.

**Hot path:**
Compiler pass execution, guard execution, inline-cache fast paths, allocation fast paths, deopt entry trampolines, and dispatch loops. Not deopt materialization, which may allocate under budget.

**Guard:**
A runtime mechanism that validates a speculative assumption. May be a hardware branch, shape and version check, patchpoint, trap, dependency invalidation, or hardware check.

**FrameState:**
A snapshot attached to a speculative node that allows the deoptimizer to reconstruct the exact lower-tier execution state.

**Deopt:**
The process of transferring execution from a higher tier to a lower tier, typically Tier 0, while preserving observable program state.

**Safe point:**
A point in generated code where the thread can safely pause for GC, deopt, or suspension. Must have bounded latency.

**Observable behavior:**
Program output, exceptions, side effects, object mutation, weak-reference behavior, finalization, frame introspection, and monitoring events, as defined by the JavaScript Semantic Profile.

**Effect:**
A JavaScript-visible operation that cannot be freely reordered, deleted, or duplicated without semantic proof.

**Dependency:**
A versioned entity that a specialization relies on. If the dependency changes, the specialization must be invalidated or guarded.

**Code installation:**
The atomic publication of a compiled function's entry point, metadata, and deopt info.

**Quiescence:**
A state where no thread is executing old code or depending on old metadata, allowing safe reclamation.

**Speculation:**
An optimization that assumes a runtime property holds, guarded by a mechanism that triggers deopt on failure.

**Static proof:**
A mechanically-checked artifact demonstrating that a property holds without runtime guards.

**Profile confidence:**
A metric combining sample count, stability, age, decay, variance, and deopt correlation. Low-confidence data must not trigger aggressive speculation.

**Tier transition:**
A change in execution tier, including Tier 0 to Tier 1 to Tier 2 to Tier 3 or deopt to a lower tier. Must be observable and recorded.

**Fallback:**
Graceful degradation to a lower tier or disabled optimization when compilation or speculation fails.

**Kill switch:**
A mechanism to disable a specific optimization for bisection and incident response.

---

## Part XIV: Normative References

- ECMAScript Language Specification (latest edition)
- Test262 Test Suite
- `docs/ir_spec.md` — TurboScript IR node specifications
- `docs/effect_system.md` — effect model and effect-chain rules
- `docs/abi.md` — calling conventions and FFI
- `docs/bytecode_spec.md` — TurboScript Bytecode format
- `docs/javascript_semantic_profile.md` — JavaScript Semantic Profile schema
- `docs/runtime_contracts.md` — runtime helper and hook contracts
- TurboScript telemetry schema
- TurboScript compatibility matrix
- Target platform ABI documents, such as SysV x86-64 and AAPCS64
- Security policy document

---

## Part XV: Rule Severity and Waiver Process

**Severity levels:**

- **P0 (Blocker):** Silent data corruption, security vulnerabilities, crashes on valid input. Blocks release.
- **P1 (Critical):** Wrong results, missing deopt metadata, GC unsafety. Blocks merge to main.
- **P2 (Major):** Performance regressions beyond threshold, missing tests, documentation debt. Requires waiver with expiry.
- **P3 (Minor):** Style, naming, minor optimizations. Tracked but non-blocking.

**Waiver process:**

1. File an exception in the exception register.
2. Include:
   - rule ID
   - reason
   - owner
   - risk assessment
   - mitigation
   - telemetry
   - expiry date
   - tech lead approval
3. Waivers auto-expire. Expired waivers become release blockers.
4. No rule may be silently bypassed. Silent bypass is itself a Rule 145 violation.

---

## Part XVI: Compliance Matrix

The full compliance matrix, mapping Rule to CI check, test suite, and owner, is maintained in `docs/compliance_matrix.md` and reviewed each release.

Example entries:

| Rule | Enforcement | Test Suite | Owner | Notes |
|---|---|---|---|---|
| 3 | IR verifier | guard_tests | compiler team | Every PGO decision needs a guard |
| 24 | static analysis and review | pass_tests | compiler team | No target hacks in generic passes |
| 67 | differential CI | js_compat | runtime team | ECMAScript oracle is authoritative |
| 86 | GC map verifier | gc_stress | GC team | No untracked refs across safepoints |
| 97 | OS memory tests | security_ci | security team | W^X mandatory |

---

*End of TurboScript Compiler Laws & Architecture Specification.*
*Compliance is not optional. It is the foundation of TurboScript.*