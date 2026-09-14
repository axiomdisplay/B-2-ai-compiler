# tests/bench — Interpreter-Only Benchmark Suite

Six kernels, each shipped twice: TSBC assembly (`.tsbc`) for TurboScript
Tier 0 and JS (`.js`) for the comparison engines. Results and analysis:
`../../docs/benchmarks_v0.2.md`.

## Run

```bash
python3 run_bench.py --with-jit-reference --out bench_results.json
```

Requirements: `build-ts/tsrun` (run `make` first), `node` on PATH,
quickjs-ng `qjs` (path overridable with `--qjs`).

## Engines

- **turbo** — `tsrun --time` (kernel phase only; feedback collection on).
- **node-jitless** — `node --jitless` (V8 Ignition only, all JIT off).
- **qjs** — quickjs-ng pure interpreter (Release build).
- **node-jit(ref)** — optional full-JIT reference via `--with-jit-reference`.

## Kernels

| file | measures |
|---|---|
| `int_loop` | integer hot loop: Shl/Add/BitOr, Smi tagging, branch IC |
| `fib` | call machinery: frames, args window, recursion, closures |
| `float_loop` | double arithmetic + double branch loop (mandelbrot-lite) |
| `string_concat` | string allocation, concat, number→string, StringLength |
| `object_fields` | NewObject, shape-transition stores, monomorphic IC loads |
| `array_loop` | NewArray fill + repeated packed-Smi element loads |
| `array_builtins` | v0.3 builtins layer: 100k push + map/reduce/filter (300k callback invocations) |

## Protocol

1 warmup + 5 measured runs per (kernel, engine); median reported. Every
kernel prints a checksum; the runner **fails if engines disagree** — the
suite doubles as a cross-engine differential test (Rule 36 spirit).
`.tsbc` and `.js` twins must keep identical operation order when edited.
