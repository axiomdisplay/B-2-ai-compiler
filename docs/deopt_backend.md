# B-2 Deoptimization System and Backend Design (v0)

Owner: cross-team contract - Interpreter Team (T0 canonical state), Baseline No-IR
Team (T1 frames and stack maps), IR Team (FrameState), Codegen Team (backend
emission); see `docs/teams/README.md`

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

Deopt is specified before the backend because the deopt system dictates the
backend frame layout, stack maps, safepoints, metadata, and the patching model:

- If the deopt system is weak, the optimizing compiler cannot safely speculate.
- If the backend is weak, even a good deopt design becomes slow or unsafe.

The design covers the B-2 tiers (Amendment B):

- **T0:** direct-threaded register interpreter
- **T1:** no-IR baseline JIT
- **T2:** optimizing IR JIT
- **T3:** AOT / static JIT

Contents: Part A specifies the deopt system. Part B specifies the backend.
Part C gives the suggested implementation order. Part D lists the hard rules.
Part E is the architectural summary.

---

# Part A — B-2 Deopt System Design

## 1. Core deopt principle

Deoptimization must reconstruct the exact canonical interpreter state.

For B-2, the canonical state is **T0 state**, not Java bytecode stack state directly.

That means deopt must reconstruct:

```text
T0 direct-threaded register interpreter state
```

including:

- current method
- RBC program counter
- register file / local registers
- operand-register state
- monitor stack
- pending Java exception, if any
- line-number / BCI mapping for stack traces
- held monitors
- JVMTI/debug/profiling state where active
- virtual-thread / continuation state where applicable
- GC-visible references

The golden rule:

> After guard failure, execution must resume in a state observationally indistinguishable from the state T0 would have reached at that RBC instruction boundary.

---

## 2. Deopt is not one mechanism

There are several distinct deopt classes.

### 2.1 Eager guard deopt

A speculative guard fails immediately.

Examples:

- wrong class
- null where non-null assumed
- array out of bounds
- wrong method target
- wrong invokedynamic linkage
- unexpected array type
- class hierarchy dependency invalid

This is the most common deopt.

---

### 2.2 Uncommon trap

Compiled code reaches a path it did not want to compile.

Examples:

- unloaded class
- unsupported intrinsic
- breakpoint becomes active
- JVMTI single-step enabled
- cold exception handler
- unsupported dynamic feature

The compiled code traps to runtime and runtime decides to fall back to T0/T1.

---

### 2.3 Exception deopt

Compiled code cannot handle an exception path directly.

Instead of emitting the handler, it deopts with a pending Java exception.

Important:

- Java exceptions are values.
- C++ exceptions are forbidden.
- exception object, cause, suppressed exceptions, and stack-trace state must be preserved.

---

### 2.4 OSR exit / OSR deopt

On-stack replacement must be reversible.

If OSR-compiled code fails a guard or is invalidated, it must reconstruct T0 state at a valid loop backedge or OSR boundary.

---

### 2.5 Lazy invalidation deopt

A dependency changes while code is already running.

Examples:

- new subclass loaded
- method redefined
- invokedynamic relinked
- class hierarchy changed
- static field stability broken
- JVMTI/debugger enabled

The code may not be instantly destroyed. Threads must deopt at safe boundaries:- safepoints
- backedges
- returns
- call sites
- tier-transition points

---

## 3. Canonical T0 state model

T0 executes **quickened register bytecode**, not raw stack bytecode.

A T0 frame is conceptually:

```cpp
struct T0Frame {
    T0Frame* prev;
    MethodId method;
    RbcPc rbc_pc;

    // Register-bytecode visible state
    RegisterFile regs;

    // Java-visible auxiliary state
    MonitorStack monitors;
    PendingException exception;
    LineInfo line_info;
    FrameFlags flags;
};
```

The register file contains the interpreter-visible values:

- locals
- temporaries
- operand registers
- possibly cached stack values

Every deopt must eventually produce a valid chain of these frames.

---

## 4. FrameState

`FrameState` is the machine-checkable snapshot attached to every speculative point.

It describes how to reconstruct one or more logical T0 frames from optimized compiled code.

A `FrameState` must describe:

