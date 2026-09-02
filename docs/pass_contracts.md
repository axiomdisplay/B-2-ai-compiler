# B-2 Pass Contracts (v1)

```text
Normative reference: docs/laws.md (Rules 1, 3-5, 10, 14, 23-26, 28, 33,
35, 40, 42-45, 72, 73, 121-126, 130, 132, 144; Amendments A and B),
docs/teams/passes-team.md (the charter and its appendix numbering),
docs/ir_spec.md (v2), docs/graph_builder.md.
If this document conflicts with docs/laws.md, laws.md wins.
```

Team: Passes Team. Implementation: `compiler/passes/src/` (Passes.cpp
registry/driver, PassSupport.cpp shared machinery, Simplify.cpp,
SCCP.cpp, ControlFlow.cpp, DCE.cpp, GVN.cpp, Escape.cpp), API
`include/b2/passes/Passes.h`, tests `tests/passes/PassTests.cpp`,
`tests/passes/SCCPTests.cpp` and `tests/passes/EscapeTests.cpp` (Rule
35: >= 10 tests per delivered pass key), tool surface `b2graph -O` /
`b2graph --pea`.

This document is the **rule-level review artifact** for the pass suite
v1: every rewrite the passes may perform, the exact soundness argument,
the budgets, the kill switches, and the tombstone-law protocol that
keeps every intermediate state verifier-clean. The registry summary
lives in code (`passRegistry()`); this file is the prose contract.

---

## 1. Delivered passes (registry keys = charter appendix numbers)

| Key | Name | Family | What it does (one line) |
|-----|------|--------|--------------------------|
| 11 | dce | cleanup | remove DCE-safe nodes with zero total referencers |
| 12 | fusion | cleanup | trivial-phi collapse + region pred repair/collapse |
| 13 | canon | simplify | commutative const-to-right + Not-of-test complements |
| 14 | constfold | simplify | evaluate constant expressions to exact-bit constants |
| 15 | strength | simplify | multiply-by-power-of-two to shifts |
| 16 | identity | simplify | identity/self patterns (exact semantics only) |
| 18 | nullfold | guard | NullCheck guards on provably non-null values pass through |
| 19 | branchnorm | control | constant-condition Ifs/Guards fold to the taken edge |
| 20 | cfs | control | unreachable sweep + region repair |
| 35 | gvn | scalar | structural global value numbering |
| 38 | sccp | scalar | sparse conditional constant propagation (rows 37-39) |
| 65 | escape | pea | escape classification lattice + decision log (analysis-only) |
| 66 | pea | pea | escape-point materialization (Materialize wired into both chains) |
| 67 | scalar | pea | no-escape allocation replacement (loads forward, stores vanish) |
| 69 | matplan | pea | vobj snapshots + nested materialization planning |

Not delivered in v1: **17 (redundant cast removal)** — needs type proofs
(CHA/hierarchy); `runSinglePass` refuses the key with a diagnostic. Items
11-20 minus 17 and item 35 are the early-cleanup core; items 12-16 share
one sweep engine (`runSimplify`) with a class mask so each key is
independently kill-switchable while the pipeline runs them as one pass
for cost. **SCCP (38) is one engine covering the charter's whole scalar
constant-propagation family**: rows 37 (CPROP) and 39 (conditional
constant propagation) are subsumed exactly the way CSE (36) is covered
by GVN for identical keys — the unconditional reading is the lattice
with both branch sides always executable, and the conditional reading
IS the phi-meet-over-executable-edges rule (section 13). The CM-PEA
family (65/66/67/69) is one engine (`Escape.cpp`)
with four stage bits — each key is independently kill-switchable and a
disabled stage REJECTS that disposition (PEA Rule option 4), never
half-applies.

**Rule 1 (one suite, two tiers)**: T2 Profile Mode and T3 Static Mode run
the same list in the same order. Tier-specific filters arrive with the
speculation passes (PGO guards, unstable-branch profiling), not by
forking this suite. Rule 2 is honored trivially: v1 passes have no PGO
inputs and no speculation, so the same pass is its static and profile
mode.

## 2. Pipeline order and fixpoint (Rule 10)

```text
round (max kMaxEarlyCleanupRounds = 3):
  simplify (fold | identity | strength | canon | trivial-phi, one sweep)
  sccp (38)               [lattice propagation; new constants land NOW]
  nullfold (18)
  branchnorm (19)          [includes the unreachable sweep + repair]
  cfs (20)
  dce (11)
  gvn (35)
  pea (65|66/67|69, one engine call; stage bits per key)
  dce (11)                 [post-PEA orphans + forwarded constants]
stop when the round changed nothing (converged=true), or at the round
budget while still changing (converged=false + telemetry diag: the graph
is valid, the fixpoint is merely unconfirmed).
```

