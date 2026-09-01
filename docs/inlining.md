# B-2 Inlining v2 — Direct-Inline + GuardInline (the ICDG Engine)

```text
Normative reference: docs/laws.md
(especially Rules 3-5, 10, 14, 16, 23, 26, 35, 40, 42-45, 72, 75,
121, 124, 126, 132, 144; Amendments A and B),
docs/icdg.md (the decision engine design: this file is its Phase 2
"T2 simple inlining" on the static-proof path),
docs/ir_spec.md (v2; FrameState caller chains, section 5.1),
docs/graph_builder.md (the builder whose inline seam this engine drives),
docs/pass_contracts.md (the tombstone law the kills comply with),
docs/interp_contract.md (section 8.1: the T0 dispatch profile the
profiled path consumes, v1.2.0).
If this document conflicts with docs/laws.md, laws.md wins.
```

Team: Passes Team (primary owner of ICDG and of this implementation).
Implementation: `compiler/passes/src/Inline.cpp` (the driver),
`compiler/passes/src/GraphBuilder.cpp` (the inline-build seam),
API `include/b2/passes/Inline.h`, tests `tests/passes/InlineTests.cpp`
(49 tests, Rule 35), tool surface `b2graph --inline` / `b2graph --pgo`.

This document is the **rule-level review artifact** for inlining: what
is inlined, the exact soundness argument for every transformation, the
id-space contract, the deopt/exception conventions for inlined frames
(the cross-team contract), the cost model and budgets, and the refusal
catalog. The executable decision log is `InlineResult::decisions`
(icdg.md 19: every decision is explainable).

**v2** (ICDG Phase 2) adds section 9: profile-driven devirtualization
(GuardInline) over the T0 dispatch profile. Everything else is v1 as
reviewed; the zero-regression property (section 9.4) pins that a
profile-less run is byte-identical to v1.

---

## 1. What v2 delivers (and deliberately does not)