```text
For each logical frame:
  - method
  - RBC pc
  - original Java BCI
  - line number
  - slot mapping
  - monitor stack
  - exception state
  - materialized object graph
  - frame flags
```

---

## 5. Slot mapping

Every T0 slot must be mapped to one of:

| Source kind | Meaning |
|---|---|
| `Register` | value is in a physical register at deopt point |
| `StackSlot` | value is spilled in the current frame |
| `Constant` | value is a known constant |
| `VirtualObject` | value must be materialized |
| `OopHandle` | value is held through a GC handle |
| `NaNBoxedRef` | value is a NaN-boxed reference representation |
| `CompressedRef` | value is a compressed oop |

Important rule:

> A FrameState may only refer to values that are live and recoverable at the deopt point.

If a value is not live, the optimizer must either:

- keep it live,
- spill it,
- recompute it with pure code,
- or reject the speculation.

No hand-waving is allowed.

---

## 6. FrameState descriptor shape

Conceptual C++26-style metadata:

```cpp
enum class SlotKind : uint8_t {
    Register,
    StackSlot,
    Constant,
    VirtualObject,
    OopHandle,
    NaNBoxedRef,
    CompressedRef
};

struct SlotRef {
    SlotKind kind;
    uint32_t location_id;
    TypeTag type;
};

struct MonitorState {
    SlotRef object;
    uint32_t reentry_count;
};

struct FrameStateDesc {
    MethodId method;
    uint32_t rbc_pc;
    uint32_t java_bci;
    uint32_t line_number;
    FrameFlags flags;

    // Slot map for T0 register file
    Span<SlotRef> slots;

    // Monitors held by this frame
    Span<MonitorState> monitors;

    // Exception state if relevant
    ExceptionState exception_state;

    // Objects that must be materialized before resuming T0
    Span<VirtualObjectId> materialize;
};
```

All references should be indices into tables, not raw pointers.

---

## 7. Deopt metadata is a required compilation output

A compiled method is not complete until it has:

1. deopt point table
2. FrameState table
3. stack maps
4. GC reference maps
5. exception tables
6. materialization plans
7. dependency list
8. OSR entry/exit maps
9. interpreter re-entry information
10. monitor reconstruction info

If any of these cannot be produced, compilation must fail and fall back.

---

## 8. Deopt point table

Each compiled code object has a table mapping native PCs to deopt information.

```cpp
struct DeoptPoint {
    NativePcRange pc_range;
    DeoptId id;
    FrameStateId state;
    DeoptReason reason;
};
```

The backend must guarantee:

- every guard has a reachable deopt point
- every deopt point has a valid FrameState
- every deopt point has correct register/spill locations
- every GC reference across the deopt point is recorded

---

## 9. Deopt reasons

Use explicit, telemetry-friendly reasons:

```cpp
enum class DeoptReason : uint32_t {
    GuardType,
    GuardNull,
    GuardNonNull,
    GuardClassVersion,
    GuardMethodTarget,
    GuardCallSite,
    GuardBounds,
    GuardArrayStore,
    GuardOverflow,
    GuardDivZero,
    UncommonTrap,
    ExceptionEdge,
    OSRExit,
    DependencyInvalidation,
    JVMTIEnabled,
    BreakpointEnabled,
    UnsupportedDynamicFeature,
    AllocationFailure,
    StackOverflow,
    RecursionLimit,
    JNIExceptionPending,
    VirtualThreadSuspension,
    Unknown
};
```

Every deopt increments counters by:

- site
- method
- reason
- tier
- speculation kind

This feeds throttling and recompilation policy.

---

## 10. Deopt runtime flow

High-level flow:

```text
Compiled code guard failure
        |
        v
Deopt stub
        |
        v
Save architectural state
        |
        v
Lookup code artifact + deopt point
        |
        v
Validate code / dependency state
        |
        v
Capture live values
        |
        v
Materialize virtual objects
        |
        v
Reconstruct T0 frame chain
        |
        v
Enter T0 interpreter at exact RBC pc
        |
        v
Update telemetry / deopt counters
```

---

## 11. Deopt stub

Every guard failure should jump to a shared or per-artifact stub.