SCCP runs immediately after the local folds (section 13): the folded
literals are its operand seeds, and every constant it discovers — a
phi-meet value, a loop-invariant backedge fixpoint, a branch condition
that only became constant through propagation — is already a literal
node when nullfold/branchnorm/cfs/dce run in the SAME round, so the
control-level consequences (a folded branch, a swept dead arm) land
without burning an extra fixpoint round. PEA runs after GVN (redundant loads are already merged) and after the
use guards were folded (nullfold + DCE) — that is the engine's documented
precondition: live guards on the allocation's uses pin FrameStates into
the locals, which would otherwise force `fs-deopt-ref` rejections.

Every pass is independently kill-switchable (`PassConfig::setPassEnabled`,
Rules 132/144); a disabled pass is a successful no-op. The graph verifier
runs after every pass while `verifyAfterEachPass` (Rule 40; default on for
tools/tests/debug, cost-decision for the T2 runtime driver); a
verification failure stops the pipeline fail-closed with diagnostics.

Per-pass rewrite budget: `kDefaultPassRewriteBudget` (200000) named
replacements per pass run; overrun stops at a structurally valid point and
reports telemetry (Rule 26) — never UB, never a half-applied rewrite.

## 3. The tombstone law (why Junk exists)

The IR verifier checks **every** node — including Dead/Replaced
tombstones — for input liveness, role kind, and operand type. A tombstone
therefore keeps its input edges forever, and those edges must stay
verifier-legal forever. The pass suite complies by rewiring tombstone
edges onto immortal **junk sinks** (`detail::Junk`):

| Tombstone slot | Sink | Why legal |
|---|---|---|
| Ctrl, Mem | `Start` | produces control AND memory state; not a terminal |
| Data (typed) | zero-constant of the edge's value type | operand-type match |
| Data (Bottom) | `Start` | memory-phi value edges: the memory origin, join-legal |
| FrameState | an empty `FrameState` | desc/caller/vobj checks all pass |
| Parent (If proj) | a live `If [Start, 0]` | checkProjectionParent wants an If |
| Parent (Switch proj) | a live `Switch [Start, 0]` | ditto |
| Parent (CallExcept) | a live `CallStatic [Start, Start, FS]` | any Call kind |
| Phi region slot | a `Region` with N Start preds (N = value count, >= 2; a 1-value phi is padded first) | Phi input-0 kind + arity |

Consequences (all deliberate, all tested):

- `kill` fires only when no LIVE node references the target
  (dead referencers are junk-rewired first). Dead-code elimination may
  only remove nodes with **zero total referencers** — a def referenced
  by a tombstone is unremovable (sound, just less clean).
- Junk sinks are flow-dead (nothing live consumes them), invisible to
  live-user analyses, and mergeable by GVN onto identical real constants
  (replaceNode rewires the tombstone users; that is how the graph
  converges within one run instead of churning sinks).
- A folded If's dead fork may remain as a live, userless If (a branch to
  nowhere). It is effect-free, unreachable in every live-user sense, and
  kept because it is structurally indistinguishable from a junk anchor —
  killing it would re-junk tombstones on every pipeline application and
  break idempotency. The sanctioned removal mechanism is requested from
  the IR team in MSG-009.
- **Idempotency is pinned by tests**: a second full pipeline run performs
  zero rewrites/removals and prints byte-identically; determinism
  (Rule 124) is pinned by double-build + double-run comparisons over the
  whole corpus.

## 4. DCE-safe set (key 11)

Removed: unused FrameState nodes (deopt metadata without a deopt point),
ArrayLength/InstanceOf (the builder null-guards receivers in control —
graph_builder.md SS3 — so the nodes cannot trap), and pure values
(arithmetic, comparisons, conversions, constants, parameters, phis).

Never removed (v1 conservatisms, each documented and tested):

- **DivI/RemI/DivL/RemL** — a userless instance may be the only trap
  witness in a hand-built graph; removal needs a guard-adjacency proof.
- **LoadField/LoadStatic/LoadElem** — volatile-read classification is
  opaque at IR level (FieldId payloads are frontend-side).
- **Stores, calls, allocations, casts, guards, control** — observable by
  definition.
- **Chained FrameStates** (inlining v1 addition, docs/inlining.md
  section 4) — a FrameState whose descriptor is the `caller` target of
  another descriptor is inlined-frame reconstruction data: the
  reference is side-table state, invisible to use-def edges, so a
  userless chained snapshot stays live (its input edges ARE the
  caller-frame slot values the deoptimizer needs). Without this guard
  DCE would destroy caller-frame reconstruction while the graph stays
  verifier-clean.

