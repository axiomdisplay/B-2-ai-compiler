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

## Documentation

| Document | Purpose |
|---|---|
| `docs/laws.md` | The law system: 150 rules, Amendments A/B, Special Pass Laws (SWLP, PEA, NaN boxing) |
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