Conceptual assembly:

```asm
guard_failed:
    mov   REG_ARG1, deopt_id
    jmp   deopt_stub

deopt_stub:
    save_general_registers
    save_float_registers
    save_vector_registers_if_needed
    mov   REG_ARG1, thread_ptr
    mov   REG_ARG2, deopt_context_ptr
    call  runtime_deopt_entry
    ; runtime never returns to compiled code
    ; it transfers to T0 interpreter
```

Important constraints:

- no C++ exceptions
- no dynamic dispatch
- no arbitrary allocation before entering runtime
- no global locks
- no `getenv`
- no slow logging on the immediate trap path

The stub should be small and predictable.

---

## 12. Deopt context

The stub creates a `DeoptContext`.

```cpp
struct DeoptContext {
    ThreadState* thread;
    CompiledCodeId code;
    NativePc pc;
    DeoptId deopt_id;
    DeoptReason reason;

    ArchRegisters gp_regs;
    FloatRegisters fp_regs;
    VectorRegisters vec_regs; // optional / target-dependent

    Oop pending_exception;
};
```

This context must be GC-safe before any allocation occurs.

---

## 13. Runtime deopt entry

Conceptual C++26 interface:

```cpp
[[nodiscard]]
Result<void> runtime_deopt_entry(DeoptContext* ctx) noexcept;
```

No exceptions.

Implementation outline:

```cpp
Result<void> runtime_deopt_entry(DeoptContext* ctx) noexcept {
    TRY(auto artifact = lookup_code(ctx->code));
    TRY(auto point = lookup_deopt_point(artifact, ctx->pc, ctx->deopt_id));

    record_deopt_event(ctx, point);

    if (is_code_invalidated(artifact)) {
        TRY(handle_invalidated_code(ctx, artifact));
    }

    HandleScope scope(ctx->thread);

    TRY(auto captured = capture_live_values(ctx, point.frame_state));
    TRY(auto materialized = materialize_virtual_objects(ctx, point.frame_state, captured));
    TRY(auto frames = build_t0_frames(ctx, point.frame_state, captured, materialized));

    ctx->thread->enter_t0_interpreter(frames, ctx->pending_exception);

    return {};
}
```

The runtime must never return to the failed compiled code.

---

## 14. Capturing live values

The runtime uses the FrameState slot map to extract values from:

- saved registers
- spilled stack slots
- constants
- handles
- encoded representations

Representation conversions may include:

- compressed oop decode
- NaN-boxed reference decode
- integer narrowing/widening where required
- float/double unboxing
- reference validation

All object references must be placed into handles before allocation-heavy work.

---

## 15. Materialization

Materialization is required when PEA or scalar replacement removed objects that now need to become real.

This is the hardest part of deopt.

---

## 15.1 Materialization requirements

Materialization must handle:

- object allocation
- field initialization
- array elements
- object headers
- identity hash preservation if required
- monitor reconstruction
- circular object graphs
- GC barriers
- OOM behavior
- deopt-time stack overflow behavior
- JNI-visible references
- weak/finalizer/Cleaner constraints

---

## 15.2 Materialization algorithm

For each FrameState requiring materialization:

1. Build a materialization plan.
2. Allocate all required objects without publishing them.
3. Register them in a handle table.
4. Fill fields and array elements.
5. Resolve cycles using the handle table.
6. Reconstruct monitors if required.
7. Apply GC write barriers.
8. Make objects visible to the reconstructed T0 state.

Pseudo:

```cpp
Result<MaterializedGraph>
materialize_virtual_objects(
    DeoptContext* ctx,
    FrameStateId state,CapturedValues& values
) noexcept {
    auto plan = get_materialization_plan(state);

    TRY(auto handles = allocate_objects(ctx, plan));
    TRY(initialize_object_fields(handles, plan, values));
    TRY(resolve_cycles(handles, plan));
    TRY(reconstruct_monitors(ctx, handles, plan));

    return MaterializedGraph{handles};
}
```

---

## 15.3 Allocation failure

If allocation fails during materialization:

- do not leave partially published object graph visible
- discard unmaterialized objects
- reconstruct enough T0 state to throw the correct Java exception
- usually `OutOfMemoryError`