## 5. The simplify rewrite catalogs (keys 13-16)

**Fold (14) — exact-semantics evaluation.** Integer ops wrap (`+ - * &
| ^ << >> >>>` with JVM count masking; `INT_MIN / -1` wraps to `INT_MIN`,
rem to 0; division by a constant zero is NOT folded — the guard owns the
trap). Long ops mirror. Comparisons fold (`CmpI/CmpL`, `EqI..GeI`).
Conversions fold per JLS: `I2L/L2I/I2B/I2C/I2S` bit semantics; `I2F/I2D/
L2F/L2D` exact IEEE; `F2I/F2L/D2I/D2L` with the JLS 5.1.3 narrowing rules
(NaN to 0, infinities clamped); `F2D` exact, `D2F` correctly rounded.
Value facts: `IsNull(null)` to 1, `IsNull(never-null)` to 0; `RefEq(x,x)`
to 1; `RefEq(null, null)` to 1; `RefEq(null, never-null)` to 0;
`InstanceOf(null)` to 0.

**The exact-bits FP policy** (Rule 72 without appealing to JLS NaN-bit
freedom): FP arithmetic folds only when neither input is NaN and the
computed result is not NaN (`x/0` to infinity folds; `0/0`, `inf-inf`,
`fmod(inf, y)`, `frem(NaN, y)` do not). FP **comparisons** always fold —
`CmpFl/Fg/Dl/Dg` produce a defined int even on NaN. `NegF/NegD` always
fold — a sign-bit flip preserves NaN payloads exactly. FP commutativity
is never assumed (NaN payload propagation can be operand-order sensitive
on IEEE-754 hardware), and FP additive identities (`x + 0.0`) are never
removed (`-0.0 + 0.0 == +0.0`).

**Identity (16) — exact for every runtime input.** Int/long: `x+0`,
`x-0`, `x*1`, `x*0` to 0, `x&-1`, `x&0` to 0, `x|0`, `x|-1` to -1,
`x^0`, shifts by 0, `x-x` to 0, `x&x`, `x|x`, `x^x` to 0, `x/1`,
`x%1` to 0, self-compares (`CmpI/L(x,x)` to 0, `EqI/LeI/GeI` to 1,
`NeI/LtI/GtI` to 0 — total orders, no NaN caveat). Exact conversions:
`L2I(I2L(x))`, narrowing idempotence `I2B/I2C/I2S`. `Neg(Neg(x))` for
int/long (wrap involution) and FP (bit flips cancel). `Not(Not(t))` only
for 0/1-producing test kinds (a general int x has `Not(Not(x)) == (x !=
0)`, not x).

**Strength (15).** `x * 2^k` (k >= 1) to `x << k` for int and long —
identical under wrap semantics. Negated powers and 0/1 stay (identity
owns those); FP multiplies never.

**Canonicalization (13).** Commutative integer ops: constant to the
right operand (FP never — see the exact-bits policy). `Not(test(a,b))`
rewrites to the complement test (`EqI<->NeI`, `LtI<->GeI`, `LeI<->GtI`)
— the builder's `ifnonnull`/`if_acmpne` shapes normalize to direct
tests.

**Trivial phi (12, value half).** A phi whose value inputs (ignoring
self inputs — the loop-invariant marker) are all one node is that node;
a single-value phi is that value. Sound for memory phis and slot phis
alike; the value identity is exact.

## 6. Null-check folding (18)

