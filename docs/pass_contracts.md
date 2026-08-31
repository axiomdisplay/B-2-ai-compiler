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
ControlFlow.cpp, DCE.cpp, GVN.cpp), API `include/b2/passes/Passes.h`,
tests `tests/passes/PassTests.cpp` (Rule 35: >= 10 tests per delivered
pass key), tool surface `b2graph -O`.

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

Not delivered in v1: **17 (redundant cast removal)** — needs type proofs
(CHA/hierarchy); `runSinglePass` refuses the key with a diagnostic. Items
11-20 minus 17 and item 35 are the early-cleanup core; items 12-16 share
one sweep engine (`runSimplify`) with a class mask so each key is
independently kill-switchable while the pipeline runs them as one pass
for cost.

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
  nullfold (18)
  branchnorm (19)          [includes the unreachable sweep + repair]
  cfs (20)
  dce (11)
  gvn (35)
  dce (11)                 [post-GVN orphans]
stop when the round changed nothing (converged=true), or at the round
budget while still changing (converged=false + telemetry diag: the graph
is valid, the fixpoint is merely unconfirmed).
```

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

`tests/passes/PassTests.cpp`: 126 tests over the ten delivered keys —
ten or more per key, each pinning: the rewrite catalogs above (positive
and negative: FP NaN non-folds, conservative exclusions, switch
refusal), tombstone-law legality (verifier after every pass), kill
switches (byte-identical no-op graphs), idempotency (zero-telemetry
second runs), determinism (double-build byte identity), and the Rule 14
FrameState auto-update. The strongest gate is the corpus sweep: all 19
interp-corpus programs, every method, through the full pipeline —
verified, deterministic, idempotent — plus `b2graph -O` as the human
surface (telemetry line per method).

## 11. Deferred (with the blocking reason)

| Item | Blocker |
|---|---|
| 17 redundant cast removal | needs type proofs (CHA / hierarchy) |
| Switch folding on constants | case labels are frontend-side (opaque payload) |
| Load CSE beyond identical keys | loads chain control (PRE/scheduling territory) |
| Div/rem DCE | guard-adjacency proof |
| Tombstone/dead-fork reclamation | IR team: sanctioned removal API or verifier dead-node skip (MSG-009) |
| CmpL-based long zero-guards | IR core classifies CmpL as Long-producing (MSG-009); the L2I composition is sound (over-deopts only) |
| Inlining registry rows (suite 21-34 speculation family) | profile import (icdg.md Phase 1) + Rule 1's tier-filter mechanism; the direct-inline driver itself is delivered (docs/inlining.md) |
| Inline verification of caller-chain fs liveness | IR team: the verifier checks chains acyclic but not that chain targets' nodes are alive (the DCE-side protection is the interim soundness fix; MSG-20260901-002) |