Important semantic rule:

> If PEA eliminated an allocation, and materialization later fails with OOM, that must be observationally equivalent to the original allocation failing.

---

## 15.4 Identity hash codes

If `System.identityHashCode` was observed before deopt, PEA must not scalarize the object unless the hash value was captured.

If hash was not observed, materialization may assign a new stable hash.

If hash was observed:

- store the scalar hash value in FrameState
- restore it into object header during materialization

---

## 15.5 Monitor reconstruction

If a FrameState contains held monitors:

- materialize the locked object first
- reacquire the monitor for the current thread
- preserve reentry count

If the object was virtual and the lock is not observable, the optimizer may avoid materializing the lock, but only if proven safe.

Default safe rule:

> If FrameState says a monitor is held, materialize and lock the object.

---

## 16. OSR design

OSR is mandatory for hot loops.

### 16.1 OSR entry

Entry from T0 to T2 happens at:

- loop backedges
- long-running method bodies
- hot interpreter positions

OSR entry needs:

- T0 register file snapshot
- RBC pc
- loop state
- exception state
- monitor state
- pending exception state, usually none at entry

The compiled OSR code must accept the T0 state and map it into optimized registers.

---

### 16.2 OSR exit

OSR exit is deopt back to T0 at a valid OSR boundary.

It must reconstruct:

- loop induction variables
- iterator state
- locals/register file
- exception state
- monitor state
- virtual-thread suspension state if applicable

OSR deopt must not lose or duplicate loop iterations.

---

## 17. Invalidations and dependency manager

Every compiled artifact records dependencies.

Examples:

- method version
- class version
- class hierarchy version
- field layout version
- vtable/itable version
- invokedynamic call-site version
- MethodHandle target version
- class initialization state
- profile version
- module/access state
- debug/JVMTI state

When a dependency changes:

1. mark dependent code `NOT_ENTRANT`
2. patch entry or install lazy deopt traps where safe
3. notify running threads via handshake/safepoint
4. prevent new entries into invalidated code
5. keep old code alive until quiescence

Important:> Invalidation must be visible before new code based on newer assumptions is installed.

No racing invalidation with installation.

---

## 18. Deopt throttling

Repeated deopt at the same site is a hazard.

Track:

- deopt count per site
- deopt count per method
- deopt reasons
- time window
- recompilation history
- tier history

If thresholds are exceeded:

1. disable the failing speculation
2. recompile with weaker assumptions
3. downgrade to T1 or T0
4. temporarily or permanently blacklist method
5. emit telemetry

No unbounded compile/deopt churn.

---

## 19. Deopt and GC

Deopt must be GC-safe.

### 19.1 Before materialization

All captured object references must be:

- in handles, or
- in GC-tracked stack locations, or
- in a GC-scanned deopt context

No raw oops across allocation.

---

### 19.2 During materialization

Every store of an object reference must execute the correct write barrier.

If the GC is moving/concurrent:

- read barriers must be used where required
- forwarding pointers must be respected
- object headers must be valid at all times

---

### 19.3 After deopt

The reconstructed T0 frames must have valid GC maps.

T0 interpreter frames must be walkable by:

- GC
- profilers
- debuggers
- exception unwinder
- stack walker
- thread dump tooling

---

## 20. Deopt and Java exceptions

Java exceptions are control-flow values.

Deopt may be triggered:

- before throwing into an unsupported handler
- after an operation that produces a pending exception
- during exception edge transfer to T0

The deopt system must preserve:

- exception type
- message
- cause
- suppressed exceptions
- stack-trace-relevant frames
- line numbers
- finally / try-with-resources semantics

If stack traces are generated while code is still compiled, the stack walker must be able to walk logical frames using deopt metadata without necessarily materializing full T0 frames.

---

## 21. Deopt and JNI

JNI transitions are opaque unless proven.

Rules:

- do not deopt inside native code
- deopt only at managed-code boundaries
- preserve pending Java exceptions from JNI
- preserve JNI local/global reference state
- preserve monitor state
- treat native calls as potential invalidation points

If a JNI call returns into invalidated compiled code, the runtime should transfer to T0/T1 at the return boundary.

