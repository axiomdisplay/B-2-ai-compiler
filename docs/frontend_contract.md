# B-2 Frontend Interface Contract (v1)

Owner: Frontend Team (`frontend`; see `docs/teams/frontend-team.md`)

```text
Normative reference: docs/laws.md
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

This is the v1 contract for the Java-source frontend: the v0 lexer/parser/
AST/diagnostics stage plus the v0 AST-to-RBC lowering (landed under RFC
`messages/closed/MSG-20260830-003-frontend-ir-RFC.md`). It defines what the
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
- Register Bytecode (`--emit-rbc`, v1): a `rbc::Program` whose class name
  is the internal form ("pkg/Foo"), printed in the deterministic RBC text
  format. The output feeds `b2rbc` (verify) and `b2run` (execute) directly:
  `b2parse --emit-rbc Hello.java > Hello.rbc && b2run Hello.rbc`.
- Diagnostics: severity, source location (file, line, column, range), a
  clear human-readable message, expected versus actual state, and a
  suggested fix or recovery action (Rule 47).
- An exit status for `b2parse`: zero only when no error-severity diagnostic
  was issued; non-zero otherwise (including tool I/O failures).

Public surface: `include/b2/frontend/` (`SourceManager.h`, `Diagnostics.h`,
`Token.h`, `Lexer.h`, `Parser.h`, `AstDumper.h`, `Lower.h`, `ast/Ast.h`).

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

## Non-Guarantees (v1)

- No full semantic resolution, binding, or type-checking. The lowering
  carries exactly enough static typing (the JType overlay) to select opcode
  families, apply JLS numeric promotions and constant narrowing, and refuse
  what v0 cannot compile honestly. Reference-subtype relationships are
  unchecked (no hierarchy knowledge until the loader).
- No semantic validation: reachability, definite assignment, checked
  exceptions, and access rules are not checked. Reading an unassigned local
  yields the prologue default (0 / 0.0 / "" — the Ref default is the
  interned empty string, never null); a missing return synthesizes a
  default return. Both land with the binding stage.
- Syntax coverage targets current Java syntax; conformance gaps are bugs,
  and unsupported constructs are diagnosed as unsupported rather than
  silently mis-parsed.
- Diagnostic message wording is not frozen; it may change between versions.

## Lowering Scope (v1)

Supported: one top-level class per compilation unit; fields (static and
instance, with initializers); methods and constructors; `<clinit>`
synthesis (JLS 12.4.2) and default constructor synthesis (JLS 8.8.9);
instance field initializer prepending (skipped for `this(...)` delegation;
`super()` is a v0 no-op); the full statement set (blocks, locals incl.
`var`, if/while/do-while/basic-for, enhanced-for over arrays, switch in
both colon and rule forms, try/catch/finally, synchronized, throw, return,
labeled break/continue); literals; JLS 5.6 numeric promotions; NaN-correct
float/double comparisons; short-circuit `&&`/`||`; conditional `?:`; casts
(numeric + reference); compound assignment with implicit narrowing; `++`/
`--`; one-dimensional array creation (with initializers); array access;
`instanceof`; `new`; calls (own static/instance, `System.out`/`err`
`println`/`print`, unknown-receiver virtual calls with dynamic resolution).

Refused with Rule 47 diagnostics: lambdas, method references, switch
expressions, patterns (invokedynamic RFC); string concatenation (runtime
builtins RFC); enums, interfaces, records, nested/local/anonymous classes
(class model); try-with-resources; assert; varargs call sites;
multi-dimensional array creation; String/Throwable method calls; boxed
arithmetic (binding stage); cross-class statics, fields, and construction
(loader); abstract/native methods; `super(...)` with arguments.

Lowering invariants (enforced by tests): every emitted method passes
`rbc::verify`; determinism (byte-identical text dumps); Java evaluation
order (receiver, then arguments, left to right; assignment targets
evaluate once); exception delivery via Ref-typed local slots (stable
across protected ranges); safepoint polls on every loop backedge target.

## Future Obligations

- ~~AST-to-RBC lowering~~ — landed (v1; RFC
  `messages/closed/MSG-20260830-003-frontend-ir-RFC.md`). The pipeline law
  the RFC pinned: RBC consumption stays read-only through the frozen
  middle-end contracts; changes to those contracts go through messages.
- String concatenation support requires the runtime builtins RFC
  (StringBuilder-shaped builtins behind the Interpreter team's seam).
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
