# B-2 Frontend Interface Contract (v0)

Owner: Frontend Team (`frontend`; see `docs/teams/frontend-team.md`)

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

This is the v0 contract for the Java-source frontend. It defines what the
frontend consumes, what it produces, what it guarantees, what it explicitly
does not guarantee yet, and what it owes later versions.

## Inputs

- A Java source file (`.java`), UTF-8 encoded.
- The file path (used by `SourceManager` for file identity and diagnostics).
- Nothing else. The frontend is the first stage of the source path; it
  consumes no other B-2 subsystem's output.

Input is untrusted. Content may be valid Java, invalid Java, truncated,
malformed, or binary. The frontend never assumes well-formedness.

## Outputs

- A token stream covering the entire input (`--dump-tokens`).
- A lossless, position-annotated AST (`--dump-ast`): every node carries a
  `SourceRange` that exactly matches the source text it was parsed from.
- Diagnostics: severity, source location (file, line, column, range), a
  clear human-readable message, expected versus actual state, and a
  suggested fix or recovery action (Rule 47).
- An exit status for `b2parse`: zero only when no error-severity diagnostic
  was issued; non-zero otherwise (including tool I/O failures).

Public surface: `include/b2/frontend/` (`SourceManager.h`, `Diagnostics.h`,
`Token.h`, `Lexer.h`, `Parser.h`, `AstDumper.h`, `ast/Ast.h`).

## Guarantees

- Lossless positions. Token ranges tile the input; each AST node range
  matches its source text exactly. Nothing is silently dropped, rewritten,
  or normalized away.
- Error recovery. Parsing resynchronizes at statement, declaration, and
  member boundaries and reports multiple diagnostics per file. After
  errors, the partial AST remains inspectable.
- Determinism. Same source, same compiler version, same flags produce
  byte-identical token streams, ASTs, and dumps (Rule 124 discipline).
  No timestamps, addresses, or iteration-order dependence in output.
- Robustness. Arbitrary input never crashes, hangs unboundedly, or reads
  out of bounds (Rule 63 discipline). Malformed input yields diagnostics.
- Diagnostic quality per Rule 47: location, message, expected versus
  actual, suggested fix — never opaque errors.

## Non-Guarantees (v0)

- No semantic resolution, binding, or type-checking. Names are unresolved;
  overload resolution, type attribution, and generic instantiation checks
  do not exist yet. `int x = "hello";` parses cleanly, by design.
- No semantic validation: reachability, definite assignment, checked
  exceptions, and access rules are not checked.
- No RBC emission. The AST is the final product of v0; no bytecode is
  generated.
- Syntax coverage targets current Java syntax; conformance gaps are bugs,
  and unsupported constructs are diagnosed as unsupported rather than
  silently mis-parsed.
- Diagnostic message wording is not frozen; it may change between versions.

## Future Obligations

- AST-to-RBC lowering conforming to the RBC pipeline laws, feeding the
  execution pipeline at the Register Bytecode (RBC) level alongside the
  classfile Loader/Verifier/Quickener path. Design work starts as an RFC
  to the RBC-consuming teams (interpreter, passes) per
  `docs/teams/messaging.md` — never as a silent contract change.
- A binding and type-checking stage, planned as future work. It must
  respect Rule 80: static types are not runtime proof, and the binding
  stage must not become a silent source of unsound assumptions.
- Diagnostics format stability: message wording may drift between versions,
  but the field set and severity model change only with a contract message
  (RFC/ADVISORY) and notice to consumers.
- Grammar coverage expansion toward full parity with the Java SE
  specification's lexical and syntactic grammar (Rule 67).

## Change Control

Changes to this contract go through the message system under team key
`frontend` (`docs/teams/messaging.md`). Cross-team consumers pin tests
against frontend output at their own risk until a contract message lands.