---

## 22. Deopt and virtual threads / continuations

If B-2 supports virtual threads or continuations:

Suspension points are deopt-safe boundaries.

Deopt must preserve:

- continuation stack state
- park/unpark state
- monitor wait state where applicable
- sleep state
- virtual-thread mount/unmount state
- local variables after resume

If this is too complex initially, virtual-thread suspension can force deopt to T0 before suspension.

---

## 23. Deopt latency budgets

Suggested budgets:

| Deopt kind | Target latency |
|---|---:|
| T1 -> T0 simple deopt | < 1 µs |
| T2 -> T0 guard failure,no materialization | < 5 µs |
| T2 -> T0 with small materialization | < 20 µs |
| T2 -> T0 with large object graph | bounded by allocation cost, but must be budgeted |
| lazy invalidation deopt | bounded by safepoint latency |

These are implementation goals, not semantic excuses.

If deopt is too slow, speculation is too aggressive.

---

## 24. Deopt telemetry

Every deopt must record:

- method
- deopt site
- reason
- tier
- speculation kind
- PGO confidence that caused speculation
- guard kind
- materialized object count
- allocation count
- latency
- code artifact id
- dependency id if invalidation-related

No silent deopt.

---

## 25. Deopt testing strategy

Required tests:

1. forced guard failure at every guard
2. forced deopt at every deopt point
3. differential T0 vs T2 vs AOT behavior
4. deopt during GC
5. deopt with moving GC
6. deopt with allocation failure
7. deopt with pending exception
8. deopt with held monitors
9. deopt with materialized cycles
10. deopt with identityHashCode-sensitive objects
11. deopt with weak refs / Cleaners / finalizers where supported
12. OSR entry + OSR exit stress
13. invalidation while executing
14. deopt during JNI return
15. deopt with JVMTI/debug enabled

If deopt tests are not first-class, the runtime is not production-grade.

---

# Part B — B-2 Backend Design

Now the backend.

The backend must be designed around the deopt system, not after it.

---

## 1. Backend goals

The backend must:

- lower optimized IR to machine code
- preserve Java semantics exactly
- emit deopt metadata
- emit GC stack maps
- emit safepoints
- emit exception tables
- support T1 no-IR baseline code
- support T2/T3 optimized code
- produce W^X-safe executable artifacts
- publish code atomically
- support patching safely
- support deterministic replay

---

## 2. Backend tiers

There are effectively two backend modes.

### 2.1 T1 no-IR backend

This backend serves the no-IR baseline JIT.

It does not consume sea-of-nodes IR.

It consumes:

- quickened RBC
- inline-cache metadata
- simple profile hints

It emits:

- linear native code
- pc-to-RBC maps
- simple stack maps
- deopt info
- safepoints

---

### 2.2 T2/T3 optimizing backend

This backend consumes:

- scheduled sea-of-nodes IR
- FrameState metadata
- dependency metadata
- PGO/static proof info
- regalloc results

It emits:

- optimized machine code
- full deopt tables
- GC maps
- exception tables
- OSR entry points
- trampolines
- patch sites
- dependency metadata

---

# 3. T1 no-IR backend design

The simplest safe design is:

> T1 frames should be close to T0-compatible frames.

That makes deopt extremely simple.

## 3.1 T1 frame strategy

For each baseline method:

- allocate a register-file area on the native stack
- keep RBC pc known at instruction boundaries
- store values to the register file at safe boundaries
- optionally cache some values in registers between boundaries

This gives:- easy deopt
- easy GC scanning
- easy debugging
- lower peak performance than T2, but acceptable for T1

---

## 3.2 T1 code generation

Use codelets or templates.

For each RBC operation:

```text
load operands
perform operation
store result
advance RBC pc map
emit safepoint if needed
```

No global optimization.

Allowed simple optimizations:

- constant patching
- field offset patching
- inline-cache stub selection
- branch target patching
- superinstruction codelets
- trivial null-check folding if profile/static info proves safety

No:

- loop unrolling
- vectorization
- escape analysis
- effect reordering
- speculative object elimination

---

## 3.3 T1 deopt

