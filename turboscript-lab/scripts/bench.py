#!/usr/bin/env python3
"""TurboScript Tier 0 benchmark harness.
Compares ts vs node --jitless (V8 Ignition) vs QuickJS.
Wall-clock, 5 runs each, reports best."""
import subprocess, time, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, "bench")
ENGINES = {
    "ts (TurboScript T0)": [os.path.join(ROOT, "ts")],
    "node --jitless (V8 Ignition)": ["node", "--jitless", "--require",
                                     os.path.join(BENCH, "node_shim.js")],
    "QuickJS": ["/tmp/quickjs/qjs"],
}
BENCHES = ["fib", "nbody", "binarytrees", "bubble", "properties"]
RUNS = 5

def once(engine, script):
    t0 = time.perf_counter()
    r = subprocess.run(engine + [script], capture_output=True, text=True, timeout=300)
    dt = time.perf_counter() - t0
    if r.returncode != 0:
        return None, r.stderr.strip()[:120]
    return dt, r.stdout.strip().replace("\n", " | ")

results = {}
for b in BENCHES:
    script = os.path.join(BENCH, b + ".js")
    row = {}
    for name, engine in ENGINES.items():
        times = []
        err = None
        for _ in range(RUNS):
            dt, out = once(engine, script)
            if dt is None:
                err = out
                break
            times.append(dt)
            if _ == 0:
                ref_out = out
        row[name] = (min(times) if times else None, err)
    results[b] = row

print(f"{'bench':<14} {'ts T0':>10} {'Ignition':>10} {'QuickJS':>10}   ts vs Ignition | ts vs QJS")
print("-" * 78)
for b in BENCHES:
    row = results[b]
    t = row["ts (TurboScript T0)"][0]
    v8 = row["node --jitless (V8 Ignition)"][0]
    qjs = row["QuickJS"][0]
    def fmt(x): return f"{x*1000:8.1f}ms" if x else "   FAIL"
    r1 = f"{v8/t:6.2f}x faster" if t and v8 else "   n/a"
    r2 = f"{qjs/t:6.2f}x faster" if t and qjs else "   n/a"
    errs = [f"{n}: {row[n][1]}" for n in row if row[n][1]]
    print(f"{b:<14} {fmt(t):>10} {fmt(v8):>10} {fmt(qjs):>10}   {r1} | {r2}")
    for e in errs: print(f"    !! {e}")
