# B-2 Special Pass Designs — SMT-Free (v1)

Owner: Passes Team (CM-PEA, speculative effect reordering, adaptive value
representation); NaN boxing representation contracts are cross-team per the
NaN Boxing Rule ownership clause (Interpreter, Baseline No-IR, IR, Passes,
RegAlloc, Codegen, AOT, GC/Runtime as affected)

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
Special Pass Laws (Part XVIII) are law: SWLP Rule, PEA Rule, NaN Boxing Rule.
This document is the engineering design that implements them at production
compilation speed — no SMT solvers, no ML training, no exponential algorithms.
```

These are the three designs, re-engineered for scalability without SMT.

---

# 1. Cross-Method Region-Based Partial Escape Analysis (CM-PEA)

## Core Insight: Escape Is a Graph Property, Not a Solver Problem

Escape analysis doesn't need SMT. It needs **reachability analysis on a finite graph**. The key innovation is making this analysis work *across inlined method boundaries* without global fixed-point iteration.

## Architecture

### 1.1 Escape Lattice (No SMT Needed)

Define a simple, finite lattice per allocation site:

```
⊥ (NoEscape)     — never escapes current region
LocalEscape      — escapes to caller's local scope only
ArgEscape(n)     — escapes only through argument n of caller
FieldEscape(f)   — escapes only through field f of receiver
ReturnEscape     — escapes only through return value
GlobalEscape (⊤) — escapes to heap/GC/native/reflection
```

This is a **height-6 lattice**. Fixed-point converges in ≤6 iterations per node. No solver needed.

### 1.2 Cross-Method Propagation via Inlining Summary Tables

When method `callee` is inlined into `caller`, we don't re-analyze from scratch. We use a **precomputed escape summary**:

```cpp
struct EscapeSummary {
    // For each allocation site in callee:
    // What escape state does it have at each return point?
    SmallVector<EscapeState, 4> return_escape_states;

    // For each parameter: does callee store it to heap/field/global?
    SmallVector<EscapeState, 8> param_escape_effects;

    // Does callee publish any object to shared state?
    bool publishes_to_shared;
};
```

Summaries are computed **once per method** during T2 compilation, cached, and reused across all inline sites. Summary computation is intra-procedural PEA (already implemented). Cross-method propagation is **summary application**, not re-analysis.

### 1.3 Materialization Placement as Graph Cut, Not Optimization Problem

Materialization points are where escape state transitions from non-⊤ to ⊤. This is a **graph cut problem** on the sea-of-nodes effect chain:

1. Mark all nodes where escape state = ⊤ (actual escape points)
2. Walk backwards through effect/data dependencies
3. Place materialization at the **latest safe point** before each ⊤ transition
4. "Safe" = no uncommitted side effects between materialization and escape

This is a **single backward pass** over the IR graph. No cost model optimization needed for placement. Cost model only decides *whether* to scalarize at all (binary decision based on estimated savings vs materialization overhead).

### 1.4 Correctness Without SMT

Correctness is ensured by **construction**, not verification:

- Escape states are propagated conservatively (monotonic join)
- Materialization is placed at provably safe points (effect chain guarantees)
- Identity hash / monitor / weakref observability checks are **syntactic predicates** on the IR, not semantic queries
- If any observability predicate matches, escape state is forced to ⊤ immediately

The proof obligation is: "If escape state < ⊤ at point P, then no Java-observable behavior distinguishes scalarized from allocated object." This is guaranteed by the lattice definition + observability predicates. No solver needed.

### 1.5 Scalability Properties

| Operation | Complexity | Notes |
| :--- | :--- | :--- |
| Intra-method escape analysis | O(N × H) | N = nodes, H = lattice height (6) |
| Summary computation | O(N × H) | Once per method |
| Cross-method propagation | O(S × H) | S = inline sites, not total nodes |
| Materialization placement | O(N) | Single backward pass |
| Observability check | O(1) per site | Syntactic predicate |

Total CM-PEA cost: **linear in IR size**. No exponential blowup. No solver timeout. Works on methods with 100K+ nodes.

### 1.6 Integration with Sea-of-Nodes

After inlining, the sea-of-nodes graph already contains cross-method data/effect flow. CM-PEA operates directly on this graph:

- Allocation nodes carry escape state annotations
- Effect edges propagate escape constraints
- Materialization nodes are inserted as explicit IR nodes with FrameState attachments
- Deopt metadata includes materialization plans

No separate analysis IR. No translation overhead.

---

# 2. Speculative Effect Reordering with Verified Compensation (Without SMT)

## Core Insight: Compensation Correctness Is Structural, Not Semantic

You don't need to prove "reordered program ≡ original program" via SMT. You need to prove "compensation restores observable state at deopt point." This is a **local structural property** checkable by graph traversal.

## Architecture

### 2.1 Effect Algebra (Finite, Compositional)

Define effect categories as a **tagged enum**, not a logic:

```cpp
enum class EffectKind : uint8_t {
    Pure,           // No observable effect
    ReadLocal,      // Read from thread-local / stack
    ReadShared,     // Read from shared non-volatile memory
    WriteLocal,     // Write to thread-local / stack
    WriteShared,    // Write to shared non-volatile memory
    VolatileRead,   // Volatile / VarHandle acquire read
    VolatileWrite,  // Volatile / VarHandle release write
    MonitorEnter,   // Lock acquisition
    MonitorExit,    // Lock release
    Allocation,     // Object allocation (GC-visible)
    CallOpaque,     // JNI / native / reflection call
    ExceptionThrow, // Java exception publication
};
```

Define composition rules as a **lookup table**, not axioms:

```cpp
// Can effect A be reordered before effect B?
// Returns: ALLOWED, FORBIDDEN, or REQUIRES_COMPENSATION(kind)
EffectOrderResult can_reorder(EffectKind a, EffectKind b);
```

This table has 12×12 = 144 entries. Fully enumerable. No solver. Each entry is justified by JMM specification reference in comments.

### 2.2 Compensation as Explicit IR Transformation

When reordering requires compensation, the compensation is **emitted as IR nodes**, not verified post-hoc:

```
Original:     Store(x, v1) → Load(y) → Store(x, v2)
Reordered:    Load(y) → Store(x, v1) → Store(x, v2)
Compensation: [none needed — WriteShared+WriteShared commutes]