v1 implements icdg.md Phase 2's "T2 simple inlining" on the **static
proof path** (icdg.md 6: `StaticProof` - "direct inline without guard if
proof valid"):

- **Direct calls only**: `CallStatic` nodes - the lowering of
  `invokestatic`, `invokespecial`, and their quickened forms
  (docs/graph_builder.md; all four lower to `CallStatic` because the
  target is a single resolvable body). The target resolves through the
  `InlineCalleeSource` (section 3) with NO speculation: no guard, no
  SpecMeta, no invalidation dependency is attached for the choice of
  target itself (a `MethodBody` dependency is future runtime work; the
  IR side records nothing speculative, so Rules 42/122 are trivially
  satisfied).
- **Virtual/interface on the static-proof path**: not attempted without
  a profile - v2's GuardInline (section 9) is the profiled path, and
  class-hierarchy analysis arrives with the CHA follow-ups (Rule 1:
  tier filters then, not now - the suite stays one list for T2/T3).
- **A do-not-inline engine first** (icdg.md 12): every refusal is a
  `KeepIndirect` decision with a structured reason (section 7); the
  engine says "no" more often than "yes" by construction.

v2 (ICDG Phase 2, section 9) adds the **profiled path**:

- **GuardInline**: a monomorphic `CallVirtual`/`CallInterface` site
  whose T0 dispatch profile (interp_contract.md section 8.1) names
  exactly one receiver class - the target agreeing with the call
  resolution and the Rule 44 confidence thresholds met - inlines behind
  a `TypeProfile` guard with complete Rule 122 SpecMeta and a
  `ClassHierarchy` invalidation dependency (Rule 42). Bimorphic /
  polymorphic / megamorphic sites refuse (the two-target guard is the
  documented follow-up).
- **Without a profile attached** (`InlineConfig::profile == nullptr`)
  virtual/interface sites are not even considered: v1 behavior,
  byte-identical graphs, telemetry, and decision log (section 9.4).

Charter coverage (docs/teams/passes-team.md appendix): the engine
delivers the machinery of **24** (monomorphic/direct call
specialization - the static-proof case) and now **25** (CHA/PGO
devirtualization - the monomorphic profiled case), **29** (recursive
inlining control), **30** (caller/callee graph budgeting), and **31**
(inlining cost modeling) as one driver. Items 22-23, 26-28, 32-34
(bimorphic/interface/indy/MethodHandle specialization beyond the
monomorphic profiled case, partial inlining, guard-based and
exception-edge-aware inlining beyond the v2 policy) are the
profile-driven follow-ups; the registry-row
integration for the speculation family arrives with them (Rule 1).

**Pipeline position**: inlining is a driver stage between the build and
the pass suite (like the builder itself; the charter's "Frontend / graph
building" items 1-10 are driver stages, not registry passes). The
canonical T2 shape: `buildGraph -> ir::verify -> runInlining ->
ir::verify -> runEarlyCleanup -> ir::verify` (`b2graph --inline -O`).

## 2. The transformation (one call site)

For each inlined `CallStatic` `C` with callee body `M`:

1. **Trial build** (per-callee, cached): `M` builds standalone through
   the ordinary `buildGraph` into a scratch graph. The trial proves
   buildability (the builder's refusal set - synchronized,
   irreducible flow, unhandled opcodes - is a property of the callee
   alone), counts the reachable return terminals (zero => refuse,
   section 7), and measures the node cost. The real inline build is
   the same lowering with different entry/exit wiring, so a successful
   trial plus the site consistency checks (step 2) make the real build
   deterministic-successful; if it nonetheless fails, the run stops
   **fail-closed** (the graph is never left silently wrong).
2. **Site consistency** (defensive re-checks of the verified-RBC
   guarantees): the argument window covers the callee's parameters
   (receiver first), and the callee's declared return type equals the
   call's result type. A mismatch refuses (mis-spaced id resolution,
   non-builder shapes).
3. **The inline build** (`detail::buildInlineBody`): the RBC->IR
   builder re-runs over `M` **inside the caller's graph**, wired to
   the call site - the entry control is `C`'s control predecessor,
   the entry memory is `C`'s memory predecessor, and the parameter
   locals are `C`'s argument defs (no Start, no Parameter nodes).
   Return instructions become recorded exits instead of `Return`
   terminals; the callee's FrameStates chain to the call-site snapshot
   (section 4); deopt ids continue the caller graph's allocation.
   The static-call site's `ClassInit` trigger (JVMS 5.5) stays in the
   memory chain before the entry, so the body's static reads observe
   post-initialization memory exactly as the call would.
4. **Exit merge**: one exit wires directly (the exit's control/memory/
   value become `C`'s replacements); two or more exits build a
   `Region` over the exit controls, a memory `Phi` (every input is a
   memory-state producer by construction - the builder only chains
   producers into the memory state), and for non-void callees a value
   `Phi`.
5. **Role-based rewiring**: a call produces control AND memory AND a
   value; one node cannot replace all three, so every LIVE user of
   `C` is rewired by its slot's role (Ctrl -> exit control, Mem ->
   exit memory, Data -> exit value) through `setInput`.
6. **The kill** (tombstone law, docs/pass_contracts.md section 3): the
   site's old exceptional continuation (the `Deopt [CallExcept, fs]`
   or `Unwind [CallExcept, CallExcept]` the builder created) dies
   first, then the `CallExcept` projection (its only live user just
   died), then `C` itself (all live users were rewired). Each kill is
   `detail::kill`: tombstone referencers are junk-rewired onto
   verifier-legal sinks, so every intermediate state stays
   verifier-clean.
7. **Verify after every site** (Rule 40): `ir::verify` runs after each
   successful inline; a failure stops the run fail-closed with
   diagnostics, leaving the graph in the post-last-good-site state.

Determinism (Rule 124): sites are considered in node-id order; a
successful inline's own body sites are processed immediately after it
(depth + 1) in id order; trials are cached per callee body; node
creation order is the builder's. Idempotency: a second run over the
post-inline graph inlines nothing and changes nothing (pinned by
tests); the remaining (refused) sites re-refuse identically.

## 3. The id-space contract (the CalleeSource)

The IR stores opaque `MethodId`s (Rule 16); call node payloads carry
ids from the resolver space the graph was built with. The
`InlineCalleeSource` is the single authority for payload -> body, and
**doubles as the `SymbolResolver`** the caller graph should be built
with:

- `ProgramCalleeSource` (the v0 one-class world) unifies the spaces BY
  CONSTRUCTION: a program method's id is its **table index** (matching
  the tool FrameState convention and the quickened payload pin,
  rbc_spec.md SS6.2 - `invokestatic_quick` carries the table index),
  external methods intern **above** the table. Un-quickened
  `MethodRef` payloads, quickened payloads, and the FrameState method
  ids therefore agree, and there is no id-space ambiguity to guess
  about.