Because the frame is mostly T0-compatible:

1. find RBC pc from native pc map
2. flush cached registers into register file
3. enter T0 interpreter at that RBC pc

No materialization.

This is the baseline deopt ideal.

---

# 4. T2/T3 optimizing backend pipeline

The optimizing backend has these phases:

```text
Scheduled IR
    |
    v
Machine IR construction
    |
    v
Legalization
    |
    v
Instruction selection
    |
    v
Block layout
    |
    v
Register allocation
    |
    v
Frame finalization
    |
    v
Safepoint/deopt emission
    |
    v
Exception table emission
    |
    v
Machine code emission
    |
    v
Metadata serialization
    |
    v
Atomic publication
```

---

## 5. Machine IR

Use a two-level machine representation.

### 5.1 High MIR

Target-independent machine operations:

```text
Load
Store
Add
Sub
Mul
Div
Rem
And
Or
Xor
Shl
Shr
Sar
Compare
Branch
Call
Return
Guard
Safepoint
WriteBarrier
ReadBarrier
Fence
CAS
MonitorEnter
MonitorExit
Alloc
Deopt
OSREntry
OSRExit
VectorOp
BarrierStub
RuntimeCall
```

High MIR uses virtual registers.

---

### 5.2 Low MIR

Target-specific instructions and register classes.

Examples:

- x86-64 instructions
- AArch64 instructions
- vector register classes
- addressing modes
- condition codes
- ABI-specific call conventions

Target-specific logic lives only in the backend.

Generic passes must not contain target hacks.

---

## 6. Instruction selection

Instruction selection converts High MIR to Low MIR.

Requirements:

- pattern-based
- data-driven where possible
- target cost model driven
- no hidden global state
- deterministic
- no C++ exceptions

Use generated tables or declarative pattern descriptions.

Avoid giant hand-written `switch` forests unless isolated and generated.

---

## 7. Register allocation interface

The backend does not own register allocation internals.

It provides RegAlloc with:

- virtual registers
- register classes
- live ranges
- clobber sets
- call instructions
- safepoint locations
- GC-reference tags
- deopt-point liveness
- vector register requirements

RegAlloc returns:

- physical register assignments
- spill slot assignments
- spill/reload instructions or edit instructions
- stack map contributions
- GC reference locations
- deopt location maps

Critical rule:> Every GC reference live across a safepoint or deopt point must be tracked.

---

## 8. Frame layout

Optimized frames need a precise layout.

A typical JIT frame:

```text
higher addresses
----------------
caller frame
----------------
return address
saved frame pointer
method/code metadata slot
deopt context slot if needed
spill slots
monitor slots if needed
GC-visible reference slots
----------------
lower addresses
```

The frame descriptor must record:

- frame size
- return address offset
- saved registers
- stack map bitmap
- monitor slots
- deopt metadata reference
- logical Java frame mapping
- native/managed transition markers

For inlined methods, one physical frame may represent multiple logical Java frames.

The deopt metadata reconstructs the logical frames.

---

## 9. Stack maps and GC maps

The backend must emit stack maps for:

- calls
- safepoints
- allocations
- deopt stubs
- runtime transitions
- OSR entry points
- JNI transitions where applicable

Each stack map identifies:

- which registers contain object references
- which stack slots contain object references
- which slots contain NaN-boxed references
- which slots contain compressed oops
- which slots are scalar primitives

If the GC moves objects, stack maps must allow updating references.

---

## 10. Safepoints

Generated code must poll safepoints at:

- loop backedges
- method calls
- allocation sites
- OSR entry/exit
- returns where required
- long native transitions
- invalidation points where required
- suspension points where applicable

Safepoint implementation options:

1. thread-local flag compare
2. poll page test
3. handshake token check

The fast path must be tiny.

Conceptual fast path:

```asm
cmp  [thread + safepoint_state], 0
je   continue
call safepoint_slow_path
continue:
```

Or:

```asm
test [poll_page], reg
; fault or branch if safepoint requested
```

The choice is target/runtime-specific.

---

## 11. Deopt stub emission

For every deopt point, the backend must know:

- guard location
- deopt id
- FrameState id
- register locations
- spill locations
- exception state if relevant

