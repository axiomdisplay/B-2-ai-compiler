# B-2 RBC-to-IR Graph Builder (v1)

```text
Normative reference: docs/laws.md (Rules 5, 7, 15, 16, 23, 31, 33, 40-42,
121-126, 130; Amendments A and B), docs/rbc_spec.md, docs/ir_spec.md (v2),
docs/interp_contract.md, docs/deopt_backend.md.
If this document conflicts with docs/laws.md, laws.md wins.
```

Team: Passes Team (`docs/teams/passes-team.md`). The builder is the T2/T3
pipeline's front door — suite items 1-6 of the charter's appendix (RBC
parsing, bytecode type stabilization, method entry graph construction,
exception edge construction, FrameState seeding; items 3/7-10 — profile
import, OSR, indy modeling, MethodHandle lowering, static-field
dependencies — await their runtime prerequisites). Implementation:
`compiler/passes/src/GraphBuilder.cpp`, API `include/b2/passes/GraphBuilder.h`,
tool `b2graph`.

The builder is a TOTAL function from one verified `rbc::Method` to one
`b2::ir::Graph`: precondition `rbc::verify(m)` passed; malformed or hostile
input yields bounded diagnostics, never UB. Every produced graph must pass
`ir::verify` (Rules 40, 126) — the corpus sweep test enforces this over all
19 interp-corpus programs.

---

## 1. SSA discipline (slots are values)

`iload`/`istore`/`imove` and their typed siblings contribute **no IR
nodes**: locals `l0..lM-1` and registers `r0..rN-1` are SSA values. A load
aliases the slot's current definition to the destination register; a store
aliases the source register's definition to the local slot. Type
correctness is the RBC verifier's proof (the input contract); the builder's
own abstract interpretation (section 6) re-derives the types as a defense.

- Merges: a `Phi` per slot whose incoming definitions differ (Undef inputs
  legal — the "Bottom on this path" marker; the RBC join(Bottom, ·) =
  Bottom discipline mirrored structurally).
- Loops: `LoopBegin`/`LoopEnd` per natural loop; slots modified anywhere in
  the loop (nested bodies included) get loop phis. A slot whose backedge
  definition is the phi itself carries the **self-input** loop-invariant
  marker (ir_spec §4.9). Backedge inputs are patched after the single
  materialization pass; loop-exit edges get `LoopExit` chains (innermost
  first, one per crossed loop boundary).
