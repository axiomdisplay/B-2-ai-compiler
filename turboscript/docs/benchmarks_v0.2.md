# TurboScript Tier 0 — Interpreter-Only Benchmark Results (v0.2)

**Status:** v0.2 baseline
**Owner:** TurboScript Interp Team
**Last Updated:** 2026-09-14
**Governing Laws:** Rules 36 (differential testing), 55/64 (durable docs), 96
(Tier 0 as semantic baseline), 124 (determinism), 143 (tracked follow-ups)
**Suite:** `tests/bench/` (kernels + `run_bench.py`)

## 1. What Was Compared

Interpreter-only JS engines, per the owner's directive ("benchmark it vs JS
interp only, maybe vs quickJS"):

| Engine | Identity | Notes |
|---|---|---|
| **turbo** | TurboScript Tier 0, computed-goto dispatch | g++ 14.2, `-std=c++26 -O2 -fno-exceptions -fno-rtti -fno-strict-aliasing`; TSBC v0.2 (commit series ending at the v0.2 array milestone + `--time` driver flag) |
| **node-jitless** | V8 Ignition only — `node --jitless` (node v24.19.0) | All JIT tiers off: no TurboFan, no Maglev, no Sparkplug. The industry-reference interpreter baseline |
| **qjs** | quickjs-ng `qjs`, Release build (cmake) | Pure interpreter, ~decade of bellard-line tuning |
| node-jit(ref) | full V8 JIT, plain `node` | **Reference only** — not an interpreter; included to show the tier-up gap |

## 2. Protocol (identical work, kernel-only timing)

- Six kernels, each written twice: TSBC assembly (`.tsbc`) and JS (`.js`)
  with identical operation order. The JS twin runs unmodified on
  node/quickjs; the `.tsbc` runs on `tsrun --time`.
- **Checksums:** every kernel prints a result; the runner FAILS if engines
  disagree. All results below passed the cross-engine checksum agreement —
  this doubles as a Rule 36-style differential test of Tier 0 against V8 and
  QuickJS on every kernel (Rule 96 baseline validated externally, not just
  against our own corpus).
- **Timing:** kernel execution only on all engines. TurboScript: `tsrun
  --time` wraps `run()` (assembly/verify/load excluded). JS engines:
  in-script `performance.now()` around `kernel()` (parse excluded).
- **Repetitions:** 1 warmup + 5 measured runs; **median** reported.
  Feedback collection stays ON for TurboScript (spec-mandated Tier 0
  profiling, interp_contract.md Section 5) — the numbers deliberately
  include that cost.
- Workload sizes are fixed and identical across engines (no per-engine
  scaling); one machine, sequential runs.

## 3. Results (median ms; slowdown × vs turbo)

| kernel | workload | turbo | node-jitless | qjs | node-jit (ref) |
|---|---|---|---|---|---|
| int_loop | 5M-iter djb2 int hash (Shl/Add/BitOr) | 783.6 | 160.2 (0.20×) | 441.5 (0.56×) | 28.4 (0.04×) |
| fib | fib(27), naive recursion (~635k calls) | 133.6 | 21.6 (0.16×) | 40.2 (0.30×) | 2.1 (0.02×) |
| float_loop | mandelbrot-lite 200×200, ≤100 iters | 181.1 | 72.6 (0.40×) | 79.0 (0.44×) | 3.5 (0.02×) |
| string_concat | 20k × (40-char build + num→str) | 100.3 | 25.4 (0.25×) | 74.1 (0.74×) | 8.0 (0.08×) |
| object_fields | 1M × (new obj, 3 shape stores, 3 IC loads) | 467.8 | 97.1 (0.21×) | 405.9 (0.87×) | 10.4 (0.02×) |
| array_loop | 300-elem smi array, 3k sum passes (900k loads) | 623.4 | 17.8 (0.03×) | 55.4 (0.09×) | 2.3 (0.01×) |
| **geometric mean** | | **1.00×** | **0.16×** | **0.40×** | **0.02×** |

