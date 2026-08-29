# B-2 Precompiled Stencil System (v0)

Owner: shared contract — Baseline No-IR Team (RBC opcode / superinstruction /
call / inline-cache / guard stencils and stencil composition), Codegen Team
(deopt / safepoint / runtime-helper / barrier stubs); stencil format changes
require a cross-team RFC (Stencil Rule 10, §14)

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
Stencil Rules 1-10 in §14 are law: see Amendment C in docs/laws.md.
```

A **stencil** is a precompiled, relocatable chunk of machine code with known
holes. Precompiled stencils are the missing piece that solves the **T1 no-IR
baseline JIT** cleanly: T1 does not need a real optimizer. It is a
**copy-and-patch stencil compositor**.

With stencils, the tier model is:

```text
T0: direct-threaded register interpreter
T1: no-IR baseline JIT using precompiled stencils
T2: optimizing IR JIT
T3: AOT
```

and the backend becomes much simpler because T1 does not need a real optimizer.

---

# 1. What "precompiled stencils" means in B-2

A **stencil** is a precompiled, relocatable chunk of machine code with known holes.

It is not interpreted.
It is not compiled at runtime from IR.
It is copied into the code cache and patched.

Conceptually:

```text
stencil = precompiled code fragment + patch holes + metadata
```

At runtime:

```text
RBC instruction sequence
        |
        v
select stencils
        |
        v
copy stencil code into staging buffer
        |
        v
patch holes:
  - register slot offsets
  - field offsets
  - method targets
  - class ids
  - inline cache stubs
  - deopt ids
  - branch targets
        |
        v
