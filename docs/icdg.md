# B-2 ICDG — Inline Call/Dispatch Graph (v1)

Owner: Passes Team (primary owner of the ICDG contract); consumed by Baseline
No-IR (T1 dispatch stencil selection), Codegen (dispatch lowering and IC
patching), IR (graph metadata and dependency records), AOT (offline
whole-program dispatch analysis), Interpreter (T0 profile source). No team may
unilaterally change the ICDG contract; changes require an RFC message and
approval from all consuming teams.

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
ICDG decisions are speculation and are governed by: Rule 3 (every PGO-driven
decision requires a guard), Rule 42 (no assumption without invalidation),
Rule 43 (no specialization without fallback), Rule 44 (no profile data
without confidence), Rule 71 (specialization requires versioned,
invalidatable dependencies), Rule 75 (frames must be reconstructible on
demand), Rule 81 (deopt metadata is a required compilation output),
Rule 101 (every compiled artifact records dependencies), Rule 117
(invalidation must be ordered and visible). The score model is a cost model
per Rule 45; thresholds are named constants per Rule 23; heuristics are
empirically validated per Rule 25; decisions are deterministic and
explainable per Rule 124 and Rule 47. No SMT, no ML, no solver: profile +
cost model + graph traversal only.
```

This document is the shared ICDG contract. It subsumes the three contract
files proposed at design time into one owned document:

| Proposed file | Lives here |
|---|---|
| `inline_dispatch_graph.md` | §4 (node types), §14 (data flow) |
| `dispatch_states.md` | §5, §6 (dispatch-state action table) |
| `inline_score_model.md` | §13 (decision model) |

---

# 1. What ICDG is

ICDG is a shared compiler subsystem that answers, for every call/dispatch
site:

```text
  - what can it dispatch to?
  - how stable is that dispatch?
  - should it be patched?
  - should it be devirtualized?
  - should it be guard-inlined?
  - should it be fully inlined?
  - should it remain indirect?
  - what does inlining unlock?
```

It is not just "find call targets." It is:

```text
call graph
+ dispatch profile
+ class hierarchy analysis
+ method handle / invokedynamic linkage
+ inlining cost model
+ downstream optimization unlock analysis
+ invalidation dependencies
```

---

# 2. Why this matters for B-2

Java performance lives or dies on call boundaries.

Call boundaries block:

- inlining
- PEA
- CM-PEA
- effect reordering
- SWLP
- representation transition elimination
- lock elision
- range-check elimination
- constant propagation
- escape analysis

If you remove the right call boundaries, everything else gets stronger.

If you remove the wrong ones, you get:

- code bloat
- deopt storms
- compile latency explosions
- register pressure explosions
- exception handling complexity
- monitor semantics complexity

So the problem is not "inline everything." The problem is:

> **Inline the call boundaries that unlock the most downstream value, and
> leave the rest indirect.**

That is exactly what ICDG decides.

---

# 3. ICDG covers two worlds

There are actually two graphs hiding inside this idea.

## 3.1 Application call graph

This is the normal Java call graph:

```text
Method A calls Method B
Method B calls Method C
lambda invoke calls functional interface target
interface call dispatches to implementation
```

This graph is used by T2/T3 optimization.

## 3.2 Dispatch implementation graph

This is the runtime dispatch machinery:

```text
virtual call
interface call
invokedynamic call
MethodHandle invocation
IC stub
vtable lookup
itable lookup
megamorphic dispatch
runtime resolve helper
JNI transition
```

This graph is used by:

- T1 stencil selection
- T2 devirtualization
- inline cache patching
- deopt traps
- AOT dispatch validation

ICDG must unify both.

---

# 4. Core ICDG node types

## 4.1 MethodNode

Represents a Java method or important runtime callable unit.

```cpp
struct MethodNode {
    MethodId id;
    MethodFlags flags;

    uint32_t estimated_code_size;
    uint32_t heat;
    uint32_t compile_tier;