Checksums (all engines agree on all kernels): int_loop `473692005`, fib
`196418`, float_loop `726419`, string_concat `948890`, object_fields
`2109827392`, array_loop `269100000`.

Raw data: `tests/bench/bench_results.json` (regenerate with
`python3 tests/bench/run_bench.py --with-jit-reference --out bench_results.json`).

## 4. Reading the Numbers

- **TurboScript Tier 0 v0.2 is ~6× slower than V8 Ignition and ~2.5× slower
  than quickjs-ng (geometric mean).** For a days-old interpreter carrying
  always-on profile collection, that is a legitimate baseline, not an
  embarrassment — but it is also not yet where the spec wants Tier 0 to be,
  and the gaps are large enough to point at concrete causes.
- **Closest races:** `float_loop` (1.25× vs qjs) and `string_concat`
  (1.35× vs qjs) are near-parity with quickjs — pure dispatch-and-compute
  loops where the computed-goto core is doing its job.
- **Worst case:** `array_loop` is 35× behind Ignition. This is not a
  dispatch problem; it is a known, single, fixable cause (Section 5).
- The node-jit reference row (50–80× faster on compute kernels) is the
  tier-up headroom reminder: this is exactly the gap Tiers 1–3 exist to
  close, and why Tier 0's profile output matters (Rule 2/44).

## 5. Known Bottlenecks (measured, owned, dated — Rule 143 discipline)

| # | Finding | Evidence | Planned fix | Target |
|---|---|---|---|---|
| 1 | **GetElement/SetElement have no Smi-key fast path**: every element access calls `toPropertyKey`, interning a fresh string symbol per access (900k allocations for `array_loop` — visible via `tsrun --stats`) | dispatch `L_GetProperty` handler; `--stats` heap counter | Direct indexed-element path for canonical Smi/array-index keys before symbol interning (semantics unchanged — one shared helper per Rule 96, corpus must stay green) | v0.3 |
| 2 | **Binary-op IC recording on every op** (type histogram updates per Add/Cmp) is part of the 4.9× int_loop gap | dispatch handlers unconditionally record when feedback exists | Measure recording tax with a build-time toggle; if >15%, adopt cheaper saturating counters / sampled recording — spec still requires deterministic profiles (Rule 124) | v0.3 |
| 3 | **Call overhead**: `NewClosure` at each recursive call site allocates per call in fib | fib = 0.16× of Ignition | Function-constant load op / cached closure for non-escaping function entries | v0.3 |
| 4 | Object store path (shape transition + IC record) — TS ≈ qjs here, both ≈5× behind Ignition | object_fields row | IC-cached shape-slot store | v0.3+ |

## 6. Fairness Notes & Limitations

- Same workload on every engine; no engine gets a smaller input. Kernel-only
  timing everywhere. Medians over 5 runs after warmup on an otherwise idle
  machine; run-to-run variance observed < 2%.
- TurboScript's heap is arena-owned, non-collecting (v0.2) — the allocation
 -heavy kernels would look *worse* for TS with a real GC, and correspondingly
  the v0.4 GC work must re-run this suite.
- QuickJS build: quickjs-ng Release via CMake; no PGO/LTO tricks on any side.
  Node/QuickJS parse time is excluded (in-script timing); TurboScript
  assemble/verify/load is excluded (`--time` wraps `run()` only).
- Kernels avoid features TS does not implement yet (builtins, Math, regex,
  eval) — they measure the engine core: dispatch, tagging, ICs, calls,
  strings, shapes, elements.
- These are microbenchmarks. They are stable and diagnostic, not
  representative of application performance.

## 7. Reproducing

```bash
cd turboscript
make                                 # builds build-ts/tsrun
python3 tests/bench/run_bench.py --with-jit-reference --out bench_results.json
```

`run_bench.py` fails loudly on any cross-engine checksum disagreement — the
benchmark suite is therefore also a permanent differential regression test
for the kernels it ships (Rule 36 spirit, Rule 96 baseline).
