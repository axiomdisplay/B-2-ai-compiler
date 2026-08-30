# B-2 Codegen Contract (v1) — T1 Stencil Instantiation, Code Cache, Execution

Owner: Codegen Team — the build-time stencil archive, the runtime
instantiator (copy-and-patch), the T1 code cache, the Tier-1 execution
engine, and the runtime-helper seam every compiled tier shares

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
Stencil Rules 1-10 (docs/laws.md Amendment C / docs/stencils.md) are law.
The T0-compatible frame derives from docs/stencils.md SS8 and
docs/interp_contract.md SS1/SS2; conflicts resolve in their favor.
```

This document pins what "T1 instantiation" means now that machine bytes
exist. The plan stage (docs/stencils.md SS7, `include/b2/baseline/`) is
unchanged: a `StencilPlan` is the copy-and-patch RECIPE. This contract
defines what consumes the recipe and produces executable code.

Contents: SS1 the pipeline; SS2 the target; SS3 the activation record;
SS4 the stencil archive; SS5 hole semantics; SS6 code layout and linking;
SS7 W^X discipline; SS8 the runtime-helper ABI; SS9 deopt and exceptions;
SS10 the code cache; SS11 determinism and testing; SS12 refusals.

---

# SS1. The pipeline (build time vs runtime)

```text
BUILD TIME (Stencil Rule 1: stencils are immutable build artifacts)
  compiler/codegen/src/StencilBodies.cpp   one emitter per manifest stencil
  tools/stencilgen/stencilgen              generates the archive
      -> build/gen/x86_64_v1.archive       binary artifact (inspectable)
      -> build/gen/ArchiveData.cpp          embedded byte array
  libb2_codegen                             links the embedded archive

RUNTIME (Stencil Rules 3/5: copy, patch, link, publish)
  StencilPlan (from b2::baseline::compilePlan)
        |
        v
  Instantiator: validate versions -> re-layout with true sizes ->
        copy bodies -> patch declared holes -> emit entry stubs,
        per-point deopt thunks (archive templates), shared tails ->
        W^X publish
        |
        v
  CompiledCode (code cache)  ->  Tier1 engine executes; deopt -> T0
```

The runtime NEVER generates stencil bodies. Every emitted byte in a
compiled method is either (a) a copy of archive bytes, or (b) a value
written through a declared archive hole. That is the auditable form of
Stencil Rule 3 (the codegen team's enforcement of "only described holes
may be patched").

---

# SS2. Target

v1 archive: x86-64, System V ABI, SSE2 baseline (no AVX requirement —
`movups`/scalar SSE are universal). `target_arch = 1` in the archive
header; the loader refuses any other. Multi-target archives arrive as
separate artifacts under their own arch ids (Stencil Rule 4).

---

# SS3. The T1 activation record (machine-visible T0-compatible frame)

The engine allocates one activation per executing T1 frame. All stencil
bodies address it through RBP. Fixed control block, then the slot array:

| offset | size | field |
|--------|------|-------|
| 0      | 8    | magic `kT1ActivationMagic` (0xB2T1ACT) |
| 8      | 8    | `CompiledCode*` (owning instantiation) |
| 16     | 8    | exit status: 0 normal, 1 deopt |
| 24     | 8    | deopt native offset (offset into the code block) |
| 32     | 8    | deopt id |
| 40     | 8    | trap kind (0 none; else TrapKind) |
| 48     | 8    | pending exception (ObjRef id; 0 = none) |
| 56     | 8    | reserved (alignment) |
| 64     | 16   | return `Value` (tag @+64, payload @+72) |
| 80     | 8    | monitor count |
| 88     | 64   | monitor ObjRef ids (16 x u32; overflow -> deopt) |
| 152    | ..   | slots: locals l0..lM-1 then regs r0..rN-1, 16 bytes each |

Named constants (Rule 23) live in `include/b2/codegen/Tier1.h` and are
`static_assert`ed against `sizeof(b2::interp::Value)` and the `RType`
enumerator values. Slot k sits at `kSlotsBase + k*16` from RBP.

Slot numbering is FLAT: local slot L -> index L; register R ->
index `numLocals + R`. This is the interp `Frame` (`locals` then `regs`)
concatenated, so deopt is a field-for-field copy (Amendment B.3).

GC note (for the GC team): the frame's GC map is the slot array itself —
walk `[kSlotsBase, kSlotsBase + (numLocals+numRegs)*16)`, read each
`Value.type`, scan `Ref` slots (SS8.2 of docs/stencils.md).

Register discipline (docs/stencils.md SS8.1): RBP = activation base
(set by the entry stub, saved/restored across the whole invocation);
RDI/RSI/RDX/RCX/R8/R9/RAX/R10/R11 and XMM0-15 are scratch (SysV
caller-saved); RBX/R12-R15 are never touched by stencil bodies (SysV
callee-saved, honored by construction).

---

# SS4. The stencil archive format

```text
header:   magic "2sar" | version | target_arch | abi_hash | count
records:  one per stencil, IN MANIFEST StencilId ORDER
  name          (null-terminated, for validation dumps)
  code_size     u32
  code bytes    code_size bytes
  hole_count    u32
  holes:        code_offset u32 | kind u8 | width u8 | source u8 | tag u8