    bool is_synchronized;
    bool has_exceptions;
    bool has_monitors;
    bool is_native;
    bool is_reflective;
    bool is_invokedynamic_bootstrap;
    bool is_method_handle_intrinsic;

    InliningSummary summary;
    DependencyVersion version;
};
```

## 4.2 CallSiteNode

Represents a call site inside a method.

```cpp
struct CallSiteNode {
    CallSiteId id;
    MethodId caller;
    RbcPc pc;

    CallKind kind;
    DispatchState dispatch_state;
    InlineState inline_state;

    TypeProfile receiver_profile;
    TargetProfile target_profile;
    StabilityScore stability;

    CostScore inline_benefit;
    CostScore inline_cost;

    DependencySet dependencies;
};
```

## 4.3 DispatchCandidate

A possible target for a dispatch site.

```cpp
struct DispatchCandidate {
    MethodId target;
    ReceiverClassId receiver_class;
    uint32_t observed_count;
    uint32_t total_count;

    StabilityScore stability;
    GuardKind required_guard;
    InlineAction recommended_action;
};
```

## 4.4 DispatchKind

```cpp
enum class CallKind : uint8_t {
    Static,
    Special,
    Virtual,
    Interface,
    InvokeDynamic,
    MethodHandle,
    NativeJNI,
    RuntimeHelper,
    StencilDispatch,
    DeoptTrap
};
```

## 4.5 DispatchState

```cpp
enum class DispatchState : uint8_t {
    Unknown,
    Unlinked,
    Monomorphic,
    Bimorphic,
    Polymorphic,
    Megamorphic,
    Invalidated,
    StaticProof
};
```

## 4.6 InlineAction

```cpp
enum class InlineAction : uint8_t {
    KeepIndirect,
    PatchIC,
    DevirtualizeOnly,
    GuardInline,
    DirectInline,
    SpeculativeInline,
    OutlineCold,
    TrapToRuntime
};
```

---

# 5. The key idea: inline based on unlock value

A normal inliner asks:

```text
Is this callee small and hot?
```

ICDG asks:

```text
If we inline this call, what becomes possible?
```

That is the research-grade part.

Inlining value comes from what it unlocks:

| Unlock | Value |
|---|---|
| Eliminates dispatch | direct throughput |
| Enables CM-PEA | allocation reduction |
| Enables effect reordering | memory throughput |
| Enables SWLP | vectorization |
| Eliminates representation transitions | fewer encode/decode ops |
| Enables lock elision | monitor removal |
| Enables constant propagation | scalar optimization |
| Enables exception edge specialization | faster exception paths |
| Enables range-check elimination | bounds-check removal |

So the ICDG score is not just "callee size." It is:

```text
inline_score =
    direct_dispatch_benefit
  + unlocked_pea_benefit
  + unlocked_effect_reordering_benefit
  + unlocked_swlp_benefit
  + representation_transition_elimination_benefit
  + constant_propagation_benefit
  + lock_elision_benefit
  - code_size_cost
  - compile_time_cost
  - register_pressure_cost
  - deopt_complexity_cost
  - exception_complexity_cost
  - monitor_complexity_cost
```

All thresholds are named constants, not magic numbers (Rule 23).

---

# 6. Dispatch-state action table

This is the heart of ICDG.

| Dispatch State | T1 Action | T2 Action |
|---|---|---|
| Unknown | generic stencil | do not inline yet |
| Unlinked | runtime resolve stencil | uncommon trap / resolve |
| Monomorphic | monomorphic IC stencil | devirtualize + inline if profitable |
| Bimorphic | bimorphic guard stencil | guarded inline for two targets |
| Polymorphic | polymorphic IC stencil | partial inline or IC only |
| Megamorphic | megamorphic dispatch stencil | keep indirect unless profile proves stability |
| StaticProof | direct stencil | direct inline without guard if proof valid |
| Invalidated | fallback stencil | deopt / recompile |

Important:

- T1 does not truly inline method bodies.
- T1 selects better dispatch stencils.
- T2 can inline actual method bodies.
- T3 can inline using static proofs and AOT profiles.

---

# 7. ICDG across tiers

## T0 — Interpreter

ICDG is not active yet, but T0 collects the raw data:

- call counts
- receiver classes
- target methods
- invokedynamic linkage targets
- MethodHandle shapes
- exception edges
- backedge heat

T0 is the profile source.

## T1 — Stencil baseline

T1 uses ICDG to choose dispatch stencils.

Example:

```text
virtual call site with 99% Foo.bar()
    -> use monomorphic virtual stencil for Foo.bar()
