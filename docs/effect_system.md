# B-2 IR Effect System (v1)

```text
Normative reference: docs/laws.md (Rules 121, 122, 126; Part XVIII effect
reordering design) and docs/special_passes.md section 2 (the effect algebra
design this implements).
If this document conflicts with docs/laws.md, docs/laws.md wins.
```

Team: IR Team. The machine-checkable twin is `include/b2/ir/Effect.h`
(the 12-entry enum, the 144-entry constexpr table, and the rule functions);
`effect_table_matches_construction_rules_for_all_144_entries` pins
table == rules so they can never drift.

---

## 1. The twelve effect classes (Rule 121)

| Class | Meaning | Java-observable? |
|---|---|---|
| `Pure` | no observable effect | no |
| `ReadLocal` | read of thread-local / stack state | thread-local |
| `ReadShared` | read of shared non-volatile memory | yes (value flow) |
| `WriteLocal` | write of thread-local / stack state | thread-local |
| `WriteShared` | write of shared non-volatile memory | yes |
| `VolatileRead` | volatile / VarHandle acquire read | yes (sync order) |
| `VolatileWrite` | volatile / VarHandle release write | yes (sync order) |
| `MonitorEnter` | lock acquisition | yes (sync order) |
| `MonitorExit` | lock release | yes (sync order) |
| `Allocation` | object allocation (GC-visible, not Java-visible) | no |
| `CallOpaque` | JNI / native / reflection / `<clinit>` — observes anything | yes |
| `ExceptionThrow` | Java exception publication point | yes |