Original:     Store(x, v1) → VolatileRead(z) → Store(x, v2)
Reordered:    VolatileRead(z) → Store(x, v1) → Store(x, v2)
Compensation: Insert Store(x, v1) BEFORE VolatileRead if deopt between
              VolatileRead and original Store position
```

Compensation nodes are **first-class IR nodes** with their own effect tags. They participate in subsequent optimization passes. They are not external proofs.

### 2.3 Verification by Construction Rules

Instead of verifying reordering correctness after the fact, enforce correctness **during transformation**:

**Rule 1: Reordering Only Applies to Adjacent Effect Chains**
Non-adjacent effects cannot be reordered directly. Must chain pairwise reorderings. Each step checked against lookup table.

**Rule 2: Compensation Nodes Are Inserted Atomically with Reordering**
Cannot reorder without simultaneously inserting compensation. Single transformation pass. No intermediate invalid state.

**Rule 3: Deopt Points Anchor Effect Ordering**
Effects cannot be reordered across deopt points unless compensation fully restores state at deopt boundary. Checked by walking effect chain between reorder site and nearest deopt point.

**Rule 4: Observable Effects Are Never Speculatively Reordered**
Only effects classified as `WriteLocal`, `ReadLocal`, or `Pure` can be speculatively reordered. `WriteShared`, `VolatileWrite`, `MonitorEnter`, etc. require static proof (no speculation). Speculation only applies to thread-local effects where compensation is trivially correct.

These rules are **syntactic checks on the IR graph**. O(N) per transformation. No solver.

### 2.4 Scalable Validation Pipeline

For additional assurance without SMT:

1. **Differential Execution**: Run reordered code alongside original on same inputs. Compare outputs. Catch bugs empirically. Already required by Law 36.
2. **Effect Chain Auditor**: Post-transformation pass walks all effect chains and verifies no forbidden orderings exist. Linear scan. Catches rule violations.
3. **Fuzzed Reordering**: Randomly apply valid reorderings to test programs. Validate output equivalence. Stress-tests the rule set.
4. **JMM Test Suite**: Run Java Memory Model torture tests (JCStress, JMM-Cookbook). Validates against known tricky cases.

None of these use SMT. All scale linearly. All integrate into existing CI.

### 2.5 Where Aggressive Reordering Actually Helps

Focus on high-value, low-risk cases:

| Reordering | Value | Risk | Compensation |
| :--- | :--- | :--- | :--- |
| Hoist ReadLocal past WriteLocal | Loop invariant loads | None | None |
| Sink WriteLocal past ReadLocal | Delay stores to reduce register pressure | None | None |
| Reorder independent WriteLocal pairs | Enable vectorization / SWLP | Low | Save old value for deopt |
| Hoist Allocation past pure computation | Reduce live range | Medium | Deopt materialization |
| Reorder ReadShared past WriteLocal | Cache locality | Medium | Guard + rollback |

Start with zero-risk reorderings. Expand as differential testing builds confidence. Never speculate on shared/volatile/monitor effects without static proof.

---

# 3. Type-Directed Adaptive Value Representation

## Core Insight: Representation Selection Is a Classification Problem, Not an Optimization Problem

Don't solve "find optimal representation." Solve "classify each value site into representation bucket based on profiled type distribution." Classification is O(1) per site with precomputed thresholds.

## Architecture

### 3.1 Representation Space (Finite Enum)

```cpp
enum class ValueRep : uint8_t {
    UnboxedInt32,       // Raw int in GP register
    UnboxedInt64,       // Raw long in GP register
    UnboxedFloat32,     // Raw float in FP register
    UnboxedFloat64,     // Raw double in FP register
    CompressedOop,      // 32-bit compressed reference
    NaNBoxedRef,        // Reference in NaN payload
    TaggedInt31,        // 31-bit int with tag bit
    BoxedObject,        // Heap-allocated box (fallback)
    Polymorphic,        // Multiple reps at same site (megamorphic)
};
```

9 representations. Not infinite. Not continuous. Discrete classification.

### 3.2 Profile-Guided Classification Rules

For each value site, collect type distribution during profiling:

```cpp
struct TypeProfile {
    uint32_t total_samples;
    uint32_t int32_count;
    uint32_t int64_count;
    uint32_t float32_count;
    uint32_t float64_count;
    uint32_t ref_count;
    uint32_t null_count;
    uint32_t other_count;
};
```

Classification is **threshold-based**, not solver-based:

```cpp
ValueRep select_representation(const TypeProfile& p, const CostModel& cm) {
    if (p.ref_count == p.total_samples)
        return cm.nan_boxing_enabled ? NaNBoxedRef : CompressedOop;

    if (p.int32_count >= p.total_samples * 0.95)
        return UnboxedInt32;

    if (p.float64_count >= p.total_samples * 0.95)
        return UnboxedFloat64;

    if (p.ref_count + p.null_count >= p.total_samples * 0.90 &&
        cm.nan_boxing_safe_at_site(site))
        return NaNBoxedRef;

    if (p.int32_count + p.ref_count >= p.total_samples * 0.85 &&
        cm.tagged_int_profitable(p))
        return TaggedInt31;

    return Polymorphic; // Megamorphic fallback
}
```

Thresholds are **named constants** (Law 23). Tunable via benchmarks. No solver. O(1) per site.

### 3.3 Cost Model as Lookup Table + Arithmetic

Cost model is not a learned function. It's a **parameterized formula**:

```cpp
struct RepCost {
    uint32_t encode_cycles;      // Cycles to convert to this rep
    uint32_t decode_cycles;      // Cycles to convert from this rep
    uint32_t barrier_overhead;   // GC barrier cycles per store
    uint32_t register_pressure;  // Extra registers consumed
    uint32_t cache_footprint;    // Bytes per value in cache
    uint32_t deopt_materialize;  // Cycles to materialize at deopt
};

