# TurboScript Tier 0 — Interpreter-Only Benchmark Results (v0.4)

**Status:** v0.4 — fast-path dispatch milestone (Smi/number lanes, call pool,
element fast path, fmt-table dispatch)
**Owner:** TurboScript Interp Team
**Last Updated:** 2026-09-15
**Governing Laws:** Rules 36 (differential testing), 55/64 (durable docs), 96
(Tier 0 as semantic baseline), 124 (determinism), 143 (tracked follow-ups)
**Suite:** `tests/bench/` (7 kernels + `run_bench.py`)
**Supersedes:** `benchmarks_v0.3.md` (kept as the v0.3 history; its Section 5
bottleneck register is closed out below).

## 1. What Changed Since v0.3

The owner directive for v0.4 was to find what makes Tier 0 slow and close the
gap to V8's Tier 0 (Ignition, measured via `node --jitless`). Every change
below is a **guard in front of the unchanged semantic helper** (Rule 96): the
fast lanes claim only the input classes listed and fall through to the exact
same code the oracles were verified against.

1. **Static format table.** `TS_FIELDS` previously called
   `formatWordCount(opcodeInfo(...))` — a compiler jump-table switch — on
   every instruction. It now reads a 256-byte static table built once at
   first dispatch (same pattern as the dispatch table).