finalize W^X code
```

This gives you a very fast baseline JIT with no IR.

---

# 2. Where stencils fit in the tier system

## T0 — Interpreter

Does not use stencils for execution.

But T0 is still important because:

- it defines the canonical deopt state
- it produces RBC
- it produces profile data
- it is the fallback target

## T1 — No-IR Baseline JIT

This is the main stencil user.

T1 compiles methods by composing stencils for:

- RBC ops
- superinstructions
- calls
- inline caches
- guards
- safepoints
- loop backedges
- exception edges
- deopt traps

T1 does **not** build an IR graph.

## T2 — Optimizing JIT

T2 does not use stencils as its main compilation model.

But T2 can still use precompiled stencils for:

- deopt stubs
- uncommon trap stubs
- runtime call trampolines
- IC stubs
- barrier stubs
- materialization helper stubs
- safepoint poll stubs
- vector scalar fallback stubs

So stencils are still backend infrastructure.

## T3 — AOT

AOT can embed:

- runtime stub stencils
- deopt stubs
- IC stub templates
- barrier stubs
- target-specific helper stencils

AOT main code is still produced by the full optimizer, but the runtime support
code can be stencil-based.

---

# 3. Stencil categories

We should make these first-class stencil classes.

## 3.1 Opcode stencils

One stencil per RBC operation.

Examples:

```text
iadd_rrr
isub_rrr
imul_rrr
iload_slot
istore_slot
aload_slot
astore_slot
goto
if_eq
if_null
return_obj
return_int
new_object
array_length
checkcast
instanceof
monitorenter
monitorexit
throw
```

These are the baseline building blocks.

## 3.2 Superinstruction stencils

Fused stencils for common RBC sequences.

Examples:

```text
aload_getfield_nullcheck
iload_iload_iadd_istore
aload_invokevirtual_pop
aload_arraylength_ifgt
iinc_goto_backedge
aload_getfield_ireturn
```

Superinstructions reduce:

- patch count
- code size
- branch overhead
- metadata size

They are very important for T1 performance.

## 3.3 Call stencils

For method calls.

Examples:

```text
call_static
call_virtual_monomorphic
call_virtual_bimorphic
call_interface_megamorphic
call_invokedynamic
call_method_handle
call_native_jni
```

Call stencils need:

- ABI handling
- stack map at call
- exception return handling
- IC patch site
- deopt metadata
- safepoint metadata

## 3.4 Inline-cache stencils

They are patchable dispatch stubs.

Examples:

```text
ic_field_get
ic_field_set
ic_method_monomorphic
ic_method_polymorphic
ic_interface
ic_invokedynamic
ic_array_clone
```

They need safe runtime patching.

## 3.5 Guard stencils

Used for lightweight T1 assumptions and T2 traps.

Examples:

```text
guard_non_null
guard_class_id
guard_class_version
guard_array_type
guard_bounds
guard_method_target
guard_call_site_version
guard_static_field_stable
```

Guard stencils must include:

- expected value hole
- deopt id hole
- optional patchpoint support

## 3.6 Deopt stencils

Precompiled stubs for entering deopt runtime.

Examples:

```text
deopt_stub_eager
deopt_stub_uncommon_trap
deopt_stub_exception
deopt_stub_osr_exit
deopt_stub_invalidated_code
```

These should be precompiled and highly tuned.

They should:

- save registers
- construct deopt context
- call runtime deopt entry
- never return to compiled code

## 3.7 Runtime helper stencils

Small precompiled trampolines to runtime services.

Examples:

```text
allocation_slow_path
write_barrier_stub
read_barrier_stub
monitor_enter_slow
monitor_exit_slow
throw_null_pointer
throw_array_index_oob
throw_stack_overflow
throw_arithmetic
class_init_check
safepoint_slow_path
```

## 3.8 NaN boxing stencils

If NaN boxing is enabled:

```text
nanbox_ref_encode
nanbox_ref_decode
nanbox_ref_guard
nanbox_ref_store_barrier
```

These must be behind a contract and kill switch.

## 3.9 Vector / SWLP support stencils

For T2/T3:

```text
vector_scalar_fallback
vector_tail_loop
vector_bounds_guard
vector_load_aligned
vector_load_unaligned
vector_store_masked
```

These are not the main vector code, but support fragments.

---

# 4. Stencil format

A stencil artifact should contain:

1. header
2. code bytes
3. patch table
4. relocation table
5. stack map info
6. deopt info
7. safepoint info
8. register/clobber info
9. effect flags
10. dependency constraints
11. target feature requirements

## 4.1 Stencil header

Conceptual C++26 structure:

```cpp
struct StencilHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t target_arch;
    uint32_t target_features_hash;
    uint32_t abi_hash;
    uint32_t compiler_hash;
    uint32_t stencil_kind;
    SymbolId name;

    uint32_t code_size;
    uint32_t patch_table_offset;
    uint32_t relocation_offset;
    uint32_t stack_map_offset;
    uint32_t deopt_info_offset;
    uint32_t safepoint_offset;
    uint32_t clobber_offset;

    StencilFlags flags;
    EffectTags effects;
};
```

No raw pointers.

Everything should be offset/index-based.

## 4.2 Patch site table

Each hole must be described explicitly.

```cpp
enum class PatchKind : uint8_t {
    Imm8,
    Imm16,
    Imm32,
    Imm64,
    BranchRel8,
    BranchRel32,
    CallRel32,
    AbsTarget64,
    SlotOffset,
    FieldOffset,
    KlassId,
    MethodId,
    CallSiteId,
    DeoptId,
    ICStubAddr,
    ConstPoolIndex,
    RuntimeHelperId
};

struct PatchSite {
    uint32_t code_offset;
    PatchKind kind;
    uint8_t width;
    PatchConstraints constraints;
    SymbolId semantic_name;
};
```

Important rule:

> Runtime may patch only described holes. It must never patch arbitrary
> instruction bytes.

This keeps patching safe and auditable.

---

# 5. Stencil generation pipeline

Build-time flow:

```text
stencil source files
        |
        v
C++26 / asm compiler
        |
        v
relocatable object
        |
        v
stencil generator tool
        |
        v
stencil binary + metadata tables
        |
        v
