# TurboScript Tier 0 — Scope & Status

**What this is:** the Laws Part I "Tier 0: Direct-Threaded Register
Interpreter" — computed goto, register bytecode, 24-bit fixed-width
instructions, 256 vregs/frame, inline-cache shape feedback, collecting
call/type/branch feedback for the future tiers. Written to the Master
Architecture Spec + the 150 Compiler Laws (see docs/laws/ in B-2-ai-compiler).

**Build:** `make` (g++ >= 12, uses GCC computed-goto extension).
**Run:** `./ts file.js` — `--dis` dumps bytecode, `--bench N` re-runs N times.
**Test:** `./ts tests/semantics.js` (81 assertions, Part 0 contract checks).
**Bench:** `python3 scripts/bench.py` (ts vs node --jitless vs QuickJS),
results + analysis in docs/PERF-NOTES.md.

**Implemented:** lexer/parser (functions, closures, template literals,
try/catch/finally with completion dispatch, for/for-in, all operators),
two-pass compiler with capture analysis and static context chains,
NaN-boxed values with unboxed doubles, shape-transition object model with
dense slots + packed elements + dictionary fallback, monomorphic inline
caches, mark-sweep GC with safepoint-only collection and scrub-free stack
accounting, Math/Array/String/Object/console builtins, `new` with
[[Construct]] semantics, TDZ enforcement.

**Registered gaps** (tracked in docs/PERF-NOTES.md §6): classes, function
hoisting, generators/async, for-of, switch, labels, getters/setters, BigInt,
Proxy, prototype-mutation IC invalidation, sparse arrays, `arguments`,
per-iteration loop-capture bindings, poly/megamorphic ICs, OOM handling.
