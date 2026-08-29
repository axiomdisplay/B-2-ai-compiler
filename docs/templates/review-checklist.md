# B-2 Team Review Checklist

This checklist is used by the reviewer AI in every team.

It does not replace `docs/laws.md`.

---

## Universal Checks

- [ ] The change only modifies paths owned by this team.
- [ ] The change references relevant message IDs if cross-team impact exists.
- [ ] No out-of-area edits are present.
- [ ] No silent fallbacks were introduced.
- [ ] No magic constants were introduced.
- [ ] No banned containers were introduced in hot paths.
- [ ] Tests were added or updated.
- [ ] Documentation was updated if public behavior changed.
- [ ] The change is reproducible from replay artifacts.
- [ ] The reviewer can identify the law references involved.

---

## Correctness Checks

- [ ] The change does not break Java semantic fidelity.
- [ ] The change does not break deopt reconstruction.
- [ ] The change does not drop exceptions.
- [ ] The change does not reorder observable effects without proof.
- [ ] The change does not hide GC references.
- [ ] The change does not remove required guards.
- [ ] The change does not remove required FrameState attachments.

---

## Performance Checks

- [ ] No allocation introduced into a hot path.
- [ ] No C++ exceptions introduced into hot path.
- [ ] No RTTI introduced into hot path.
- [ ] No `std::shared_ptr` introduced into hot IR code.
- [ ] No `std::function` introduced into hot IR code.
- [ ] No global locks introduced into hot runtime/compiler paths.
- [ ] Cache-friendly data structures are used.

---

## Tier-Specific Checks

For **Baseline No-IR (T1)** changes:

- [ ] No IR graph is constructed; no optimization pass is run (Amendment A).
- [ ] All seven Amendment A provisions remain satisfied.

For **IR / Passes / RegAlloc / Codegen (T2, T3)** changes:

- [ ] Pass contracts, budgets, kill switches, and golden tests are updated (Rules 123, 132).
- [ ] SWLP / PEA / NaN boxing changes comply with Part XVIII of `docs/laws.md`.

For **AOT (T3)** changes:

- [ ] No dynamic-behavior assumption is baked in without mechanical proof (Amendment B).
- [ ] Manifests and load-time validation are updated (Rules 107, 108).

For **Interpreter (T0)** changes:

- [ ] The T0 state contract is updated and advisories sent if the deopt target changed.

---

## Review Decision

The reviewer must record one of:

```text
APPROVED
CHANGES_REQUESTED
REJECTED
BLOCKED
```

If `CHANGES_REQUESTED`, list exact required fixes.

If `REJECTED`, cite violated laws or team boundaries.

If `BLOCKED`, open or reference a message.
