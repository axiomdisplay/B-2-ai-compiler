---
id: MSG-YYYYMMDD-NNN
type: ADVISORY
from: <team>
to:
  - <affected-team-or-all>
severity: P0 | P1 | P2 | P3
status: OPEN
laws_refs:
  - Rule N
related_prs: []
related_tests: []
created: YYYY-MM-DD
---

## Summary

We found a bug while testing our own subsystem.

Describe the bug.

## Ownership

This bug is in the `<team>` area.

The `<team>` will fix it.

Do not modify the following paths:

```text
<owned paths>
```

## Potential Impact

Explain whether other teams may be affected.

Examples:

- changed output format
- changed metadata shape
- changed FrameState contents
- changed stack map expectations
- changed test baselines
- changed IR verification behavior
- changed codegen assumptions
- changed T0 state contract
- changed patch-site ABI
- changed artifact or manifest format

## Current Status

State one of:

- investigating
- fix in progress
- fix ready
- waiting for replay validation

## Requested Action From Other Teams

Usually:

```text
No patch is requested.
If your team is affected, reply with impact notes.
```

## Boundaries

Our team will not modify other teams' areas while fixing this bug.
