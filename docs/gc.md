# B-2 GC — Generational Concurrent Region-Based Collector (GCR) (v1)

Owner: GC Team (new; charter at `docs/teams/gc-team.md`); consumed by
Interpreter (T0 barrier helpers, RBC-metadata stack scanning), Baseline No-IR
(T1 barrier stencils, slot-map scanning), IR (effect model includes GC
barriers), Passes (barrier optimization and elimination), Codegen (stack
maps, barrier lowering), RegAlloc (GC-reference-safe allocation, Rule 128),
AOT (GC compatibility manifest). Changes to this contract require an RFC
message and approval from all consuming teams.

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
Part VIII (Deoptimization and GC Integration Laws) is law. In particular:
Rule 78 (object identity and GC semantics explicit), Rule 86 (all GC
references in JIT code tracked), Rule 87 (read and write barriers correct),
Rule 88 (generated code polls safepoints), Rule 89 (all JIT frames
walkable), Rule 90 (stack overflow limits checked), Rule 91 (allocation
fast paths handle failure safely), Rule 94 (weak refs / finalizers /
cleaners / GC callbacks see valid state), Rule 100 (old code freed only
after quiescence), Rule 109 (compiler/runtime/GC shared state race-free),
Rule 111 (safepoint latency bounded), Rule 132 (every optimization has a
kill switch), Rule 136 (GC and deopt stress tests first-class).
Correctness is validated exclusively through Java tests (differential,
stress, fuzz) per the testing contract in docs/cpp26_standards.md.
```

GC is the most critical design decision in B-2 after the tier model. The GC
determines tail latency, throughput ceiling, memory efficiency, and whether
the compiler optimizations actually matter in production.

Given B-2's goals (low latency + high throughput + absurd scale + Java
semantics), this is the GC design.

---

# 0. Why Not Just Use ZGC or Shenandoah?

Because B-2 has unique constraints:

- **T1 stencil frames** are T0-compatible with known slot layouts → simpler stack scanning
- **NaN-boxed references** require tag-aware scanning
- **CM-PEA materialization** creates objects during deopt → allocation must be safe mid-deopt
- **Stencil code cache** needs separate reclamation from heap
- **Absurd scale** means NUMA-awareness is mandatory, not optional
- **Java-only testing** means GC must expose observable hooks for stress/validation

A custom GC tuned to B-2's architecture beats bolting on a generic one.

---

# 1. Core Design Principles

### Principle 1: Generational Because Java Is Generational

Java allocation patterns are overwhelmingly young-generation.
Non-generational concurrent collectors waste effort scanning long-lived
objects. GCR uses generational collection with concurrent old-gen
collection.

### Principle 2: Region-Based Because Scale Demands It

Fixed-size regions enable:

- Parallel work distribution without global locks
- NUMA-local allocation and evacuation
- Incremental compaction without stop-the-world
- Independent reclamation of cold regions
- Memory-mapped heap growth without contiguous reservation

### Principle 3: Concurrent Because Latency Is Non-Negotiable

All phases except initial mark and remark are concurrent. Pause target:
**< 500µs p99, < 100µs median**.

### Principle 4: Load Barriers Because Moving GC Requires Them

Read barriers enable concurrent relocation. Write barriers enable remembered
sets. Both are optimized by the JIT (T2 effect reordering knows barrier
semantics).

### Principle 5: Observable Because Java-Only Testing Requires It

GC exposes telemetry, stress hooks, and validation APIs accessible from
Java. No C++ unit tests for GC behavior.

---

# 2. Heap Layout

```
┌─────────────────────────────────────────────────────┐
│                    B-2 Heap                          │
├──────────────┬──────────────┬───────────────────────┤
│  Young Gen   │   Old Gen    │    Humongous Region   │
│  (Regions)   │  (Regions)   │    (Large Objects)    │
├──────────────┴──────────────┴───────────────────────┤
│              Metadata / Class Space                  │
├─────────────────────────────────────────────────────┤
│              Code Cache (Separate, W^X)              │
└─────────────────────────────────────────────────────┘
```

### Region Size

- **Young regions:** 2 MB (tuned for TLAB refill rate)
- **Old regions:** 8 MB (tuned for evacuation granularity)
- **Humongous threshold:** > 1 MB (half young region size)
- **Region alignment:** Cache-line aligned, page-aligned

### NUMA Topology

Each NUMA node has:

- Local young region pool
- Local old region pool
- Local TLAB pool
- Local refinement thread
- Local allocation preference

Cross-NUMA allocation only when local pool exhausted. Evacuation prefers
same-NUMA destination.

---

# 3. Object Model

### Header Layout (64-bit)

```
┌──────────────────────────────────────────────────┐
│  Mark Word (64 bits)                              │
│  ┌─────────┬──────────┬────────┬───────────────┐ │
│  │ Lock(2) │ Age(4)   │ Hash(31)│ Tag/GC(27)   │ │
│  └─────────┴──────────┴────────┴───────────────┘ │
├──────────────────────────────────────────────────┤
│  Klass Pointer (32-bit compressed or 64-bit)      │
├──────────────────────────────────────────────────┤
│  Array Length (32-bit, arrays only)               │
└──────────────────────────────────────────────────┘
```

### NaN-Boxed Reference Encoding

When NaN boxing is active, references stored in NaN-boxed slots use:

```
Quiet NaN pattern: 0x7FF8_0000_0000_0000 | (compressed_oop << 3) | REF_TAG
```

GC scanner checks tag bits before treating payload as reference. Real
doubles never collide because REF_TAG occupies bits that valid IEEE 754 NaN
payloads don't use in practice. Canonicalization enforced at encode time.

### Compressed Oops

- 32-bit compressed oops for heaps ≤ 32 GB
- Base + shift encoding
- Zero-compressed null
- Decode cost: 1 shift + 1 add (folded by JIT)

---

# 4. Allocation

### TLAB (Thread-Local Allocation Buffer)

```cpp
struct TLAB {
    uint8_t* top;
    uint8_t* end;
    uint8_t* hard_end;       // safety margin
    RegionId owning_region;
    NumaNode numa_node;
    uint64_t bytes_allocated;
};
```

Allocation fast path (inline in JIT):

```asm
mov  rax, [thread + tlab_top]
add  rax, object_size
cmp  rax, [thread + tlab_end]
ja   slow_path
mov  [thread + tlab_top], rax
; initialize header + fields
ret
```

**Fast path: 5 instructions, no branches taken, no locks.**

TLAB refill: lock-free CAS on region bump pointer. If region full, acquire
new region from NUMA-local pool.

### Humongous Allocation

Objects > 1 MB allocated directly in humongous region. No TLAB. Synchronous
allocation with region lock. Rare path.

### CM-PEA Materialization Allocation

Deopt materialization uses a **dedicated allocation context**:

- Pre-reserved TLAB-sized buffer per thread
- Falls back to normal TLAB if buffer exhausted
- All allocations tracked in handle scope
- If any allocation fails, entire materialization rolled back safely
- Never publishes partially-initialized object graph

This ensures deopt allocation is safe even during concurrent GC.

---

# 5. Barriers

### Write Barrier (Remembered Set Maintenance)

```cpp
void write_barrier(oop* field, oop new_value) {
    if (is_old(field) && is_young(new_value)) {
        card_table.mark(card_for(field));
        // Optional: SATB buffer for concurrent mark
    }
}
```

JIT optimization:

- Barrier inlined as 3–5 instructions
- Effect reordering knows barrier is idempotent
- Redundant barrier elimination via effect chain analysis
- Batched card marks for adjacent stores

### Read Barrier (Concurrent Relocation)

```cpp
oop read_barrier(oop ref) {
    if (is_forwarding(ref)) {
        return resolve_forwarding(ref);  // slow path
    }
    return ref;  // fast path: single branch
}
```

JIT optimization:

- Fast path is predicted-not-taken branch
- Barrier hoisted out of loops when safe
- Barrier eliminated for provably non-relocating regions
- NaN-boxed refs: tag check folded into barrier

### Barrier Interaction with Adaptive Representation

Different representations need different barrier forms:

- **CompressedOop:** decode → barrier → use
- **NaNBoxedRef:** tag check → decode → barrier → use
- **Unboxed primitives:** no barrier
- **TaggedInt31:** tag check only, no GC barrier

Adaptive representation pass selects optimal barrier form per site. ICDG
inlines barrier transitions when profitable.

---

# 6. Collection Phases

### Young Generation Collection (Parallel Scavenge)

**Trigger:** Young gen occupancy > threshold OR allocation failure

**Phases:**

1. **Flip:** Swap young gen semi-spaces (atomic pointer swap)
2. **Root scan:** Parallel scan of thread roots, JNI handles, class statics
3. **Copy:** Parallel copy live objects to survivor/old gen
4. **Update:** Update forwarded references in roots and remembered sets

**Pause:** Only root scan flip point. Target: **< 100µs**.

**Parallelism:** One worker per core. Work stealing for load balance.
NUMA-aware copying.

### Old Generation Collection (Concurrent Mark-Evacuate)

**Trigger:** Old gen occupancy > threshold OR promotion pressure

**Phases:**

1. **Initial Mark (STW):** Scan roots, mark direct referents. Pause: **< 200µs**.
2. **Concurrent Mark:** Trace object graph concurrently. Load barriers handle mutations.
3. **Remark (STW):** Rescan modified cards, fix missed references. Pause: **< 300µs**.
4. **Concurrent Evacuate:** Copy live objects from selected regions to free regions. Read barriers redirect reads.
5. **Concurrent Compact:** Update forwarding pointers, reclaim source regions.
6. **Cleanup:** Return empty regions to pool.

**No full stop-the-world old-gen collection.** Ever.

### Humongous Object Collection

Humongous objects collected during old-gen cycles. Dead humongous regions
reclaimed immediately after mark. Live humongous objects evacuated only if
fragmentation requires it.

---

# 7. Stack Scanning

### T0 Interpreter Frames

T0 frames have known layout. Stack map is implicit:

- Register file slots typed by RBC method metadata
- Scan only reference-typed slots
- No per-PC stack map needed (conservative within frame)

### T1 Stencil Frames

T1 frames are T0-compatible. Same scanning strategy as T0. Stencil metadata
provides slot type map per method. **No generated stack maps needed for T1.**

### T2/T3 Optimized Frames

Generated stack maps at safepoints and calls. Maps encode:

- Register bitmask (which regs hold refs)
- Stack slot bitmap (which slots hold refs)
- NaN-boxed slot tags (which slots need tag-aware scan)
- Compressed oop indicators

Stack map table is compact, indexed by PC offset. Generated by backend,
validated by verifier.

### Deopt Frames During Materialization

Materialization uses handle-scoped allocation. Handles are scanned as
roots. No stack map needed for intermediate materialization state. After
materialization completes, normal T0 frame scanning applies.

---

# 8. Concurrency & Scaling

### Thread Safety Model

- **Allocation:** Lock-free TLAB + CAS region bump
- **Root scanning:** Per-thread snapshot, parallel workers
- **Marking:** Concurrent with mutators, load barriers
- **Evacuation:** Concurrent with mutators, read barriers
- **Reclamation:** Epoch-based, quiescence-gated
- **Metadata updates:** Lock-free queues + batch processing

### NUMA Awareness

- Allocation prefers local NUMA node
- Evacuation copies within same NUMA when possible
- Cross-NUMA copies only when local space insufficient
- Worker threads pinned to NUMA nodes
- Region pools partitioned by NUMA

### Absurd Scale (> 256 cores)

- Hierarchical work stealing (NUMA-local → cross-NUMA)
- Per-NUMA refinement threads for remembered sets
- Distributed root scanning with per-core snapshots
- Region-level parallelism (not just object-level)
- Adaptive worker count based on heap pressure

---

# 9. Integration with Compiler Tiers

### T0 Interpreter

- Write barrier called via runtime helper
- No read barrier (T0 doesn't relocate concurrently)
- Stack scanning uses RBC metadata

### T1 Stencil Baseline

- Write barrier inlined as stencil
- Read barrier inlined as stencil (if concurrent old-gen active)
- Stack scanning uses stencil slot map
- No generated stack maps needed

### T2/T3 Optimizing JIT

- Barriers inlined and optimized by effect reordering
- Redundant barrier elimination
- Barrier hoisting out of loops
- Stack maps generated at safepoints
- NaN-boxed ref handling integrated into lowering
- CM-PEA materialization uses safe allocation context

### AOT

- Barriers embedded in precompiled code
- Stack maps serialized in artifact
- Read barrier form depends on GC mode at load time
- Manifest records GC compatibility version

---

# 10. Java-Visible Testing & Observability

Since all tests are Java, GC exposes:

### Stress Hooks

```java
RuntimeHooks.enableGCStress(GCStressMode.MOVING);
RuntimeHooks.enableAllocationFailureInjection(0.001);
RuntimeHooks.forceYoungGC();
RuntimeHooks.forceOldGC();
RuntimeHooks.enableConcurrentRelocationStress();
```

### Telemetry API

```java
GCTelemetry.getYoungGCPauseStats();
GCTelemetry.getOldGCPauseStats();
GCTelemetry.getAllocationRate();
GCTelemetry.getPromotionRate();
GCTelemetry.getHeapOccupancy();
GCTelemetry.getNumaDistribution();
GCTelemetry.getBarrierOverhead();
```

### Validation Tests

```java
@StressTest(duration = MINUTES_5)
void concurrentRelocationPreservesReferences() { ... }

