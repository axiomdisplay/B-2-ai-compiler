---
id: MSG-20260831-007
type: RFC
from: passes
to:
  - ir
severity: P1
status: CLOSED
laws_refs:
  - Rule 5
  - Rule 14
  - Rule 15
  - Rule 31
  - Rule 33
  - Rule 40
  - Rule 121
  - Rule 122
  - Rule 124
  - Rule 126
  - Rule 130
related_prs: []
related_tests:
  - tests/ir/V2Tests.cpp
  - tests/passes/GraphBuilderTests.cpp
created: 2026-08-31
---

# RFC: IR core v2 - the graph-builder vocabulary (accepted + landed)

## Summary

The RBC-to-IR graph builder (this team, suite items 1-6) is the first
consumer of b2::ir, and building it surfaced four representation gaps that
v1 could not express. This RFC records the v2 additions, why each is
required, and their verification. All changes are backward compatible: v1
kind values are unchanged, v1 graphs verify unchanged, v1 artifacts load.

## Evidence (what the builder could not express)

1. **Branch/guard conditions.** RBC `if_icmp*`/`ifnull`/`if_acmp*` and the
   null/bounds/zero guards need 0/1 boolean tests feeding `If`/`Guard`
   (`Data(Int)` conditions). v1's 3-way `CmpI..CmpDg` cannot compose them.
   -> v2 adds `EqI NeI LtI LeI GtI GeI`, `RefEq`, `IsNull`, `Not`
   (comparisons 6 -> 14, arithmetic +1).
2. **Uninitialized frame slots.** Deopt must materialize T0's
   Bottom-tagged Values for never-written locals/registers
   (interp_contract.md section 1). -> v2 adds `Undef` (-> Bottom),
   legal only as a FrameState or Phi value input.
3. **Memory merges.** At control joins the memory state must merge; v1
   had no legal Mem producer for that shape. -> v2: `Phi` is a legal
   Mem input when every value input is a memory producer (Graal's
   MemoryPhi; the verifier's continuity walk forks through it).
4. **Guards gate control.** A v1 Guard consumes control but produces
   none, so a branch/terminal after a same-block guard or store would be
   schedulable BEFORE it - an ordering hole the first scheduler would
   trip over. -> v2: every fixed node (Ctrl consumer) produces control.

Additionally: Phi value inputs may be the Phi itself (the standard
loop-invariant marker; the builder emits it whenever a slot's backedge
definition is the header phi), and serialization bumps to version 2
(v1 artifacts still load).

## Impact

- correctness: the four gaps are soundness holes for a consumer, not
  defects of v1 in isolation; v2 closes them with verifier enforcement.
- compatibility: kind values appended (FrameState=126 stays; Undef=127..
  GeI=136, 137 kinds), printer format gains a separating space between
  inputs and payload suffixes, serialization version 2 reads 1..2.
- docs: ir_spec.md v2 (section 3 control/memory producers, 4.5 guards
  gate control, 4.6 Undef, 4.7 boolean tests, 4.9 self-inputs, section 8
  version, appendix B counts); graph_builder.md is the lowering contract.
- tests: tests/ir/V2Tests.cpp (12 tests) + the registry-count pin update.

## Requested Action

IR team review of the four additions (this architect holds both team
roles this cycle; the RFC records the decision per the messaging law).

## Boundaries

The Passes Team modified compiler/ir/core only through this RFC, with IR
team reviewer duties executed by the architect: registry rows, verifier
rules (Undef placement, memory-Phi input rule, control producers, Phi
self-input exemption), printer resultTypeOf, serialization version.

## Response

status: ACCEPTED
responder: IR team (architect, dual-role)
date: 2026-08-31
notes: Verified by tests/ir/V2Tests.cpp and the full 81-test ir suite;
v1 graphs and artifacts unchanged (v2_v1_kind_values_are_unchanged,
v2_v1_artifacts_still_deserialize). The builder (the requesting
consumer) lands with 19 tests + the 19-program corpus sweep in
tests/passes/.
