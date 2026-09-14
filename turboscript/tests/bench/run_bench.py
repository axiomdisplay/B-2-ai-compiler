#!/usr/bin/env python3
"""TurboScript Tier 0 benchmark runner (interpreter-only comparison).

Engines:
  turbo        TurboScript Tier 0 (computed-goto dispatch) via tsrun --time
               (timing covers the run() execution phase only — assembly,
               verification and module load are excluded).
  node-jitless node --jitless — V8 Ignition interpreter ONLY (all JIT tiers
               off: no TurboFan, no Sparkplug, no Maglev).
  qjs          quickjs-ng qjs (pure interpreter, Release build).
  node-jit     OPTIONAL reference only (full V8 JIT) via --with-jit-reference.

Protocol per (bench, engine): 1 warmup + 5 measured runs, median reported.
Every kernel prints "RESULT <checksum>"; checksums MUST agree across engines
or the runner fails loudly (cross-engine differential check, Rule 36 spirit).
Timing: TurboScript — tsrun's elapsed_ms (kernel phase); JS engines —
in-script performance.now()/Date.now() around kernel(), printed as TIME.
"""
import argparse
import json
import math
import os
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TSRUN = os.path.abspath(os.path.join(HERE, "..", "..", "build-ts", "tsrun"))
QJS_DEFAULT = "/home/z/my-project/thirdparty/quickjs/build/qjs"

BENCHES = [
    ("int_loop",      "5,000,000 iters, djb2 int hash (Shl/Add/BitOr)"),
    ("fib",           "fib(27), naive double recursion (~635k calls)"),
    ("float_loop",    "mandelbrot-lite 200x200 grid, max 100 iters"),
    ("string_concat", "20k x (40-char build + number->string concat)"),
    ("object_fields", "1M x (new object, 3 shape stores, 3 IC loads)"),
    ("array_loop",    "300-elem smi array, 3k sum passes (900k loads)"),
    ("array_builtins","100k push + map/reduce/filter (300k callbacks)"),
]


def run_once(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError(f"exit {p.returncode}: {p.stderr.strip()[:400]}")
    result = None
    time_ms = None
    for line in p.stdout.splitlines():
        if line.startswith("RESULT"):
            result = int(line.split()[1])
    if result is None:
        # TurboScript kernels print "<label> <checksum>"; take the last token.
        lines = [l for l in p.stdout.splitlines() if l.strip()]
        if lines:
            try:
                result = int(lines[-1].split()[-1])
            except ValueError:
                pass
    for line in p.stdout.splitlines():
        if line.startswith("TIME"):
            time_ms = float(line.split()[1])
    for line in p.stderr.splitlines():
        if line.startswith("elapsed_ms="):
            time_ms = float(line.split("=", 1)[1])
    if result is None or time_ms is None:
        raise RuntimeError(f"missing RESULT/TIME: out={p.stdout!r} err={p.stderr!r}")
    return result, time_ms


def measure(cmd, warmup=1, reps=5):
    checksum = None
    times = []
    for i in range(warmup + reps):
        r, t = run_once(cmd)
        if checksum is None:
            checksum = r
        elif checksum != r:
            raise RuntimeError(f"checksum unstable: {checksum} vs {r}")
        if i >= warmup:
            times.append(t)
    return checksum, statistics.median(times), min(times)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--with-jit-reference", action="store_true")
    ap.add_argument("--qjs", default=QJS_DEFAULT)
    ap.add_argument("--out", default=None, help="write JSON results here")
    args = ap.parse_args()

    engines = [
        ("turbo", ["tsrun", "<f>", "--time"]),
        ("node-jitless", ["node", "--jitless", "<f>"]),
        ("qjs", [args.qjs, "<f>"]),
    ]
    if args.with_jit_reference:
        engines.append(("node-jit(ref)", ["node", "<f>"]))

    engine_meta = {
        "turbo": "TurboScript Tier 0 (computed goto)",
        "node-jitless": "V8 Ignition only (node --jitless)",
        "qjs": "quickjs-ng (pure interpreter)",
        "node-jit(ref)": "V8 full JIT (reference, not interp)",
    }

    table = {}
    checksums = {}
    failures = []

    for name, desc in BENCHES:
        tsbc = os.path.join(HERE, f"{name}.tsbc")
        jsf = os.path.join(HERE, f"{name}.js")
        print(f"\n=== {name} — {desc} ===", flush=True)
        for eng, tmpl in engines:
            path = tsbc if eng == "turbo" else jsf
            cmd = [c.replace("<f>", path) if c == "<f>" else c for c in tmpl]
            if eng == "turbo":
                cmd[0] = TSRUN
            try:
                cks, med, best = measure(cmd)
                rel = None
                print(f"  {eng:16s} median {med:9.2f} ms   (best {best:9.2f})  checksum {cks}", flush=True)
                table[(name, eng)] = med
                prev = checksums.setdefault(name, cks)
                if prev != cks:
                    failures.append(f"{name}: checksum mismatch across engines "
                                    f"({prev} vs {cks} on {eng}) — Rule 36 differential FAILURE")
            except Exception as e:  # noqa: BLE001
                print(f"  {eng:16s} FAILED: {e}", flush=True)
                failures.append(f"{name}/{eng}: {e}")

    # Ratios and geometric means (slowdown vs turbo).
    print("\n=== summary (median ms, slowdown x vs turbo) ===", flush=True)
    eng_names = [e[0] for e in engines]
    ratios = {e: [] for e in eng_names}
    header = "| kernel | " + " | ".join(f"{e} ms" for e in eng_names) + " |"
    sep = "|---" * (len(eng_names) + 1) + "|"
    rows = []
    for name, _ in BENCHES:
        cells = []
        base = table.get((name, "turbo"))
        for e in eng_names:
            ms = table.get((name, e))
            if ms is None:
                cells.append("FAIL")
                continue
            if base is not None and e != "turbo":
                ratios[e].append(ms / base)
                cells.append(f"{ms:.1f}" + f" ({ms / base:.2f}x)")
            else:
                cells.append(f"{ms:.1f}" if base is not None else f"{ms:.1f}")
        rows.append(f"| {name} | " + " | ".join(cells) + " |")
    print(header)
    print(sep)
    for r in rows:
        print(r)
    gm = {e: (math.exp(sum(math.log(x) for x in v) / len(v)) if v else None) for e, v in ratios.items()}
    gm_row = "| **geomean slowdown** | " + " | ".join(
        "1.00x" if e == "turbo" else (f"{gm[e]:.2f}x" if gm[e] else "-") for e in eng_names) + " |"
    print(gm_row)

    if failures:
        print("\nFAILURES:", flush=True)
        for f in failures:
            print("  " + f, flush=True)
        sys.exit(1)

    if args.out:
        payload = {
            "benchmarks": [{"name": n, "desc": d} for n, d in BENCHES],
            "engines": engine_meta,
            "median_ms": {f"{n}|{e}": v for (n, e), v in table.items()},
            "geomean_slowdown_vs_turbo": {k: v for k, v in gm.items()},
        }
        with open(args.out, "w") as fh:
            json.dump(payload, fh, indent=2)
        print(f"\nwrote {args.out}", flush=True)


if __name__ == "__main__":
    main()
