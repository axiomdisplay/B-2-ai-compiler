# TurboScript Tier 0 — Interpreter-Only Benchmark Results (v0.5)

**Status:** v0.5 — 64-bit NaN-boxed Value milestone (register item #1 closed;
call-path prologue + slot-growth work from item #2)
**Owner:** TurboScript Interp Team
**Last Updated:** 2026-09-16
**Governing Laws:** Rules 36 (differential testing), 55/64 (durable docs), 96
(Tier 0 as semantic baseline), 124 (determinism), 143 (tracked follow-ups)
**Suite:** `tests/bench/` (7 kernels + `run_bench.py`)
**Supersedes:** `benchmarks_v0.4.md` (kept as the v0.4 history; its Section 5
register items #1/#2 are closed or re-scoped below).

## 1. What Changed Since v0.4

The owner goal is unchanged: Tier 0 must be faster than V8's Tier 0
(Ignition, measured via `node --jitless`). v0.4's Section 3 identified the
16-byte Value as the dominant structural term and registered the 64-bit
NaN-boxed refactor as the v0.5 item. That refactor is the substance of this
release.

1. **64-bit NaN-boxed Value (register item #1, CLOSED).** A `Value` slot is
   now exactly 8 bytes (`sizeof(Value) == 8`, trivially copyable, was 16).
   Encoding: doubles are stored raw except that every NaN is canonicalized
   to +qNaN at boxing; all other values live in the tag space whose top 13
   bits are `0x1FFF` (sign=1, exponent=all-ones, mantissa[51]=1 — a pattern
   only negative NaNs can have, which is exactly why boxing
   canonicalization makes the space collision-free). Kind nibble in bits
   50..47, payload in bits 46..0. Consequences:
   - Every register read/write, every Mov, every slot load moves **8 bytes,
     not 16** — the tax that touched every op in every kernel is halved.
   - Doubles never allocate and never leave SSE registers on the number
     lanes (`-0.0` and infinities are raw bits; NaNs canonicalize).
   - Smi/int32 payload in the low 32 bits with a single shift+compare test;
     constants (Undefined/Null/Hole) are single-compare u64 patterns.
   - Pointer kinds carry the full 47-bit user-space address (Linux VA <
     2^47): no base+offset arithmetic, no compressed-pointer cage.
   - The tag boundary is 13 bits **specifically so that -Infinity
     (mantissa 0) stays outside the tag space**; a 12-bit tag would have
     collided with it. This was caught by the corpus before it shipped
     (Rule 36), then pinned by compile-time `static_assert` proofs in
     `ts_value.h` (encoding invariants: tag/NaN disjointness incl. ±Inf,
     Smi round-trips, constant decodes).
   - The accessor API is unchanged, so the semantic helpers (Rule 96: one
     semantic source) were ported, not rewritten. Every double-boxing site
     funnels through `Value::heapNumber` (the ONLY canonicalization point)
     or `normalizeNumber`/`tsSmiOrNumber`.
2. **Call-path prologue (register item #2, partial).** `runFrame`'s per-call
   prologue was two vector-of-vector indirections, two emptiness checks and
   three `data()` loads; it is now one indexed load plus one branch
   (`CallSetup`, built once at `loadModule`). Closures cache their resolved
   `Function*` at creation (no per-call unique_ptr walk of the module
   function table), and `frameStack_` is pre-reserved to the Rule 90 depth.
   Measured effect on fib: neutral within noise — the remaining call cost is
   frame setup/teardown shape, not the prologue (see Section 3).
3. **Slot-vector growth floor (new, unregistered finding).** `object_fields`
   allocates a fresh 3-slot object per iteration; the transition pushes grew
   the slot vector 0→1→2→4 (three allocations per object). A geometric
   reserve with `kMinSlotCapacity = 4` (Rule 23) makes a fresh k≤4-slot
   object allocate its slot storage exactly once. Storage-only: slotCount
   stays shape-driven (Rule 96: no semantic surface). object_fields
   0.40x → 0.45x.
4. **Branch-light binary-site classification.** `recordBinarySite`'s
   value-class buckets now answer Smi/HeapNumber with one shift+compare
   each and fall to a switch only for pointer kinds. Same buckets, same
   counts (Rule 124: recording is exact).

No ISA changes: v0.1 opcode numbering is untouched and the corpus is
byte-stable. Build flags unchanged (-O2).

## 2. Results (median ms; slowdown × vs turbo; 1 warmup + 5 runs)

| kernel | workload | turbo | node-jitless | qjs | node-jit (ref) |
|---|---|---|---|---|---|
| int_loop | 5M-iter djb2 int hash | 234.8 | 160.0 (0.68×) | 441.6 (1.88×) | 78.1 (0.33×) |
| fib | fib(27) naive recursion (~635k calls) | 34.9 | 21.7 (0.62×) | 40.3 (1.15×) | 2.2 (0.06×) |
| float_loop | mandelbrot-lite 200×200 | 70.1 | 73.3 (1.05×) | 79.0 (1.13×) | 3.9 (0.06×) |
| string_concat | 20k × (40-char build + num→str) | 64.4 | 25.2 (0.39×) | 74.5 (1.16×) | 8.3 (0.13×) |
| object_fields | 1M × (new obj, 3 stores, 3 loads) | 216.7 | 97.8 (0.45×) | 405.7 (1.87×) | 9.9 (0.05×) |
| array_loop | 300-elem smi array, 3k sum passes | 28.4 | 17.9 (0.63×) | 54.3 (1.91×) | 2.3 (0.08×) |
| array_builtins | 100k push + map/reduce/filter (300k callbacks) | 16.8 | 16.9 (1.01×) | 26.3 (1.57×) | 5.8 (0.35×) |
| **geometric mean** | | **1.00×** | **0.65×** | **1.48×** | **0.11×** |

**TurboScript Tier 0 v0.5 is 1.48× ahead of quickjs-ng on the geomean, and
two kernels (float_loop 1.05×, array_builtins 1.01×) now BEAT V8 Ignition
outright.** The geomean gap to Ignition closed another 1.33× this release.
All checksums agree on all 7 kernels × 4 engines (the runner fails loudly
otherwise): int_loop `473692005`, fib `196418`, float_loop `726419`,
string_concat `948890`, object_fields `2109827392`, array_loop `269100000`,
array_builtins `599990`.

### Version deltas (geomean vs node-jitless = V8 Ignition)

| baseline | v0.3 | v0.4 | v0.5 |
|---|---|---|---|
| node-jitless (V8 Ignition) | 0.27× | 0.49× | **0.65×** |
| qjs (quickjs-ng) | 0.68× | 1.16× | **1.48×** |

Per-kernel v0.4 → v0.5: float_loop 0.84× → **1.05×** (crossed 1.0 — unboxed
doubles beat Ignition's number path), array_builtins 0.56× → **1.01×**
(crossed 1.0), int_loop 0.53× → 0.68×, fib 0.40× → 0.62×, array_loop 0.51×
→ 0.63×, object_fields 0.36× → 0.45×, string_concat 0.36× → 0.39×.

Raw data: `tests/bench/bench_results.json` (regenerate with
`python3 tests/bench/run_bench.py --with-jit-reference --out bench_results.json`).

## 3. Reading the Numbers — What Is STILL Making Us Slow

The remaining ~1.55× to Ignition on the geomean is concentrated and
measured. Per-kernel diagnosis:

1. **object_fields (0.45×) — allocation + IC-miss shape.** Each iteration
   news an object: the arena deque emplace is not a bump pointer, `Object`
   carries an empty sparse `std::map` (48 bytes) in every instance, and IC
   misses after shape transitions re-walk `lookupProperty`. Ignition's
   new-space allocation is a bump pointer and its IC hit path is
   assembly-local. Registered as the v0.6 allocation/IC item (#1 below).
2. **string_concat (0.39×) — unchanged diagnosis, fix unchanged.** One flat
   string allocation per concat. Cons-strings with lazy flattening remain
   the registered fix (carried from v0.4; now the oldest open item).
3. **fib (0.62×) — handler-code quality, not prologue.** The v0.5 prologue
   work (CallSetup, cached `Function*`) measured neutral on fib: the call
   round-trip cost is frame setup/teardown and the per-handler dispatch +
   decode width. Ignition's handlers are specialized machine code emitted
   by its interpreter assembler — the portable C++ equivalent of that
   structural advantage is Tier 1 copy-and-patch (Laws Part I), not more
   C++ micro-surgery on Tier 0.
4. **int_loop (0.68×) — dispatch + decode + recording per op.** ~28
   cycles/op: TS_FIELDS decode, indirect dispatch, per-op feedback
   recording (~10%, Rule 124, stays), and the Smi lane itself. The lane is
   already minimal; further gains need wider superinstructions (ISA
   additions, kernel re-pinning) or Tier 1.
5. **Feedback recording tax** stays ~10% (re-measured on fib via
   `--no-record`: 34.7 → 31.2 ms). Always-on per Rule 124; the Tier 1
   stencil consumes the profiles.

## 4. Fast-Path Guard Discipline (unchanged, interp_contract.md 3.4)

Every lane added in v0.5 (branch-light classification, slot-growth floor)
is storage/measurement-level: no observable semantics moved. The
`fastpath_smi_edges` / `fastpath_tostring` corpus pins and the
cross-engine checksum equality continue to enforce the discipline on every
`make test`.

## 5. Updated Bottleneck Register (Rule 143: measured, owned, dated)

| # | Finding | Evidence | Planned fix | Target |
|---|---|---|---|---|
| 1 | Fresh-object allocation is deque+map+slots, not a bump arena | object_fields 0.45×; 1M news dominate | dedicated new-space bump allocator for JS objects (arena gains a young space); shrink `Object` (sparse map → unique_ptr) | v0.6 |
| 2 | One flat string allocation per concat | string_concat 0.39× | cons-strings with lazy flattening (semantics unchanged) | v0.6 |
| 3 | Per-handler dispatch+decode width; Ignition handlers are specialized machine code | fib 0.62×, int_loop 0.68× | Tier 1 copy-and-patch stencils per Laws Part I (Tier 0 remains the Rule 96 baseline) | Tier 1 milestone |
| 4 | Superinstruction width (LoadGlobal+Call, cmp+branch fusion) | fib call sites; kernel twins would need re-pinning | ISA additions with differential re-pin of bench twins | with Tier 1 |
| 5 | Feedback recording tax ~10% | fib --no-record: 34.7→31.2 ms | keep (Rule 124); Tier 1 stencil consumes the profiles | open |
| 6 | 16-byte Value (v0.4 #1) | CLOSED this release: geomean 0.49×→0.65×, two kernels crossed 1.0× | — | closed v0.5 |

## 6. Fairness Notes & Limitations

Same protocol as v0.3/v0.4: identical workloads, kernel-only timing,
medians over 5 runs after warmup, feedback ON for TurboScript, -O2 builds
on both sides (qjs built by its Release config).

- The `node-jit` column is a REFERENCE ONLY (full V8 JIT); all comparison
  claims are against `node --jitless` (pure Ignition, all JIT tiers off).
- The v0.3 twin-stability note stands: bench twins are byte-stable; the
  v0.5 gains come from the engine, not the kernels.
- Known environmental note (pre-existing at v0.4, not a v0.5 regression):
  under AddressSanitizer the `stack_overflow_rangeerror` corpus test
  overflows the machine stack before `kMaxCallDepth` (ASan frames are much
  larger than -O2 frames); with `ulimit -s 65536` it passes. ASan is clean
  on the rest of the positive corpus.

## 7. Reproducing

```bash
cd turboscript
make                                 # builds build-ts/tsrun
make test                            # 89-test corpus, both dispatch paths
python3 tests/bench/run_bench.py --with-jit-reference --out bench_results.json
```
