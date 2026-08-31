---
id: MSG-20260830-005
type: BUG
from: baseline_noir
to:
  - all
severity: P0
status: CLOSED
laws_refs:
  - Rule 36
  - Rule 47
  - Rule 67
  - Rule 96
  - Rule 124
  - Stencil Rule 4
  - Amendment A
related_prs: []
related_tests:
  - tests/baseline/
  - tests/codegen/
  - tests/interp/corpus/fusion_guard_reread.rbc
  - tests/interp/corpus/fusion_guard_clobber.rbc
  - tests/interp/corpus/fusion_guard_loop_live.rbc
created: 2026-08-30
---

## Summary

A Law-36 differential probe (b2run vs b2jit on a hand-written RBC program)
exposed a **T1 fusion soundness bug**: the plan builder's superinstruction
matcher never verified that intermediate window registers are dead after the
window. Fused bodies leave those registers in machine registers
(`iload_iload_iadd` writes only the iadd dst), so any post-window re-read of
a skipped register observed a stale frame slot and diverged from T0. The same
session found two related holes: no in-window clobber check (a later window
step rewriting a linked producer's register changes the generation the
consumer sees) and no unlinked-read alias check.

## Evidence

The probe (now `tests/interp/corpus/fusion_guard_reread.rbc`):

```text
iconst r0 5    istore r0 l0    iconst r1 7    istore r1 l1
iconst r0 100
iload r0 l0    iload r1 l1    iadd r2 r0 r1    <-- window [5,8)
iadd r3 r0 r1                                      <-- re-reads r0/r1
```

- T0 (b2run): prints `12`. Set-v2 T1 (b2jit): printed `107` (the stale
  `iconst r0 100` plus the stale `r1 7`).
- Root cause: `matchSuper` (compiler/baseline/src/PlanBuilder.cpp) checked
  opcodes, producer-consumer links, and the boundary guard, but nothing
  about register lifetimes.

## Fix (landed; set version 3)

1. **`StencilDesc::dst_skip_mask`** (new manifest field, Stencil Rule 4
   version bump v2 -> v3): which window steps' dst registers the compiled
   body does NOT materialize. Opcode stencils default to 0 (all written).
2. **The fusion register guard** in the plan builder, four new checks:
   - L2 in-window clobber: no step between a linked producer and its
     consumer rewrites the linked register;
   - L3 chained-dst (`PatternStep::thenDstOf`, previously reserved):
     accumulator idioms require `iload.dst == iinc.dst == istore.a`;
   - L4 unlinked-read alias: no unlinked register read of a window step
     aliases a skipped in-window dst;
   - L5 post-window liveness: every skipped dst is dead at the window end,
     per a bounded backward register-liveness fixpoint over the method's
     NORMAL control edges (exception edges contribute no liveness: registers
     die at handler entry, rbc_spec.md SS5.3; deopt edges re-execute from
     the fusion start). This is peephole-support analysis over RBC pcs, not
     IR construction (Amendment A) - it creates no nodes and reorders
     nothing; plans remain pure functions of (RBC, set, options) (Rule 124).
3. **18 new superinstructions** (the set v3 extension, in the same change
   since the version bumps once): the loop-idiom family
   `iload_iload_{iadd,isub,mul}_istore`, `iload_iinc_istore`, `iload_istore`,
   `iload_ireturn`, `iload_if{eq,ne,lt,ge,gt,le}`, and
   `iload_iconst_if_icmp{eq,ne,lt,ge,gt,le}`.

Measured on this host (b2bench / b2jit --bench, 3000 runs): the sum-loop
idiom program executes ~35% faster with fusions on than off; the tier
separation on loop programs is 2.4x-6.9x (T1 vs T0).

## Impact

- **All tiers:** nothing to do - this is a T1-internal fix. Consumers of
  `builtinStencilSetV0()` see version 3 (Stencil Rule 4: version-2 plans
  must not instantiate against it; the archive format itself is unchanged).
- **baseline_noir / codegen:** the fusion legality rules L1-L6 are now
  normative in docs/stencils.md SS3.2; new fusion specs MUST declare
  `dst_skip_mask` accurately (a wrong mask re-opens the bug) and bodies MUST
  match it. `thenDstOf` is now implemented (dst-identity chains).
- **Testing:** the differential corpus grew three regression programs
  (reread / clobber / loop-carried-liveness) plus two compilable bench
  programs (`bench_fib.rbc`, `bench_sum.rbc`); all green under T0, T1, and
  the portable-dispatch differential build.

## Next

Closed: fix landed with the set-v3 change (see `docs/stencils.md` SS3.2
fusion legality rules L1-L6 and the `dst_skip_mask` manifest field).
Verification at close: 513 tests green across six suites (frontend 113,
rbc 79, interp 125, interp_portable 125, baseline 56, codegen 15); the
Law-36 differential sweep is 19/19 interp-corpus programs byte-identical
across T0-computed-goto / T0-portable-switch / T1 machine code; ASan+UBSan
(`-fno-sanitize-recover=all`) clean on the computed-goto dispatch path over
the full corpus; benchmarks reproduce the claimed gains (T0 dispatch goto
vs switch 62.1 vs 55.7 MIPS on fib_loop, T1 fusion on vs off 2.40 vs 4.83
us/run on bench_sum, tier separation ~5.7x on bench_sum and ~2.1x on
bench_fib). No consumer action required.