The builder's guard shape is `Guard(NullCheck, Not(IsNull(x)))`
(graph_builder.md SS3). The fold fires when x is provably never null:
kinds `New/NewArray/NewRefArray/NewMultiArray/Materialize/ConstantSym`,
or the `NeverNull` flag. The guard is replaced by its control input (the
guarded op consumes the predecessor's control); the tombstone junking
frees the condition and — when unshared — the FrameState for DCE.
Guards on maybe-null values (parameters, loads, phis, null constants)
are kept: a null receiver deopts to T0, which raises the NPE — the guard
is the mechanism. ZeroCheck/BoundsCheck guards are untouched by this key.

## 7. Branch normalization (19) and the sweep (20)

**If with constant condition**: the taken projection is replaced by the
If's control input (control flows around the branch); the untaken side,
the dead branch, and the If are reclaimed by the sweep. **Guard with
constant condition**: true passes control through; false becomes an
unconditional `Deopt [ctrl, framestate]` carrying the guard's DeoptId and
FrameState (the deopt reconstructs exactly the T0 state the guard would
have) — idempotently (a re-run finds the existing Deopt). **Switch on a
constant is NOT folded** (v1 refusal): the case labels live in the
frontend's switch table behind an opaque payload; folding needs that
side channel.

**The sweep**: flow reachability from Start where terminators
(Return/Unwind/Deopt/End) stop flow and constant-condition Ifs/Guards
propagate only through their live outcome; unreachable control consumers
die bottom-up (kill fires only with zero live referencers — the region
repair that runs first drops unreachable region preds, which orphans the
projections and chains for the kill phase). Regions with dead or
unreachable preds are REBUILT through the public API (fresh Region/phi
with slot-for-pred values — Graph has no removeInput); single-pred
regions collapse to the pass-through edge. `LoopBegin` with a dead
backedge collapses to the entry edge; its phis collapse to entry values.
Blocks with no in-edges never materialize at build time (the builder
drops them); the sweep's business is only unreachable code CREATED by
transforms.

## 8. GVN (35)

Two live nodes with the same `(kind, payload, payload2, constValue,
exact input vector)` compute the same value everywhere either is
computable — both are the same total function of the same inputs, so
replacing the later node with the earlier (lowest id wins; id-order
sweep; the hash map is only probed, never iterated — Rule 124) is sound
WITHOUT a schedule or dominator analysis: pure nodes float, and anywhere
the removed node was computable, the survivor is too. `replaceNode`
rewires every user — including FrameState snapshots, so deopt states
stay value-identical (Rule 14 doing real work; pinned by tests).

Eligible: pure values (constants, arithmetic incl. trapping div — the
survivor inherits all users, so the trap fires exactly when any original
would have — comparisons, conversions, parameters, phis), ArrayLength,
InstanceOf, and the Load* readers (identical ctrl+mem+address yield the
same read; loads chain control, so builder output rarely produces equal
keys — they serve transformed graphs). Excluded: Undef and
VirtualObjectState (analysis state, not values), FrameState, guards,
calls (CallOpaque), stores, allocations, control. Junk sinks are not
protected: one that duplicates a real constant merges onto it (the
tombstone users rewire; that is the one-run convergence mechanism).

## 9. Telemetry (Rule 26), budgets (Rule 23), replay

`PassTelemetry` reports rewrites (replaceNode), removals (killNode),
folds (simplify-family), gvnDedups, fixpoint rounds, budget overruns, and
`converged`. All knobs are named constants (`kMaxEarlyCleanupRounds`,
`kDefaultPassRewriteBudget`, `kMaxSimplifySweeps`, `kMaxDceSweeps`,
`kMaxSweepIterations`, `kMaxRepairIterations`); overrun never corrupts —
the budget charges before each atomic mutation. Deterministic execution
(Rules 124/125): fixed id-order sweeps, replacement by lowest id, node
creation in sweep order, no hidden global state (PassConfig is a value
type; one Junk cache per pipeline run, created lazily in a fixed order).
Replay artifacts (Rule 38) arrive with the pipeline's integration into
the T2 driver; the printer's deterministic dump is the current golden
surface.

## 10. Testing (Rule 35) and the corpus gate

`tests/passes/PassTests.cpp`: 126 tests over the ten pre-SCCP keys —
ten or more per key, each pinning: the rewrite catalogs above (positive
and negative: FP NaN non-folds, conservative exclusions, switch
refusal), tombstone-law legality (verifier after every pass), kill
switches (byte-identical no-op graphs), idempotency (zero-telemetry
second runs), determinism (double-build byte identity), and the Rule 14
FrameState auto-update. `tests/passes/SCCPTests.cpp` adds 21 tests over
key 38 (section 13.6). The strongest gate is the corpus sweep: all 19
interp-corpus programs, every method, through the full pipeline —
verified, deterministic, idempotent — plus `b2graph -O` as the human
surface (telemetry line per method; the sccp= counter rides it).

## 11. Deferred (with the blocking reason)

| Item | Blocker |
|---|---|
| 17 redundant cast removal | needs type proofs (CHA / hierarchy) |
| Switch folding on constants | case labels are frontend-side (opaque payload) |
| Load CSE beyond identical keys | loads chain control (PRE/scheduling territory) |
| Div/rem DCE | guard-adjacency proof |
| Tombstone/dead-fork reclamation | decision recorded in MSG-009's close: deferred to a dedicated RFC (a removal API or dead-node input-check skip changes verifier semantics, serializer stability, and every golden dump; the junk-sink protocol stays the sanctioned sound mechanism) |
| CmpL-based long zero-guards | the MSG-009 resultTypeOf fix LANDED (CmpL now types Int, verified); the L2I composition stays the shipped v1 shape — sound (over-deopts only), tested, and byte-pinned by goldens; switching to the CmpL form is an optional passes-team cleanup |
| Inlining registry rows (suite 21-34 speculation family) | profile import (icdg.md Phase 1) + Rule 1's tier-filter mechanism; the direct-inline driver itself is delivered (docs/inlining.md) |
| ~~Inline verification of caller-chain fs liveness~~ | RESOLVED by the MSG-20260901-002 close: the IR verifier now requires every caller chain target to resolve to a live snapshot node (ir_spec 7 check 7); the DCE-side conservatism stays as belt-and-suspenders |

---

## 12. CM-PEA (keys 65/66/67/69 — one engine, special_passes.md section 1)

**The disposition table (PEA Rule options 1-4).** Every live use of an
allocation (`New`/`NewArray`/`NewRefArray`, id-ascending, at most
`kPeaMaxAllocsPerGraph`) lands in a fixed classification table; the
lattice value is the monotone join (max) of the use grades:

| Use shape | Grade / disposition |
|---|---|
| `LoadField`/`LoadElem` obj slot (const idx) | reader (no escape) |
| `StoreField`/`StoreElem` obj slot (const idx) | writer (no escape) |
| `ArrayLength`, `IsNull` | length reader / never-null fold (floating, position-independent) |
| the allocation as a ctrl/mem-chain predecessor | ChainLink: spliced onto the allocation's ctrl predecessor |
| `VirtualObjectState` field edge | LocalEscape (`virtual-object-held`): the inner allocation stays real |
| `Call*` Data slot | ArgEscape (`call-arg`): materialize at the call |
| `Return` value | ReturnEscape (`return-escape`): materialize at the return |
| `StoreField`/`StoreStatic`/`StoreElem` value slot | FieldEscape (`foreign-store`): materialize at the store |
| `Load/StoreElem` non-const idx | dynamic-index: materialize at the access |
| `RefEq`, `MonitorEnter/Exit` | REJECT `identity-observable` (Rule 73) |
| `Unwind` exception | REJECT `exception-observable` |
| live `FrameState` locals edge, fs-only | **fs-escape listing** (MSG-20260901-006): SCALARIZE + per-instant vobj |
| live `FrameState` locals edge + other escape | FOLLOWS the materialization (replaceNode rebases the locals edge onto the Materialize reference) |
| `Phi` value input | REJECT `phi-merge` |
| `CheckCast` / `InstanceOf` obj | REJECT `cast-observable` / `instanceof-observable` |
| anything else | REJECT `unknown-use` (conservative default) |

**SCALARIZED (key 67, NoEscape).** Readers forward BEFORE any writer is
removed (the chain is then still complete, so every reader resolves to
the store that defines its Java value — the nearest same-field/
same-index store on the mem chain, else the typed default constant for
never-written fields; the forwarded value must carry the load's declared
type, a hand-built-graph defense). Writers splice out of BOTH chains
(ctrl users onto the ctrl predecessor, mem users onto the mem
predecessor). `ArrayLength` forwards the `NewArray` input-1 length;
`IsNull(alloc)` folds to 0; the ChainLink users rewire onto the
allocation's ctrl predecessor; the allocation is killed (kill refuses
if any live referencer remains — reported as `partial-bail`, every
landed rewrite being individually sound).

