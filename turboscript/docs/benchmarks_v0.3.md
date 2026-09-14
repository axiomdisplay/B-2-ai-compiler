# TurboScript Tier 0 — Interpreter-Only Benchmark Results (v0.3)

**Status:** v0.3 — element fast path + IC layer + builtins milestone
**Owner:** TurboScript Interp Team
**Last Updated:** 2026-09-14
**Governing Laws:** Rules 36 (differential testing), 55/64 (durable docs), 96
(Tier 0 as semantic baseline), 124 (determinism), 143 (tracked follow-ups)
**Suite:** `tests/bench/` (7 kernels + `run_bench.py`)
**Supersedes:** `benchmarks_v0.2.md` (kept as the v0.2 history; its Section 5
bottleneck register is closed out below).

## 1. What Changed Since v0.2

The four owned fixes from the v0.2 bottleneck register
(benchmarks_v0.2.md Section 5) are implemented:

1. **Smi-key element fast path** (`Isolate::getElementValue/setElementValue`):
   canonical Smi keys walk element storage directly — zero string
   interning on element hits (was: one fresh string + intern per access).
2. **Feedback plumbing**: recording sites take resolved `FeedbackSlot*`
   pointers (no per-op `frameStack_.back()` + vector re-derivation), and
   the recording tax was measured (Section 5).
3. **Monomorphic IC layer** (bytecode_spec.md 8.1): shape-IC on
   GetProperty/SetProperty/LoadGlobal/StoreGlobal plus a shape-**transition**
   IC on SetProperty for fresh-object stores; global ICs give fib its
   declaration-closure lookups at direct-slot speed.
4. **fib kernel correction**: the v0.2 twin re-ran `NewClosure` per
   recursive call (a function-expression-per-call pattern the JS twin does
   not contain). The closure is now created once in `@main` and reached
   through the `@fib` global binding — exactly what the JS twin's function
   declaration means. Cross-engine checksums still agree (Rule 96).

New in v0.3 and now measurable: the **builtins layer**
(Object.prototype/Array.prototype/Function.prototype wiring, 13
Array.prototype methods, Object/Array/Symbol/Proxy namespaces) and the
**Symbol/Proxy value kinds** — exercised by the new `array_builtins` kernel.

## 2. Results (median ms; slowdown × vs turbo; 1 warmup + 5 runs)

| kernel | workload | turbo | node-jitless | qjs | node-jit (ref) |
|---|---|---|---|---|---|
| int_loop | 5M-iter djb2 int hash | 807.9 | 158.4 (0.20×) | 442.5 (0.55×) | 28.8 (0.04×) |
| fib | fib(27) naive recursion (~635k calls) | 67.5 | 21.7 (0.32×) | 40.2 (0.59×) | 2.2 (0.03×) |
| float_loop | mandelbrot-lite 200×200 | 180.3 | 73.6 (0.41×) | 79.8 (0.44×) | 3.6 (0.02×) |
| string_concat | 20k × (40-char build + num→str) | 102.9 | 25.8 (0.25×) | 74.6 (0.72×) | 8.5 (0.08×) |
| object_fields | 1M × (new obj, 3 stores, 3 loads) | 411.5 | 96.5 (0.23×) | 404.6 (0.98×) | 9.8 (0.02×) |
| array_loop | 300-elem smi array, 3k sum passes | 84.8 | 17.9 (0.21×) | 54.7 (0.65×) | 2.4 (0.03×) |
| array_builtins | 100k push + map/reduce/filter (300k callbacks) | 32.4 | 11.8 (0.36×) | 32.2 (0.99×) | 6.3 (0.19×) |
| **geometric mean** | | **1.00×** | **0.27×** | **0.68×** | **0.04×** |

Checksums agree on all 7 kernels × 4 engines (the runner fails loudly
otherwise): int_loop `473692005`, fib `196418`, float_loop `726419`,
string_concat `948890`, object_fields `2109827392`, array_loop
`269100000`, array_builtins `599990`.

### v0.2 → v0.3 delta (geomean)

| baseline | v0.2 | v0.3 | delta |
|---|---|---|---|
| node-jitless (V8 Ignition) | 0.16× | **0.27×** | gap closed ~1.7× |
| qjs (quickjs-ng) | 0.40× | **0.68×** | gap closed ~1.7× |

Per-kernel highlights: array_loop closed 7× (0.03× → 0.21×), fib closed
2× (0.16× → 0.32×), object_fields and the new array_builtins kernel are at
**near-parity with quickjs-ng** (0.98× / 0.99×).

