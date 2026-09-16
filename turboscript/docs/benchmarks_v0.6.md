# TurboScript Tier 0 — Interpreter-Only Benchmark Results (v0.6)

**Status:** v0.6 — allocation + string representation milestone (register
items #1 and #2 closed; accessor-epoch IC guard + feedback-slot reorder)
**Owner:** TurboScript Interp Team
**Last Updated:** 2026-09-16
**Governing Laws:** Rules 36 (differential testing), 55/64 (durable docs), 96
(Tier 0 as semantic baseline), 124 (determinism), 143 (tracked follow-ups)
**Suite:** `tests/bench/` (7 kernels + `run_bench.py`)
**Supersedes:** `benchmarks_v0.5.md` (kept as the v0.5 history; its Section 5
register items #1/#2 are closed below).

## 1. What Changed Since v0.5

The owner goal is unchanged: Tier 0 must be faster than V8's Tier 0
(Ignition, measured via `node --jitless`). v0.5's Section 3 measured the
remaining gap and registered the v0.6 items: bump-allocated young space +
Object shrink (item #1) and cons-strings (item #2). Both shipped, plus two
unregistered findings closed in-session (accessor-epoch guard, slot-layout
reorder).

1. **Bump-allocated object space + Object shrink (register item #1,
   CLOSED).** `Heap::makeObject` now allocates from `BumpArena<Object>`
   (ts_object.h): segment-chained bump allocation (256 objects/segment,
   placement-new, addresses stable for the Isolate lifetime — the same
   never-collect contract as the deque it replaces, bytecode_spec.md
   Section 11). `Object` shrank from ~136 to ~80 bytes: the sparse-element
   `std::map` (48 bytes, constructed in EVERY object including plain ones
   that can never use it) moved out of line behind `unique_ptr`
   (`sparseMap()`/`ensureSparse()` accessors; allocated only on a first
   sparse-element write, i.e. indices >= kMaxDenseElements).
   Consequences for object_fields (1M news per run):
   - Object construction is a pointer bump + placement-new (no deque
     bookkeeping, no 48-byte map ctor/dtor per object).
   - Per-object heap footprint 136 -> ~80 bytes (-41%), so the 1M-object
     working set loses ~56 MB of cache pressure.
2. **Cons-strings with lazy flattening (register item #2, CLOSED).**
   `StringObj` gained two representations (ts_value.h): `kFlat` (materialized
   UTF-16 payload, as before) and `kCons` (a concat node holding its two
   operands; non-owning pointers, Heap-owned nodes). `Heap::makeCons` builds
   the node without copying or flattening either operand; results shorter
   than `kMinConsLength` (13, named constant, Rule 23) build flat eagerly.
   `StringObj::flat()` materializes IN PLACE (iterative DFS with an explicit
   stack — `s += piece` loops build left-leaning trees of depth == iteration
   count, so a recursive flatten would overflow the machine stack), after
   which the node keeps its identity/address and every consumer observes the
   flat text. Length/emptiness/bounds checks read the header field — they
   never flatten (StringLength is now O(1) on any representation).
   The `+`/`StringConcat` paths (`Isolate::addValues`, `L_StringConcat`) are
   now allocation-free of copies: one 56-byte header per concat.
   Overflow guard: combined length above `kMaxStringCodeUnits` makes
   `makeCons` return nullptr and the call sites raise a proper JS
   RangeError (Rule 74). Previously the same input would have hit
   `std::length_error` inside libstdc++ and aborted the process under
   -fno-exceptions — a latent native-crash path, now a semantic exception.
3. **Accessor-epoch transition-IC guard (new, unregistered finding).** The
   v0.3 transition-IC hit guard re-verified "no accessor claims this key on
   the prototype chain" PER HIT with a full chain walk
   (`chainHasAccessor`: proto hop x shape-chain walk; Object.prototype's
   ~20-method chain made this ~40 shape-node steps per fresh-property
   store). v0.6 replaces the per-hit walk with one u64 compare against
   `Isolate::accessorEpoch_`: any accessor definition anywhere
   (`defineProperty`/`definePropertyDescriptor`, both replace-in-place and
   fresh-slot paths) bumps the epoch and conservatively retires every
   transition IC until re-install re-proves the claim through the real slow
   path. Guard is strictly MORE conservative than the walk it replaces
   (global invalidation vs per-chain); per-hit cost O(chain) -> O(1).
   Ordinary-object [[Prototype]] swaps do not exist in the v0.1 ISA
   (bytecode_spec.md Section 11), so accessor definition is the only
   invalidation trigger; the comment in ts_interpreter.h pins this.
4. **Transition-IC lane slot reserve (new, unregistered finding).** The
   dispatch transition-IC fast lane pushed slots WITHOUT the
   `reserveForSlotPush` floor (v0.5 fixed only the `defineProperty` slow
   path), so a fresh 3-slot object still paid the 0->1->2->4 vector growth
   (three allocations) on the HOT path. `reserveForSlotPush` moved to
   ts_object.h and is now shared by both push sites: one allocation per
   fresh object.
5. **FeedbackSlot hot->cold field reorder (storage-only).** The new
   `icEpoch` field initially pushed the binary-op histogram deeper into the
   slot; fields are now ordered hot -> cold (kind + icShape/icSlot +
   classCounts + branch counters first; transition-IC shapes + epoch last).
   Same members, same semantics; feedback layout is internal.

No ISA changes: v0.1 opcode numbering is untouched and the corpus is
byte-stable. Build flags unchanged (-O2).

## 2. Results (median ms; slowdown x vs turbo; 1 warmup + 5 runs)

| kernel | workload | turbo | node-jitless | qjs | node-jit (ref) |
|---|---|---|---|---|---|
| int_loop | 5M-iter djb2 int hash | 241.5 | 157.8 (0.65×) | 440.4 (1.82×) | 28.5 (0.12×) |
| fib | fib(27) naive recursion (~635k calls) | 35.7 | 21.6 (0.61×) | 40.4 (1.13×) | 2.2 (0.06×) |
| float_loop | mandelbrot-lite 200×200 | 69.1 | 74.2 (1.07×) | 79.1 (1.14×) | 3.8 (0.05×) |
| string_concat | 20k × (40-char build + num→str) | 46.2 | 25.6 (0.55×) | 74.3 (1.61×) | 8.6 (0.19×) |
| object_fields | 1M × (new obj, 3 stores, 3 loads) | 176.0 | 97.5 (0.55×) | 406.2 (2.31×) | 9.8 (0.06×) |
| array_loop | 300-elem smi array, 3k sum passes | 29.7 | 17.9 (0.60×) | 54.6 (1.84×) | 2.3 (0.08×) |
| array_builtins | 100k push + map/reduce/filter (300k callbacks) | 16.8 | 11.6 (0.69×) | 25.7 (1.54×) | 6.1 (0.36×) |
| **geometric mean** | | **1.00×** | **0.66×** | **1.58×** | **0.10×** |

**Headline (like-for-like turbo-vs-turbo, same machine, medians of 5):**
string_concat 64.4 -> 46.2 ms (**-28%**), object_fields 216.7 -> 176.0 ms
(**-19%**). TurboScript is now 1.58× ahead of quickjs-ng on the geomean
(v0.5: 1.48×), driven by exactly the kernels the register predicted, and
float_loop again beats V8 Ignition outright (1.07×).

### Honest accounting of the geomean (Rule 124: record what is measured)

- The published geomean vs node-jitless moved only 0.65× -> 0.66×, for two
  reasons that are NOT engine regressions in the kernels we shipped:
  1. The node-jitless array_builtins baseline shifted 16.9 -> 11.6 ms
     BETWEEN SESSIONS (V8 did not change; our time is identical at 16.8 ms).
     Against the v0.5 baseline the same kernel is ~1.00×, and the v0.6
     like-for-like geomean is ~0.69×.
  2. int_loop/fib/array_loop measure a consistent ~3% slower in every v0.6
     run (242 vs 234.8 ms int_loop). Section 3 below registers the
     mechanism and the follow-up.
- The v0.5 twin-stability note stands: bench twins are byte-stable; v0.6
  gains come from the engine, not the kernels.
- All checksums agree on all 7 kernels × 4 engines (the runner fails loudly
  otherwise): int_loop `473692005`, fib `196418`, float_loop `726419`,
  string_concat `948890`, object_fields `2109827392`, array_loop
  `269100000`, array_builtins `599990`.

### Version deltas (geomean vs node-jitless = V8 Ignition)

| baseline | v0.3 | v0.4 | v0.5 | v0.6 |
|---|---|---|---|---|
| node-jitless (V8 Ignition) | 0.27× | 0.49× | 0.65× | **0.66×** (0.69× like-for-like) |
| qjs (quickjs-ng) | 0.68× | 1.16× | 1.48× | **1.58×** |

Raw data: `tests/bench/bench_results.json` (regenerate with
`python3 tests/bench/run_bench.py --with-jit-reference --out bench_results.json`).

## 3. Reading the Numbers — What Is STILL Making Us Slow

1. **int_loop/fib/array_loop: ~3% code-layout regression (NEW, registered).**
   The v0.6 handler edits (SetProperty/StringConcat/CharCodeAt in the same
   dispatch function) shifted computed-goto label layout and alignment; the
   int kernels measured consistently ~3% slower across every v0.6 run, and a
   hot->cold FeedbackSlot field reorder did NOT recover it (data layout
   ruled out; both dispatch builds share the shift). Label-alignment swings
   of ±3-5% are a known property of threaded dispatch loops. Registered as
   a Tier 1 follow-up: copy-and-patch regenerates handler machine code
   wholesale per Laws Part I, which supersedes C++-level layout tuning.
   (Rule 143: measured, owned, dated — see Section 5 #6.)
2. **object_fields (0.55×) — slots vector still mallocs per fresh object.**
   The remaining allocation in the 1M-news loop: the slot vector's 32-byte
   buffer (one malloc after the v0.6 reserve floor). The registered next
   lever is slot storage from the object bump arena (requires an allocator
   parameter through `std::vector`, or inline storage for k <= 4 slots) —
   registered as Section 5 #1, v0.7 candidate.
3. **string_concat (0.55×) — flatten happens once, on display.** The kernel
   still beats v0.5 by 28% because the inner loop never materializes; the
   remaining cost is 41 cons-node allocations per iteration (deque emplace).
   Bump-allocating string nodes like objects (Section 5 #2) is the next
   lever; beyond that the kernel is allocation-bound, not copy-bound.
4. **fib (0.61×) — handler-code quality, unchanged diagnosis.** The v0.5
   conclusion stands: the call round-trip cost is frame setup/teardown and
   per-handler dispatch + decode width; the portable equivalent of
   Ignition's specialized handlers is Tier 1 copy-and-patch (Laws Part I).
5. **Feedback recording tax** stays ~10% (v0.5 measurement, unchanged
   design). Always-on per Rule 124; the Tier 1 stencil consumes the
   profiles.

## 4. Fast-Path Guard Discipline (interp_contract.md 3.4)

Every lane added in v0.6 (cons-strings, accessor-epoch guard, slot reserve
in the IC lane) is representation/guard-level: no observable semantics
moved. Evidence:
- 89/89 corpus green on BOTH dispatch paths (computed goto + switch), which
  includes the string/property differential pins (`fastpath_smi_edges`,
  `fastpath_tostring`, string semantics tests).
- ASan+UBSan clean on the positive corpus (41/41 with the documented
  `ulimit -s 65536` for the ASan stack-depth inflation note).
- Checksums identical across all engines on every kernel.
- The accessor-epoch guard is strictly more conservative than the walk it
  replaced: any accessor add anywhere retires every transition IC globally.
- Cons-strings are flattened before any text observable can read them
  (keys intern, ToNumber/ToBigInt, comparisons, CharCodeAt in-range reads,
  display/export); length/emptiness are header reads and cannot distinguish
  representations.

## 5. Updated Bottleneck Register (Rule 143: measured, owned, dated)

| # | Finding | Evidence | Planned fix | Target |
|---|---|---|---|---|
| 1 | Slot vector still mallocs per fresh object | object_fields 176ms/1M news = ~1 malloc/obj remains | slot storage from the object bump arena (allocator through std::vector or inline k<=4 storage) | v0.7 |
| 2 | Cons nodes pay deque emplace per concat | string_concat 46.2ms; 41 node allocs/iter | BumpArena<StringObj> for string nodes (same contract as objects) | v0.7 |
| 3 | Per-handler dispatch+decode width; Ignition handlers are specialized machine code | fib 0.61×, int_loop 0.65× | Tier 1 copy-and-patch stencils per Laws Part I (Tier 0 remains the Rule 96 baseline) | Tier 1 milestone |
| 4 | Superinstruction width (LoadGlobal+Call, cmp+branch fusion) | fib call sites; kernel twins would need re-pinning | ISA additions with differential re-pin of bench twins | with Tier 1 |
| 5 | Feedback recording tax ~10% | fib --no-record (v0.5 measurement) | keep (Rule 124); Tier 1 stencil consumes the profiles | open |
| 6 | ~3% int-kernel code-layout regression from v0.6 handler edits | int_loop 234.8 -> 241.5 ms, stable across runs; data reorder ruled out | superseded by Tier 1 handler regeneration (#3); do NOT hand-tune alignment in Tier 0 | Tier 1 milestone |
| 7 | Fresh-object allocation was deque+map+slots | CLOSED this release: object_fields 216.7 -> 176.0 ms (-19%), Object -41% | — | closed v0.6 |
| 8 | One flat string allocation per concat | CLOSED this release: string_concat 64.4 -> 46.2 ms (-28%) | — | closed v0.6 |
| 9 | 16-byte Value (v0.4 #1) | CLOSED v0.5 | — | closed v0.5 |

## 6. Fairness Notes & Limitations

Same protocol as v0.3–v0.5: identical workloads, kernel-only timing,
medians over 5 runs after warmup, feedback ON for TurboScript, -O2 builds
on both sides (qjs built by its Release config).

- The `node-jit` column is a REFERENCE ONLY (full V8 JIT); all comparison
  claims are against `node --jitless` (pure Ignition, all JIT tiers off).
- Cross-session caveat: the node-jitless array_builtins baseline moved
  16.9 -> 11.6 ms between the v0.5 and v0.6 measurement sessions on the
  same machine (environmental, not an engine change; our time is
  unchanged). Section 2 reports both the published and the like-for-like
  geomean so neither direction is overstated.
- The ASan note from v0.5 stands: under AddressSanitizer the
  `stack_overflow_rangeerror` corpus test overflows the machine stack
  before `kMaxCallDepth` (ASan frames are much larger than -O2 frames);
  with `ulimit -s 65536` the full corpus passes. ASan+UBSan is clean on
  the rest of the positive corpus.

## 7. Reproducing

```bash
cd turboscript
make                                 # builds build-ts/tsrun
make test                            # 89-test corpus, both dispatch paths
python3 tests/bench/run_bench.py --with-jit-reference --out bench_results.json
```