```

Example:

```text
interface call with two stable targets
    -> use bimorphic interface guard stencil
```

Example:

```text
megamorphic site
    -> use generic IC dispatch stencil
```

T1 does not inline callee bodies. That would violate the no-IR baseline
model (Amendment A).

But T1 can eliminate generic dispatch overhead using ICDG data.

## T2 — Optimizing JIT

T2 uses ICDG for real inlining.

It asks:

```text
Should this call site be inlined into the current IR graph?
```

If yes:

- inline callee IR
- attach FrameState for inlined frames
- record dependency on class hierarchy / method version
- update deopt metadata
- feed CM-PEA / effect reordering / representation optimization

If no:

- keep call
- maybe devirtualize
- maybe patch IC
- maybe outline cold path

## T3 — AOT

AOT uses ICDG offline.

It can use:

- whole-program call graph if closed-world
- static proofs
- final/sealed class info
- module boundaries
- no-reflection proofs
- persistent profiles

AOT can be more aggressive because it has more time and more global
information.

But it still needs invalidation if dynamic Java features break assumptions.

---

# 8. ICDG and CM-PEA

This is where ICDG becomes extremely valuable.

CM-PEA needs to know whether an object escapes across a call boundary.

If the call is not inlined, the object usually escapes conservatively.

If the call is inlined, CM-PEA can see:

```text
object passed to callee
callee only reads fields
callee does not store object
callee does not publish object
callee returns scalar result
```

Then the object can still be scalarized.

So ICDG should prioritize inlining calls that carry short-lived objects.

ICDG query:

```text
Which call sites, if inlined, would allow CM-PEA to scalarize allocations?
```

That is a huge score bonus.

Example:

```java
stream
  .map(x -> transform(x))
  .filter(x -> isValid(x))
  .collect(...)