```

The archive's records carry the SAME stencils as `builtinStencilSetV0()`
in the SAME order (stencilgen builds the set and emits exactly one record
per entry, including manifest-only entries). The manifest's declared
holes appear FIRST in every record, in manifest order, tagged `Plan`;
the archive's additional holes follow, tagged `Stream`, `Helper`, or
`Layout` (SS5). Validation: archive magic/version/arch/abi_hash match the
set; record count matches; every `Plan`-tagged hole list equals the
manifest descriptor's hole list (kind/source/width) in order; every hole
offset+width lies inside the code bytes. Any mismatch refuses the archive
(Stencil Rule 4).

`abi_hash` folds: `sizeof(Value)`, the RType enumerator values, the
activation-record offsets, the SysV pointer sizes. A change to any pinned
constant is an ABI break -> new archive -> set version bump.

---

# SS5. Hole semantics (how each hole is filled at instantiation)

`Plan`-tagged holes pair with the instance's PatchValues BY SOURCE, in
order (a greedy subsequence walk; the manifest kind names the semantic value
- DeoptId, FieldOffset - while the archive kind names the ENCODING -
BranchRel32, Imm32 - so kind equality is not required):

| kind / source | fill |
|---------------|------|
| `SlotOffset` / `FrameSlot` | `kSlotsBase + value*16` (local slot) |
| `Imm32` / `InsImm` | raw value (iconst payload, iinc delta) |
| `BranchRel32` / `BranchTarget` | rel32 to the instance starting at rbc pc `value` |
| `DeoptId` / `DeoptIdSource` | rel32 to the deopt thunk of the DeoptPoint with that id |
| `ConstPoolIndex` / `CpIndex` | raw value (helper argument) |
| `FieldOffset` / `InsImm` | raw value (quickened byte offset) |
| `FieldOffset` / `RuntimeField` | resolve `cp[value]`: instance ops patch `slot*16` (getfield additionally packs the resolved RType into bits 28-31 for the A5 unset-field default); static ops patch the FieldId (getstatic of a builtin singleton patches `kStaticBuiltinBase \| objrefId`; a store to a builtin refuses - T0 raises it) |
| `KlassId` / `RuntimeClass` | resolve `cp[value]` -> ClassId |
| `MethodId` / `RuntimeMethod` or `InsImm` | resolved/raw MethodId (helper argument) |
| `CallSiteId` / `RuntimeICStub` or `InsImm` | call-site id (call rbc pc; helper argument) |

`Stream`-tagged holes are filled from the RBC stream (the documented
"re-read from the stream by the instantiator" channel):

| source | fill |
|--------|------|
| `InsDst`/`InsA`/`InsB` | `kSlotsBase + (numLocals + reg)*16` as disp32/imm32 |
| `InsImm` | raw imm |

`Helper`-tagged holes are `CallRel32` sites: filled with the address of
the runtime helper whose id the archive record carries.

`Layout`-tagged holes are branches the instantiator resolves from
instantiation state: entry-stub targets (instance offsets) and
trap-branch targets (the deopt thunk of the DeoptPoint covering the
instance).

Every fill is width-checked and range-checked (rel32 within the block);
every branch target must be an instance start or an emitted stub — a miss
is an instantiation refusal, never a guess (Amendment A discipline).

---

# SS6. Code layout, entry, deopt thunks, exit

A compiled method's code block, in order:

```text
[entry stub]      one per entry point: method entry + each exception
                  handler used as a re-entry target
                  push rbp; mov rbp, rdi; jmp <hole: target instance>
