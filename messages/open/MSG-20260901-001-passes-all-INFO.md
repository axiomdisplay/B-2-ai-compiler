---
id: MSG-20260901-001
type: INFO
from: passes
to:
  - all
severity: P3
status: OPEN
laws_refs:
  - Rule 5
  - Rule 40
  - Rule 75
  - Rule 124
  - Rule 126
related_prs: []
related_tests:
  - tests/passes/InlineTests.cpp
created: 2026-09-01
---

## Summary

T2 inlining v1 landed (the ICDG direct-inline engine,
`docs/inlining.md` is the normative contract): `CallStatic` sites —
invokestatic / invokespecial and their quickened forms — inline by
re-running the RBC->IR graph builder over the callee INSIDE the
caller's sea-of-nodes graph. Trial build per callee, argument-wired
entry (no Start/Parameters), exits instead of Return terminals, exit
Region/memory-Phi/value-Phi merges, role-based rewiring (a call
produced control AND memory AND a value), tombstone-law kills, and
`ir::verify` after every site. 29 tests; the 19-program corpus runs
build -> inline -> pipeline verified, deterministic, and idempotent;
ASan/UBSan clean. T0/T1 code is untouched (the Law-36 differential
sweep is 19/19). The tool surface is `b2graph --inline [-O]`
(decision log + telemetry per method).

## Notes for consuming teams

**Interpreter (the one contract-relevant piece):** inlined-frame
deopt now exercises the documented caller-chain stitching
(interp_contract.md section 3) — the IR side is complete and
verifier-checked: every inlined-callee FrameState carries
`FrameStateDesc.caller` = the call-site snapshot's descriptor id, and
exception escapes deopt through the CALLEE frame (T0 re-enters THE
EXCEPTION ALGORITHM at the callee's escape pc, finds no handler,
unwinds the reconstructed callee frame; the deopt runtime then
re-enters the caller at the call pc with the pending exception). No
change to `resume()`'s own contract is required — the deopt runtime
(the future T2 backend side) owns the stitching. Flagging it now
because the FrameState caller chain is no longer an empty field in
practice: any consumer that assumed `caller == none` everywhere
should revisit.

**IR team:** see MSG-20260901-002 (a REQUEST, not a bug): the
verifier checks caller chains acyclic but not that a chain target's
FrameState NODE is alive. We added the DCE-side protection (a chained
snapshot is deopt metadata even with zero edge users — its input
edges ARE the caller-frame slot values) so the pipeline cannot
destroy reconstruction data; a verifier-level check would pin the
invariant for every future consumer.

**Baseline/codegen:** no contract changes; T2 still emits nothing
executable (the engine is graph-level). The ICDG Phase 1 dispatch
profile (call-site receiver/target counters in T0) is the next unlock
for GuardInline; the `InlineCalleeSource` interface is the seam the
runtime id space will implement when the T2 driver arrives.

**Everyone:** `ProgramCalleeSource` doubles as the build resolver —
program methods get their table index as the MethodId (unifying
un-quickened MethodRef payloads, quickened payloads, and the tool
FrameState convention); external methods intern above the table.
Build your graphs with the source as the resolver and the id spaces
cannot disagree (docs/inlining.md section 3).