```

The lambda call sites are the barriers. ICDG detects:

- functional interface target is stable
- lambda body is small
- intermediate objects escape only through pipeline
- inlining unlocks CM-PEA

Decision:

```text
Inline lambda targets.
```

Result:

```text
allocation reduction
+ dispatch elimination
+ better representation handling
```

---

# 9. ICDG and effect reordering

Effect reordering is blocked by calls.

A call is usually an opaque effect barrier unless proven otherwise.

If ICDG inlines a callee whose effects are local and understood, effect
reordering can proceed.

Example:

```text
caller stores to local array
callee reads from local array
callee writes to local array
```

Before inlining:

```text
call is opaque barrier
```

After inlining:

```text
effect chain visible
reordering possible
```

So ICDG should give bonus score to calls that:

- have only local effects
- do not throw
- do not enter monitors
- do not call JNI
- do not publish objects
- do not access volatile/shared state
- are small enough to analyze

---

# 10. ICDG and adaptive representation

Representation transitions often appear at call boundaries.

Example:

```text
caller has UnboxedInt32
callee expects TaggedInt31
```

That requires:

```text
encode
guard
maybe deopt
```

If ICDG inlines the callee, the transition can often be eliminated.

ICDG query:

```text
Which call sites have expensive representation transitions at the boundary?
```

Those sites get inlining bonus if the callee can use the caller
representation after inlining.

This is especially useful for:

- stream pipelines
- polymorphic numeric code
- serialization
- MethodHandle combinators
- invokedynamic call sites

---

# 11. ICDG and SWLP

SWLP needs independent scalar operations.

Calls often break scalar independence.

If ICDG inlines a small callee, the body may become visible and vectorizable.

Example:

```java
for (int i = 0; i < n; i++) {
    out[i] = scale(input[i]);
}
```

If `scale` is not inlined, SWLP sees:

```text
loop contains call
cannot vectorize
```

If ICDG inlines `scale`, SWLP sees:

```text
out[i] = input[i] * constant
```

Now SWLP can pack it.

So ICDG gives bonus score to calls inside hot counted loops where inlining
enables vectorization.

---

# 12. ICDG should not inline everything

This is important.

ICDG must also be a **do-not-inline engine**.

Do not inline:

- huge methods
- recursive cycles without budget
- JNI/native calls
- reflective calls unless proven
- invokedynamic sites with unstable linkage
- megamorphic sites without strong profile
- synchronized methods unless monitor semantics are safe
- methods with complex exception handling unless justified
- methods that would blow code cache budget
- methods that would explode register pressure
- methods with active JVMTI breakpoints unless lower-tier fallback
- cold methods unless static AOT proves value

A good ICDG says "no" often.

---

# 13. ICDG decision model

## 13.1 Benefit terms

```cpp
struct InlineBenefit {
    uint32_t dispatch_elimination;
    uint32_t pea_unlock;
    uint32_t effect_reorder_unlock;
    uint32_t swlp_unlock;
    uint32_t representation_transition_elimination;
    uint32_t constant_propagation_unlock;
    uint32_t lock_elision_unlock;
    uint32_t exception_specialization_unlock;
};
```

## 13.2 Cost terms

```cpp
struct InlineCost {
    uint32_t code_size;
    uint32_t compile_time;
    uint32_t register_pressure;
    uint32_t deopt_metadata_size;
    uint32_t exception_complexity;
    uint32_t monitor_complexity;
    uint32_t dependency_fragility;
};
```

## 13.3 Score

```cpp
struct InlineScore {
    uint32_t total_benefit;
    uint32_t total_cost;
    uint32_t net_score;
    InlineAction action;
};
```

The action is selected by thresholds:

```cpp
if (net_score < DoNotInlineThreshold)
    action = KeepIndirect;
else if (dispatch_state == Monomorphic && net_score >= DevirtThreshold)
    action = DevirtualizeOnly;
else if (net_score >= GuardInlineThreshold)
    action = GuardInline;
else if (net_score >= DirectInlineThreshold)
    action = DirectInline;
```

No solver. No Z3. Just profile + cost model + graph traversal.

---

# 14. ICDG data flow

```text
T0 interpreter profile
        |
        v
ICDG profile ingestion
        |
        v
CallSiteNode creation
        |
        v
DispatchCandidate accumulation
        |
        v
CHA / invokedynamic / MethodHandle resolution
        |
        v
Stability scoring
        |
        v
Inline benefit estimation
        |
        v
Inline cost estimation
        |
        v
Action selection
        |
        +---> T1 stencil selector
        +---> T2 inliner
        +---> T3 AOT planner
        +---> dependency manager
        +---> telemetry
