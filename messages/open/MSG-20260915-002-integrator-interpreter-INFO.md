id: MSG-20260915-002
type: INFO
from: integrator
to:
  - interpreter
cc:
  - all
severity: P3
status: OPEN
laws_refs:
  - Rule 36
  - Rule 55
  - Rule 64
  - Rule 96
  - Rule 145
  - Rule 150
related_prs: []
related_tests:
  - turboscript-lab/tests/semantics.js
created: 2026-09-15

---

## Summary

Per the owner directive ("look at what is making us so slow; the interp should
be faster than T0 on V8"), the integrator built an **independent Tier 0
interpreter lineage** to (a) establish an independent measured baseline against
V8 Ignition and QuickJS, and (b) surface implementation pitfalls a second
implementation hits that the primary one may not have met yet.

It is delivered on branch **`integrator/interp-lab`** at
`turboscript-lab/` — **no file under `turboscript/` (your area) was touched.**
Mine it freely; it is not a competing line, it is a lab.

## What is in the branch

- `turboscript-lab/` — a complete second Tier 0: lexer/parser → two-pass
  compiler (capture analysis, static context chains) → computed-goto register
  interpreter. NaN-boxed values with **unboxed doubles** (no HeapNumber for
  float arithmetic), shape-transition objects with **inline slots**,
  monomorphic ICs in fixed 32-bit words, mark-sweep GC with safepoint-only
  collection, `[[Construct]]` semantics, TDZ.
- `turboscript-lab/tests/semantics.js` — 81-assertion Part 0 suite (SameValue/
  SameValueZero/strict-eq -0 and NaN edges, TDZ, coercion tables, closure
  cells at depth 2, try/finally completion dispatch, [[Construct]], for-in
  canonical keys). Green: 81/81.
- `turboscript-lab/docs/PERF-NOTES.md` — measured analysis + roadmap.
- `turboscript-lab/scripts/bench.py` — harness: ts vs `node --jitless` (pure
  Ignition) vs QuickJS, 5 runs best-of, checksum agreement enforced.

## Measured baseline (independent harness, best-of-5)

| bench       | lab T0  | node --jitless | QuickJS | lab/Ignition |
|-------------|---------|----------------|---------|--------------|
| fib         | 86ms    | 82ms           | 54ms    | 0.95x        |
| nbody       | 306ms   | 236ms          | 164ms   | 0.77x        |
| binarytrees | 1126ms  | 543ms          | 1401ms  | **1.24x** (vs QJS) |
| bubble      | 486ms   | 235ms          | 243ms   | 0.48x        |
| properties  | 443ms   | 240ms          | 339ms   | 0.54x        |

Your v0.4 reports geomean 0.49x vs node / 1.16x vs quickjs-ng on your 7
kernels — different bench sets, so the numbers are complementary, not
contradictory.

## Six pitfalls the lab hit (universal Tier 0 hazards; check your tree)

1. **GC request flag must be cleared** after collection — an uncleared flag
   turns every backedge/call into a full GC (45s → 1.0s on binarytrees).
2. **Value-stack high-water must be restored on exception unwind** — stale
   refs above the live region get marked, then dangle (ASan UAF in the
   marker).
3. **Intern table must be single-canonical** — two spellings of the same key
   (`ascii` vs UTF-16 entry) silently break pointer-identity key checks and
   send every special property (`.length`, `.prototype`) to the miss path.
4. **Frame size must cover the register high-water mark, not the final
   allocator position** — and the call-arg window must never move the
   allocator downward onto live locals (symptom: doubled method results,
   phantom values).
5. **`[[Construct]]` must wrap non-object returns** with the created `this`.
6. **Le/Ge must map Unordered (NaN) to false** — you found and fixed this in
   v0.4; the lab hit the same edge independently (Rule 36 works: two
   implementations, same oracle, same bug class).

## Design offer worth evaluating against TSBC

**Unboxed doubles via NaN-boxing** (LuaJIT-style): doubles are raw IEEE bits
inside the tagged value — `nbody`-style kernels never allocate HeapNumbers.
Your v0.4 notes mention HeapNumber normalization; a NaN-boxing value cell is
a drop-in candidate for the same guarded-lane discipline (Rule 96): the
double lane becomes "already a double", the boxing allocation disappears.

## Requested action

Interpreter team: review `turboscript-lab/docs/PERF-NOTES.md` §5 roadmap and
§4 gap analysis; adopt anything that survives your guard discipline. The
semantics suite can be merged into your corpus as non-differential Part 0
assertions (Rule 36 complement). Close this message when triaged.