versioned stencil archive / embedded runtime image
```

Stencil sources can be:

- handwritten assembly
- intrinsics C++
- annotated C++ functions
- generated opcode templates

The stencil generator extracts:

- code bytes
- patch holes
- relocations
- clobber masks
- stack map points
- deopt labels
- safepoint labels

---

# 6. Runtime stencil instantiation

Runtime flow for T1:

```text
Method becomes hot
        |
        v
Select stencil plan from RBC
        |
        v
Allocate writable staging buffer
        |
        v
For each RBC op/superinstruction:
    copy stencil code
    record native offset -> RBC pc
    apply patch sites
    record stack maps / deopt ids
        |
        v
Link internal branches
        |
        v
Finalize metadata
        |
        v
Publish W^X executable code atomically
```

No optimization passes.

No IR.

No dataflow analysis.

Just stencil composition and patching.

---

# 7. Stencil plan

For each method, T1 creates a stencil plan.

```cpp
struct StencilPlan {
    MethodId method;
    SmallVector<StencilInstance, 64> instances;
    SmallVector<PcMapEntry, 64> pc_map;
    SmallVector<StackMapPoint, 16> stack_maps;
    SmallVector<DeoptPoint, 16> deopt_points;
    SmallVector<ExceptionEdge, 8> exception_edges;
};
```

A `StencilInstance` is one copied stencil in the output buffer.

```cpp
struct StencilInstance {
    StencilId stencil;
    uint32_t output_offset;
    uint32_t output_size;
    uint32_t rbc_pc_start;
    uint32_t rbc_pc_end;
    SmallVector<PatchValue, 8> patch_values;
};
```

---

# 8. T1 frame model should be T0-compatible

This is the key design choice.

For T1 stencils, use a **T0-compatible register file frame**.

That means the baseline frame contains the T0 register file in memory.

Why?

Because deopt becomes trivial.

If T1 already keeps values in a T0-compatible frame, deopt is mostly:

```text
find current RBC pc
flush cached registers if any
enter T0 interpreter
```

No complex materialization.

No speculative object reconstruction.

No IR state reconstruction.

## 8.1 Stencil register discipline

Stencils should follow a simple discipline:

- method/frame pointer in a dedicated register
- thread pointer in a dedicated register
- values loaded from T0 register file
- results stored back to T0 register file
- scratch registers declared as clobbers
- no GC references held in scratch registers across calls/safepoints unless declared

This makes stack maps simple.

## 8.2 GC maps become easier

If the frame is T0-compatible, the GC can often use:

- method slot type map
- current RBC pc
- known frame layout

instead of a highly optimized T2 stack map.

This is a huge simplification.

---

# 9. Stencils and deopt

Stencils must be deopt-aware.

Every T1 compiled method still needs:

1. native pc -> RBC pc map
2. FrameState or T0-compatible equivalent
3. stack map at safepoints/calls
4. exception edge information
5. monitor state mapping
6. deopt ids for traps/guards

Because T1 is no-IR, its FrameState can be simplified.

For many T1 points:

```text
FrameState == current RBC pc + T0-compatible frame
```

That is much simpler than T2.

## 9.1 Guard stencils and deopt

Example guard stencil:

```asm
guard_class_id:
    cmp   [obj + klass_id_slot], KLASSED
    jne   DEOPT