```

---

# 15. ICDG invalidation

ICDG decisions are assumptions.

They must be invalidated when:

- new subclass loaded
- interface implementor added
- method redefined
- invokedynamic relinked
- MethodHandle target changed
- class initializer state changed
- reflection/instrumentation enabled
- JVMTI breakpoint enabled
- profile drift detected
- deopt storm detected

Invalidation actions:

1. mark dependent compiled code not entrant
2. patch IC if safe
3. request lazy deopt at safepoint
4. update ICDG node state
5. recompute affected call sites
6. emit telemetry

No silent stale dispatch.

---

# 16. ICDG and deopt

Every speculative inline decision needs:

- FrameState for inlined frames
- dependency record
- deopt reason
- guard or invalidation mechanism
- materialization plan if CM-PEA participated

If an inlined call assumption fails:

```text
deopt to T0 state at call boundary
```

or, if safe:

```text
deopt to lower-tier code with same logical state
```

ICDG must not create inlined frames that cannot be reconstructed.

Rule:

> If the deoptimizer cannot reconstruct the inlined T0 frame stack, the
> inlining decision is invalid.

---

# 17. ICDG and stencils

ICDG should not make T1 do full inlining.

But it should make T1 smarter.

T1 stencil choices influenced by ICDG:

| Call Shape | Stencil Choice |
|---|---|
| static known target | direct call stencil |
| monomorphic virtual | monomorphic IC stencil |
| bimorphic virtual | guarded bimorphic stencil |
| monomorphic interface | interface IC stencil |
| stable invokedynamic | direct target stencil |
| unstable invokedynamic | generic invokedynamic stencil |
| MethodHandle constant | direct handle stencil |
| megamorphic | generic dispatch stencil |
| native | JNI transition stencil |

This gives T1 much better dispatch performance without breaking the no-IR
rule.

---

# 18. ICDG and runtime dispatch stubs

"Inline anything in the dispatch and calls" must be read carefully. We need
to distinguish:

## User calls

Yes, inline user methods when profitable.

## Runtime dispatch helpers

Usually no.

Runtime helpers like:

- resolve stub
- ic miss handler
- deopt stub
- barrier stub
- safepoint stub
- allocation slow path

should usually remain shared stencils/trampolines.

Why?

- they are cold
- they need centralized state
- they need patching control
- they need security constraints
- inlining them bloats code
- they often call into runtime services

Exception:

Some tiny dispatch fast paths can be emitted inline:

```text
load receiver klass
compare expected klass
branch to direct target
```

That is not "inlining the runtime." That is lowering the dispatch decision
into compiled code.

ICDG should decide that too.

---

# 19. ICDG should produce explanations

This is important for a research compiler.

Every ICDG decision should emit a structured explanation:

```text
CallSite: Foo.bar() at RBC pc 42
State: Monomorphic
Target: Bar.doWork()
Stability: 0.99
Benefit:
  dispatch_elimination: 30
  pea_unlock: 55
  swlp_unlock: 0
Cost:
  code_size: 12
  compile_time: 8
  deopt_complexity: 5
Action: GuardInline
Reason: High PEA unlock and stable monomorphic target.
```

This is not just logging.

It is how you debug inlining decisions.

It is also how Java tests can validate why a method was or was not inlined.

---

# 20. Java-visible testing for ICDG

Since tests are Java, expose an internal diagnostic API:

```java
RuntimeDiagnostics.getInlineDecision("com/example/Foo.bar")
```

or:

```java
RuntimeDiagnostics.getCallSiteDecisions(methodHandle)
```

Then Java tests can assert things like:

```java
@Test
void monomorphicLambdaPipelineIsInlined() {
    warmupStreamPipeline();

    InlineDecision d = RuntimeDiagnostics.getInlineDecision(
        "com/example/StreamTest.lambda$map$0"
    );

    assertEquals(InlineAction.GUARD_INLINE, d.action());
    assertTrue(d.benefit().peaUnlock() > 0);
}
```

And semantic tests:

```java
@DifferentialTest(tiers = {T0, T1, T2})
void inlinedPipelineProducesSameResults() {
    ...
}
```

Forced deopt tests:

```java
@Test
void inlinedCallDeoptsToCorrectCallerState() {
    RuntimeHooks.enableForcedDeoptAtCallSite("Foo.bar", 1);
    ...
}
```

---

# 21. Expected performance impact

ICDG by itself is not a single number. It multiplies the other features.

## Dispatch-heavy code

Monomorphic virtual/interface dispatch elimination:

```text
+5% to +15%
```

if currently paying vtable/itable/IC cost.

## Lambda/stream pipelines

Inlining lambda targets unlocks CM-PEA and representation optimization:

```text
+10% to +30%
```

on modern stream-heavy Java.

## Serialization/DTO code

Inlining getters/setters/mappers unlocks PEA and effect reordering:

```text
+10% to +25%
```

## Numeric loops with small helper calls

Inlining enables SWLP:

```text
+10% to +40%
```

depending on vectorizability.

## Megamorphic enterprise code

Limited benefit:

```text
0% to +5%
```

ICDG should mostly avoid bad inlining here.

## Startup

T1 stencil dispatch selection improves warmup:

```text
+5% to +10% warmup throughput
```

not because of true inlining, but because T1 chooses better dispatch
stencils.

---

# 22. Risks

## Risk 1: Inlining explosion

Too many inlined calls create huge graphs.

Mitigation:

- per-compilation node budget
- per-method inlining budget
- SCC recursion limits
- code size budget
- compile time budget

## Risk 2: Deopt storms

Speculative inlining based on stale profile can fail repeatedly.

Mitigation:

- stability score
- deopt correlation
- exponential backoff
- downgrade to IC-only dispatch
- blacklist site

## Risk 3: Code cache bloat

Inlining too much increases code size.

Mitigation:

- cold outlining
- shared stubs
- stencil reuse
- code cache pressure feedback into ICDG score

## Risk 4: Monitor/exception semantic bugs

Inlining synchronized methods or exception-heavy methods is dangerous.

Mitigation:

- conservative defaults
- explicit monitor FrameState
- exception edge verification
- Java differential tests

## Risk 5: Megamorphic false confidence

A site may look stable in one workload phase and become unstable later.

Mitigation:

- profile decay
- confidence thresholds
- invalidation
- re-specialization

---

# 23. Where ICDG lives in the team structure

ICDG is cross-cutting.

It touches:

- Passes Team: inlining decisions
- Baseline Team: T1 dispatch stencil selection
- Codegen Team: dispatch lowering and patching
- IR Team: graph metadata and dependencies
- AOT Team: offline whole-program dispatch analysis

So ICDG is a shared contract (this document).

Ownership:

```yaml
icdg:
  primary_owner: passes
  consumers:
    - baseline_noir
    - codegen
    - aot
    - ir
  changes_require:
    - RFC message
    - approval from all consuming teams
