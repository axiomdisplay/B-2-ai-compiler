# Law & Specification Archive

This directory is the verbatim archive of law and specification documents
provided directly by the project owner. Files here are stored uncompressed
and unmodified — they are authoritative reference material.

## Standing Directive (owner instruction, 2026-09-12)

Every law / specification Markdown document supplied by the owner is to be
archived in this directory, byte-for-byte, alongside every future MD the
owner provides. No compression, no summarization, no paraphrasing of
archived documents (Rule 55 — No Implicit Knowledge Transfer; Rule 64 —
No Documentation Debt).

## Archive Contents

| Document | Target | Status |
|---|---|---|
| `turboscript_compiler_laws.md` | TurboScript — high-performance JavaScript engine (Sea of Nodes, 4 tiers, 150 rules, Parts 0–XVI) | Stable, owner-supplied 2026-09-12 |
| `turboscript_master_architecture_spec.md` | TurboScript — uncompressed master architecture: C++26 data structures, exhaustive node taxonomy, all 53 passes, FrameState/deopt protocol, C++26 constraints | Stable, owner-supplied 2026-09-12 |

## Relationship to `docs/laws.md`

- `docs/laws.md` (repo root docs) remains the **B-2 Java runtime** law set —
  the normative rulebook for the nine B-2 teams, as amended
  (Amendment A, Amendment B, Special Pass Laws).
- The TurboScript documents in this directory are the **JavaScript-engine
  sibling specification**, supplied by the owner. They are reference law for
  any TurboScript-targeted work and cross-reference material for B-2 design
  decisions (same rule skeleton: speculation laws, FrameState discipline,
  anti-slop laws, tiering, GC integration, security, governance).

If a B-2 rule in `docs/laws.md` conflicts with a TurboScript law in this
archive for B-2 work, `docs/laws.md` wins. TurboScript documents govern
TurboScript-targeted work only.

## Amendment Path

Changes to documents in this directory follow the shared-path rule: they
require direct owner (integrator) approval and must be announced through
the message system (`docs/teams/messaging.md`). Silent edits are forbidden
(Rule 145 — Exception Register applies to any deviation).
