# B-2 — AI-Compiler

B-2 is a Java/JVM runtime and tiered compiler experiment, written in C++26 and built by AI-agent teams operating under a strict law system.

## Tier Model

```text
Java source (.java)                      Java classfiles
      |                                         |
      v                                         v
Frontend                               Loader / Verifier /
(lexer / parser -> AST ->                   Quickener
binding -> RBC generation)
      |                                         |
      +-----------------------------------------+
      |
      v
Register Bytecode (RBC)
      |
      +--------------------+----------------------+
      |                    |                      |
      v                    v                      v
   T0 Interpreter      T1 No-IR Baseline      T2 Optimizing JIT
   direct-threaded     template/copy-patch    sea-of-nodes IR
      |                    |                      |
      +--------------------+----------+-----------+
                                    |
                                    v
                                  T3 AOT
                         offline T2-style pipeline
```

- **Entry paths** — Java source (`.java`) enters via the Frontend (lexer / parser -> AST -> **AST-to-RBC lowering, landed v1**); Java classfiles enter via the Loader / Verifier / Quickener. Both entry paths converge at Register Bytecode (RBC).
- **T0** — direct-threaded register interpreter; the universal correctness fallback and deopt reconstruction target.
- **T1** — no-IR baseline JIT (template / copy-and-patch); no IR graph, no optimization passes (Amendment A).
- **T2** — optimizing JIT; first IR tier; sea-of-nodes; full pass suite including SWLP, PEA, and NaN boxing lowering.
- **T3** — AOT / static JIT; drives the same T2 pipeline offline; manifests and mechanically checked proofs.

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Parse a Java file with the frontend driver, or lower it all the way to
Register Bytecode:

```bash
./build/compiler/frontend/b2parse --dump-tokens SomeFile.java
./build/compiler/frontend/b2parse --dump-ast SomeFile.java
./build/compiler/frontend/b2parse --emit-rbc SomeFile.java > SomeFile.rbc
```

`b2parse` prints diagnostics to stderr and exits non-zero when any error-severity diagnostic was issued.

**Source to execution is closed**: lower a Java file and run it on the T0
interpreter:

```bash
./build/compiler/frontend/b2parse --emit-rbc Hello.java > Hello.rbc
./build/compiler/interp/b2run Hello.rbc
```

Every method the v1 lowering emits passes the RBC verifier by construction;
unsupported constructs (lambdas, string concatenation, enums, ...) are
refused with diagnostics - never silently miscompiled
(`docs/frontend_contract.md`).

Verify and dump an RBC text file with the middle-end driver:

```bash
./build/compiler/rbc/b2rbc --verify-only tests/rbc/corpus/arithmetic.rbc   # parse + verify
./build/compiler/rbc/b2rbc tests/rbc/corpus/controlflow.rbc               # also print it back
```

`b2rbc` prints parse errors as `file:offset:` and verification errors as `file [method descriptor]: pc N: error: ...`, and exits non-zero on any error.

Execute an RBC text program in Tier 0 with the interpreter driver:

```bash
./build/compiler/interp/b2run tests/interp/corpus/fib_loop.rbc     # run main()V, print program stdout
./build/compiler/interp/b2run tests/interp/corpus/div_catch.rbc    # exception handling end to end
./build/compiler/interp/b2run prog.rbc --entry run "(I)I" --stats  # custom entry + counters
```

`b2run` parses, verifies (the hard gate: the interpreter refuses unverified input), and executes in T0. Program output goes to stdout; an uncaught exception is reported to stderr in the Java launcher shape (`Exception in thread "main" <class>: <message>`) with exit status 1.

Compile a Tier-1 stencil plan from verified RBC with the baseline driver:

```bash
./build/compiler/baseline/b2plan tests/interp/corpus/uncaught.rbc            # plan main()V, print the golden dump
./build/compiler/baseline/b2plan fields.rbc --entry bump "(Ljava/lang/Object;)V"  # fused aload_getfield + pending runtime patches
./build/compiler/baseline/b2plan prog.rbc --no-fusion --check-only           # opcode-stencil-only, audit without dumping
```

`b2plan` parses, verifies, and lowers RBC to a `StencilPlan` against the stencil manifest: stencil selection with superinstruction fusion, patch values (plan-computable vs runtime-pending), the native-to-RBC pc map, stack maps, deopt points, and translated exception edges. Refusals are the contract: methods containing ops without an Available stencil (`invokedynamic`, `guard_class`, `deopt_trap`, `multianewarray`) stay on T0 — exit 1 with `refused: <reason> <detail>`.