```

No team should unilaterally change the ICDG contract.

---

# 24. Recommended implementation phases

## Phase 1: Dispatch profiler

Build:

- call site profiling
- receiver type profile
- target stability
- invokedynamic linkage tracking
- MethodHandle target tracking

This alone improves T1 stencil selection.

**LANDED (MSG-20260901-003)**: the T0 side — per-call-site receiver-class/target
histograms keyed (caller MethodId, call pc), bounded 3-entry + sticky
megamorphic (the 1/2/3/inf shape), saturating, always-on, exposed read-only
via `Interpreter::dispatchProfiles()` (contract:
`docs/interp_contract.md` §8.1, v1.2.0; 12 tests in `tests/interp/`).
invokedynamic/MethodHandle tracking is vacuous in v0 (traps at dispatch) and
lands with the bootstrap machinery. The consumption side — dispatch-state
classification, stability scoring, and the T1 stencil / T2 GuardInline
decisions — is Phase 2's ingestion work over this data.

## Phase 2: ICDG core

Build:

- CallSiteNode
- MethodNode
- DispatchCandidate
- stability scoring
- dependency records
- action selection

Use it for:

- T1 stencil dispatch
- T2 devirtualization
- T2 simple inlining

## Phase 3: Unlock-aware scoring

Add:

- PEA unlock estimation
- representation transition elimination
- effect reordering unlock
- SWLP unlock estimation

This is where ICDG becomes research-grade.

## Phase 4: AOT whole-program ICDG

Add:

- closed-world call graph
- static proof integration
- module/sealed/final analysis
- persistent dispatch profiles
- offline inlining plans

---

# 25. Verdict

Yes — but do not make it "just an inliner."

Make it the **central call/dispatch decision engine**.

It should decide:

```text
patch?
devirtualize?
guard?
inline?
outline?
trap?
keep indirect?
```

and it should decide that based on:

```text
dispatch stability
+ downstream optimization unlock
+ cost/budget
+ deopt safety
+ invalidation risk
```

That is the correct design.

It is the fourth major research pillar after:

1. CM-PEA
2. Verified effect reordering
3. Adaptive value representation

ICDG is the thing that decides where those three are allowed to operate.

