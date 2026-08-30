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

- **Entry paths** — Java source (`.java`) enters via the Frontend (lexer / parser -> AST -> binding -> RBC generation); Java classfiles enter via the Loader / Verifier / Quickener. Both entry paths converge at Register Bytecode (RBC).
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

Parse a Java file with the frontend driver:

```bash
./build/compiler/frontend/b2parse --dump-tokens SomeFile.java
./build/compiler/frontend/b2parse --dump-ast SomeFile.java
```

`b2parse` prints diagnostics to stderr and exits non-zero when any error-severity diagnostic was issued.

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

`b2plan` parses, verifies, and lowers RBC to a `StencilPlan` against the target-neutral v0 stencil manifest: stencil selection with superinstruction fusion, patch values (plan-computable vs runtime-pending), the native-to-RBC pc map, stack maps, deopt points, and translated exception edges. Refusals are the contract: methods containing ops without an Available stencil (un-quickened calls, `ldc`, `invokedynamic`) stay on T0 — exit 1 with `refused: <reason> <detail>`.

## Documentation

| Document | Purpose |
|---|---|
| `docs/laws.md` | The law system: 150 rules, Amendments A/B/C, Special Pass Laws (SWLP, PEA, NaN boxing) |
| `docs/special_passes.md` | Special pass designs (SMT-free): CM-PEA escape lattice + summary tables, speculative effect reordering with compensation, adaptive value representation |
| `docs/deopt_backend.md` | Deopt system and backend design: T0 canonical state, FrameState, deopt metadata, MIR pipeline, W^X publication |
| `docs/stencils.md` | Precompiled stencil system: T1 copy-and-patch composition, stencil format and categories, Stencil Rules 1-10 (Amendment C) |
| `docs/cpp26_standards.md` | C++26 code standards (CS-1..CS-13) and the Java/C++ two-domain testing contract |
| `docs/rbc_spec.md` | Register Bytecode (RBC) specification: frame model, 150-opcode set, type system, verification rules, quickening, text format |
| `docs/interp_contract.md` | Tier-0 interpreter state contract (v1): the T0 frame, value model, deopt entry (`resume`), quickened-opcode pins, state-dump fixture format |
| `docs/teams/README.md` | Team organization: eight teams of two AI roles (implementer + reviewer) |
| `docs/teams/messaging.md` | Inter-team message system |
| `docs/teams/ownership.yaml` | Machine-readable path ownership map |
| `docs/teams/frontend-team.md` | Frontend Team charter: the source path (Java source -> AST) |
| `docs/frontend_contract.md` | Frontend v0 interface contract: inputs, outputs, guarantees, future obligations |
| `docs/teams/*-team.md` | Team charters (frontend, interpreter, baseline-noir, ir, passes, regalloc, codegen, aot) |
| `docs/templates/` | Message and review templates |

## Laws First

Every commit to `compiler/`, `runtime/`, `tools/`, and `tests/` must comply with `docs/laws.md`. CI verifies compliance. There are no exceptions.

If any document conflicts with `docs/laws.md`, `docs/laws.md` wins.

## License

Apache License 2.0 — see `LICENSE`.
