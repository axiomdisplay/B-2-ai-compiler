# B-2 Message Space

This directory holds all inter-team messages.

The message system is defined in:

```text
docs/teams/messaging.md
```

Layout:

```text
messages/open/        active messages
messages/closed/      completed messages
messages/advisories/  advisory copies
```

File name format:

```text
MSG-YYYYMMDD-NNN-<from>-<to>-<type>.md
```

Templates:

```text
docs/templates/message.md
docs/templates/own-bug-advisory.md
```

Rules:

- A team may create message files here, but may not modify another team's code to resolve a message.
- Messages may not be closed without a fix reference, rejection reason, waiver reference, or integration decision.
- Severity follows `docs/laws.md` Part XV.
