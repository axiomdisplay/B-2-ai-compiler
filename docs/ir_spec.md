# B-2 Sea-of-Nodes IR Specification (v2)

```text
Normative reference: docs/laws.md
(especially Rules 5, 7-9, 14-20, 31-33, 40-42, 121-126, 130,
Amendments A and B, and Part XVIII.)
If this document conflicts with docs/laws.md, docs/laws.md wins.
```

Team: IR Team (`docs/teams/ir-team.md`). This is the specification of the
T2/T3 core intermediate representation (Amendment B.1: T2 is the first IR
tier; T3 reuses the same pipeline per B.5; T1 gets nothing from this
document per Amendment A).

The machine-checkable twin of this document is the `NodeInfo` registry in
`compiler/ir/core/NodeInfo.cpp`: one row per `NodeKind` carrying the input
signature, variadic role, FrameState requirement, and the primary effect
class. The test `ir_node_registry_covers_every_kind_with_unique_names` and
`ir_node_registry_rows_match_the_spec_document_counts` pin registry ↔
document agreement (Rule 130's documentation lint).

---

## 1. Storage model

| Aspect | Design | Law |
|---|---|---|
| Allocation | `std::pmr::monotonic_buffer_resource` arena; bulk-freed with the `Graph` | Rule 7 |
| Edges | `NodeId` (`uint32_t`) indices only; never pointers | Rule 15 |
| Node records | One flat `pmr::vector<Node>`; id = creation order; stable for the graph's lifetime (tombstones stay) | Rules 8, 124 |
| Inputs | Flat edge pool; node inputs are the slice `[edgeOffset, edgeOffset + numInputs)`; `appendInput` relocates the slice to the pool tail (arena discipline: old slice abandoned) | Rule 15 |
| Use-def chains | Per-node `SmallVector<Use, 3>`; `Use = {user, slot}`; maintained by `make`/`setInput`/`appendInput`/`replaceNode` | Rule 19 |
| Identifiers | `SymbolId`/`TypeId`/`FieldId`/`MethodId`/`DeoptId` are opaque `uint32_t`s from frontend tables; the IR stores no strings | Rule 16 |
| Replacement | `replaceNode(old, with)` rewires all users, tags old `Dead|Replaced` with the current epoch, appends to the replacement log | Rule 14 |
| Determinism | Same construction sequence ⇒ same ids, same serialized bytes, same printed dump | Rule 124 |

Invariant (pinned by tests): an edge `(user, slot)` exists in
`usesOf(def)` iff `user.input(slot) == def`, under every mutation.

`Graph` is non-copyable and non-movable: its pmr containers point at the
member arena. Compile jobs own graphs by reference.

`make` is the trusted fast path (builder discipline, exactly like
`RbcBuilder` vs the RBC verifier): it stores inputs verbatim without
checking them. The verifier (section 7) is the gate.

## 2. Type lattice (`IRType`)

Scalars mirror `rbc::RType` one-to-one (the RBC→IR builder is a total
function on RBC types):

- `Bottom` (unreachable / no value), `Int` (boolean/byte/char/short folded),
  `Long`, `Float`, `Double`, `Null`, `Ref`, `Tagged`.
- `Null <: Ref` is the only subtype relation; incompatible merges produce
  `Bottom` (the merge point is unreachable — same convention as RBC).

Extensions for Part XVIII:

- Lane types `Int8`, `Int16` exist ONLY as vector lane payloads (byte/char/
  short scalars fold into `Int` per the RBC discipline). The verifier
  rejects them as scalar operand types.
- Vector result types `VectorI8/I16/I32/I64/F32/F64`. Lane count is a
  per-node payload, not part of the type; masks are `VectorI8` lanes of 0/1.
- `Tagged` is the representation-polymorphic type produced by NaN-boxing
  tag/untag nodes (Part XVIII NaN Boxing Rule: representation only, never
  Java-visible semantics).

Rule 33 enforcement: every type change is an explicit conversion node;
the verifier rejects any operand type mismatch as an error (never a
coercion).

## 3. Input roles and signatures

Every node kind fixes a signature (registry row):

- a fixed input prefix with per-slot **roles**:
  - `Ctrl` — control predecessor (producer must produce control);
  - `Mem` — memory-state predecessor (producer must produce memory state);
  - `Data` — data value (type-checked per kind);
  - `FrameState` — deopt state (producer must be a `FrameState` node);
  - `Parent` — projection source (the `If`/`Switch`/`Call` node itself;
    kind-checked by the projection-parent rule);
- an optional variadic tail (role applies to all appended inputs);
- an optional mandatory trailing `FrameState` input (checked after the
  variadic tail).

Control producers (v2): `Start`, `Region`, `IfTrue`, `IfFalse`,
`SwitchCase`, `SwitchDefault`, `LoopBegin`, `LoopEnd`, `LoopExit`, `Call*`,
`CallExcept`, `LoadException`, and EVERY fixed node — a node whose registry
row consumes a `Ctrl` input (`If`, `Switch`, `Guard`, `RepTransitionGuard`,
loads, stores, barriers, monitors, allocations, `ClassInit`, `CheckCast`,
`Materialize`, `VectorLoad/Store`) produces control for the fixed nodes
after it. This is Graal's fixed-node discipline: control chains through the
schedule, so a branch or terminal following a same-block store cannot be
scheduled before it. Terminals (`Return`, `Unwind`, `Deopt`, `End`) end
chains: they produce neither control nor values and must have no users.

Memory-state producers (legal `Mem` inputs): `Start`, `StoreField`,
`StoreStatic`, `StoreElem`, `MemBar`, `MonitorEnter`, `MonitorExit`, `New`,
`NewArray`, `NewRefArray`, `NewMultiArray`, `ClassInit`, `Call*`,
`Materialize`, `VectorStore`, and `Phi` — the v2 memory-state MERGE
(Graal's MemoryPhi shape). A `Phi` used as a `Mem` input must have every
value input be a memory-state producer (transitively; the verifier rejects
a pure value smuggled into the memory chain). Loads read a memory state but
do not produce one (the sea-of-nodes memory model: writers chain, readers
point at the current state). `Start` is the origin of BOTH control and
memory.

v1 graphs remain legal under v2: the added producers only widen the set of
legal `Ctrl`/`Mem` inputs.

## 4. Node kinds (137)

Payload conventions below are normative; `payload2` marked "—" is unused
(and must be 0 for serialization stability).

### 4.1 Control (15)

| Kind | Inputs | payload / payload2 | Result | Effect | Notes |
|---|---|---|---|---|---|
| `Start` | — | — / — | — | Pure | Node 0 of every graph; control + initial memory origin |
| `End` | variadic Ctrl | — / — | — | Pure | Terminal sink for paths that neither return nor throw |
| `Region` | variadic Ctrl (≥2) | — / — | — | Pure | Control merge |
| `If` | [Ctrl, Data(Int)] | — / — | — | Pure | Condition truthiness (0 = false), RBC `if*` shape |
| `IfTrue` / `IfFalse` | [Parent] | — / — | Ctrl | Pure | Projections of `If` |
| `Switch` | [Ctrl, Data(Int)] | sw=SwitchTableId / — | — | Pure | Multi-way branch |
| `SwitchCase` / `SwitchDefault` | [Parent] | case=ordinal / — | Ctrl | Pure | Projections of `Switch` |
| `LoopBegin` | variadic Ctrl (≥2) | — / — | Ctrl | Pure | Entry + backedge(s) merge |
| `LoopEnd` | [Ctrl] | — / — | Ctrl | Pure | Backedge anchor |
| `LoopExit` | [Ctrl, FrameState] | — / — | Ctrl | Pure | Loop exit; safepoint/deopt point (Rule 126) |
| `Return` | [Ctrl, Data?] (≤1 value) | — / — | — | Pure | Method return |
| `Unwind` | [Ctrl, Data(Ref)] | — / — | — | ExceptionThrow | Exceptional return to caller |
| `Deopt` | [Ctrl, FrameState] | deopt=DeoptId / — | — | Pure | Unconditional deopt |

### 4.2 Memory (15)

| Kind | Inputs | payload / payload2 | Result | Effect | Notes |
|---|---|---|---|---|---|
| `LoadField` | [Ctrl, Mem, Data(Ref)] | f=FieldId / ret=IRType | IRType | ReadShared | |
| `StoreField` | [Ctrl, Mem, Data(Ref), Data] | f=FieldId / — | — | WriteShared | |
| `LoadStatic` | [Ctrl, Mem] | f=FieldId / ret=IRType | IRType | ReadShared | |
| `StoreStatic` | [Ctrl, Mem, Data] | f=FieldId / — | — | WriteShared | |
| `LoadElem` | [Ctrl, Mem, Data(Ref), Data(Int)] | t=elemIRType / — | elemIRType | ReadShared | |
| `StoreElem` | [Ctrl, Mem, Data(Ref), Data(Int), Data(elem)] | t=elemIRType / — | — | WriteShared | Value type-checked against elem type |
| `ArrayLength` | [Data(Ref)] | — / — | Int | ReadShared | Null-check is a separate `Guard` |
| `MemBar` | [Ctrl, Mem] | bar=MemBarKind / — | — | VolatileWrite | JMM fence |
| `MonitorEnter` | [Ctrl, Mem, Data(Ref)] | — / — | — | MonitorEnter | |
| `MonitorExit` | [Ctrl, Mem, Data(Ref)] | — / — | — | MonitorExit | |
| `New` | [Ctrl] | t=TypeId / — | Ref | Allocation | |
| `NewArray` | [Ctrl, Data(Int)] | t=elemIRType / — | Ref | Allocation | Primitive arrays |
| `NewRefArray` | [Ctrl, Data(Int)] | t=TypeId / — | Ref | Allocation | Reference arrays |
| `NewMultiArray` | [Ctrl, Data(Int)...] | t=TypeId / — | Ref | Allocation | One input per dimension |
| `ClassInit` | [Ctrl, Mem] | t=TypeId / — | — | CallOpaque | Runs `<clinit>`: opaque by design |

Effect refinement contract: the registry's `WriteShared`/`ReadShared` is the
CONSERVATIVE class. CM-PEA refines stores to proven-non-escaping objects to
`WriteLocal` as a pass-level proof (docs/special_passes.md §1); that
refinement changes analysis results, never the registry.

### 4.3 Calls (6)

| Kind | Inputs | payload / payload2 | Result | Effect |
|---|---|---|---|---|
| `CallStatic` / `CallVirtual` / `CallInterface` / `CallDynamic` | [Ctrl, Mem, Data args..., FrameState] | m=MethodId / ret=IRType | IRType (Bottom = void) | CallOpaque |
| `CallExcept` | [Parent] | — / — | Ref | Pure |
| `LoadException` | [Ctrl] | — / — | Ref | Pure |

Calls are mandatory deopt points (Rule 126): the trailing `FrameState`
input is checked. `CallExcept` is the exceptional projection; the handler
block's entry is `LoadException` on the caught-exception control path.

### 4.4 Object / type ops (2)

| Kind | Inputs | payload / payload2 | Result | Effect |
|---|---|---|---|---|
| `CheckCast` | [Ctrl, Data(Ref)] | t=TypeId / — | Ref | ExceptionThrow (ClassCastException is a Java exception, not a deopt) |
| `InstanceOf` | [Data(Ref)] | t=TypeId / — | Int | ReadShared |

### 4.5 Guards (1)

| Kind | Inputs | payload / payload2 | Result | Effect |
|---|---|---|---|---|
| `Guard` | [Ctrl, Data(Int), FrameState] | kind=GuardKind / deopt=DeoptId | — | Pure |

Deopts when the condition evaluates 0. Every guard carries a `FrameState`
(Rule 5); speculative guards additionally carry complete `SpecMeta`
(Rule 122, section 5.3). Guards GATE control (v2): a guard consumes a
control predecessor and produces control for everything that must execute
only if the guard holds — the graph builder places `NullCheck`/`BoundsCheck`/
`ZeroCheck` guards in front of the memory ops and calls they protect. GuardKind taxonomy (Rule 32): `None`, `NullCheck`,
`BoundsCheck`, `ZeroCheck`, `ClassCast`, `ArrayStore`, `TypeProfile`,
`UnstableIf`, `UnstableSwitch`, `LoopLimit`, `Deoptimize`.

### 4.6 Constants / parameters (7)

| Kind | Inputs | payload / payload2 | Result | Effect |
|---|---|---|---|---|
| `ConstantI` | — | constValue / — | Int | Pure |
| `ConstantL` | — | constValue / — | Long | Pure |
| `ConstantF` | — | constValue = exact bits / — | Float | Pure |
| `ConstantD` | — | constValue = exact bits / — | Double | Pure |
| `ConstantNull` | — | — / — | Null | Pure |
| `ConstantSym` | — | s=SymbolId / — | Ref | Pure |
| `Parameter` | — | #=index / IRType | IRType | Pure |
| `Undef` | — | — / — | Bottom | Pure |

`Undef` (v2) is the uninitialized-slot placeholder: T0's Bottom-tagged
`Value`. It may feed ONLY a `FrameState` (a deopt slot that materializes
Bottom) or a `Phi` value input (the "Bottom on this path" marker); every
typed-operand use is a verifier error — verified RBC never reads an
uninitialized slot.

Float/double constants store exact bit patterns (NaN payloads and −0.0
survive round-trips; pinned by test).

### 4.7 Arithmetic (36), comparisons (6)

Typed kinds, no generic arithmetic (self-describing graphs; kind-compare
pattern matching):

- Int (12): `AddI SubI MulI DivI RemI NegI ShlI ShrI UShrI AndI OrI XorI` —
  both operands `Int`, result `Int`.
- Long (12): same set with `L` suffix — operands `Long`, result `Long`,
  EXCEPT `ShlL/ShrL/UShrL` whose count operand stays `Int` (JVM rule).
- Float (6): `AddF SubF MulF DivF RemF NegF` — `Float`.
- Double (6): same with `D` — `Double`.
- Logical (1, v2): `Not` [Int] → `Int` — boolean complement
  (0 → 1, nonzero → 0).
- Comparisons (6): `CmpI`, `CmpL`, `CmpFl`, `CmpFg`, `CmpDl`, `CmpDg` —
  result `Int` = `(a>b) − (a<b)`; the `l/g` variants fix NaN comparison
  direction (JVM `fcmpl`/`fcmpg`).
- Boolean tests (8, v2 — the graph-builder branch/guard vocabulary):
  `EqI NeI LtI LeI GtI GeI` [Int, Int] → `Int` (0/1, the `if_icmp*`
  family), `RefEq` [Ref, Ref] → `Int` (identity, `if_acmp*`; Null legal on
  either side), `IsNull` [Ref] → `Int` (1 if null; Null legal). `If`
  conditions and `Guard` conditions are composed from these (plus `Not`,
  `AndI`/`OrI`) so that IfTrue is always the TAKEN edge.

All Pure. `Div`/`Rem` trap semantics are carried by explicit guards
(`ZeroCheck`), not hidden effects.

### 4.8 Conversions (23) — Rule 33's explicit-only discipline

`I2L I2F I2D L2I L2F L2D F2I F2L F2D D2I D2L D2F I2B I2C I2S` (the JVM set),
plus the machine-adjacent set: `SignExtend` (Int→Long), `ZeroExtend`
(Int→Long), `Truncate` (Long→Int), `BitCast` (T→T, same width),
`CompressedRefEncode` (Ref→Int), `CompressedRefDecode` (Int→Ref),
`BoxPrimitive` (primitive→Ref), `UnboxPrimitive` (Ref→primitive,
payload2 = target IRType).

Unary `[Data]` inputs, all Pure.

### 4.9 Merge (1)

| Kind | Inputs | Notes |
|---|---|---|
| `Phi` | [Ctrl(Region/LoopBegin), Data per pred] | Arity must be 1 + region predecessors; result type = join of value inputs. A value input may be the Phi ITSELF (v2: the loop-invariant marker — the value flows around the backedge unchanged; the structural self-reference check exempts Phis) or `Undef` (Bottom on that path) |

### 4.10 Vector / SIMD (9) — SWLP representation (Part XVIII)

| Kind | Inputs | payload / payload2 | Result |
|---|---|---|---|
| `VectorBroadcast` | [Data(carrier)] | t=lane / packed(lane,lanes) | Vector |
| `VectorPack` | [Data(carrier) × lanes] | t=lane / — (lanes = numInputs) | Vector |
| `VectorExtract` | [Vector, Data(Int)] | — / — | lane type |
| `VectorInsert` | [Vector, Data(carrier), Data(Int)] | — / — | Vector |
| `VectorOp` | [Vector, Vector] | op=VectorOp / packed | Vector |
| `VectorMaskOp` | [VectorI8 mask, Vector, Vector] | op=VectorOp / packed | Vector |
| `VectorLoad` | [Ctrl, Mem, Data(Ref), Data(Int)] | packed in payload2 | Vector |
| `VectorStore` | [Ctrl, Mem, Data(Ref), Data(Int), Vector] | packed in payload2 | — |
| `VectorReduce` | [Vector] | op=VectorOp / packed | lane scalar type |

`packed = (lanes << 8) | laneType`. Lane counts must be powers of two in
`[kMinVectorLanes=2, kMaxVectorLanes=64]`. Lane carrier types: `Int8`/`Int16`
lanes carry their values in `Int` scalars (byte/char/short fold into `Int`);
lane formation is a packing decision, not an implicit coercion. Element-wise
ops (`Add..Max`) are distinct from reduction ops (`Sum Min Max And Or Xor`);
FP lanes reject bitwise ops (Java numeric semantics, no fast-math — SWLP
Rule). Mask lane counts must match operand lane counts. Memory vector ops
are gather/scatter-capable loads/stores at consecutive indices with explicit
bounds guards supplied by the pass.

### 4.11 PEA virtual objects / materialization (2) — Part XVIII

| Kind | Inputs | payload / payload2 | Result | Effect |
|---|---|---|---|---|
| `VirtualObjectState` | [field values...] (Array: length first) | t=TypeId-or-elemIRType / 0=instance, 1=array | Ref | Pure |
| `Materialize` | [Ctrl, Mem, Data(VirtualObjectState)] | — / — | Ref | Allocation |

The `VirtualObjectState` NODE IS the virtual object (`VirtualObjectId` ==
its `NodeId`): fields are input EDGES so node replacement automatically
updates virtual state — side-table ids would silently miss rewrites (the
design decision that keeps PEA sound under GVN-style replacement).

Verifier rules (Rule 126):

- materialization graphs must be acyclic (DFS over vobj field edges);
- FrameState deopt lists are closed: a vobj referenced by a field of a
  listed vobj must be listed too (deopt materializes in dependency order);
- a `Materialize` must not store still-virtual fields (the pass replaces
  them with materialized references first).

### 4.12 Tagged values (3) — NaN boxing representation (Part XVIII)

| Kind | Inputs | payload / payload2 | Result | Effect |
|---|---|---|---|---|
| `TagValue` | [Data] | from=ValueRep / to=ValueRep | Tagged | Pure |
| `UntagValue` | [Data(Tagged)] | from / to | rep's scalar type | Pure |
| `RepTransitionGuard` | [Ctrl, Data(Tagged), FrameState] | from / to | — | Pure; always speculative |

The 9-rep space is docs/special_passes.md §3.1 verbatim (`UnboxedInt32`,
`UnboxedInt64`, `UnboxedFloat32`, `UnboxedFloat64`, `CompressedOop`,
`NaNBoxedRef`, `TaggedInt31`, `BoxedObject`, `Polymorphic`).

Unguarded `Polymorphic` transitions are verifier errors (Part XVIII: every
representation change is guarded; Law 126). `RepTransitionGuard` deopts on
unexpected tags and must carry complete Rule 122 metadata.

### 4.13 State (1)

| Kind | Inputs | payload / payload2 | Notes |
|---|---|---|---|
| `FrameState` | [local values...] | fs=FrameStateId / — | The Rule 5 deopt snapshot node |

## 5. Metadata structures

### 5.1 FrameState (`FrameStateDesc`)

Carries everything that is not an edge: `method`, `pc` (RBC resume point),
`caller` (inlined caller's FrameStateId — chains must be acyclic; the
inlining depth seam for ICDG), and the deopt-materialization vobj list.

The LOCAL VALUES ARE INPUT EDGES of the `FrameState` node. This is the
soundness-critical choice (pinned by test): replacing a local's producer
rewires every snapshot that captured it automatically.

The vobj list is stored as a flat `fsVobjs_` vector; each descriptor owns a
`[vobjOffset, vobjOffset + vobjCount)` slice. `makeFrameState` accepts a vobj
list at creation time only. `appendFrameStateVobj(fs, vobj)` (MSG-20260901-006)
splices a vobj into the END of an existing descriptor's slice, grows that
descriptor's `vobjCount`, and shifts the `vobjOffset` of every later
descriptor by one (the flat-vector insert moves their slices). Deterministic
(Rule 124): the insert position is a pure function of the target descriptor's
current `(vobjOffset, vobjCount)`; no reordering; FrameStateId space is
unchanged; no serializer format change (the per-descriptor vobj list already
round-trips). Out-of-range `fs` is a silent no-op. The existing FrameState
closure check (section 7) validates the post-append result — a bogus append
(e.g. listing an outer vobj whose inner is not listed) still rejects.

### 5.2 SpecMeta (Rule 122 — the complete mandatory field set)

`kind` (ClassHierarchyStable, MethodFinal, TypeMonomorphic, TypeBimorphic,
NullNeverSeen, BoundsAlwaysValid, ArgumentConstant, LoopTripCountProfile,
StaticProofCarried), `source` (PGO / Static / Assumption), `confidence`
(0–10000 basis points), `guard` (the enforcing guard node; required unless
source is Static), `deoptTarget`, `cost`, `dependency` (Rule 42 watchdog
entry; required for PGO/Assumption sources), `rollback` (None /
DeferredEffects / Compensated).

Attached via `attachSpecMeta` (sets the `Speculative` flag). A speculative
node without complete metadata is a verifier error.

### 5.3 Dependency (Rule 42)

`kind` (ClassHierarchy, MethodBody, FieldFinality, ProfileCounter,
StaticProof) + `target`. No assumption without an invalidation path: the
watchdog consumes these records (the runtime registry itself is future
runtime work; the IR side of the contract is complete).

### 5.4 Replacement log (Rule 14)

`{oldNode, newNode, epoch}` appended by every `replaceNode`. Tombstones
(`Dead|Replaced` + epoch) stay in the node array; ids remain valid within
the graph snapshot (Rule 124). Reclamation is bulk-free with the graph; the
cross-thread epoch machinery arrives with the concurrent compilation work
(documented scope: the representation and the log are in place).

## 6. Node flags (Rule 32)

`Dead`, `Replaced`, `Speculative`, `Pinned`, `NeverNull`,
`OnExceptionPath` — a typed `Flags<NodeFlag>` bitmask, never raw integers.

## 7. Verifier contract (Rules 40, 126)

`verify(const Graph&)` is total: any malformed graph yields ≤ 100 bounded
diagnostics, never UB. Checks, in order:

1. structural: input ids in range and alive, no self-reference, input
   count matches the registry signature (fixed + variadic + trailing
   FrameState), `reserved == 0`;
2. role conformance: Ctrl/Mem producers by kind, FrameState inputs are
   FrameState nodes, projection parents by kind;
3. merge shapes: Phi arity = 1 + region preds, Phi input 0 is
   Region/LoopBegin, Region ≥ 2 preds, LoopBegin ≥ 2 preds, Return ≤ 1
   value;
4. operand types per kind (Rule 33: mismatches are errors, never
   coercions);
5. memory-chain continuity: every Mem input traces back to Start through
   memory-state producers, cycle-free — where the one LEGAL cycle shape is
   the loop backedge closure: an on-path revisit of a **LoopBegin-backed
   memory Phi** (the header whose backedge value input chains through the
   body producers back to the header) is skipped and the phi is proven
   through its other inputs; a revisit of a Region-backed Phi or any
   non-Phi node is a real cycle (MSG-20260901-004);
6. guards/speculation: FrameState attached (Rule 5); Speculative nodes
   carry complete Rule 122 metadata incl. a valid dependency (Rule 42);
7. FrameState caller chains acyclic AND every chain target resolves to a
   live FrameState node (the chain reference is side-table data — a desc
   id, not a use-def edge — so a killed target would otherwise pass
   while its junked input edges no longer carry the caller-frame slots;
   Rule 75, MSG-20260901-002); vobj entries are live
   VirtualObjectState nodes;
8. PEA: materialization graphs acyclic; deopt-list closure; no
   still-virtual fields under Materialize;
9. vector well-formedness: power-of-two lanes in [2,64], operand type/lane
   agreement, mask agreement, op legality per lane family;
10. tagged transitions: valid reps; no unguarded Polymorphic transitions.
The pipeline wiring (run-after-every-pass in debug builds) belongs to the
Passes Team's pass registry; the reusable hook is this function.

## 8. Serialization (Rules 31, 38, 124)

Versioned binary format, little-endian, layout documented in
`compiler/ir/core/Serialize.cpp`. Magic `B2IR`, format version **2** (v2
appended the graph-builder value kinds after `FrameState` and made
guards/fixed nodes control producers; all v1 kind VALUES are unchanged, so
v1 artifacts load unchanged — the reader accepts versions 1..2). Nodes in
id order (tombstones included — ids preserved), side tables in id order,
replacement log in log order. Byte-deterministic for the same graph state.

`deserializeInto` is a total parser: bounds-checked reads, validated
kinds/enums/ids, two-pass edge replay (forward references legal), and the
rebuilt graph must pass verification before use. Wrong magic, stale
version, truncation, corrupt records, and verification failures are all
load errors — never crashes.

## 9. Appendix A — printer format (normative)

Deterministic; golden fixtures pin it; changes require a format-version
bump (Rule 31). Shape:

```text
; B-2 IR v1 nodes=27 live=27 epoch=0
n0   Start
n1   Parameter #0 : int
n2   ConstantI 42
n4   If n0 n3
n5   IfTrue n4
n9   FrameState m=7 pc=12 caller=none locals=[n1 n3 n8] vobjlist=[n6]
n10  Guard n0 n3 n9 kind=nullcheck deopt=4 spec=s0
n11  StoreField n0 n0 n1 n8 f=14
n12  CallVirtual n0 n11 n8 n1 n9 m=3 ret=int
n13  VectorObjectState t=12 kind=instance
; spec s0 kind=typemono src=pgo conf=7500 guard=n10 deopt=4 cost=12 dep=d0 rollback=deferred
; dep d0 kind=classhierarchy target=17
; replaced n3 -> n14 (epoch 1)
```

Float/double constants print exact bits (`bits=0x...`) — lossless including
NaN payloads and −0.0.

## 10. Appendix B — category counts (test-pinned)

control 15, memory 15, calls 6, type-ops 2, guards 1, constants/params 8,
arithmetic 37, comparisons 14, conversions 23, merges 1, vector 9, PEA 2,
tagged 3, state 1 — **137 kinds** (v2: +`Undef`, +`Not`, +`RefEq`,
+`IsNull`, +`EqI..GeI`).
`ir_node_registry_rows_match_the_spec_document_counts` fails if this table
and the enum drift apart.
