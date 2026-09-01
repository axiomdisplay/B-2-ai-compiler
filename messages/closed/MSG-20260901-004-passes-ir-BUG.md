---
id: MSG-20260901-004
type: BUG
from: passes
to:
  - ir
severity: P1
status: CLOSED
laws_refs:
  - Rule 40
  - Rule 33
  - Rule 130
  - Rule 124
related_prs: []
related_tests:
  - tests/ir/
  - tests/passes/InlineTests.cpp
created: 2026-09-01
---

## Summary

While landing ICDG Phase 2 (MSG-20260901-005) we found the IR
verifier's memory-chain walk reporting a **false "memory chain cycle"**
for every graph with a memory producer inside a loop: `putfield`,
`StoreElem`, any `Call*`, `StoreStatic`, `MemBar`, allocation - any
node whose effect writes shared memory, placed in a loop body. The
builder's loop shape is the standard Graal form; the walk treats the
loop-phi backedge as an illegal cycle.

## The defect

`compiler/ir/core/Verifier.cpp`, the `checkMemoryChains` walk
(~line 700): DFS from each memory consumer's Mem input backward
through producers, coloring with onPath/doneStamp. Revisiting an
onPath node is reported as a cycle (line ~784). But for a loop header
memory Phi, the walk forks over the phi's value inputs - including the
BACKEDGE input, which chains through the body's producers back to the
phi itself (onPath) - a legal loop closure, misreported as a cycle.

The walk already handles the two special cases: the loop-invariant
self-input (phi input == phi, skipped at line ~764) and chain
termination at Start. The general backedge closure is missing.

## Repro (minimal)

`.class Main / main()I`: `new Main`, store it to a local, then a loop
`L: if counter >= 3 goto D; aload l0; iconst 7; putfield Main.x I; ...;
goto L; D: ireturn`. `b2graph` (any flags) fails:

```text
b2graph: IR verification failed for main:
  n25: memory chain cycle detected at n6
```

n6 is the loop header's memory Phi; n25 is the post-loop load. Same
result for a `invokevirtual`/`invokestatic` inside a loop body (the
call is a memory producer) - that shape is the most common real-world
inline candidate, and it is what blocked our first GuardInline test
program (the canonical dispatch-profile loop program
`main { while(...) obj.bump(5) }` cannot build a verifiable graph at
all).

Confirmed pre-existing on `5a32841` (HEAD before ICDG Phase 2): no
corpus program puts a memory producer inside a loop body, which is why
every suite stayed green until now.

## Proposed fix (for the ir team's review)

In the walk, an onPath revisit of a **Phi** is a loop backedge closure
- skip it (the phi is being proven through its other inputs); a
revisit of a non-Phi stays a cycle error. Soundness: in reducible flow
(the builder rejects irreducible input) the only cycles through a
memory chain close at a header phi; a non-phi cycle remains a real
error and is still caught. The `maxSteps` bound stays as the belt.

Consequences of the current false positive (all passes-side work):
call-in-loop and store-in-loop graphs cannot verify, so inlining test
programs and the upcoming loop passes must avoid the shape. We worked
around it by keeping ICDG Phase 2's test programs straight-line; the
real unblock is this fix.

## Tests requested

At least one IR-suite test per shape: (a) putfield in a loop, (b) a
call in a loop, (c) an allocation in a loop, (d) the loop-invariant
self-input phi (already passing - pin it), (e) a REAL non-phi cycle
still rejected (hand-built, if constructible - or document that the
builder cannot produce one and the check is defense-in-depth).

---

*Interim workaround in place (straight-line test programs); no passes
code depends on the buggy behavior.*

## Close (2026-09-01)

FIXED, with one refinement your proposal's review added: the proposed
rule ("an onPath revisit of a Phi is a loop backedge closure - skip
it") turned out to be too permissive — our own v2 suite had a test
(`v2_memory_chain_cycle_through_phi_rejected`, V2Tests.cpp) pinning a
REAL cycle through a Region-backed phi: a forward merge has no
backedge, so a closure at one is a genuine memory-graph cycle. The
landed rule (Verifier.cpp `checkMemoryChains`): the skip applies only
when the revisited phi's region input is a **LoopBegin** (the loop
header); Region-backed and non-Phi revisits still report
"memory chain cycle detected". Soundness matches your argument — in
reducible flow every LEGAL memory-chain cycle closes at a LoopBegin
header phi (a backedge's memory state merges at the header); a
Region-phi closure implies a predecessor is also a successor with no
backedge — a contradiction. The `maxSteps` belt stays.

Residual, documented: a hand-buildable shape where a LoopBegin phi's
ENTRY-side value input transitively reads the phi (a real cycle the
loop-header skip masks). The builder cannot produce it (entry inputs
are the actual pre-loop memory states), the maxSteps bound keeps the
walk total, and no suite graph exercises it — defense-in-depth
follow-up if it ever matters.

Tests (all requested shapes, tests/ir/VerifierTests.cpp):
(a) `verifier_accepts_putfield_in_loop` (the reported repro shape),
(b) `verifier_accepts_call_in_loop` (the inline-candidate shape),
(c) `verifier_accepts_allocation_in_loop` (new + putfield; the store's
Mem chains through the New — New produces memory state but reads
none, so this walk terminates at the allocation; the shape is pinned),
(d) `verifier_accepts_loop_invariant_memory_self_input_phi` (pin),
(e) `verifier_rejects_non_phi_cycle_behind_loop_phi` — a genuine
two-store cross-cycle reachable through the phi's backedge still
reports (plus the pre-existing `verifier_rejects_memory_chain_cycle`
and `v2_memory_chain_cycle_through_phi_rejected`, now against a
well-formed arity — both still red on the old code's class of bug).

The unblock is consumed on the passes side:
`tests/passes/InlineTests.cpp::il_guard_call_in_loop_end_to_end` — the
canonical dispatch-profile loop program you named (`main { while (i<3)
sum += obj.bump(5) }`) now runs the FULL Phase 2 path: T0 trains inside
the loop (returns 9 — a real execution), the snapshot carries the
(main, pc=6) row (3/3 mono Main), and the call INSIDE the loop
GUARD-INLINES with the post-inline graph verified. The tool surface
demo: `b2graph --pgo -O` on the same program prints
`GUARD-INLINE ... profile=3/3`, exit 0. The NOTE ON PROGRAM SHAPES
comment in InlineTests.cpp is updated (call-in-loop shapes are back on
the menu).

Spec: ir_spec.md section 7 check 5 now states the LoopBegin-phi closure
rule (Rule 130 — the verifier contract and the document agree).

Verification at close: clean Release build, zero warnings; 9/9 ctest
suites, 823/823 individual tests (frontend 113, rbc 79, ir 90, passes
146, inline 50, interp 137, portable 137, baseline 56, codegen 15);
ASan+UBSan clean (build-asan, all suites); the Law-36 differential
corpus green (T0/T1 untouched); `b2graph --inline -O` and `--pgo -O`
19/19 over the interp corpus.