uint32_t estimate_cost(ValueRep rep, const TypeProfile& p, const Target& t) {
    RepCost c = get_rep_cost(rep, t); // Lookup table, target-specific
    uint32_t dominant_ops = max(p.int32_count, p.ref_count, ...);
    return c.encode_cycles * (p.total_samples - dominant_ops)  // Conversion cost
         + c.barrier_overhead * p.ref_count                    // Barrier cost
         + c.deopt_materialize * estimated_deopt_rate;         // Deopt risk
}
```

All parameters measured offline via microbenchmarks per target. Runtime selection is arithmetic on profile counts. No ML. No solver. No training.

### 3.4 Representation Transitions as Guarded IR Nodes

When representation changes between producer and consumer:

```
Producer (UnboxedInt32) → TransitionGuard → Consumer (NaNBoxedRef)
```

TransitionGuard is a first-class IR node that:
- Checks type tag at runtime if polymorphic
- Converts representation
- Deopts if unexpected type encountered
- Has associated FrameState

Transitions are **optimized away** when producer and consumer agree on representation. Only emitted at representation boundaries.

### 3.5 Correctness Without Solver

Correctness is ensured by:

1. **Representation invariants**: Each rep has documented encoding/decoding contract. Encode(decode(x)) == x for all valid x. Verified by unit test matrix, not solver.
2. **Transition guards**: Every representation change is guarded. Unguarded transitions are verifier errors (Law 126).
3. **NaN boxing safety predicate**: `nan_boxing_safe_at_site()` checks syntactic conditions: no raw double operations at site, no identityHashCode on boxed values, no JNI escape, no reflection access. Syntactic = scalable.
4. **Differential testing**: Same program with different representation policies must produce identical results. Catches encoding bugs.
5. **Round-trip fuzzing**: For each rep, fuzz encode→decode→encode cycles. Verify idempotence. Catches bit-level corruption.

### 3.6 Adaptation Without Recompilation

Representation policy can be updated **without full recompilation**:

- Profile counters continue accumulating during execution
- When distribution shifts beyond threshold, mark site for re-specialization
- Next T2 compilation uses updated profile
- Existing compiled code continues working (conservative fallback)
- No patching of live representation code needed

This avoids the complexity of live representation migration while still adapting to workload changes.

### 3.7 Scalability Properties

| Operation | Complexity | Notes |
| :--- | :--- | :--- |
| Profile collection | O(1) per site | Atomic counter increment |
| Representation selection | O(1) per site | Threshold comparison |
| Cost estimation | O(1) per site | Arithmetic on profile |
| Transition insertion | O(E) | E = representation edges in IR |
| Safety predicate check | O(1) per site | Syntactic scan |
| Round-trip fuzzing | Offline | Not in compilation path |

Total adaptive representation cost: **linear in IR size**. No solver. No learning. No exponential blowup.

---

# Cross-Cutting Design Principles (All Three Features)

## No SMT, No Problem

| Traditional Approach | B-2 Approach | Why It Scales |
| :--- | :--- | :--- |
| SMT-based escape proof | Lattice fixed-point + syntactic observability | Height-bounded lattice; O(N×H) |
| SMT-based reordering verification | Effect lookup table + construction rules | 144-entry table; O(N) validation |
| ML-based representation selection | Threshold classification + cost formula | O(1) per site; no training |
| Post-hoc verification | Correctness by construction | Errors prevented, not detected |
| Global fixed-point | Summary-based modular analysis | Per-method summaries; linear scaling |
| Semantic equivalence checking | Differential execution + fuzzing | Empirical; scales with test budget |

## Shared Infrastructure

All three features share:

1. **Sea-of-nodes effect chains** — foundation for escape tracking, effect ordering, and representation flow
2. **FrameState / deopt metadata** — materialization, compensation, and representation transitions all anchor to deopt points
3. **Profile infrastructure** — type profiles feed representation selection; escape profiles feed PEA; effect profiles feed reordering decisions
4. **Differential testing framework** — validates all three without formal proof
5. **Stencil support** — representation encode/decode, materialization helpers, and compensation stubs can be precompiled stencils

## Implementation Priority

```
Phase 1 (Months 1-6): CM-PEA
├── Highest allocation reduction potential
├── Builds on existing intra-method PEA
├── Summary tables benefit all future interprocedural analysis
└── Directly measurable: allocation rate reduction

Phase 2 (Months 4-10): Adaptive Representation
├── Overlaps with CM-PEA (profile infra)
├── NaN boxing becomes one option among many
├── Enables safe NaN boxing deployment
└── Measurable: polymorphic code throughput

Phase 3 (Months 8-14): Effect Reordering
├── Depends on mature effect chain infrastructure
├── Highest correctness risk; needs Phase 1/2 stability
├── Start with zero-risk reorderings only
└── Measurable: memory-bound benchmark throughput
```

---

# Bottom Line

These three features are **research-grade innovations that compile at production speed**. No SMT. No ML training. No exponential algorithms. Just careful engineering on finite domains with correctness by construction.

They leverage B-2's architectural advantages (sea-of-nodes, stencils, clean IR) in ways retrofitted JVMs cannot easily replicate. They are publishable as systems papers, defensible as engineering decisions, and measurable as performance wins.

The common thread: **replace general-purpose solving with domain-specific structure**. Escape analysis is graph reachability, not SAT. Effect reordering is table lookup, not theorem proving. Representation selection is classification, not optimization. Structure beats generality when the domain is well-understood. And Java's domain *is* well-understood — we just stopped pretending otherwise.
