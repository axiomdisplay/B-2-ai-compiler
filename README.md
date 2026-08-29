# B-2 — AI-Compiler

B-2 is a Java/JVM runtime and tiered compiler experiment, written in C++26 and built by AI-agent teams operating under a strict law system.

## Tier Model

```text
Java classfiles
      |
      v
Loader / Verifier / Quickener
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

- **T0** — direct-threaded register interpreter; the universal correctness fallback and deopt reconstruction target.
- **T1** — no-IR baseline JIT (template / copy-and-patch); no IR graph, no optimization passes (Amendment A).
- **T2** — optimizing JIT; first IR tier; sea-of-nodes; full pass suite including SWLP, PEA, and NaN boxing lowering.
- **T3** — AOT / static JIT; drives the same T2 pipeline offline; manifests and mechanically checked proofs.

## Documentation

| Document | Purpose |
|---|---|
| `docs/laws.md` | The law system: 150 rules, Amendments A/B, Special Pass Laws (SWLP, PEA, NaN boxing) |
| `docs/teams/README.md` | Team organization: seven teams of two AI roles (implementer + reviewer) |
| `docs/teams/messaging.md` | Inter-team message system |
| `docs/teams/ownership.yaml` | Machine-readable path ownership map |
| `docs/teams/*-team.md` | Team charters (interpreter, baseline-noir, ir, passes, regalloc, codegen, aot) |
| `docs/templates/` | Message and review templates |

## Laws First

Every commit to `compiler/`, `runtime/`, `tools/`, and `tests/` must comply with `docs/laws.md`. CI verifies compliance. There are no exceptions.

If any document conflicts with `docs/laws.md`, `docs/laws.md` wins.

## License

Apache License 2.0 — see `LICENSE`.
