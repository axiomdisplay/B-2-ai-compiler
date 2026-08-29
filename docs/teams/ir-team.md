# IR Team Charter

```text
Normative reference: docs/laws.md
(B-2 Java Runtime Compiler Laws & Architecture Specification, as amended by
Amendment A, Amendment B, and the Special Pass Laws in Part XVIII.)
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

Team size: 2

Roles:

- `IR-IMPL` — Implementer. Produces the sea-of-nodes IR core, node kinds, effect model, metadata attachments, verifier core, serialization, and IR documentation inside the team's owned write paths. Also owns the RBC (Register Bytecode) core: opcode set, encoding, builder, verifier, and text format (docs/rbc_spec.md), the shared middle-end contract consumed by all tiers.
- `IR-REV` — Reviewer. Checks every change for representation bugs, law compliance, boundary violations, missing tests, and missing node specifications. Does not rewrite code unless explicitly reassigned.

---

## Mission

The IR Team owns the core sea-of-nodes compiler intermediate representation used by T2 and T3. Per Amendment B, T2 is the first IR tier — "Core IR: sea-of-nodes, index-based `NodeId`, explicit effects, guards, `FrameState`, dependencies, and deopt metadata" (Amendment B.1) — and T3 runs the same optimizer pipeline offline (Amendment B.5: "AOT uses the T2 pipeline — not a separate optimizer"), so every representation decision must serve both a JIT mode (PGO, guards, deopt) and an AOT mode (static proofs, guarded or unguarded). Per Amendment A, T1 is a no-IR baseline JIT; the IR Team does NOT serve T1 and adds no IR machinery or carve-outs for it.

In scope:

- The RBC (Register Bytecode) core per `docs/rbc_spec.md`: the 150-opcode set, fixed 12-byte `Ins` encoding, constant pool, `RbcBuilder`, the structural + type verifier, and the text format. RBC is the convergence point of both entry paths (frontend lowering; loader/verifier/quickener) and the input of every tier; opcode and format changes require a cross-team message per `docs/teams/messaging.md`.
- Sea-of-nodes graph representation and storage, arena-allocated per Rule 7.
- Index-based `NodeId` edges (`uint32_t`) and use-def / def-use chain storage.
- The `NodeKind` taxonomy: pure computation, control, memory, exception, guard, conversion, call, vector/SIMD, and tagged-value nodes.
- The explicit effect model and its effect classes (Rule 121).
- Control, data, memory, and exception dependency representation.
- The `FrameState` attachment API (Rule 5).
- Guard node representation and the `GuardKind` taxonomy (Rule 32).
- Speculative metadata representation (Rule 122).
- Invalidation-dependency representation (Rule 42).
- IR serialization and replay support.
- The IR verifier's core data structures and checks (Rules 40, 126).

The IR must be able to REPRESENT what the Part XVIII first-class passes require — those laws bind T2/T3, and the Passes Team cannot satisfy them on an insufficient representation:

- SIMD/vector operations for SWLP: lane types, pack/unpack, masked operations, reductions, gathers/scatters.
- Scalar-replacement and object materialization structures for PEA: virtual-object state, materialization points, deopt reconstruction metadata.
- Tagged-value representation nodes for NaN boxing lowering: tag/untag nodes with explicit canonicalization constraints.

The IR Team delivers the representation, not the optimizations: optimization passes, machine lowering, register allocation, codegen, the interpreter, the baseline JIT, and the AOT driver are owned by other teams.

---

## Owned Write Paths

```text
compiler/ir/core/
include/b2/ir/
tests/ir/
docs/ir_spec.md
docs/effect_system.md
```

---

## Forbidden Write Paths

```text
compiler/interp/
compiler/baseline/
compiler/passes/
compiler/regalloc/
compiler/codegen/
compiler/aot/
runtime/
```

The team never writes outside its owned paths, even for one-line fixes. Any issue in another team's area — or any change to a cross-team contract this team publishes (IR API, node semantics, verifier behavior) — is raised as a message under `messages/` per `docs/teams/messaging.md`; the owning team resolves it.

---

## Key Laws

- Rule 7 — Zero-Allocation Hot Path: IR storage is arena-backed (`std::pmr::monotonic_buffer_resource` or equivalent), bulk-freed after compilation.
- Rule 8 — No RTTI: node type switching uses `enum class NodeKind`, never dynamic type queries.
- Rule 9 — No `std::shared_ptr` / `std::function` in Hot IR Code: raw pointers plus stable `NodeId`s; ownership explicit and arena-scoped.
- Rule 15 — Index-Based Graph: never raw pointers for edges; all node references are `NodeId` (`uint32_t`).
- Rule 16 — Interned Symbols: no `std::string`/`std::string_view` in the IR; identifiers are `SymbolId`.
- Rule 17 — Cache-Friendly Hash Maps: `std::unordered_map`/`std::map` are forbidden in the compiler hot path.
- Rule 18 — Sparse Sets and BitVectors: pass-side node sets use sparse sets or bitvectors, not `std::set`/`std::vector<bool>`.
- Rule 19 — Small Buffer Optimization: use-def chains, operands, guard dependency lists, and small edge lists use `SmallVector<T, N>`.
- Rule 20 — Structure of Arrays: bulk per-attribute node processing uses SoA layout, not full-`Node` iteration.
- Rule 32 — Bitmasked Boolean State: `NodeFlags`, `EffectTags`, `GuardKinds` are type-safe `Flags<E>` bitmasks; raw integers forbidden.
- Rule 33 — No Implicit Conversions: every conversion is an explicit node (`SignExtend`, `ZeroExtend`, `Truncate`, `BitCast`, `CompressedRefEncode`/`Decode`, boxing/unboxing nodes).
- Rule 121 — The IR Must Have an Explicit Effect Model: all effect classes represented; passes must not reorder effects without proof; enforced by the effect-chain verifier.
- Rule 122 — Speculative Nodes Must Carry Metadata: speculation kind, PGO/static source, confidence, guard plan, `FrameState`, deopt target, cost, invalidation dependency, rollback/deferred-effect plan.
- Rule 126 — The Verifier Must Check Deopt and GC Metadata: guards have `FrameState`; deopt points reachable; GC references mapped across safepoints; effect chains continuous; invalidation info present; materialization graphs acyclic or properly handled.
- Rule 130 — Every IR Node and Trampoline Must Have a Specification: semantics, effects, tier behavior, lowering, verifier constraints, deopt behavior, GC behavior, tests — enforced by documentation lint and the node registry.
- Amendment A — No-IR Baseline Tier: the unified IR pipeline applies to T2/T3 only; T1 gets no IR support from this team.
- Amendment B — Tier Model Redesign: T2 is the first IR tier; T3 AOT reuses the same pipeline (B.1, B.2, B.5).
- Part XVIII — Special Pass Laws: the representation must cover SWLP vector nodes, PEA materialization structures, and NaN-boxing tagged values.

---

## Deliverables

1. `Graph` — arena-backed sea-of-nodes container with node storage, edge slots, and epoch-tagged replacement (Rule 14).
2. `NodeId` — the `uint32_t` node index type, the invalid-node sentinel, and ID stability rules within a graph snapshot.
3. `NodeKind` — the `enum class` taxonomy for all nodes, including vector, materialization, and tagged-value kinds.
4. `EffectKind` — the effect classification covering all Rule 121 effect classes.
5. `GuardKind` — the guard taxonomy, bitmask-friendly per Rule 32.
6. `FrameStateRef` — the `FrameState` attachment type and API (Rule 5).
7. `DependencyRef` — invalidation/dependency records for the watchdog (Rule 42).
8. Vector/SIMD node kinds sufficient for SWLP — pack, unpack, lane operations, masked operations, reductions, gathers/scatters, with lane-count and mask well-formedness rules.
9. Materialization structures for PEA — virtual-object state, field/value tracking, materialization points, deopt reconstruction metadata.
10. Tagged-value representation for NaN boxing lowering — tag/untag nodes carrying the canonicalization constraints of Part XVIII.
11. IR verifier core — structural and metadata checks (Rules 40, 126), exposed as a reusable hook for the pipeline.
12. IR printer — deterministic, diff-stable textual form for golden tests and bug reports.
13. IR serialization/replay support — versioned format (Rule 31) sufficient for Rule 38 replay artifacts.
14. Golden IR tests — checked-in fixtures covering construction, effects, metadata, vector/materialization/tagged nodes, and round-trips.

---

## Interface Contract

### Consumes

- Graph-building calls from the Passes Team's frontend/graph-building passes (suite items 1-10), via node-creation APIs only.
- Interned `SymbolId`s from the frontend symbol table (Rule 16); the IR never stores raw strings.
- Read-only target descriptions when a representation decision depends on hardware (Rules 24, 27).
- Profile metadata schemas (read-only) so Rule 122 speculative metadata can carry source and confidence.

### Outputs

- Stable public headers in `include/b2/ir/`: node creation, effect-chain construction, guard attachment, `FrameState` attachment, read-only traversal, verifier hooks.
- `docs/ir_spec.md` — the per-node specification required by Rule 130.
- `docs/effect_system.md` — the effect model definition (Rule 121).
- Verifier diagnostics consumed by the pass pipeline in debug builds (Rule 40).
- Versioned serialization for replay artifacts (Rules 31, 38).

### Promises

- No raw pointer edge storage anywhere in the graph (Rule 15).
- Deterministic node IDs within a graph snapshot; serialization preserves them (Rule 124).
- Explicit effect ordering; nothing in the IR silently reorders effects (Rule 121).
- No implicit type coercion; conversions exist only as explicit nodes (Rule 33).
- No hidden global mutable state (Rule 125).

---

## Testing Responsibilities

```text
tests/ir/
```

- Graph construction: empty, single-node, deep chains, wide fan-in, replacement and epoch behavior.
- Effect-chain continuity across all Rule 121 effect classes.
- Use-def / def-use integrity under node replacement and reclamation (Rule 14).
- Guard metadata attachment: every Rule 122 field present; malformed metadata rejected.
- `FrameState` attachment API behavior (Rule 5).
- Vector node well-formedness: lane counts, mask operands, reductions, gathers/scatters.
- Materialization structure well-formedness: acyclic or properly handled (Rule 126).
- Tagged-value node well-formedness: NaN boxing representation constraints (Part XVIII).
- Serialization/replay round-trip, including rejection of stale format versions (Rule 31).
- Malformed IR rejection: dangling `NodeId`s, broken dominance, missing `FrameState` on guards, discontinuous effect chains (Rules 40, 126).

Test names must be self-documenting (Rule 41); bug fixes require five regression tests (Rule 34).

---

## When To Send Messages

- The Passes Team requests a missing node kind — they send the RFC; this team reviews, accepts, or proposes an alternative representation.
- The Codegen Team needs new IR metadata carried through to lowering.
- The RegAlloc Team needs GC-reference metadata schema changes.
- Verifier behavior changes in ways that affect other teams' tests (new mandatory checks, changed diagnostics).
- The NaN boxing representation contract changes — cross-team; requires all affected teams' approval per Part XVIII (interpreter, baseline_noir, passes, regalloc, codegen, aot, GC/runtime).
- A public API in `include/b2/ir/` is added, renamed, or deprecated.

Bugs in IR code are owned and fixed by this team regardless of who reports them; if a fix affects other teams, an ADVISORY accompanies it. The team never writes into another team's paths, regardless of severity.

---

## Reviewer Checklist

- [ ] No raw pointer edges; all references are `NodeId` (Rule 15).
- [ ] No strings in hot IR paths; identifiers are interned `SymbolId` (Rule 16).
- [ ] No `std::unordered_map`/`std::map` in hot paths; sets are sparse sets or bitvectors (Rules 17, 18).
- [ ] No implicit conversions; every conversion is an explicit node (Rule 33).
- [ ] Flags are typed bitmasks, never raw integers (Rule 32).
- [ ] Every new node kind is documented in `docs/ir_spec.md` with semantics, effects, verifier constraints, deopt and GC behavior, and tests (Rule 130).
- [ ] The effect model stays explicit; new effect classes are enumerated in `docs/effect_system.md` (Rule 121).
- [ ] Speculative nodes carry the complete Rule 122 metadata set.
- [ ] Vector, materialization, and tagged-value nodes are specified for SWLP, PEA, and NaN boxing per Part XVIII.
- [ ] The verifier rejects invalid graphs: dangling IDs, missing `FrameState`, broken effect chains, bad GC metadata (Rules 40, 126).
- [ ] Serialization changes are versioned (Rule 31) and round-trip tested; node IDs stay deterministic (Rule 124).
- [ ] Tests cover positive and negative cases, with self-documenting names (Rule 41).
- [ ] No file outside the owned write paths is modified; cross-team requests went through `messages/`.
