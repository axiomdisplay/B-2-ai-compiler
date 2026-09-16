# TurboScript Tier 0 — Why Interpreters Are Slow (Measured)

Owner directive: *"look at what is making us so slow. the interp, for the goal,
should be faster than T0 on V8."* This document records the performance
architecture, the measured baseline against V8 Ignition (`node --jitless`) and
QuickJS, the root causes of the remaining gap, and the prioritized plan to
close it. Rules 55/64: knowledge goes to documents, no undocumented debt.

Status: **work in progress toward the goal.** Semantics suite 81/81 passing
(`tests/semantics.js`).

## 1. Baseline (this machine, 2-core x86-64, -O3, best of 5)

| bench        | ts T0    | node --jitless (Ignition) | QuickJS | ts / Ignition | ts / QuickJS |
|--------------|----------|---------------------------|---------|---------------|--------------|
| fib          | 86.0ms   | 81.2ms                    | 54.4ms  | 0.94x         | 0.63x        |
| nbody        | 305.8ms  | 235.6ms                   | 163.5ms | 0.77x         | 0.53x        |
| binarytrees  | 1124.0ms | 547.5ms                   | 1394.1ms| 0.49x         | **1.24x**    |
| bubble       | 485.3ms  | 232.8ms                   | 242.6ms | 0.48x         | 0.50x        |
| properties   | 442.4ms  | 241.7ms                   | 339.6ms | 0.55x         | 0.77x        |

All engines print identical checksums (validated per bench). `node --jitless`
runs pure Ignition (Sparkplug/Maglev/TurboFan all disabled), so this is a
Tier-0-vs-Tier-0 comparison, per the owner's "JS interp only" baseline rule.

## 2. The performance architecture (what we did from day one)

Each decision below maps to a classic interpreter bottleneck:

1. **Computed goto, direct threading** (`interp.cpp`): `goto *disp[*ip & 0xFF]`
   per handler — no switch bounds check, per-opcode branch-predictor training.
2. **Fixed 32-bit instruction words** (Laws Part I Tier 0: 24-bit payload +
   8-bit opcode, 256 vregs): operand decode = byte extracts, no resync.
3. **Register bytecode, not stack**: no push/pop traffic; operands name vregs.
4. **NaN-boxed values with unboxed doubles** (`value.h`): doubles live inline
   in registers/frames. Ignition (pointer compression on) boxes every non-Smi
   double into a HeapNumber — our `nbody` never allocates for arithmetic.
5. **Monomorphic inline caches** on Get/SetNamed and GetGlobal: shape check +
   slot load. Prototyped properties cache (holder, holder shape, slot, depth).
6. **Contiguous value stack, callee frame = caller base + caller nRegs**: a
   call is an argument memcpy, zero heap allocation, frames never individually
   heap-allocated.
7. **Shape (hidden-class) transitions + interned keys**: own-property hit is a
   pointer compare plus a slot index; keys never compare by content on the hot
   path.
8. **GC safepoints only at loop backedges and calls** (Rule 88 analog): GC
   never lands mid-expression, so handlers need no write barriers.
9. **Superinstructions** (fused immediates): `AddImm/SubImm/MulSmi` and
   `LtSmi/LeSmi/GtSmi/GeSmi` fold literal operands into one word, removing
   constant-load traffic from loops (compiler peephole in `emitArith`).
10. **Smi fast paths in every bitwise handler**: smi == int32 in our value
    representation, so `i & 0xff` never touches a conversion helper.

## 3. What was making us slow (found and fixed this session)

These were real bugs/measurables found by profiling (gprof) and differential
testing — each with its measured cost:

| # | Problem | Fix | Effect |
|---|---------|-----|--------|
| 1 | **GC flag never cleared** — once the allocation threshold crossed, *every* backedge and call ran a full mark-sweep forever. binarytrees: 45s | clear the request flag in `runGC()` (re-armed by the next crossing) | 45s → 1.0s (45x) |
| 2 | **GC scanned dead stack regions** — stale values below the high-water mark (after exception unwinding) were marked; the next GC then walked freed objects (ASan heap-use-after-free) | restore `stackWords` from the TryFrame snapshot on exception landing | correctness |
| 3 | **Intern table split-brain** — `intern("length")` and `internUTF16(L"length")` produced two different String objects, so pointer-identity key checks failed and every array/string `.length` took the miss path | single canonical intern path | massive (all special props) |
| 4 | **`nRegs` recorded the final watermark, not the peak** — callee frames overlapped caller registers; argument loads stomped live locals (wrong `instanceof`, doubled method results) | track `peakReg`; never move the allocator downward on the call arg window | correctness |
| 5 | **Frame-size bookkeeping**: `function` declarations never stored (parser wrapped them in ExprStmt so the store path was dead) | decl marker + store | correctness |
| 6 | **`[[Construct]]` return** — interpreted constructors returned the raw `return` value instead of wrapping non-objects with the created `this` | construction flag + wrap on Return | correctness |
| 7 | **Bitwise ops called `toDoubleForArith` unconditionally** — 16M helper calls in properties.js | smi fast paths in all bitwise handlers | properties 527→454ms |
| 8 | Object = 144B header + separate 32B slots malloc | inline 3 slots + lazy Dict pointer (functions carry fn fields in JSFunction only) | binarytrees 1487→1144ms |

## 4. Why Ignition is still faster (the honest analysis)

1. **Accumulator machine = fewer dispatches.** Ignition's accumulator removes
   most operand Movs; our 3-address form emits ~20-30% more instructions for
   the same source. This is the single largest remaining structural factor
   (bubble, properties: ~2x).
2. **Torque-generated handlers.** V8's bytecode handlers are generated C++
   with hand-tuned register allocation and per-handler specialization (e.g.
   `KeyedLoadIC` handlers per elements kind). Our handlers are already tight,
   but generic.
3. **Nursery allocation.** Ignition allocates objects from a bump-pointer
   young generation with a filtered write barrier; we calloc per object. This
   is binarytrees' 2x gap (we still beat QuickJS there because QJS pays more
   per property access).
4. **Fused call sequences.** Ignition pairs `LdaNamedProperty` + `CallProperty`
   with receiver-passing builtins; our method calls are GetNamed + Call with a
   side-channel receiver, which is fine, but the callee prologue is heavier.

## 5. Roadmap to "faster than V8 T0" (prioritized)

1. **Accumulator-style codegen or liveness-driven 2-address folding** (est.
   1.3-1.5x on CPU benches): eliminate operand Movs like Ignition's Star.
2. **Size-class free-list allocator** (est. 1.3x on binarytrees): sweep pushes
   blocks onto per-size free lists; alloc pops O(1) with a memset — no
   system-allocator churn. Then a bump nursery once the GC can promote.
3. **Call-site fusion**: `CallNamed` superinstruction (GetNamed + Call in one
   dispatch) and a direct-call fast path for monomorphic call targets.
4. **Elements-kind specialization** (packed-smi arrays) for GetKeyed/SetKeyed:
   bounds check + raw smi load, no tagged decode.
5. **Loop-invariant global IC hoisting** and `GetGlobal` fusion into Call for
   hot call sites (call target feedback slot — also required by the Laws'
   "call target profiles" feedback mandate).
6. **Copy-and-patch Tier 1** (Laws Part I Tier 1): the interpreter also serves
   as the feedback source for the stencil JIT — after Tier 1 exists, "faster
   than T0" is achieved by tiering, which is the architecture's intent.

## 6. Registered MVP scope gaps (no silent cuts — Rule 150 discipline)

Classes, function hoisting, generators/async, for-of, switch, labels,
getters/setters, BigInt, Proxy, prototype-mutation IC invalidation, sparse
arrays, `arguments`, per-iteration loop-capture bindings, poly/megamorphic
ICs (mega falls back to generic), Error subclasses, String builder (Rope),
iterative GC marker (recursion depth-bounded by heap shape), OOM handling.
