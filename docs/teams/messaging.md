# B-2 Inter-Team Message System

This document defines the only approved way for B-2 teams to communicate about bugs, contract changes, missing features, and integration issues.

All teams must follow this system.

Valid team keys: `frontend`, `interpreter`, `baseline_noir`, `ir`, `passes`, `regalloc`, `codegen`, `aot`, and `all`.

---

## Core Message Rule

A team may not modify another team's area.

If a team needs something from another team, it creates a message.

Messages are stored as Markdown files with YAML frontmatter.

---

## Message Directory

```text
messages/
  open/
  closed/
  advisories/
```

Active messages live in:

```text
messages/open/
```

Completed messages are moved to:

```text
messages/closed/
```

Advisory messages may be copied into:

```text
messages/advisories/
```

---

## Message File Name Format

```text
MSG-YYYYMMDD-NNN-<from>-<to>-<type>.md
```

Examples:

```text
MSG-20260830-001-passes-ir-BUG.md
MSG-20260830-002-codegen-regalloc-RFC.md
MSG-20260830-003-ir-all-ADVISORY.md
MSG-20260830-004-interpreter-baseline_noir-RFC.md
MSG-20260830-005-aot-passes-QUESTION.md
```

---

## Message Types

| Type | Purpose |
|---|---|
| `BUG` | A bug appears to exist in another team's area |
| `ADVISORY` | A team found a bug in its own area and is notifying others |
| `RFC` | Request for a contract or interface change |
| `QUESTION` | Clarification needed |
| `REVIEW_RESULT` | Formal reviewer decision |
| `INTEGRATION_REQUEST` | Request to coordinate cross-team integration |
| `WAIVER_REQUEST` | Request for an exception under the laws |
| `STATUS` | Progress or blocking update |

---

## Message Metadata

Every message must begin with YAML frontmatter:

```yaml
---
id: MSG-20260830-001
type: BUG
from: passes
to: ir
severity: P1
status: OPEN
laws_refs:
  - Rule 15
  - Rule 121
related_prs: []
related_tests: []
created: 2026-08-30
---
```

Valid statuses:

```text
OPEN
ACK
IN_PROGRESS
BLOCKED
RESOLVED
REJECTED
```

Severity must match the severity model in `docs/laws.md` (Part XV).

---

## Message Body Requirements

Every message must contain:

1. Summary
2. Evidence
3. Impact
4. Requested Action
5. Boundaries

The `Boundaries` section must state what the sending team will not touch.

---

## Bug Found In Another Team's Area

If Team A finds a bug that appears to belong to Team B:

- Team A must create a `BUG` message.
- Team A must include replay artifacts if available.
- Team A must not patch Team B's code.
- Team B owns the fix.

Example:

```text
The passes team found that IR effect-chain verification accepts a broken memory edge.
This belongs to the IR team. The passes team will not modify compiler/ir/core.
```

---

## Bug Found While Testing Own Side

If a team finds a bug in its own subsystem, that team owns the fix.

However, if the bug may affect another team, the team must send an `ADVISORY`.

This is mandatory when the bug affects:

- public contracts
- IR semantics
- pass output shape
- FrameState assumptions
- GC reference metadata
- stack map assumptions
- register assignment expectations
- code emission assumptions
- test baselines
- replay artifacts
- the T0 state contract
- patch-site ABIs
- artifact or manifest formats

The advisory must say clearly:

```text
This bug is in our area.
We are fixing it.
Do not modify our area.
If you are affected, reply with impact information.
```

---

## Advisory Message Template

Use:

```text
docs/templates/own-bug-advisory.md
```

---

## Contract Changes

If a team needs a change to another team's interface:

1. Create an `RFC` message.
2. List the contract being changed.
3. List all affected teams.
4. Provide a compatibility plan.
5. Wait for reviewer approval from affected teams.

No contract change may be implemented silently.

Cross-team representation contracts — such as the NaN boxing representation contract (Part XVIII of `docs/laws.md`) — additionally require approval from every affected team before the feature may be enabled.

---

## Response Rules

The receiving team must respond with one of:

- `ACK`
- `REJECTED`
- `NEED_INFO`
- `IN_PROGRESS`
- `BLOCKED`

Responses are added by editing the message file or by adding a linked response file.

A message may not be closed without:

- a fix reference, or
- a rejection reason, or
- a waiver reference, or
- an integration decision

---

## Message Example

```yaml
---
id: MSG-20260830-006-passes-codegen-ADVISORY
type: ADVISORY
from: passes
to:
  - codegen
  - regalloc
severity: P1
status: OPEN
laws_refs:
  - Rule 4
  - Rule 82
related_prs: []
related_tests:
  - tests/passes/framestate_materialization.b2
created: 2026-08-30
---
```

```markdown
## Summary

During testing of our own pass pipeline, we found a bug in the Passes Team's
FrameState attachment logic.

The bug can cause a missing monitor-state entry in deopt metadata for
synchronized methods.

## Ownership

This bug is in the Passes Team area.

The Passes Team will fix it.

Do not modify `compiler/passes/` or `tests/passes/`.

## Impact

This may affect:

- Codegen deopt stub generation
- RegAlloc stack maps if monitor slots are tracked there
- deopt reconstruction correctness

## Requested Action

No patch is requested from other teams.

If your team consumes FrameState monitor metadata, please reply with impact
notes.

## Boundaries

The Passes Team will not modify:

- compiler/codegen/
- compiler/regalloc/
- compiler/ir/core/
```

---

## Forbidden Message Behavior

Teams must not:

- send a message demanding direct code changes outside the owner's area
- close another team's bug without owner approval
- patch first and message later
- silently change contracts
- ignore advisory messages that affect them

---

## Message Compliance Checklist

Before sending a message:

- [ ] correct type selected
- [ ] severity selected
- [ ] law references included
- [ ] evidence included
- [ ] impact included
- [ ] requested action included
- [ ] boundary statement included
- [ ] no out-of-area patch attached unless explicitly requested by owner