Rule 121's full class list maps onto this algebra as follows: static-field
and array mutation are `WriteShared`/`ReadShared` on their node kinds;
class-initialization, JNI/FFI, and invokedynamic linkage effects are
`CallOpaque`; GC barrier effects ride the store nodes (barrier selection
is a lowering concern, the reorder class is the store's); thread/virtual
thread effects and monitoring/tracing go through calls (`CallOpaque`);
deopt/guard effects are control effects, not memory effects (`Pure` —
guards are transparent re-execution); memory reads/writes are
`ReadShared`/`WriteShared`. Location identity is NOT part of the class: the
table assumes possible aliasing, and RequiresCompensation is where alias
proofs, versioning, or compensation nodes plug in.

Every NodeKind maps to exactly ONE primary EffectKind (the NodeInfo
registry row; docs/ir_spec.md tables). PEA refines `WriteShared` to
`WriteLocal` for proven-non-escaping objects as a pass-level proof; the
registry stays conservative.

## 2. Reorder query semantics

`canReorder(a, b)` answers: may an effect of class `a`, currently ordered
AFTER an adjacent effect of class `b`, be moved BEFORE it? Both effects are
assumed to still execute. The answer is one of:

- `Allowed` — no Java-observable difference for ANY location assignment;
- `Forbidden` — no sound reordering exists at this class level;
- `RequiresCompensation` — sound only with an alias proof, a versioned
  check, or explicit compensation nodes restoring observable state.

## 3. Construction rules (the justification of every entry)

Each entry is produced by the first matching rule; the default is Forbidden.

- **R1 — Pure commutes with everything.**
- **R2 — Opaque calls observe everything except thread-local state** (no
  callee can read or write the caller's thread-locals): `(RL|WL, CO)` and
  `(CO, RL|WL)` are Allowed; every other pair against `CallOpaque` is
  Forbidden.
- **R3 — ExceptionThrow is a publication point.** Handlers, stack traces,
  and deopt reconstruction observe prior `WriteShared`/`VolatileWrite`/
  `Monitor*`/`CallOpaque`/`WriteLocal`/throw effects, so those pairs are
  Forbidden; pure reads (RL/RS/VR) and Allocations commute (the value is
  the same on either side).
- **R4 — Thread-local vs thread-local:** two reads commute; any pair
  involving a write RequiresCompensation (same-location value/order
  sensitivity).
- **R5 — Thread-local vs shared/monitor/allocation is Allowed** by
  classification disjointness: monitors and volatiles synchronize shared
  state only (JLS 17.4); allocations are GC-visible, not Java-visible.
- **R6 — Synchronization-order participants (VR, VW, ME, MX) are Forbidden
  among themselves** (JLS 17.4.4: sync order follows program order between
  them).
- **R7 — A shared write crossing a strong sync participant (VW, ME, MX) is
  Forbidden**: it would break a happens-before edge another thread can
  observe (JLS 17.4.5). A shared write crossing an acquire read (VR), and
  any shared read-vs-read pairing, RequiresCompensation.
- **R8 — Plain shared reads commute; write-involved plain-shared pairs
  RequireCompensation.**
- **R9 — Allocation commutes with every non-opaque, non-throw class**
  (allocation order and identity are not Java-observable;
  `identityHashCode` is unspecified; the GC observes neither Java values
  nor monitor state).

## 4. The 144-entry table

A = Allowed, F = Forbidden, C = RequiresCompensation. Row = the effect
being moved; column = the effect it would move before.

| a \ b | pure | read.local | read.shared | write.local | write.shared | volatile.read | volatile.write | monitor.enter | monitor.exit | alloc | call.opaque | throw |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **pure** | A | A | A | A | A | A | A | A | A | A | A | A |
| **read.local** | A | A | A | C | A | A | A | A | A | A | A | A |
| **read.shared** | A | A | A | A | C | C | C | C | C | A | F | A |
| **write.local** | A | C | A | C | A | A | A | A | A | A | A | F |
| **write.shared** | A | A | C | A | C | C | F | F | F | A | F | F |
| **volatile.read** | A | A | C | A | C | F | F | F | F | A | F | A |
| **volatile.write** | A | A | C | A | F | F | F | F | F | A | F | F |
| **monitor.enter** | A | A | C | A | F | F | F | F | F | A | F | F |
| **monitor.exit** | A | A | C | A | F | F | F | F | F | A | F | F |
| **alloc** | A | A | A | A | A | A | A | A | A | A | F | A |
| **call.opaque** | A | A | F | A | F | F | F | F | F | F | F | F |
| **throw** | A | A | A | F | F | A | F | F | F | A | F | F |

**78 Allowed, 50 Forbidden, 16 RequiresCompensation.** Tightening any cell
is a design change: it requires an RFC message (the diff of the frozen
constexpr table is the review artifact) plus new differential tests
(Law 36) before it lands.

## 5. Compensation as first-class IR nodes

When a reordering is `RequiresCompensation`, the compensation is EMITTED
AS IR NODES, not verified post-hoc (special_passes.md 2.2): compensation
nodes carry their own effect tags and participate in later passes. The
four construction-by-safety rules of special_passes.md 2.3 hold by design:

1. reordering applies to adjacent effect chains only (pairwise, each step
   table-checked);
2. compensation is inserted atomically with the reordering (single
   transformation, no intermediate invalid state);
3. deopt points anchor effect ordering — walking the effect chain between
   the reorder site and the nearest deopt point proves the compensation
   restores the FrameState-visible state;
4. only `Pure`/`ReadLocal`/`WriteLocal` effects may be SPECULATIVELY
   reordered (`isThreadLocalEffect`); shared, volatile, monitor, opaque,
   and throw classes require static proof.

## 6. Enforcement

- **Effect-chain verifier** (this library, Rules 40/121/126): memory-chain
  continuity from `Start` through memory-state producers, cycle-free
  (docs/ir_spec.md 7). Runs in debug builds after every pass (pipeline
  wiring: Passes Team) and at replay load.
- **Effect-chain auditor** (Passes Team, post-transformation): walks all
  effect chains and verifies no Forbidden ordering exists — a linear scan
  over adjacent pairs using `canReorder`.
- **Differential testing** (Law 36): reordered programs must be
  observationally identical to originals; fuzzed valid reorderings stress
  the rule set.

## 7. Test coverage pins

`tests/ir/EffectTests.cpp`: table == rules for all 144 entries; Pure
commutes; TL vs shared/sync/alloc Allowed; TL write pairs compensate;
sync x sync Forbidden; shared write x strong sync Forbidden, x acquire read
compensates; plain shared read pairs commute, write pairs compensate;
CallOpaque blocks everything except thread-locals; throw observes writes
but commutes with reads/allocs; allocation commutes with all non-opaque
non-throw classes; speculative-reorderability is exactly the thread-local
classes; the classification helpers agree with the table.