```

Patched holes:

- `KLASSED`
- `DEOPT`

The deopt target is a deopt stub with a `DeoptId`.

## 9.2 Call stencils and exceptions

Call stencils must record:

- call site RBC pc
- stack map after call
- exception return path
- pending exception handling

If the called method throws and T1 cannot handle it inline, it can deopt to T0
with the pending exception.

---

# 10. Stencils and inline caches

T1 performance depends heavily on inline caches.

Stencil IC model:

1. emit IC stencil call site
2. patch initial target from profile
3. if wrong target observed, update IC
4. if megamorphic, patch to generic IC stub
5. if dependency invalidates, patch to fallback/deopt

IC patching must be safe under concurrent execution.

Use:

- atomic patch sequences
- aligned patch sites
- safe instruction sequences
- W^X-compatible patching
- quiescence for stub retirement

---

# 11. Stencils and superinstructions

Superinstruction stencils are where T1 gets real speed.

Example RBC sequence:

```text
iload a
iload b
iadd
istore c
```

Instead of four stencils, use one:

```text
iload_iload_iadd_istore
```

Patch holes:

- slot a
- slot b
- slot c

Benefits:

- fewer patches
- fewer branches
- fewer pc-map entries
- better icache behavior
- better register scratch reuse

The stencil generator should mine common RBC sequences from benchmarks and
generate superinstruction stencils.

---

# 12. Stencils and NaN boxing

If NaN boxing is used, stencils can implement common encode/decode paths.

Examples:

```text
load_ref_nanboxed
store_ref_nanboxed
decode_ref_from_nanbox
encode_ref_to_nanbox
nanbox_ref_guard
```

But this must be very carefully owned.

Rules:

- NaN boxing must be disabled by default
- NaN boxing stencils must be target-specific
- stack maps must understand NaN-boxed reference slots
- GC must understand NaN-boxed reference scanning
- floating-point semantics must not be corrupted
- kill switch mandatory

Do not let NaN boxing leak into generic T1 stencils unless the value
representation contract is approved.

---

# 13. Stencils and SWLP

For SWLP, stencils are mostly support code, not the main optimizer output.

Useful stencils:

```text
swlp_guard_bounds
swlp_guard_aliasing
swlp_scalar_fallback
swlp_tail_loop
swlp_masked_store
swlp_alignment_check
```

The T2 optimizer still decides whether SWLP is valid.

Stencils just provide precompiled helper fragments.

---

# 14. Stencil laws

These are added to the law set as Amendment C in `docs/laws.md`.

## Stencil Rule 1 — Stencils are immutable

Runtime must not generate new stencil bodies.

Runtime may only:

- copy existing stencils
- patch described holes
- compose them into code buffers

If a new stencil body is needed, it is built by the toolchain, not the runtime.

## Stencil Rule 2 — Every stencil must have metadata

No stencil may exist without:

- patch table
- clobber info
- stack map info
- safepoint info
- deopt info if applicable
- exception behavior
- target feature requirements
- effect tags

## Stencil Rule 3 — Only described holes may be patched

Patch sites must be explicit.

Arbitrary instruction mutation is forbidden.

## Stencil Rule 4 — Stencils must be versioned

Stencil artifacts must include:

- stencil format version
- target arch
- target feature hash
- ABI hash
- compiler hash
- runtime config hash

If validation fails, the stencil must be rejected.

## Stencil Rule 5 — Stencil instantiation must be W^X safe

Stencil copying and patching must happen in writable staging memory.

Publication to executable memory must preserve W^X.

## Stencil Rule 6 — Stencil deopt metadata is mandatory

If a stencil can trap, call, safepoint, or deopt, it must carry enough metadata
to reconstruct T0 state.

## Stencil Rule 7 — Stencils must not hold untracked GC references

Any GC reference live across:

- call
- safepoint
- runtime helper
- deopt stub
- allocation

must be tracked in stack maps or handles.

## Stencil Rule 8 — Stencil fallback is mandatory

If no valid stencil exists for an RBC sequence, T1 must fall back to T0.

No silent miscompile.

## Stencil Rule 9 — Stencil cache pressure must be managed

The runtime must track:

- stencil code size
- patch table size
- metadata size
- IC stub count
- retired stencil instances

When pressure is too high, throttle T1 compilation or evict cold code.

## Stencil Rule 10 — Stencil changes require cross-team message

Because stencils affect:

- baseline
- codegen
- deopt
- GC maps
- ICs
- ABI
- security patching

any stencil format change requires an RFC message to affected teams.

---

# 15. Ownership

Suggested ownership:

```yaml
teams:
  baseline_noir:
    write:
      - compiler/baseline/
      - compiler/baseline/stencils/
      - tests/baseline/
      - docs/baseline_contract.md
      - docs/stencil_contract.md

  codegen:
    write:
      - compiler/codegen/
      - compiler/codegen/stubs/
      - tests/codegen/
      - docs/codegen_contract.md

  ir:
    write:
      - compiler/ir/core/
      - include/b2/ir/
      - tests/ir/
      - docs/ir_spec.md

  passes:
    write:
      - compiler/passes/
      - compiler/pipeline/
      - tests/passes/
      - docs/pass_contracts.md