**MATERIALIZED (keys 66/69, Arg/Field/ReturnEscape).** At the FIRST
escape use E (id order == chain order in one straight segment):
forward the pre-E readers, snapshot the vobj from E's pre-splice mem
chain (typed defaults for unread fields; array layout `[length,
elements...]`, unlisted slots materialize to their type default),
splice the pre-E writers, re-read E's (rewired) ctrl/mem predecessors,
insert `Materialize [ctrl, mem, vobj]`, rewire E's ctrl AND mem inputs
onto it (the object state is published to the memory chain before any
observer runs), splice the allocation out of the ctrl chain (so its
downstream never rewires onto the escape-point Materialize — cycle
prevention), then `replaceNode(alloc, Materialize)`: every remaining
live user — later stores/loads, later escape uses, FrameState locals
edges — reads the real reference (Rule 14 auto-updates the snapshots,
so deopt reconstruction sees a live object). Post-escape accesses are
KEPT as real-object semantics. Nested vobjs never occur in v1: a
stored inner allocation is a foreign-store escape for the inner
allocation, which is visited first (id order), so the outer snapshot's
field value is already a Materialize reference or a rejected raw New —
the verifier's still-virtual-field rule holds by construction.

**The straight-segment gate (the v1 scope).** All PINNED rewriting uses
must reach the allocation's ctrl input without crossing a
Region/LoopBegin and with EQUAL branch-projection sets (the
If/Switch projections collected on each path). Equal sets = one
straight segment = "chain order == program order", which is exactly
the condition under which the nearest-store lookup is the Java value;
a merge between a store and its reader (or uses in sibling branches)
rejects with `merge-crossed`. Floating readers (ArrayLength, IsNull) skip
the gate — their rewrites cannot observe chain order. Cost gates
(Rule 45): `kPeaMaxFields` (16) instance fields, `kPeaMaxArrayElems`
(16) constant indices, `kPeaMaxAllocsPerGraph` (64) allocations —
overruns REJECT (`too-many-fields`/`too-many-elems`), leaving the
graph untouched.

