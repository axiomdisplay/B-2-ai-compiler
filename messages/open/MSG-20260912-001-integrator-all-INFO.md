---
id: MSG-20260912-001
type: INFO
from: integrator
to:
  - all
severity: P3
status: OPEN
laws_refs:
  - Rule 55
  - Rule 64
  - Rule 145
  - Rule 150
related_prs: []
related_tests: []
created: 2026-09-12
---

## Summary

The project owner has supplied two new authoritative reference documents,
now archived verbatim under `docs/laws/`:

- `docs/laws/turboscript_compiler_laws.md` — the TurboScript Compiler Laws
  & Architecture Specification (JavaScript-engine sibling of the B-2 law
  set; 150 rules, Parts 0–XVI).
- `docs/laws/turboscript_master_architecture_spec.md` — the TurboScript
  Uncompressed Master Architecture Specification (C++26 core data
  structures, exhaustive node-kind taxonomy, all 53 optimization passes,
  FrameState / deoptimization protocol, C++26 implementation constraints).
- `docs/laws/README.md` — archive index, standing directive, and the
  precedence rule between `docs/laws.md` (B-2, normative for B-2 work) and
  the TurboScript archive.

This addition was made under direct owner (integrator) instruction: every
law/spec MD the owner provides is archived byte-for-byte in `docs/laws/`.

## Evidence

- `git log` commit adding `docs/laws/` on 2026-09-12.
- File sizes match the owner-supplied originals (78,458 and 50,383 bytes;
  2,048 and 543 lines) — no compression or paraphrasing applied.

## Impact

- **No code impact.** No file under any team's write list was touched.
- `docs/laws.md` (B-2) remains untouched and normative for all B-2 teams.
- Teams working on cross-cutting design (guards, FrameState, PEA, deopt,
  GC integration, security) gain a sibling reference specification.

## Requested Action

None. Informational only. Teams should treat `docs/laws/` as a read-only
shared archive per the ownership map comment on shared paths; any proposed
change to archived documents goes through integrator approval, per
`docs/laws/README.md` and Rule 145.

## Boundaries

The integrator agent team will not modify `docs/laws.md`, any team's write
paths (`compiler/`, `include/`, `tests/`, team contract docs), or CI
configuration. This message and the `docs/laws/` archive are the only
additions.

## Response

```text
status:
responder:
date:
notes:
```