The backend may emit:

- direct guard branches
- trap instructions
- patchpoints
- jump to shared deopt stub
- uncommon trap stub

The deopt stub must be reachable and GC-safe.

---

## 12. Guard lowering

Guards can be lowered in several ways.

### 12.1 Immediate branch guard

```asm
cmp   klass, expected_klass
jne   deopt_stub_type_guard
```

Good for hot, predictable guards.

---

### 12.2 Patchpoint guard

Initial code assumes true, but can be patched to jump to deopt.

Useful for dependency invalidation.

---

### 12.3 Trap guard

Use an explicit trap instruction if guard fails.

Useful for cold/uncommon traps.

---

### 12.4 Dependency-only guard

No runtime check, but code is invalidated if dependency changes.

Only safe when invalidation is guaranteed to prevent further execution.

---

## 13. Exception handling

The backend must not use native C++ exceptions.

Java exceptions are modeled with:

- exception object registers/thread slots
- exception control edges
- exception handler tables
- runtime throw helpers
- deopt-to-handler where needed

For each call that may throw:

- record exception edge
- record stack map at call
- preserve exception object
- branch to handler or deopt

If compiled code cannot handle an exception path, it must deopt with the pending exception.

---

## 14. Code buffer and W^X

Compilation writes into a staging buffer.

The staging buffer is writable but not executable.

After finalization:

1. validate code
2. validate metadata
3. copy or remap to executable memory
4. make memory executable
5. make it read-only or remove write permission
6. flush instruction cache if required
7. publish atomically

No page may be simultaneously writable and executable.

If patching existing executable code is required:

- patch only architecturally atomic sequences
- ensure no torn instruction execution
- handle icache coherence
- use platform-approved mechanisms

---

## 15. Code publication

Publication must be atomic.

The runtime must never observe:

- partially initialized code
- missing deopt metadata
- missing GC maps
- missing dependency records
- half-installed entry point

Publication order:

1. allocate code artifact id
2. finalize code and metadata
3. register dependencies
4. publish metadata with release semantics
5. publish entry point with release semantics
6. mark code active

Consumers use acquire semantics.

---

## 16. Old code retirement

Old code may still be executing.

Therefore:

- do not free old code immediately
- mark not entrant
- wait for quiescence
- use epoch-based reclamation or RCU-like mechanism
- preserve deopt metadata until no thread can need it

Code reclamation and IR reclamation are separate.

---

## 17. Patching

Patching is used for:

- inline caches
- method entry transitions
- deopt patchpoints
- invokedynamic call sites
- dependency traps
- tier transitions

Patching rules:

- patch sites must be aligned
- patch sequences must be architecturally safe
- no invalid intermediate instructions
- no torn reads by concurrent threads
- W^X preserved
- icache coherence handled

If safe atomic patching is impossible, use safepoint-based patching.

---

## 18. Inline caches

Inline caches are essential for Java dispatch performance.

Backend must support:

- monomorphic IC stubs
- polymorphic IC stubs
- megamorphic fallback
- interface dispatch caches
- invokedynamic call-site caches
- field access caches

IC stubs must be GC-aware and deopt-aware.

IC mutation must be safe under concurrent execution.

---

## 19. Backend support for NaN boxing

If NaN boxing is enabled, backend must handle:- encoding references into NaN-boxed form
- decoding NaN-boxed references
- distinguishing NaN-boxed refs from real floating-point values
- inserting GC barriers after reference stores
- marking NaN-boxed reference slots in stack maps
- preserving Java floating-point semantics

Rules:

- real float/double computation should use FP registers
- NaN-boxed representation should be used only where contract permits
- no Java double bits may be corrupted unless canonicalization is proven safe
- NaN boxing must have a kill switch

Backend must not treat a NaN-boxed reference as a double or a double as a reference.

---

## 20. Backend support for SWLP

For superword-level parallelism, backend must support:

- vector register classes
- vector load/store lowering
- vector arithmetic lowering
- masked operations where target supports
- scalar fallback blocks
- tail handling
- alignment handling
- vector spill/reload
- vector-aware register allocation

SWLP code must still have correct:

