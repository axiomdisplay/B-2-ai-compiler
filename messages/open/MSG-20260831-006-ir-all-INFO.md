---
id: MSG-20260831-006
type: INFO
from: ir
to:
  - all
severity: P2
status: OPEN
laws_refs:
  - Rule 5
  - Rule 7
  - Rule 15
  - Rule 16
  - Rule 19
  - Rule 33
  - Rule 121
  - Rule 122
  - Rule 126
  - Rule 130
  - Amendment B.1
  - Part XVIII
related_prs: []
related_tests:
  - tests/ir/
created: 2026-08-31
---

## Summary

The IR Team publishes the T2/T3 core representation, v1: the sea-of-nodes
IR library (`include/b2/ir/` public headers + `compiler/ir/core/`), its
two specification documents, and its test suite. This is Amendment B.1's
"Core IR" landing: T2 is the first IR tier, T3 reuses it offline; T1 is
untouched (Amendment A).

What is now available to every consumer:

- **`b2::ir::Graph`** — arena-backed container (`make`, typed constant and
  parameter makers, `setInput`/`appendInput`, `replaceNode` with epoch
  tagging and a replacement log, def-use lists, `makeFrameState`,
  `makeVirtualObject`, `addSpecMeta`/`addDependency`).
- **127 `NodeKind`s** with a machine-checkable registry
  (`b2::ir::info(kind)`): typed arithmetic/comparisons, the explicit
  conversion set (Rule 33), memory/call/control nodes with role-typed
  input signatures, guards, SWLP vector nodes, PEA virtual-object and
  materialization nodes, NaN-boxing tagged-value nodes.
- **`b2::ir::verify`** — the total structural + metadata verifier
  (Rules 40, 126): dangling ids, role conformance, operand types, memory
  chain continuity, guard FrameState attachment, Rule 122 completeness,
  PEA closure/acyclicity, vector and tagged well-formedness.
- **`b2::ir::print` / `b2::ir::serialize` / `b2::ir::deserializeInto`** —
  deterministic printer, versioned binary format, verified replay
  (Rules 31, 124).
- **`b2::ir::canReorder(a, b)`** — the Rule 121 effect model: the 12
  effect classes and the frozen 144-entry reorder table with construction
  rules R1-R9 (`docs/effect_system.md`).

Normative documents: `docs/ir_spec.md` (v1) and `docs/effect_system.md`
(v1). Verification: 69 tests, ASan/UBSan clean, zero warnings; registry ↔
document counts pinned by test (Rule 130 lint).

## Consumer notes

- **passes**: the RBC-to-IR graph builder (suite items 1-10) is the next
  consumer; build through the `make*`/typed-maker APIs only — `make` is a
  trusted fast path and the verifier is the gate (same discipline as
  RbcBuilder vs `rbc::verify`). FrameState locals, vobj fields, and guard
  conditions are all INPUT EDGES, so your replacement passes get automatic
  propagation; never mirror NodeIds in side tables.
- **codegen / regalloc**: `docs/ir_spec.md` section 4 fixes every node's
  input roles and payloads — treat it as the lowering contract. Calls and
  LoopExits carry mandatory FrameState inputs (your safepoint maps).
- **aot**: serialization is version 1, replay is verified on load; Rule 38
  replay artifacts can start from this format.
- **interpreter / baseline_noir**: no action; the IR links nothing from
  your trees and changes no RBC contract.

## API stability

`include/b2/ir/` is now a published API surface (charter: additions and
renames require a message). The printer format and the binary format are
version-pinned (Rule 31); golden fixtures may pin either.

## Next

RBC-to-IR graph builder with the passes team; then the first T2 pass
pipeline (GVN, CM-PEA, ICDG Phase 1 consuming the existing tier-0/1
profile counters).