@DifferentialTest(tiers = {T0, T1, T2})
void gcDoesNotCorruptNaNBoxedReferences() { ... }

@Test
void cmPeaMaterializationSafeDuringGC() { ... }

@Test
void identityHashCodeStableAcrossRelocation() { ... }

@Test
void weakRefsProcessedCorrectly() { ... }

@Test
void finalizersRunInCorrectOrder() { ... }
```

### No C++ GC Unit Tests

GC correctness validated exclusively through Java stress tests, differential
tests, and fuzzing. Internal C++ assertions catch implementation bugs during
development but are not the validation mechanism.

---

# 11. Performance Targets

| Metric | Target | Notes |
|--------|--------|-------|
| Young GC pause (p99) | < 100 µs | Root scan only |
| Old GC pause (p99) | < 500 µs | Initial mark + remark |
| Old GC pause (median) | < 100 µs | Most cycles skip remark |
| Allocation fast path | < 10 ns | 5 instructions, no branch |
| Write barrier overhead | < 3% throughput | Inlined, optimized |
| Read barrier overhead | < 5% throughput | Predicted-not-taken |
| Heap overhead | < 15% vs raw objects | Headers + metadata |
| NUMA locality | > 90% local allocation | For NUMA-aware workloads |
| Max heap size | 4 TB+ | Region-based, no contiguous reservation |
| Core scalability | Linear to 256+ cores | Hierarchical work stealing |

---

# 12. Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Read barrier overhead kills throughput | Medium | High | JIT optimization, barrier elimination, kill switch for non-moving mode |
| NaN-boxed ref scanning bugs | Medium | Critical | Exhaustive round-trip fuzzing, differential tests |
| CM-PEA materialization races with GC | Medium | Critical | Handle-scoped allocation, dedicated buffer |
| NUMA imbalance under skewed allocation | Medium | Medium | Adaptive region migration, cross-NUMA stealing |
| Remembered set overflow | Low | High | Overflow to full scan, bounded buffer size |
| Identity hash instability after relocation | Low | Critical | Hash stored in header before first relocation |
| Finalizer/Cleaner ordering bugs | Medium | High | Spec-compliant queue processing, Java validation tests |
| Code cache / heap interaction | Low | Medium | Separate reclamation epochs, no shared GC state |

---

# 13. Implementation Phases

### Phase 1: Foundation (Months 1–4)

- Region allocator
- TLAB allocation
- Basic object model
- Young gen parallel scavenge
- Write barrier
- Stack scanning for T0/T1
- Java stress test harness

**Deliverable:** Working young-gen GC, T1 stencils functional, basic Java
tests pass.

### Phase 2: Concurrent Old Gen (Months 3–8)

- Concurrent mark
- Read barrier
- Concurrent evacuate
- Remark phase
- Old gen stack maps (T2)
- Humongous object support

**Deliverable:** Full concurrent GC, sub-ms pauses, T2 integration.

### Phase 3: Scale & Polish (Months 6–12)

- NUMA awareness
- Hierarchical work stealing
- Barrier optimization passes
- NaN-boxed ref scanning
- CM-PEA materialization safety
- Telemetry & observability
- Performance tuning

**Deliverable:** Production-grade GC meeting all targets.

### Phase 4: Hardening (Months 10–16)

- GC stress torture tests
- Fuzzing (barrier sequences, relocation timing)
- Weak ref / finalizer / Cleaner validation
- Identity hash stability tests
- AOT GC compatibility
- Documentation & compliance matrix

**Deliverable:** Release-ready GC with Java-validated correctness.

---

# 14. Team Ownership

```yaml
gc_team:
  primary_owner: gc
  consumers:
    - ir           # effect model includes GC barriers
    - passes       # barrier optimization, elimination
    - baseline_noir # T1 barrier stencils
    - codegen      # stack maps, barrier lowering
    - interpreter  # T0 barrier helpers
    - aot          # GC compatibility manifest
  changes_require:
    - RFC message
    - approval from all consuming teams
    - Java stress test additions
```

GC is not isolated. Every team touches it. The contract must be shared and
versioned.

---

# Bottom Line

GCR gives B-2:

- **Sub-ms pauses** via concurrent mark-evacuate
- **High throughput** via generational collection + JIT-optimized barriers
- **Absurd scale** via region-based NUMA-aware design
- **Compiler synergy** via barrier-aware optimization and T0-compatible frame scanning
- **Java-validated correctness** via stress hooks and differential testing

This GC is not a generic collector bolted onto a JVM. It is a collector
designed *for* B-2's specific architecture: stencils, sea-of-nodes, CM-PEA
materialization, NaN boxing, adaptive representation, and Java-only
validation.