Raw data: `tests/bench/bench_results.json` (regenerate with
`python3 tests/bench/run_bench.py --with-jit-reference --out bench_results.json`).

## 3. Reading the Numbers

- TurboScript Tier 0 v0.3 is ~3.7× slower than V8 Ignition and ~1.5×
  slower than quickjs-ng (geometric mean) — from 6.3×/2.5× in v0.2 — while
  simultaneously shipping the builtins layer, symbols, and the full Proxy
  trap protocol. Every remaining gap has a named, owned cause (Section 4).
- int_loop remains the worst case vs Ignition (0.20×). The kernel is pure
  Smi arithmetic where Ignition's register-machine handlers are
  hand-tuned assembly stubs; our dispatch loop is generic C++ with
  heap-number normalization on every op. The next lever there is
  Smi-specialized arithmetic fast paths (in-place dst update, overflow
  check), registered below.
- object_fields at 0.98× of qjs shows the transition-IC working: fresh
  objects per iteration mean every store transitions shapes, and the
  transition IC collapses the repeated define-property walks.
- array_builtins at 0.99× of qjs validates the builtins layer design:
  native methods compose the same public semantic helpers the opcodes
  use (Rule 96: one semantic source), and callback dispatch through
  callValue is already competitive.

## 4. Recording Tax Measurement (v0.2 register item #2, closed with data)

Method: `tsrun --time` vs `tsrun --time --no-record` (feedback recording
and IC state off), medians of 3, same machine.

| kernel | recorded | --no-record | net delta |
|---|---|---|---|
| int_loop | ~810 ms | ~732 ms | +10.7% (recording cost) |
| object_fields | ~415 ms | ~438 ms | **−5.5% (ICs win more than recording costs)** |
| array_loop | ~85 ms | ~79 ms | +7.5% |

Verdict per the v0.2 register's decision rule ("if >15%, adopt cheaper
counters"): the always-on tax is **below the 15% threshold everywhere**,
and negative where the property IC layer earns it back. Feedback recording
stays always-on as the spec requires (Part I Tier 0, Rule 124: profiles
are deterministic); no cheaper-counter scheme is adopted. `--no-record`
remains a documented measurement toggle only — it is NOT a supported
operating mode (disabling it also disables the IC layer, as the
object_fields row shows).

## 5. Updated Bottleneck Register (Rule 143: measured, owned, dated)

| # | Finding | Evidence | Planned fix | Target |
|---|---|---|---|---|
| 1 | Smi arithmetic has no specialized fast path (every Add/Sub normalizes through double + heap-number check) | int_loop 0.20× of Ignition; --stats shows Add/Shl/BitOr dominate | Smi-only in-handler arithmetic with overflow bailout to the semantic helper (semantics unchanged, Rule 96) | v0.4 |
| 2 | Call sequence still allocates the frame's register vector per call (`regs.assign`) | fib 0.32×; ~635k frames per kernel | Register-file pooling / reuse across frames (single-threaded T0, Rule 119) | v0.4 |
| 3 | Element IC: element sites record kinds but installs no fast-path state (unlike property sites) | array_loop 0.21× | Element-kind monomorphic IC (kind id -> dense-slot fast read/write) | v0.4 |
| 4 | `string_concat` allocates a fresh string per concat (flat strings only, no ropes) | string_concat 0.25× | Cons-strings with lazy flattening (frontend-visible semantics unchanged) | v0.4+ |
| 5 | GC is arena-owned, non-collecting (v0.2 register, unchanged) | heap growth on allocation-heavy kernels | GC team, v0.4 (must re-run this suite per fairness note 6 in v0.2 doc) | v0.4 |

## 6. Fairness Notes & Limitations

Same protocol as v0.2 (identical workloads, kernel-only timing, medians
over 5 runs after warmup, feedback ON for TurboScript, no PGO/LTO on any
side). Additions for v0.3:

- `array_builtins` hoists the `push` method reference out of the loop in
  the TSBC twin; the JS twin performs the same lookup per call
  (`a.push(...)`). Engines on both sides cache this lookup internally, so
  the twins measure the same effective work: 100k pushes + 300k callback
  invocations in identical order.
- The builtins layer participates in the checksum agreement, so every
  builtin used by the kernel (push/map/reduce/filter/join/length) is
  differentially tested against V8 and QuickJS on every suite run
  (Rule 36 spirit, Rule 96).
- These are microbenchmarks: stable and diagnostic, not representative of
  application performance.

## 7. Reproducing

```bash
cd turboscript
make                                 # builds build-ts/tsrun
python3 tests/bench/run_bench.py --with-jit-reference --out bench_results.json
```