**The fs-escape listing (key 67, NoEscape + FrameState references).**
A no-escape allocation referenced by deopt snapshots still scalarizes —
readers forward, writers splice — but every referencing FrameState must
be able to materialize the object at deopt. The plan resolves ONE
observation instant per referencing fs and builds a `VirtualObjectState`
with the field state visible there (a backward chain lookup, NOT id
order: inlined-body stores carry higher ids than the call-site fss
that precede them in program order). The fs's slot edges rebase onto
the vobj (the deoptimizer's slot-to-object mapping) and the desc lists
it via `appendFrameStateVobj` (the deopt-materialization channel —
MSG-20260901-006). The instant rules:

- a LIVE fs (consumed by a guard/call/Deopt) resolves its own
  instant: the call's mem input, or the state at the guard's ctrl
  predecessor; every consumer must observe the same field state;
- IDENTITY: every fs of the allocation inside ONE live caller chain
  is reconstructed at the same instant — the same object in (possibly)
  several frames — so they share ONE vobj (union-find over the fss
  co-occurring in a chain), and every deopt point serving the cluster
  must observe the SAME field state (one vobj cannot carry two
  states; disagreement rejects `fs-multi-deopt`);
- a deopt-UNREACHABLE userless fs (the chain below it was folded
  away — the post-inline call-site snapshots after nullfold+DCE) is
  dead metadata: it shares one final-state vobj (never reconstructed,
  any deterministic sound state);
- a snapshot consumed by a non-deopt node rejects
  (`fs-unknown-consumer`); unresolvable states reject
  (`snapshot-merge`).

The plan phase is pure (a refusal leaves the graph untouched); the
apply creates the vobjs in cluster order (min fs id, dead-metadata
last — Rule 124), rebases the edges, and appends the descs before the
reader/writer splicing (the chain lookups need the writers in place).
Default constants are cached per plan (constants are not interned;
node-stable defaults keep the agreement check exact and the graph
minimal). The fields.rbc post-inline e2e scalarizes to ZERO
allocations: the chained call-site snapshots share one final-state
vobj, main's live println snapshot lists its own, and every field
access forwards away.

**Determinism / idempotency.** Allocations in id order, fields in
FieldId order, escape uses by id, node creation in visit order
(Rule 124); the hash-free engine is a fixed sequence of table lookups.
A second run finds no live candidate (scalarized allocations are gone,
materialized ones are Materialize nodes, rejected ones re-derive the
same rejection without rewriting) — zero rewrites, byte-identical.

**Kill switches.** 65 off: the whole engine is a no-op. 67 off:
no-escape allocations REJECT `scalarize-disabled`. 66 or 69 off:
escape-grade allocations REJECT `materialize-disabled`. Key 65 in
isolation (runSinglePass) is the analysis-only mode: decisions carry
the theoretical disposition, the graph and the telemetry counters
stay untouched.

**Tool surface.** `b2graph --pea` prints the per-allocation decision
log (grade, disposition, refusal reason, field/load/store counts, the
materialization node); `-O` runs the engine inside the pipeline with
the telemetry line extended to `pea=<scalarized>/<materialized>/<
rejected>`.

**Testing (Rule 35).** 49 tests in `tests/passes/EscapeTests.cpp`:
the classification table (12), the materialization shape and chain
wiring (8, including the fs-edge auto-rebase and the nested inner-first
end state), the scalar replacement (10, including typed defaults,
chain splicing, branch-local lifetimes), the fs-escape listing (9:
per-instant values, instant-disagreement refusal, the chained
identity-sharing vobj, the array snapshot, unknown-consumer refusal,
the kill switch, analysis-only, idempotency+determinism, and the
elems cost-gate companion), the cost gates (2: the instance-field
shape restored after the MSG-20260901-007 fix + the array shape),
kill switches (4), idempotency/determinism/no-op (4), and the
integration gates (the 19-program corpus through the full pipeline —
verified, idempotent, byte-stable; the fields.rbc e2e pair: call-arg
materialization without inlining, and the post-inline ZERO-allocation
fs-escape scalarization).

**Known v1 conservatisms (the growth paths).** (a) ~~`fs-deopt-ref`~~
RESOLVED by the fs-escape listing above (MSG-20260901-006's
appendFrameStateVobj landed and is consumed). (b) `phi-merge` /
`merge-crossed`: field state SSA (phi-of-stores) arrives with the
loop/merge growth path. (c) dynamic array indices materialize rather
than speculate. (d) CheckCast/InstanceOf reject rather than fold.
(e) Cross-inline-site EscapeSummary reuse (special_passes.md 1.2)
arrives with the multi-caller path; v1 analyzes the merged post-inline
directly, which is the same information. (f) ~~The instance-field
cost-gate test uses the array shape~~ RESOLVED: the ir verifier's
memory-chain DFS step belt now admits well-formed long chains
(MSG-20260901-007 fixed); the instance-field shape is the primary
cost gate again. (g) An fs serving two deopt points that observe
different field states rejects (`fs-multi-deopt`); per-deopt-point
caller-frame copies (the Graal shape) would lift it — that is an
inlining-engine IR change, not a PEA one.

---

## 13. SCCP (key 38 — one engine for charter rows 37/38/39)

Implementation: `compiler/passes/src/SCCP.cpp` (`detail::runSCCP`);
telemetry `sccpConstants`; tool surface `b2graph -O` (the `sccp=`
counter). The value-level fold semantics are NOT duplicated here: SCCP
calls the same `detail::evalBinOp` / `detail::evalUnaryOp` / `constOf`
evaluators constfold (14) uses (lifted to `PassInternal.h` linkage in
`Simplify.cpp`) — one Java-semantics table for both passes (Rule 72),
which is why an SCCP claim is exactly as trustworthy as a constfold
replacement: same wrap arithmetic, same JVM div/rem special cases, same
JLS 5.1.3 narrowing, same IEEE exact-bits NaN policy (NaN-free FP
arithmetic folds only; the FP comparisons fold on NaN because their
result is a defined int).

### 13.1 The lattice

Per live node, values only DESCEND (monotone, Rule 10):

- **Top** — unresolved (optimistic: the propagation is still running).
- **Const(c)** — provably `c` on every executable path.
- **Bot** — not a compile-time constant (variable input, opaque op, or
  a fold refusal such as division by a constant zero: the runtime value
  does not exist).

`meet(Top, x) = x; meet(c, c) = c; meet(c1, c2) = Bot; Bot absorbs.`
Seeding: ConstantI/L/F/D/Null resolve to themselves; parameters,
ConstantSym, Undef, vector/tag/box/extension kinds, and every
control-pinned or memory-producing node (loads, calls, allocations,
CheckCast, Start's memory state) seed **Bot**; the evaluator's kinds
plus Phi seed **Top** and are eagerly queued.

### 13.2 The executable-edge rule (the "conditional")

Control propagates from Start through Ctrl/Parent-slot users — the
exact successor rule the unreachable sweep uses. An **If** whose
condition lattice is Const marks only the taken projection executable
(Bot marks both; Top waits for the condition and re-fires when it
lowers). An always-failing **Guard** (Const cond == 0) stops flow —
exactly the Deopt it becomes under branchnorm; a passing-or-unknown
guard flows onward. **Region/LoopBegin** are executable when any input
is: the backedge only fires once the body does (natural causality — no
forward/backedge special case; the builder appends the LoopEnd anchor
after the forward inputs, and SCCP reads executability, not slot
order). **Switch** marks ALL projections: case ordinals are
frontend-opaque payloads (the documented switch-folding blocker), so a
constant selector is over-approximated — conservatism (b) below.
**CallExcept** rides the call's Parent edge like every other projection
(an exception may be thrown).

**Phi meets only over executable region inputs.** A value flowing
solely on a never-executed edge does not exist; a not-yet-executable
backedge contributes Top (ignored) — this optimism is what resolves
loop-invariant phis: `phi = meet(0, phi)` stabilizes at 0, while
`phi = meet(0, phi+1)` correctly descends to Bot when the backedge
value refutes the optimism. Memory-state phis can never claim a
constant (no flavor types as Bottom and no memory producer is Const),
pinned by test.

### 13.3 The completion rule (the soundness bolt)

At the propagation fixpoint, a **Top** value with at least one LIVE
user is not "unreachable code" (the classic CFG reading — floating
values have no block): it is UNRESOLVED, and an unresolved value
feeding a live use is treated as overdefined — forced to Bot and
re-propagated until stable (iterated, monotone, bounded by the node
count). This is what keeps the optimism honest: a phi whose executable
edge carries a never-resolving value (a hand-buildable shape; builder
output resolves everything) collapses to Bot instead of claiming the
meet of its resolved edges alone. Userless Top values (dead-code
leftovers) stay Top — nobody observes them.

### 13.4 The rewrite surface (deliberately minimal)

APPLY (id order, Rule 124): every live node whose lattice value is
Const and whose kind is in the evaluator's floating set or Phi is
replaced by an **interned** constant node (one node per distinct value
per run, created on first need — byte-stable prints; GVN merges the
interned nodes with pre-existing identical constants later in the same
round). Replacements go through `detail::replace`, so FrameState
snapshots auto-update (Rule 14: deopt reconstruction sees exactly the
constant the phi was on every executable path) and tombstone edges are
junked per the tombstone law. A cycle-safe type belt (`typeOfNoCycle`:
the phi self-marker's type is the phi's own join, and `join(x, Bottom)
= Bottom`, so on-path inputs are EXCLUDED rather than contributed)
refuses any replacement whose flavor type disagrees with the node's
type — unreachable in verified graphs, cheap insurance in hand-built
ones. **Control is never rewritten**: guards and branches stay
branchnorm's business — SCCP only makes their conditions literal, and
the same round's branchnorm/cfs/dce consume them (test-pinned: a
decided-by-propagation branch folds within one pipeline round).

The value facts mirror constfold's `foldValueFacts` through the
lattice: `IsNull(null-phi) = 1`, `IsNull(NeverNull node) = 0` (the
allocation's own lattice value is Bot — the fact is read at NODE
level), `RefEq(x, x) = 1`, `RefEq(null, null) = 1`,
`RefEq(null, never-null) = 0`, `InstanceOf(null) = 0`.

### 13.5 Budget

The `Budget` is charged per worklist pop AND per replacement. A
propagation or completion overrun aborts BEFORE any rewrite — claims
from a partial fixpoint are not sound, so none are applied (the graph
is untouched; telemetry reports the overrun). An apply overrun stops
mid-apply at a valid point: every replacement is individually sound at
the fixpoint, so a prefix is too.

### 13.6 Testing inventory (Rule 35)

`tests/passes/SCCPTests.cpp`, 21 tests: the meet rule (same value from
different expressions folds, three rewrites; distinct constants
refuse), the arithmetic cascade through a meet, the executable-edge
rule (a decided-by-lattice branch prunes the dead arm; a parameter
condition keeps both edges), same-round pipeline consumption (the
branch folds and sweeps in ONE round), the loop family (self-marker
resolves; `phi+0` invariant resolves INCLUDING the AddI; a varying
counter refuses; a zero-trip loop resolves to its init value), the
null lattice (null phi meets to ConstantNull and IsNull folds to 1),
the NeverNull fact, the guard-condition contract (condition folds,
guard untouched), the completion rule (an unresolved-but-used value
collapses the meet — no claim), the parameter seed (never constant),
idempotency (zero-telemetry second run, byte-identical print),
determinism (double build), the kill switch (byte-identical no-op),
the pipeline counter, memory-phi refusal, and the Rule 14 FrameState
auto-update. Plus the corpus sweep (all 19 programs through the
SCCP-bearing pipeline, verified + deterministic + idempotent) in
`PassTests.cpp`, and the `--pgo -O loop_call_demo.rbc` tool demo
(sccp=6 on the post-inline graph — the guard-inline path feeds SCCP
phi-meet constants).

### 13.7 Conservatisms (deliberate, each with its lifter)

(a) Parameters are never constants — T3 static mode has no
argument-constant facts; the PGO ArgumentConstant speculation
(SpecMeta kind) is the lifter. (b) Switch marks all projections — the
case-ordinal mapping is frontend-side; the lifter is exposing case
values in the IR payload. (c) Guard conditions evaluate only through
the value lattice — no path-refinement along taken branches (the
classic sparse-predicate extension); the lifter is a dedicated
predicate-propagation pass, not SCCP growth. (d) Loads, calls, and
allocations are Bot regardless of context (a LoadStatic after
ClassInit of a constant static is constant in principle) — the lifter
is the static-field value table. (e) RefEq of two distinct non-null
references is Bot (identity is runtime state).