- bounds checks
- dependency guards
- exception behavior
- deopt state before entering vector loop
- scalar resume state if fallback is taken

---

## 21. Backend support for PEA

For partial escape analysis, backend must support:

- materialization stubs or runtime calls
- virtual object metadata references
- handle-based allocation paths
- barrier emission during materialization
- monitor reconstruction runtime calls
- deopt-time object graph tables

The backend does not decide escape analysis policy.

It must faithfully emit what the optimizer and deopt contract require.

---

## 22. Backend security requirements

The backend must enforce:

- W^X
- no arbitrary syscalls from generated code
- only approved runtime entrypoints
- no user-controlled instruction streams
- constant blinding where needed
- separation of code and embedded data
- relocation validation for AOT
- target-feature gating
- code signing where platform requires
- JIT spraying mitigations

Generated code is powerful. It must be constrained.

---

## 23. Backend determinism and replay

Given:

- same IR
- same PGO snapshot
- same compiler version
- same target description
- same flags
- same RNG seed

the backend should produce deterministic code selection and metadata, except for explicitly documented nondeterminism.

Nondeterminism must be logged.

---

## 24. Backend testing

Required backend tests:

1. golden lowering tests
2. instruction selection tests
3. ABI conformance tests
4. register clobber tests
5. stack map validation tests
6. deopt stub reachability tests
7. forced deopt at every deopt point
8. GC stress during deopt
9. exception edge tests
10. OSR entry/exit tests
11. patch-under-load tests
12. W^X validation tests
13. code publication race tests
14. AOT relocation tests
15. target-feature mismatch tests

---

# Part C — Suggested implementation order

If I were building this practically, I would do it in this order:

## Phase 1: Deopt foundations

1. Define T0 frame state precisely.
2. Define FrameState format.
3. Define deopt metadata tables.
4. Implement T0-compatible T1 frames.
5. Implement T1 -> T0 deopt.
6. Implement stack maps and basic GC-safe runtime entry.

This gives a safe foundation.

---

## Phase 2: Minimal optimizing backend

1. Scheduled IR to High MIR.
2. Simple instruction selection.
3. Simple linear-scan regalloc.
4. Frame layout with spill slots.
5. Emit deopt points with full FrameState.
6. Emit GC stack maps.
7. Emit simple guard branches to deopt stubs.

Do not try to be clever yet.

---

## Phase 3: Advanced deopt

1. Materialization for PEA.
2. Monitor reconstruction.
3. Exception deopt.
4. OSR entry/exit.
5. Dependency invalidation.
6. Deopt throttling and telemetry.

---

## Phase 4: Advanced backend

1. Block layout.
2. Hot/cold splitting.
3. inline caches.
4. patchpoints.
5. vector/SWLP lowering.
6. NaN boxing lowering.
7. AOT artifact emission.

---

# Part D — Hard rules for this design

These should be treated as non-negotiable:

1. **No C++ exceptions on deopt or backend hot paths.**
2. **No uncontrolled allocation in deopt entry.**
3. **No raw oops across allocation without handles or stack maps.**
4. **No deopt point without FrameState.**
5. **No FrameState without recoverable slot locations.**
6. **No speculative side effects before the guard that protects them.**
7. **No backend lowering that silently changes Java semantics.**
8. **No W^X violations.**
9. **No torn code publication.**
10. **No freeing old code before quiescence.**
11. **No silent deopt.**
12. **No silent fallback.**
13. **No patching that can be observed as corrupted instructions.**
14. **No materialized object graph visible before it is fully initialized.**
15. **No optimization that makes deopt impossible.**

---

# Part E — The clean architectural summary

The deopt system is:

```text
T0 canonical state
+ FrameState snapshots
+ deopt point tables
+ materialization engine
+ GC-safe runtime entry
+ dependency invalidation
+ telemetry/throttling
```

The backend is:

```text
Scheduled IR
-> High MIR
-> Low MIR
-> instruction selection
-> regalloc
-> frame/stack map finalization
-> safepoint/deopt/exception emission
-> W^X code publication
```

and the two meet at:

```text
guard locations
FrameState locations
stack maps
exception tables
OSR maps
patch sites
code publication
```