- CONTRACT: the graph must have been built with THIS object as its
  resolver. The driver's per-site consistency checks (argument window,
  return type) additionally refuse mis-spaced resolutions that happen
  to be shape-inconsistent.
- A payload at or above the table size is an external method (e.g.
  `java/io/PrintStream.println`): unresolved -> `KeepIndirect`.

The unified runtime id space (real global method ids, IC site ids)
arrives with the T2 runtime driver; the source interface is the seam
it will implement.

## 4. FrameState chains and the exception-deopt conventions (soundness core)

**FrameState caller chains (Rule 75).** Every FrameState the inline
build creates carries `FrameStateDesc.caller` = the call-site
snapshot's descriptor id. The deoptimizer therefore reconstructs the
inlined frame stack: the innermost (callee) frame materializes from
the callee snapshot, runs to completion in T0, and the deopt runtime
stitches the caller chain (interp_contract.md section 3; the runtime
side is the documented future work, the IR-side contract is complete).
The verifier pins the chains acyclic (ir_spec 7).

**Inlined-frame DCE protection.** A caller chain is a SIDE-TABLE
(desc) reference, invisible to use-def edges. After the call dies, the
call-site FrameState may have zero live edge users - but its input
edges ARE the caller-frame slot values the deoptimizer needs. DCE
therefore refuses to remove any FrameState node whose descriptor is
the `caller` target of another descriptor (the `isCallerChained`
guard, `DCE.cpp`; pass_contracts.md section 4). Without this, DCE
could destroy caller-frame reconstruction data while the graph stays
verifier-clean - the hazard is documented here because it is the one
place inlining crosses into the pass suite's soundness table.

