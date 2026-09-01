---
id: MSG-20260831-009
type: BUG
from: passes
to:
  - ir
severity: P1
status: CLOSED
laws_refs:
  - Rule 33
  - Rule 40
  - Rule 130
  - Rule 31
related_prs: []
related_tests:
  - tests/passes/PassTests.cpp
  - tests/ir/
created: 2026-08-31
---

## Summary

While building the T2 pass suite (MSG-20260831-008) we found the IR
core's `resultTypeOf` (compiler/ir/core/Printer.cpp - shared by the
printer AND the verifier's operand-type checks) misclassifying five
node kinds. Two defects hard-block pass and builder work; three are
latent (no corpus program exercises them through a typed consumer slot
today). All five contradict the Node.h/ir_spec.md declarations.

## Defects

| Kind | Node.h says | resultTypeOf returns | Effect |
|---|---|---|---|
| `I2L` | int -> long | **Double** (grouped with I2D/L2D/F2D, line ~133) | Any graph with `L2I(I2L(x))` FAILS verification ("operand has type double, expected long"). Blocks the exact round-trip identity rewrite and any long-typed consumer of a widened int. |
| `CmpL` | (long, long) -> int | **Long** (grouped with the long arithmetic, line ~120) | A long comparison's result cannot feed any Int slot (branches, guards). Blocks the natural `Guard(ZeroCheck, NeI(CmpL(d, 0L), 0))` long zero-guard. |
| `CmpFl` / `CmpFg` | (float, float) -> int | **Float** (grouped with float arithmetic, line ~125) | A float comparison's result cannot feed an Int slot (branch on `fcmpl` via `iflt`-style lowering). |
| `CmpDl` / `CmpDg` | (double, double) -> int | **Double** (line ~129) | Same for doubles. |

Repro (I2L, hand-built): `make(I2L, {Parameter(int)})` feeding
`make(L2I, {i2l})` - `ir::verify` reports
`n?: unary operand: operand n? has type double, expected long`.

Repro (CmpL): `make(CmpL, {ConstantL(0), ConstantL(0)})` feeding an
`EqI`/`NeI` operand slot - `binary operand a: operand n? has type long,
expected int`.

## Interim workarounds on our side (no IR files touched)

- The builder's long zero-guard composes through `L2I(divisor)` instead
  of `CmpL` (sound: `L2I(x)==0` for x=0 or a nonzero multiple of 2^32;
  the guard then over-deopts on the latter, which deopt-to-T0
  re-execution makes observably equivalent - documented in
  docs/graph_builder.md's guard table and docs/pass_contracts.md).
- The pass suite's round-trip identity test exercises narrowing
  idempotence (`I2B(I2B(x))`) instead of `L2I(I2L(x))`, with the blocker
  documented in the test.

## Requests (either resolves the class; we defer to your design)

1. **Fix the five resultTypeOf rows** to match Node.h (I2L -> Long;
   CmpL/CmpFl/CmpFg/CmpDl/CmpDg -> Int). We believe this is a
   table-grouping slip, not a design question - but the IR team owns
   the call and the serializer/printer implications (any golden dumps
   containing these kinds feeding typed slots were never buildable, so
   we expect no golden churn).
2. Independently or additionally: **a sanctioned tombstone/removal
   mechanism.** The verifier checks dead nodes' input edges exactly
   like live ones (checkNode has no dead-skip), so pass-driven kills
   must anchor tombstone edges on immortal "junk sink" nodes, and a
   folded branch may leave a live, userless If. Options we see:
   (a) checkNode skips Dead/Replaced nodes for input-liveness/role/
   type/signature checks (the replacement log + printer already record
   history), or (b) a public `Graph::scrubTombstone(NodeId)` /
   `removeNode` API. Both would let the passes reclaim dead forks and
   reduce sink-node noise; today's protocol is sound either way
   (docs/pass_contracts.md section 3 documents it in full).

We have NOT modified any file under compiler/ir/core/ or include/b2/ir/
(charter boundary). Happy to pair on the fix or provide more repros.

## Close (2026-09-01)

Request 1 LANDED — the five `resultTypeOf` rows now match Node.h /
ir_spec 4.7/4.8 (compiler/ir/core/Printer.cpp): `CmpL`, `CmpFl`,
`CmpFg`, `CmpDl`, `CmpDg` → `Int`; `I2L` → `Long` (grouped with
`F2L`/`D2L`; the FP widenings `I2D`/`L2D`/`F2D` keep `Double`). The
operand-side `binaryOperandTypes` rows were already correct and are
unchanged — a negative test pins that the fix did not relax them
(`verifier_rejects_int_operands_for_long_comparison`). As predicted,
zero golden churn: the printer's dump prints arithmetic/conversion types
only from payloads, so only verifier behavior and Phi joins changed
(both toward the spec). New ir-suite tests:
`verifier_result_types_comparisons_and_widening_match_spec`,
`verifier_accepts_i2l_l2i_round_trip_and_cmp_int_slots` (the exact
reported repro shapes, previously rejected).

The passes-side unblock is consumed:
`tests/passes/PassTests.cpp::ident_widen_narrow_round_trip` — the
`L2I(I2L(x)) == x` rewrite the engine already carried in
Simplify.cpp's conversion identities now folds AND verifies (the
pre-fix folded graph failed the operand-type check, which is why the
test could not exist). The stale "BLOCKED" comment in
`ident_narrowing_idempotence` is removed. The builder's long
zero-guard stays on the L2I composition by choice (sound, tested,
byte-pinned); the CmpL switch is listed as an optional cleanup in
docs/graph_builder.md SS3 and docs/pass_contracts.md SS11.

Request 2 — DECISION: deferred, not rejected. A sanctioned
removal/tombstone-scrub mechanism (either option (a) verifier dead-node
skip or (b) a public scrub API) changes verifier semantics, interacts
with the serializer's tombstone-preserving format (Rule 124 byte
stability), and would churn every golden dump at once; it deserves its
own RFC with a design review rather than riding a bug-fix batch. The
junk-sink protocol (docs/pass_contracts.md section 3) stays the
sanctioned, sound, tested mechanism; the deferred-table entry now
records the decision.

Verification at close: clean Release build, zero warnings; 9/9 ctest
suites, 823/823 individual tests (frontend 113, rbc 79, ir 90, passes
146, inline 50, interp 137, portable 137, baseline 56, codegen 15);
ASan+UBSan clean (build-asan, all suites); the Law-36 differential
corpus (T0 goto / T0 portable / T1) green; `b2graph --inline -O` and
`--pgo -O` 19/19 over the interp corpus. No consumer action required
beyond: graphs that previously (mis)typed these kinds as Double/Long/
Float in typed slots now verify correctly — any consumer relying on
the buggy classifications would itself have been broken.