```

But stencils are a shared contract.

So:

- **Baseline No-IR Team** owns RBC opcode/superinstruction stencils.
- **Codegen Team** owns deopt/safepoint/runtime helper stubs.
- **IR/Passes Teams** do not modify stencils.
- Any shared stencil format change requires an RFC.

---

# 16. This document

This document is the normative stencil reference (`docs/stencils.md`).

Summary:

Stencils are used by:

- T1 no-IR baseline JIT
- T2/T3 runtime stubs
- deopt stubs
- inline-cache stubs
- barrier stubs
- safepoint stubs

Stencils are immutable build-time artifacts.

Runtime may copy and patch stencils only through declared patch sites.

Runtime must not generate new stencil bodies.

Stencil artifacts must be versioned, validated, and W^X-safe.

---

# 17. Suggested stencil directory layout

```text
compiler/
  baseline/
    stencil_plan.cpp
    stencil_installer.cpp
    stencil_selector.cpp
    stencils/
      opcode/
      superinstruction/
      call/
      ic/
      guard/
  codegen/
    stubs/
      deopt/
      safepoint/
      barrier/
      materialization/
      runtime_call/
tools/
  stencilgen/
    stencilgen.cpp
    patch_table.cpp
    stencil_validator.cpp
```

---

# 18. Updated T1 backend pipeline

With stencils, T1 becomes:

```text
RBC
 |
 v
Stencil Selector
 |
 v
Stencil Plan
 |
 v
Copy/Patch Stencils
 |
 v
Build pc map
 |
 v
Build T0-compatible stack maps
 |
 v
Install code atomically
```

No IR.

No optimizer.

No regalloc.

This is exactly what was wanted for no-IR baseline.

---

# 19. Updated T2/T3 backend relationship

T2/T3 still uses the full backend.

But stencils are still useful as precompiled stubs.

```text
T2 optimizing backend
        |
        v
emits optimized code
        |
        +-- links to precompiled deopt stubs
        +-- links to precompiled IC stubs
        +-- links to precompiled barrier stubs
        +-- links to precompiled safepoint stubs
        +-- links to precompiled materialization helpers
```

This means stencils are not only a T1 feature.

They are a backend infrastructure feature.

---

# 20. What this improves

Precompiled stencils give you:

- much faster T1 compile time
- lower compiler complexity
- predictable baseline performance
- easier deopt for T1
- easier W^X handling
- easier security auditing
- reusable runtime stubs
- better startup
- less runtime code generation surface area

This is probably the correct implementation strategy for the baseline tier.

---

# 21. What to be careful about

The main risks are:

1. **code bloat**
   - too many superinstruction stencils
   - too many specialized variants

2. **patch-site fragility**
   - holes must be architecturally safe
   - immediate ranges must be validated

3. **GC reference discipline**
   - stencils must not hide oops in scratch registers

4. **deopt mapping**
   - every stencil instance must map back to RBC pc

5. **target variance**
   - stencils are target-specific and feature-specific

6. **IC patch races**
   - live patching must be safe

7. **NaN boxing complexity**
   - keep it isolated and kill-switchable

---

# 22. Recommendation

Make precompiled stencils a **first-class subsystem**.

Specifically:

1. T1 no-IR baseline JIT is implemented as a **stencil compositor**.
2. T2/T3 use stencils for **stubs and runtime support**.
3. Stencil format is a shared contract.
4. Stencil generation happens at build time.
5. Runtime only copies and patches stencils.
6. All stencils carry deopt/GC/safepoint metadata.

That makes the whole redesign much cleaner.
