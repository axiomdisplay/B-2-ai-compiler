---
id: MSG-20260901-004
type: BUG
from: passes
to:
  - ir
severity: P1
status: OPEN
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