```bash
./build/compiler/codegen/b2jit tests/interp/corpus/fields.rbc          # execute on Tier-1 machine code
./build/compiler/frontend/b2parse --emit-rbc tests/frontend/lower_corpus/fib.java | ./build/compiler/codegen/b2jit   # source -> x86-64
./build/compiler/codegen/b2jit prog.rbc --stats --code fib             # tier counters + compiled-code dump
```

`b2jit` is the Tier-1 execution driver: it loads the build-time stencil archive (generated by `tools/stencilgen` — Stencil Rule 1), compiles the plan, instantiates it as x86-64 code (copy-and-patch at declared holes only — Stencil Rule 3), publishes it W^X, and runs it. Field/array/monitor/call ops execute through the runtime-helper seam; every trap deoptimizes to T0 with a fully reconstructed frame (Amendment B.3); exceptions caught in-frame re-enter compiled code at the handler entry. Its output is byte-identical to `b2run` on every program both can run (the differential law in `tests/codegen`).

## Documentation

| Document | Purpose |
|---|---|
| `docs/laws.md` | The law system: 150 rules, Amendments A/B/C, Special Pass Laws (SWLP, PEA, NaN boxing) |
| `docs/special_passes.md` | Special pass designs (SMT-free): CM-PEA escape lattice + summary tables, speculative effect reordering with compensation, adaptive value representation |
| `docs/icdg.md` | ICDG — Inline Call/Dispatch Graph: the shared call/dispatch decision engine (dispatch states, unlock-value inline score model, invalidation, tier integration) |
| `docs/gc.md` | GCR — Generational Concurrent Region-Based Collector: heap layout, barriers, collection phases, stack scanning, NUMA scaling, Java-visible testing |
| `docs/codegen_contract.md` | T1 instantiation: the build-time stencil archive, copy-and-patch instantiator, W^X discipline, runtime-helper ABI, the T1 activation record, deopt and exception dispatch |
| `docs/deopt_backend.md` | Deopt system and backend design: T0 canonical state, FrameState, deopt metadata, MIR pipeline, W^X publication |
| `docs/stencils.md` | Precompiled stencil system: T1 copy-and-patch composition, stencil format and categories, Stencil Rules 1-10 (Amendment C) |
| `docs/cpp26_standards.md` | C++26 code standards (CS-1..CS-13) and the Java/C++ two-domain testing contract |
| `docs/rbc_spec.md` | Register Bytecode (RBC) specification: frame model, 150-opcode set, type system, verification rules, quickening, text format |
| `docs/ir_spec.md` | Sea-of-nodes IR specification (T2/T3, v2): storage model, IRType lattice, all 137 node kinds with input signatures/effects/verifier constraints, fixed-node control chaining + memory Phis, FrameState/virtual-object/SpecMeta/dependency structures, serializer format, printer format |
| `docs/effect_system.md` | The explicit effect model (Rule 121): the 12 effect classes, the 144-entry reorder table with construction rules R1-R9, the compensation-node contract, enforcement |
| `docs/graph_builder.md` | The RBC-to-IR graph builder contract (T2 pipeline entry): SSA slot discipline, guard placement, FrameState policy, the v1 exception-deopt policy, class-init triggers, quickened ops, determinism |
| `docs/inlining.md` | Inlining v1 — the ICDG direct-inline engine: the transformation and its soundness argument, the id-space contract (the CalleeSource), inlined-frame deopt/exception conventions, budgets, refusal catalog |
| `docs/pass_contracts.md` | The T2/T3 optimization pass suite (v1): pass registry with contracts, the early-cleanup + GVN rewrite catalogs, the tombstone-law soundness protocol, budgets, kill switches, telemetry |
| `docs/interp_contract.md` | Tier-0 interpreter state contract (v1): the T0 frame, value model, deopt entry (`resume`), quickened-opcode pins, state-dump fixture format |
| `docs/teams/README.md` | Team organization: nine teams of two AI roles (implementer + reviewer), including the GC Team |
| `docs/teams/messaging.md` | Inter-team message system |
| `docs/teams/ownership.yaml` | Machine-readable path ownership map |
| `docs/teams/frontend-team.md` | Frontend Team charter: the source path (Java source -> AST) |
| `docs/frontend_contract.md` | Frontend v0 interface contract: inputs, outputs, guarantees, future obligations |
| `docs/teams/*-team.md` | Team charters (frontend, interpreter, baseline-noir, ir, passes, regalloc, codegen, aot, gc) |
| `docs/templates/` | Message and review templates |

## Laws First

Every commit to `compiler/`, `runtime/`, `tools/`, and `tests/` must comply with `docs/laws.md`. CI verifies compliance. There are no exceptions.

If any document conflicts with `docs/laws.md`, `docs/laws.md` wins.

## License

Apache License 2.0 — see `LICENSE`.