[instance bodies] stencils copied in plan order at TRUE offsets
                  (archive sizes; the plan's nominal offsets are re-laid)
[deopt thunks]    one per DeoptPoint: archive template
                  mov edi, <id>; mov esi, <trap-site offset>;
                  mov edx, <trap kind>; jmp shared tail
[shared tail]     archive template: store status/pc/id/kind into the
                  activation, pop rbp, return kStatusDeopt
```

True re-layout: `real_offset[0] = entry_area_end`;
`real_offset[i+1] = real_offset[i] + archive_size(stencil_i)`. The
instantiator builds the REAL pc map (real offset -> rbc pc) alongside;
deopt translation uses it, never the plan's nominal offsets.

Switch expansion (Stencil Rule 1: copies of archive templates only): a
tableswitch/lookupswitch instance emits one copy of `__switch_case` per
case (holes: selector slot disp, match imm32, je rel32 -> the target
instance) plus one copy of the manifest `goto` record for the default.
Case chains are capped at `kMaxSwitchCases` (1024) - over -> BadSwitchTable
refusal (the method stays on T0, whose table dispatch handles any size).

Exit: every return-flavored stencil embeds its own exit sequence
(`mov [rbp+status], 0` — folded into the entry stub's invariant —
`pop rbp; xor eax, eax; ret` for void; value returns copy the operand
`Value` to `[rbp+64]` first). Re-entry at a handler enters through that
handler's entry stub, so RBP is always re-established.

Stencil Rule 1 note: entry stubs, thunks, and tails are COPIES of archive
templates (`method_entry`, `deopt_point_thunk`, `deopt_exit`) with only
declared holes patched. The instantiator emits no bytes of its own.

---

# SS7. W^X discipline (Stencil Rule 5)

Staging: a page-aligned `posix_memalign` block (RW). Copying, patching,
and the helper-call patching all happen there; helper calls use ABSOLUTE
addresses (`movabs r11, addr; call r11`) because the block may be mmap'd
far from the binary's text (a rel32 cannot reach - a lesson pinned by the
differential tests). Publication: `mprotect(PROT_READ | PROT_EXEC)`, then
`__builtin___clear_cache` (no-op on x86, kept for portability), then the
entry pointer is published through the code cache. Destruction flips the
block back to `PROT_READ | PROT_WRITE` BEFORE `free` - the allocator
writes free-list metadata INTO the freed chunk, and writing RX memory
segfaults (the second pinned lesson). v1 does no IC patching in published
code; when it arrives it will use the atomic-patch protocol from
docs/stencils.md SS10.

---

# SS8. Runtime-helper ABI

Helpers are C++ functions in `compiler/codegen/src/Engine.cpp`,
`extern "C"`, called from stencil bodies via `movabs r11, <absolute
address>; call r11` with the SysV integer argument registers:

```text
T1HelperU32: u32 f(T1Activation* act, u32 a, u32 b, u32 c, u32 d)
```

- arg1 (RDI) = activation pointer (always)
- arg2..arg5 (RSI, RDX, RCX, R8D) = semantic arguments (slot offsets,
  field offsets/ids, class ids, packed call targets, element kinds) —
  always PATCHED values, never raw pointers
- return EAX: 0 = success; nonzero = a TrapKind. Value-producing helpers
  write their results DIRECTLY into the activation slot they receive as
  an argument (no register marshalling); on trap, the helper has already
  built the full exception ObjRef (JVM-pinned message included) into
  `act->pending_exc`, and the body branches to its deopt thunk

The trap-side effect means helper-trap bodies never continue executing.
Trap kinds are named constants (`TrapKind`): Npe, ArrayBounds,
Arithmetic, ClassCast, NegativeArraySize, ArrayStore, Monitor,
StackOverflow, NoSuchMethod — the exception classes T0 raises, with
JVM-pinned messages built inside the helper (it owns `Runtime&`).

Calls (all invoke* forms) route through `t1_call`: arg2 = arg base slot
offset, arg3 = arg count, arg4 = packed (method id or site id + call
flavor). The helper executes the callee via the engine (compiled or
T0-resume), writes the result Value to the caller's dst slot, and runs
the caller-side exception algorithm on callee throws (SS9).

---

# SS9. Deopt and exceptions

Deopt reasons map to the plan's `DeoptReason`:

- **Trap**: an instruction trapped (idiv by zero inline; helper traps).
  The thunk carries the trap site; the engine builds the T0 `Frame`
  (locals/regs from slots, monitors LIFO, pc = trap rbc pc, pending
  exception set) and calls `Interpreter::resume`. T0's exception
  algorithm handles in-method catch or unwinding (the deopted invocation
  finishes on T0 — Amendment A).
- **Exception dispatch (uniform, engine-side)**: ANY deopt with
  `pending_exc` set enters the same path - athrow traps, helper traps,
  and callee-threw-past-the-call. The engine checks the frame's plan
  `exception_edges` at the deopt rbc pc (table order, catch-type match
  through `Runtime::isAssignableFrom`): covered -> reset all regs to
  Bottom, regs[0] = the exception, re-enter compiled code at the
  handler's ENTRY STUB (the catch keeps executing on T1); not covered ->
  deopt to T0 with the pending exception (T0's exception algorithm
  releases the frame's monitors LIFO and unwinds further). No longjmp,
  no setjmp: the trap-kind return through the thunk is the whole
  transfer mechanism.
- **Guard** (`guard_non_null`): body tests the slot tag; failure branches
  to the guard's deopt thunk (the plan's RBC-declared DeoptId); the
  engine deopts WITHOUT a pending exception (a guard failure is not a
  Java exception — T0 re-executes from that pc).

The engine's frame depth budget `kT1MaxCallDepth` (512, Rule 23) turns
native-stack exhaustion into a Java-visible StackOverflowError through
the same trap path (never a C++ crash — the T0 discipline, Rule 47).

---

# SS10. The code cache

`Tier1::codeFor(method_index)`: compile plan -> instantiate -> cache.
Keyed by (program pointer, method index, set version, archive version).
v1: entries live for the engine's lifetime; the arena never evicts
(Stencil Rule 9 counters — code bytes, instances, patches, thunks — are
reported via `Tier1::stats()` and never act; eviction + throttling is
the documented future). Cache hits are pointer-stable; the engine
shares one cache across an execution (Rule 15: `CompiledCode*` is
stable, referenced by index nowhere — it is engine-internal and never
escapes as a handle).

---

# SS11. Determinism and testing

- stencilgen is deterministic: identical manifest -> byte-identical
  archive (Rule 124; golden-tested by regenerating and comparing).
- Instantiation is deterministic: identical plan + archive + method ->
  identical code bytes (tested byte-for-byte across runs).
- The differential law (Rule 36 form): every tests/codegen corpus
  program executes on T0 AND on T1; stdout, exit status, return value,
  and the printed exception for throwing programs must be IDENTICAL.
  The corpus is the union of tests/interp/corpus plus T1-specific
  programs (calls, fields, monitors, deopting traps).
- ASan/UBSan clean under `-fno-sanitize-recover=all`; zero warnings at
  `-Wall -Wextra -Wpedantic -Wshadow` (docs/cpp26_standards.md).

---

# SS12. Refusals (Amendment A: always safe to abandon)

Instantiation refuses — the method simply stays on T0 — when: version
mismatch (set vs plan vs archive), an op's archive record is missing or
invalid, a hole cannot be filled (width, range, unresolvable cp entry,
a store to a builtin static), a branch target is not an instance start,
a trap-capable instance has no covering DeoptPoint, a switch payload is
malformed or exceeds `kMaxSwitchCases`, W^X publication fails, or the
code budget `kMaxCodeBytes` is exceeded. `multianewarray` refuses at the
PLAN stage (the manifest marks it NeedsRuntimeFeature - the v1 archive
gap; T0 executes it correctly, so the fallback is transparent). Refusals
carry a human-readable reason surfaced through `Tier1` stats; they are
NEVER silent and never partial (a refused method has no CompiledCode
entry).