**Exception escapes (the policy, mirroring the builder's v1 policy).**
An exception ESCAPING the callee routes through the call site's own
continuation policy:

- **Uncovered site**: the callee's escape stays an `Unwind [ctrl,
  value]` - the exception value (the `athrow` operand, or the nested
  call's `CallExcept` value) propagates out of the caller directly.
  This replaces the site's old `Unwind [CallExcept, CallExcept]`
  one-for-one: same shape, same observable behavior.
- **Covered site**: the escape becomes a Deopt whose FrameState is
  the CALLEE's at the escape point, chained to the call site:
  - an escaping `athrow` deopts as a NORMAL deopt at the `athrow` pc -
    T0 re-executes the athrow (idempotent: the exception rides in the
    FrameState's operand slot, graph_builder.md section 5), THE
    EXCEPTION ALGORITHM finds no handler in the callee, unwinds the
    reconstructed callee frame, and the deopt runtime re-enters the
    caller at the call pc with the pending exception - the caller's
    handler runs. Every effect the inlined body performed before the
    throw stays applied (the deopt does NOT re-execute them; the
    frame is restored AT the athrow).
  - an escaping NESTED CALL exception (the callee called `C'`, `C'`
    threw, no matching handler in the callee) deopts as the class-2.3
    exception deopt `Deopt [CallExcept', callee-fs]` - the existing
    covered-call convention, now inside the inlined body: the
    pendingException is `CallExcept'`'s value, the algorithm runs at
    the callee's call-to-`C'` pc, finds no handler, unwinds the
    callee frame, and the runtime re-enters the caller at the call pc.

Both covered-site shapes REUSE existing IR conventions exactly - no
new node kinds, no new transport contracts. The alternative "deopt to
the pre-call state and re-execute the call" is **unsound** (the
callee's partial effects would re-apply: `x = f.x; f.x = x+1; throw`
would double-apply) and is rejected by design. In-callee catches are
untouched: the callee's own covered `athrow`/call deopts keep the
builder's shapes (the callee's handlers run in T0 exactly as without
inlining).

## 5. Cost model, budgets, telemetry (Rules 23, 26, 45)

The v1 cost model is refusal caps, all named constants
(`include/b2/passes/Inline.h`), plus the measured node cost from the
trial:

| Constant | Default | Meaning |
|---|---|---|
| `kMaxInlineCalleeInsns` | 35 | RBC instruction cap (cheap pre-trial) |
| `kMaxInlineCalleeSlots` | 24 | frame width cap (FrameState cost) |
| `kMaxInlineCalleeNodes` | 300 | trial node count cap |
| `kMaxInlineDepth` | 3 | nesting depth cap |
| `kMaxInlineSitesPerGraph` | 64 | site budget |
| `kMaxInlineNodesPerGraph` | 4096 | graph growth budget |

v2 adds the GuardInline confidence thresholds (section 9.2):

| Constant | Default | Meaning |
|---|---|---|
| `kGuardInlineMinObservations` | 3 | site total below which the profile says nothing |
| `kGuardInlineMinRatioBP` | 9000 | the monomorphic entry's minimum share, basis points |

Recursion control (icdg.md 12): the callee on the current inline
stack refuses (recursive cycle; one expansion of a self-recursive
site is sound - the body's recursive call stays a call). Depth beyond
the cap refuses. Graph-level budget stops set `budgetStops` and
`converged = false` (Rule 26: never silent). The unlock-value score
terms of icdg.md 13 (PEA/SWLP/effect-reorder/representation bonuses)
are all zero in v1 - those passes do not exist yet; the score model
grows with them (icdg.md Phase 3). The kill switch (Rules 132/144)
is `InlineConfig::enabled = false`: a successful no-op, byte-identical
graph (pinned by test).

Telemetry reports sites considered/inlined/refused, nodes added,
deopts emitted, exit merges, removals, max depth, budget stops, and
`converged`. Every decision - inline or refuse - is a logged
`InlineDecision` (call node, target, depth, action, callee cost
numbers, reason) in processing order: this is the icdg.md 19
explanation engine, and `b2graph --inline` prints it per method.

## 6. Junk sinks are not call sites

The kills create tombstone-law junk anchors (including a junk
`CallStatic [Start, Start, fsSink]` for projection-parent slots). The
driver skips its own run's sinks (`detail::isJunkSink`) during site
discovery; foreign sinks from earlier pass runs are refused by the
shape check (a junk call has no `CallExcept` child) - noise in the
decision log at worst, never a wrong inline.

## 7. The refusal catalog (v2)

| Reason | Trigger |
|---|---|
| `target unresolved` | external method, or id at/above the table |
| `callee flags refuse` | synchronized (monitor record / interp_contract 1), abstract, native |
| `callee too large (instruction cap)` | insns > kMaxInlineCalleeInsns |
| `callee frame too wide` | slots > kMaxInlineCalleeSlots |
| `callee has no normal return` | zero reachable return terminals (the zero-exit merge - post-call control that never runs - is the documented follow-up) |
| `callee does not build` | trial-build diagnostics (irreducible flow, unhandled opcodes, ...) |
| `callee node cost over cap` | trial nodes > kMaxInlineCalleeNodes |
| `argument window ...` | args < callee params (+receiver) |
| `return type disagrees` | callee descriptor return != call result type |
| `recursive cycle` | callee already on the inline stack |
| `inline depth cap` | depth > kMaxInlineDepth |
| `graph site budget` / `graph node budget` | graph-level caps (budget stops) |
| `call-site shape is not builder-canonical` | missing/odd exceptional continuation, non-FrameState trailing input, void call with value users |
| `no dispatch profile row for the site` | virtual/interface site the training run never dispatched (section 9) |
| `profile row kind disagrees with the call site` | the snapshot is not this program's (Virtual row for a CallInterface site, ...) |
| `profile has no receiver entry` | count-only row (builtin executions; no body to inline) |
| `megamorphic dispatch site` | sticky frozen histogram (4+ receiver classes) |
| `bimorphic dispatch site` | two receiver classes (the two-target guard is the follow-up) |
| `polymorphic dispatch site` | three receiver classes |
| `site total below the observation floor` | count < kGuardInlineMinObservations (Rule 44) |
| `profile confidence below the guard threshold` | entry share < kGuardInlineMinRatioBP (Rule 44) |
| `profile target disagrees with the call-site resolution` | the entry names a different target than the call resolved to |
| `profiled receiver class is java/lang/Object` | InstanceOf is not an exact check for Object (section 9.3) |

## 8. Testing (Rule 35) and the growth path

`tests/passes/InlineTests.cpp` - 49 tests: the exit-merge shapes
(direct wiring, Region+phis, memory threading through stores,
loops), argument flow (constants fold through the post-inline
pipeline), FrameState chains and their DCE protection, deopt-id
uniqueness, the full exception matrix (uncovered/covered escapes,
in-callee catches, nested-call escapes), the refusal catalog,
budgets, the kill switch, determinism, idempotency, the quickened
payload resolution, and the corpus sweep: all 19 interp-corpus
programs through build -> inline -> verify -> pipeline -> verify with
double-run determinism and re-run idempotency. v2 adds 20 GuardInline
tests (section 9.5) and extends the corpus sweep with a
profile-attached variant (byte-identical graphs, deterministic). The
suite's binary links `b2::interp` for the end-to-end T0 test - a test
harness may integrate tiers like the tools do; the `b2_passes` library
never does. ASan/UBSan clean over the suite and the `b2graph
--pgo --inline -O` corpus sweep; the Law-36 differential sweep (T0/T1)
is untouched (no interp/baseline code changed).

Growth path (icdg.md Phase 2 -> 3): the GuardInline surface grows the
two-target guard for bimorphic sites (charter item 26), CHA-backed
subclass proofs once class-hierarchy analysis exists (the
ClassHierarchy dependency already carries the invalidation shape),
unlock-value scoring that prioritizes sites enabling CM-PEA, effect
reordering, SWLP, and representation elimination (the moment those
passes exist to consume it); partial inlining and
exception-edge-aware inlining (in-graph handlers) follow the codegen
team's compiled-handler work. Known orthogonal gap: the IR verifier's
memory-chain walk false-positives on any memory producer inside a loop
(MSG-20260901-004) - GuardInline test programs are deliberately
straight-line until that lands; a call inside a loop is the most
common real-world inline shape and unblocks with the verifier fix.

---

## 9. GuardInline (v2: the ICDG Phase 2 profiled path)

The dispatch profile (Phase 1, interp_contract.md section 8.1) is the
raw per-site receiver-class/target histogram keyed `(caller MethodId,
call pc)`. This section is the contract for consuming it: the snapshot,
the classification, the guard construction, and the soundness
argument.

### 9.1 The snapshot and the site key

`passes::DispatchProfile` (`include/b2/passes/Inline.h`) is the raw T0
shape verbatim - one representation change: receiver CLASS IDS become
internal NAMES, because the guard's `ir::TypeId` must resolve through
the CalleeSource's resolver (the graph's id space), and the NAME is the
only value valid in both the runtime ClassId space and the resolver
space. The snapshot is a compilation input (strings legal; Rule 16
scopes to the graph), attached read-only via `InlineConfig::profile`;
it must outlive the run. The conversion is
`compiler/passes/tools/DispatchProfileSnapshot.h` (shared by `b2graph
--pgo` and the tests; it includes the interp headers, so only
interp-linking integrators may use it - the passes LIBRARY stays
interp-free, the charter boundary).

The site key is recovered from the call node itself: the trailing
FrameState input's descriptor (`FrameStateDesc.method/.pc` - the
builder pins the fs at the call pc; quickened forms share the key by
the imm == pc pin). The row lookup, the kind check (Virtual row for a
`CallVirtual`, Interface for a `CallInterface`), and the
target-agreement check (the entry's target == the call's payload, both
program-table indices in the unified space) all refuse on mismatch: a
snapshot that does not describe this build only ever refuses, never
mis-inlines.

### 9.2 The classification (dispatch states + stability)

The engine classifies (T0 stays data-only - the charter's
"the team produces the data, not the policy"):

| Profile shape | Decision |
|---|---|
| megamorphic (sticky, 4+ classes) | refuse `megamorphic dispatch site` |
| 3 entries | refuse `polymorphic dispatch site` |
| 2 entries | refuse `bimorphic dispatch site` (two-target guard: follow-up) |
| 0 entries (count-only row) | refuse `profile has no receiver entry` |
| 1 entry, count < 3 | refuse `site total below the observation floor` (Rule 44) |
| 1 entry, share < 9000bp | refuse `profile confidence below the guard threshold` (Rule 44) |
| 1 entry, recvClass = java/lang/Object | refuse `profiled receiver class is java/lang/Object` (9.3) |
| 1 entry, target != call payload | refuse `profile target disagrees` |
| 1 entry, otherwise | **GuardInline** |

Confidence is 64-bit saturating-safe integer math
(`entry.count * 10000 / site.count`); counts saturate at 2^32-1 in T0
(Rule 114) and the ratio degrades conservatively. The decision log
carries the numbers (`InlineDecision.siteCount/recvCount`; `b2graph
--pgo` prints `profile=recv/site`).

### 9.3 The transformation and its soundness core

For each monomorphic site, the guard chain (all nodes created in the
caller's graph, ids in creation order - Rule 124):

```text
nullGuard (builder output, already before the call)
  -> Guard [nullGuard, InstanceOf(recv, T), fs]   // TypeProfile, deopt id
     -> <the callee body: entry control = the guard>  // the v1 machinery
```

- **The condition** is `InstanceOf [recv] payload = T`, T resolved
  through THIS run's resolver from the profiled class NAME. InstanceOf
  is a subtype check; in v0's flat hierarchy (every chain is
  [self, Object]; arrays match exactly) that is exact for every class
  except **java/lang/Object** - the one class whose check admits every
  non-array receiver, including PrintStream receivers that dispatch
  the site to the builtin family. Object-profiled sites refuse. In the
  future hierarchy world the subtype check is sound WITH the
  dependency: a subclass that changes dispatch fires the
  ClassHierarchy invalidation (the C2 shape), not a wrong execution.
- **The deopt** lands at the call pc with the call-site FrameState:
  T0 re-enters AT THE CALL and re-executes the invoke, dispatching
  exactly as without inlining. This is sound BECAUSE the guard runs
  BEFORE any body effect - the section 4 rejection of re-execution
  applies to MID-BODY escapes (partial effects would re-apply), never
  to a pre-body guard.
- **Rule 122 (complete) + Rule 42** ride on the guard: SpecMeta
  (TypeMonomorphic / PGO / confidence snapshot / guard node / deopt
  target / measured trial cost / rollback None) plus a
  `ClassHierarchy` dependency keyed by T. The verifier's Rule 122
  completeness checks pass by construction (PGO source => dependency
  valid; non-Static => guard set).
- **The body** is the v1 machinery unchanged: same trial, same
  argument-window and return-type checks, same exception policy, same
  exit merge, same tombstone kill sequence, verify after every site.
  The entry control is the guard (chained after the null guard); the
  entry memory is the call's memory predecessor (the type guard
  touches no memory); the body's deopt ids seed above the guard's.

### 9.4 The zero-regression property

`InlineConfig::profile == nullptr` (the default) keeps virtual and
interface sites OUT of the site discovery entirely: identical graphs,
identical telemetry, identical decision log, byte-identical serialized
output vs v1. With a profile attached, every virtual site gains a
decision (inline or refused) but a site that refuses changes NOTHING in
the graph - pinned by the corpus sweep's profile variant (all 19
programs, empty snapshot: graphs byte-identical to the profile-less
run). The kill switch (Rules 132/144) is a no-op with a profile
attached, byte-identical to v1 output.

### 9.5 The tool surface

`b2graph --pgo file.rbc` (implies `--inline`): runs the program in T0
first (b2run-style entry inference: `main` `()V`, then the String[]
form, then any zero-parameter `main`), snapshots the dispatch profile
through the shared converter, and feeds it to the engine per method
graph. Returned AND Threw both keep the accumulated profile (a
partially-run program still profiled every site it dispatched); no
runnable main keeps an empty profile (every virtual site refuses
"no row" - no data, no speculation). The training run's program output
is not part of the tool's output surface - the decision log is. The
decision lines carry the profile numbers:

```text
# inline n26 -> m2 d2: GUARD-INLINE (guard inline: monomorphic type
  profile (guard + SpecMeta + class-hierarchy dependency); insns=6
  slots=7 nodes=13 profile=3/3)
```

### 9.6 Tests (Rule 35)

Twenty new tests over the engine's profiled surface: the
zero-regression property (no profile), the core transformation (call
dies, guard + body live, decision carries profile numbers), Rule 122
completeness (SpecMeta + ClassHierarchy dependency + Speculative
flag), the wiring shapes (guard chains after the null guard; fs
descriptor (method, pc); InstanceOf reads the receiver def; TypeId
resolves through the resolver), deopt-id uniqueness, idempotency +
determinism, the full classification refusal catalog (mega / bi /
poly / thin / low-confidence / target-mismatch / Object / no-row /
kind-mismatch / no-entry), the shared v1 cost caps, the interface
family, the kill switch with a profile, and the end-to-end path (a
real T0 training run -> the shared snapshot -> guard-inline, including
depth-2 sites in inlined copies keyed by the callee's site row).