- Trivial-phi cleanup (builder hygiene, Graal's own): a phi whose non-self
  inputs are all one node is that node — `replaceNode` rewires FrameStates
  automatically (Rule 14 doing real work). A loop with no memory writes
  collapses its memory phi back to the incoming state this way.
- Reducibility: materialization order is a topological sort over forward
  edges; a forward-edge cycle (irreducible flow) refuses the method.
  javac-shaped bytecode is always reducible.

## 2. Control and memory construction

- Fixed nodes chain control (ir_spec v2 §3): guards, loads, stores,
  barriers, monitors, allocations, `ClassInit`, `CheckCast`, and calls each
  consume the current control and produce the next. Branching: `If` +
  `IfTrue`/`IfFalse` with **IfTrue always the taken edge** (conditions are
  composed so truthiness = branch); `Switch` + one `SwitchCase` per table
  entry (payload = ordinal, in table order) + `SwitchDefault`; `Goto` is a
  bare edge. Blocks ending without a terminator fall through.
- Memory chains through writers (`StoreField`/`StoreElem`/`Monitor*`/
  `New*`/`ClassInit`/`Call*`); readers point at the current state. Merges
  are memory Phis (the exception path of a call uses the `Call` node as its
  memory state — the call's effect, exceptional flavor).
- `Start` is the origin of control and memory; parameters are `Parameter`
  nodes typed from the descriptor (receiver first for instance methods);
  every other local and every register starts `Undef`.

## 3. Guard placement (traps that deopt)

Every non-call trap deopts to T0, which re-executes the faulting
instruction and raises the Java exception there — behavior-preserving by
construction (deopt-to-T0 re-execution is always observably equivalent).
Guards gate control in front of the protected operation and share the
instruction's FrameState:

| Trap | Guard shape |
|---|---|
| NPE (field/array/monitor receivers, dispatching call receivers) | `Guard(NullCheck, Not(IsNull(x)))` |
| AIOOBE (array loads/stores) | `Guard(BoundsCheck, AndI(GeI(idx, 0), LtI(idx, ArrayLength(a))))` |
| ArithmeticException (idiv/irem by zero) | `Guard(ZeroCheck, NeI(divisor, 0))` |
| ArithmeticException (ldiv/lrem by zero) | `Guard(ZeroCheck, NeI(L2I(divisor), 0))` |

The long-division guard narrows through `L2I`: `NeI` takes Int operands
and there is no `NeL` kind. `L2I(x) == 0` for x = 0 OR x a nonzero
multiple of 2^32, so the guard deopts on a non-trapping divisor — always
observably equivalent (deopt-to-T0 re-execution), astronomically rare.
The `resultTypeOf(CmpL)` classification that blocked the clean
`CmpL(divisor, 0L)` comparison is FIXED (MSG-009 closed: CmpL now types
Int and feeds Int slots, verified); the L2I shape stays the shipped v1
form — sound, tested, and byte-pinned — with the CmpL switch listed as
an optional passes-team cleanup.

Divide-by-zero on floats/doubles needs no guard (IEEE semantics). `checkcast`
keeps its `ExceptionThrow` effect node (a Java exception, not a deopt);
`aastore`'s ArrayStoreException is a runtime store check (the `StoreElem`
effect models it; the trap path is codegen's concern). Division,
bounds-check, and null-check ELIMINATION are later passes (items 15-18),
not builder concerns — the builder always emits, soundly.

## 4. FrameState policy (Rules 5, 126)

FrameStates are seeded exactly at deopt-capable points: every `Call`
(mandatory trailing input), every `Guard`, every `LoopExit`, and every
`Deopt`. Inputs are ALL frame slots, locals first then registers
(`numLocals + numRegs` edges) — the T0 frame contract
(interp_contract.md §1): `Undef` slots materialize as T0's Bottom-tagged
Values, with the correct runtime tag per slot. Guards and the guarded op
of one instruction share the node. The `FrameStateDesc` records the
method id, the pc of the guarded/calling instruction (re-entry point),
no caller (no inlining in v1), no virtual objects.

Deopt ids: builder-allocated ids start at `kBuilderDeoptIdBase`
(0x80000000) so they never collide with the stream's own
`deopt_trap`/`guard_non_null` ids; stream ids ride verbatim in payloads.

## 5. Exception policy v1 — every exceptional continuation deopts

Uncaught exceptions from calls: `CallExcept → Unwind` (the frame unwinds to
its caller with the exception value). Uncaught `athrow`:
`Guard(NullCheck)` (a null would be an NPE-at-this-pc instead of the thrown
value) then `Unwind` with the value. Caught exceptions from calls: the
deopt doc's class 2.3 — `CallExcept → Deopt` sharing the call's
FrameState; the Deopt's control being the CallExcept signals the class and
the CallExcept's VALUE is the exception (transport convention for the deopt
stub). Caught `athrow`: `Deopt` — T0 re-executes the athrow (idempotent;
the exception rides in the FrameState's operand slot) and dispatches
through THE EXCEPTION ALGORITHM with full type info. Handler bodies are
therefore unreachable in v1 graphs and do not materialize; "cold exception
handler" is a sanctioned uncommon-trap example (deopt_backend.md §2.2).

**The follow-up** (documented path, not v1): in-graph handler compilation —
exception edges into handler-entry `Region`s, `LoadException` feeding r0,
handler-local phis, and instanceof dispatch chains ordered by handler-table
priority — lands when T2 codegen wants compiled handlers.

## 6. Bytecode type stabilization (suite item 2)

The same join abstract interpretation the RBC verifier runs (§5.2/§5.3 of
rbc_spec.md), including its handler-seeding discipline: covered call pcs
seed handler entries with r0 = Ref and locals = the pre-instruction types.
The builder needs it for:

1. **Quickened ops** — they carry resolved offsets/ids instead of
   descriptors, so their dst types are the pre-established register types
   (rbc_spec §6.2); the builder refuses a quickened op whose dst is
   Bottom (an unverified stream).
2. **The type-consistency defense** — at every FrameState, each captured
   def's IRType must be assignable to the stabilized slot type; a mismatch
   is an "internal:" diagnostic (the builder's own Law-36-style guard: the
   SSA construction cannot diverge from the verified types without the
   build refusing).
3. **Receiver defense** — dispatching calls whose receiver stabilizes to a
   non-reference refuse (the IR verifier would reject `IsNull(int)`).

## 7. Class-initialization triggers (T0 pins)

The builder emits `ClassInit` exactly where T0 calls `initClassIfNeeded`
(v0 pins, including the two v0 divergences): `getstatic`, `putstatic`,
`invokestatic` (+ quick), `new`, and `ldc`-of-Class on the cp-named class.
`invokespecial` and `anewarray` deliberately do NOT trigger (T0 v0 pins;
revisit when the class model lands). Elimination of redundant inits is a
later pass.

## 8. Quickened ops

`getfield_quick`/`putfield_quick`: FieldId payload = the resolved byte
offset verbatim (opaque to the IR; the runtime interprets). Quick calls:
MethodId payload = the quickened id verbatim, receiver guards and (for
static) the ClassInit trigger as usual, dst typed from the stabilized
pre-type. No descriptor exists, so call args use the stream's count; a
never-written argument refuses (unverified stream).

## 9. Call lowering

`invokestatic`/`invokespecial` (+ quick) → `CallStatic` (direct dispatch);
`invokevirtual` → `CallVirtual`; `invokeinterface` → `CallInterface`;
`invokedynamic` refuses. Arguments are the descriptor's declared
parameters (plus the receiver): T0 copies the stream's full b-window into
callee locals, but the tail beyond the declared parameters is unread —
passing it would feed `Undef` into Call args. The receiver null guard
precedes dispatching calls (a null receiver NPEs at the call site in T0;
the guard deopts to exactly that). Every call carries `CallExcept`; void
calls leave dst unset (payload2 = Bottom).

## 10. Refusals (v1 scope)

| Input | Diagnostic |
|---|---|
| `synchronized` methods | FrameState has no held-monitor record (interp contract §1) — deferred |
| `invokedynamic`, `multianewarray`, `guard_class`, `ldc` of MethodType/MethodHandle | no runtime / v0-verifier-only ops |
| irreducible control flow | no topological materialization order exists |
| abstract/native/empty bodies, stale graph, out-of-range operands, malformed switch tables, bad handler ranges, quickened dst/arg never written, primitive receiver | defensive totality |

`safepoint_poll` is dropped: the codegen team owns T2 poll placement (its
charter); the RBC op is the hook for tiers without their own polls. OSR
entry construction (suite item 7) arrives with the tiering loop.

## 11. Errata (T2-IR3)

The v2 arithmetic lowering indexed its kind tables by `op - Iadd`/`op -
Ladd` without placeholder rows for the guarded div/rem/neg entries at
offsets 3-5: `Ishl/Ishr/Iushr` lowered to the bitwise kinds and
`Iand/Ior/Ixor` (and the long twins) read past the array - an
out-of-bounds read that produced Start-kind nodes. The tables now carry
placeholder rows and the opcode layout is pinned by `static_assert`s.
Found by the pass suite's bitwise tests (the interp corpus has no
bitwise programs); the long zero-guard's operand-type violation is fixed
in the same pass (see the guard table above).

## 12. Determinism (Rule 124)

Fixed successor orders (taken edge first, switch cases in table order),
blocks in pc order, Kahn tie-break by block index, memoized constants in
first-encounter order, one deopt-id counter, `CounterResolver` interning in
encounter order: the same (method, resolver ids) produces identical node
ids, printed dumps, and serialized bytes — pinned by tests (build twice,
compare bytes; the corpus sweep does it per method).

## 13. The inline-build seam (inlining v1)

The builder has a second entry point, `detail::buildInlineBody`
(PassInternal.h): the same RBC->IR lowering re-runs over a callee
INSIDE the caller's graph (Graal-style), wired to one call site.
`docs/inlining.md` is the normative contract for the driver that calls
it; the builder-side differences are:

- **Entry**: no `Start`, no `Parameter` nodes. The entry control and
  memory are the call site's predecessors (`C.input(0)`,
  `C.input(1)`); the callee's parameter locals (receiver first) are
  the call's argument defs, remaining locals and all working
  registers start `Undef`; a loop header at pc 0 seeds its entry edge
  the same way. The graph is NOT required to be fresh.
- **Returns**: return instructions record exits —
  `(control, memory, value)` triples — instead of creating `Return`
  terminals; the driver merges them at the call site.
- **FrameStates**: every callee snapshot chains to the call-site
  FrameState's descriptor (`FrameStateDesc.caller`, Rule 75); the
  deopt ids continue the caller graph's allocation (the caller's
  max + 1 seeds the counter).
- **Exception escapes**: `coveredByHandler(pc) || siteCovered` governs
  both the athrow and the call continuation — a covered CALL SITE
  routes the callee's escaping exceptions to deopts (the shapes are
  the existing conventions; inlining.md section 4 has the soundness
  argument), an uncovered site keeps the `Unwind` with the callee's
  exception value.
- **Trivial-phi collapse is skipped** in inline mode (it is a
  caller-wide sweep; the pipeline's fusion key owns post-inline
  cleanup), and `scanStructure`'s fresh-graph check does not apply.

The trial build (the same body through the ordinary `buildGraph` into
a scratch graph) proves buildability and counts return terminals; the
inline build is the same lowering with the different wiring, so
determinism carries over unchanged.