2. **Smi arithmetic lanes** (register item #1, closed): in-handler
   Smi+Smi lanes for Add/Sub/Mul with int64 overflow guards; Div/Mod as
   IEEE double lanes with inline normalization; BitAnd/BitOr/BitXor/Shl/
   Shr/UShr int lanes; BitNot; Neg (0 -> -0, INT32_MIN -> 2^31 as
   HeapNumbers, Rule 72); Inc/Dec overflow lanes; Lt/Le/Gt/Ge Smi and
   number-pair lanes; StrictEq Smi + cross-kind lanes; branch ToBoolean
   fast lane. Every overflow/edge case routes to the unchanged helper.
3. **Number-pair lanes**: Add/Sub/Mul/Div/Mod/Inc/Dec accept Smi/HeapNumber
   mixes with inline `tsSmiOrNumber` normalization (same contract as
   `normalizeNumber`: integral, in range, never -0).
4. **Register-file pooling** (register item #2, closed): `callClosure`
   reuses freed frame register vectors LIFO (bounded by `kRegPoolMax`,
   Rule 23); a fib-scale call no longer allocates.
5. **In-handler element fast path** (register item #3, closed):
   GetElement/SetElement with a Smi key on an array receiver read/write
   dense storage directly (bounds + hole guards); everything else falls
   through to `getElementValue`/`setElementValue`.
6. **Call fast hop**: L_Call/L_CallMethod dispatch straight to
   `callClosure` for closure callees; `callValue`'s proxy/native routing
   only runs for non-closures.
7. **Property-key inline lane**: GetProperty/SetProperty read a const-pool
   string's cached interned key id inline instead of calling
   `toPropertyKey` per access.
8. **doubleToString integer fast path** (register item #4, partial —
   cons-strings still open): integral doubles below 2^53 emit digits
   directly instead of up to 17 snprintf/strtod round-trips; the golden
   `fastpath_tostring` corpus test is generated from node AND quickjs.
9. **slotOfPc hoist**: the pc -> feedback-slot map is a raw pointer
   (one dependent load per recorded op, not two). Feedback recording stays
   always-on (Rule 124; the v0.3 tax measurement stands).
10. **Semantic defect fixed by the fast-path review (Rule 96):** the Le/Ge
    opcode mapping turned `Relational::Unordered` into `true`, so
    `NaN <= 1` and `1 >= NaN` returned **true** (oracles: node, quickjs —
    both `false`). Fixed and pinned by the new `fastpath_smi_edges`
    corpus test (differential golden from both oracles).

No ISA changes: v0.1 opcode numbering is untouched and the corpus is
byte-stable. Build flags unchanged (-O2, the -O3 experiment was neutral).

## 2. Results (median ms; slowdown × vs turbo; 1 warmup + 5 runs)

| kernel | workload | turbo | node-jitless | qjs | node-jit (ref) |
|---|---|---|---|---|---|
| int_loop | 5M-iter djb2 int hash | 302.1 | 159.4 (0.53×) | 439.9 (1.46×) | 28.5 (0.09×) |
| fib | fib(27) naive recursion (~635k calls) | 54.0 | 21.8 (0.40×) | 40.6 (0.75×) | 2.2 (0.04×) |
| float_loop | mandelbrot-lite 200×200 | 88.7 | 74.5 (0.84×) | 78.8 (0.89×) | 3.7 (0.04×) |
| string_concat | 20k × (40-char build + num→str) | 71.1 | 25.7 (0.36×) | 74.8 (1.05×) | 8.9 (0.12×) |
| object_fields | 1M × (new obj, 3 stores, 3 loads) | 274.4 | 99.4 (0.36×) | 405.4 (1.48×) | 10.2 (0.04×) |
| array_loop | 300-elem smi array, 3k sum passes | 35.4 | 17.9 (0.51×) | 54.3 (1.53×) | 2.5 (0.07×) |
| array_builtins | 100k push + map/reduce/filter (300k callbacks) | 21.9 | 12.2 (0.56×) | 26.9 (1.23×) | 6.0 (0.27×) |
| **geometric mean** | | **1.00×** | **0.49×** | **1.16×** | **0.08×** |

**TurboScript Tier 0 v0.4 is now FASTER than quickjs-ng on the geomean
(1.16×) and ~2× behind V8 Ignition (0.49×).** All checksums agree on all
7 kernels × 4 engines (the runner fails loudly otherwise): int_loop
`473692005`, fib `196418`, float_loop `726419`, string_concat `948890`,
object_fields `2109827392`, array_loop `269100000`, array_builtins
`599990`.

### v0.3 → v0.4 delta (geomean)

| baseline | v0.3 | v0.4 | delta |
|---|---|---|---|
| node-jitless (V8 Ignition) | 0.27× | **0.49×** | gap closed ~1.8× |
| qjs (quickjs-ng) | 0.68× | **1.16×** | crossed 1.0 — now ahead |

Per-kernel highlights: int_loop closed 2.7× (0.20× → 0.53×), object_fields
closed 1.4× (0.25× → 0.36×, and is 1.48× faster than qjs), array_loop
closed 2.4× (0.21× → 0.51×), float_loop closed 2× (0.41× → 0.84×).

Raw data: `tests/bench/bench_results.json` (regenerate with
`python3 tests/bench/run_bench.py --with-jit-reference --out bench_results.json`).

## 3. Reading the Numbers — What Is STILL Making Us Slow

The remaining ~2× to Ignition is structural, measured, and registered
(Section 5). Per-op cost on int_loop is ~23 cycles vs Ignition's ~12:

1. **Value is 16 bytes** (kind byte + 8-byte union + padding). Every
   register read/write, every Mov, every slot load moves twice the bytes
   of V8's compressed 4-byte tagged values (and 2× full-pointer 8-byte
   engines on top). This taxes every op in every kernel and is THE
   dominant structural term. The fix — a 64-bit NaN-boxed Value with the
   same accessor API — is a cross-cutting refactor and is registered as
   the v0.5 item with its cost model.
2. **Call frames remain C++** (fib 0.40×): pooled registers removed the
   allocation, but frame setup, the expected-based return path, and
   frameStack_ bookkeeping are still several times wider than Ignition's
   assembly trampoline.
3. **Feedback recording tax** (~10%, measured in v0.3, unchanged):
   always-on per Rule 124 — deterministic profiles are a spec
   requirement, not a luxury. The Tier 1 stencil consumes them.
4. **string_concat** (0.36×) still allocates a flat string per concat;
   cons-strings with lazy flattening remain the registered fix (v0.4+
   carry-over). The number->string fast path removed the snprintf
   round-trips, but the allocation per concat stands.

## 4. Fast-Path Guard Discipline (new, interp_contract.md 3.4)

Every v0.4 fast lane obeys one rule, enforced by review and by the
differential corpus: **a fast path is a guard in front of the semantic
helper, never a replacement.** The lanes claim only the input classes they
provably cover (Smi ranges, dense non-hole elements, cached key ids,
integral doubles < 2^53); every other input executes the unchanged helper
the oracles verified. The `fastpath_smi_edges` corpus test pins the exact
overflow/-0/NaN/edge behavior against node AND quickjs goldens.

## 5. Updated Bottleneck Register (Rule 143: measured, owned, dated)

| # | Finding | Evidence | Planned fix | Target |
|---|---|---|---|---|
| 1 | 16-byte Value doubles register-file and slot traffic on every op | int_loop ~23 cyc/op vs Ignition ~12; all kernels pay it | 64-bit NaN-boxed Value (same accessor API), cross-cutting refactor + full corpus re-run | v0.5 |
| 2 | Call frame setup is C++-wide vs Ignition's assembly trampoline | fib 0.40× of Ignition despite register pooling | narrower call path: frame layout single-allocation, return-value slot, tail-merge frameStack_ upkeep | v0.5 |
| 3 | string_concat allocates a fresh flat string per concat | string_concat 0.36× | cons-strings with lazy flattening (semantics unchanged) | v0.5+ |
| 4 | Feedback recording tax ~10% | v0.3 measurement stands | keep (Rule 124); revisit only if Tier 1 stencil format changes | open |
| 5 | GC is arena-owned, non-collecting (v0.2 register, unchanged) | heap growth on allocation-heavy kernels | GC team, v0.4 milestone (must re-run this suite per fairness note 6 in v0.2 doc) | GC team |

## 6. Fairness Notes & Limitations

Same protocol as v0.3: identical workloads, kernel-only timing, medians
over 5 runs after warmup, feedback ON for TurboScript, -O2 builds on both
sides (qjs built by its Release config; the TurboScript -O3 experiment was
neutral and is not adopted). Additions for v0.4:

- The `fastpath_smi_edges` and `fastpath_tostring` corpus goldens are
  generated from node AND quickjs agreement, so the new fast lanes are
  differentially pinned on every `make test` (Rule 36, Rule 96).
- The v0.3 twin-stability note stands: bench twins are byte-stable; the
  v0.4 gains come from the engine, not the kernels.

## 7. Reproducing

```bash
cd turboscript
make                                 # builds build-ts/tsrun
make test                            # 89-test corpus, both dispatch paths
python3 tests/bench/run_bench.py --with-jit-reference --out bench_results.json
```
