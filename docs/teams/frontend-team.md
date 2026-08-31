# Frontend Team Charter

```text
Normative reference: docs/laws.md
(B-2 Java Runtime Compiler Laws & Architecture Specification, as amended by
Amendment A, Amendment B, and the Special Pass Laws in Part XVIII.)
If this document conflicts with `docs/laws.md`, `docs/laws.md` wins.
```

Team size: 2

Team key: `frontend`

Roles:

- `FRONTEND-IMPL` — Implementer. Produces the Java-source frontend: source management, the lexer, the recursive-descent parser, the lossless position-annotated AST, diagnostics with error recovery, the `b2parse` driver, and the frontend test suites inside the team's owned write paths.
- `FRONTEND-REV` — Reviewer. Checks every change for grammar-conformance bugs, law compliance, boundary violations, missing tests, and missing AST node documentation. Does not rewrite code unless explicitly reassigned.

---

## Mission

The Frontend Team owns the source entry path of B-2: it turns arbitrary Java source into a lossless, position-annotated AST with high-quality diagnostics and error recovery. It is the first stage of the macro structure — Frontend (source to AST), then Middle End (RBC, IR, passes, interpreter and baseline tiers), then Backend (regalloc, codegen, AOT). The classfile path keeps its own entry through Loader / Verifier / Quickener; both entry paths converge at Register Bytecode (RBC), which this team will feed through a future AST-to-RBC lowering stage.

In scope (v0):

- UTF-8 source loading, file identity, line/column mapping, and source ranges (`SourceManager`).
- The complete JLS lexical grammar: unicode escape processing (`\uXXXX`) before tokenization, all integer, floating, char, and string literal forms, text blocks, separators, operators, and keyword versus identifier handling for contextual keywords (`var`, `record`, `sealed`, `permits`, `yield`).
- A recursive-descent parser producing a lossless AST with a source range on every node, covering current Java syntax: classes, interfaces, enums, records, sealed types, annotations, generics, lambdas, method references, switch expressions, pattern matching, text blocks, `var`, and the remaining current-language constructs.
- Diagnostics with error recovery: multiple diagnostics per file, resynchronization at statement, declaration, and member boundaries, and a usable partial AST after errors.
- Deterministic textual dumps of tokens and the AST for golden tests (`AstDumper`, `b2parse`).
- The `b2parse` driver tool and unit plus corpus tests.

Explicit non-goals for v0 (future work, documented — not abandonment):

- No semantic resolution, binding, or type-checking: the AST is purely syntactic. A later binding stage is planned; when it lands it must respect Rule 80 — Static Typing Is Not Runtime Proof Unless Certified.
- No RBC emission: AST-to-RBC lowering is future work; it will feed the pipeline at the RBC level alongside the Loader/Verifier/Quickener path, under contract with the RBC-consuming teams.
- No IR: sea-of-nodes IR belongs to the IR Team (`compiler/ir/core/`); the frontend never constructs IR graphs.
- No codegen: machine code emission belongs to the Codegen Team.

The AST must carry, losslessly, everything later tiers need to preserve Java semantics — including try/catch/finally and try-with-resources structure, so downstream tiers can satisfy Rule 74 — Java Exceptions Are Control-Flow Values, Not Native Exceptions. The Frontend Team delivers syntax, not semantics: resolution, IR construction, optimization, and emission are owned by other teams.

---

## Owned Write Paths

```text
compiler/frontend/
include/b2/frontend/
tests/frontend/
docs/frontend_contract.md
```

---

## Forbidden Write Paths

```text
compiler/interp/
compiler/baseline/
compiler/ir/core/
compiler/passes/
compiler/pipeline/
compiler/regalloc/
compiler/codegen/
compiler/aot/
runtime/
```

The team never writes outside its owned paths, even for one-line fixes. Any issue in another team's area — or any change to a cross-team contract this team publishes (AST shape, source-range rules, diagnostics format) — is raised as a message under `messages/` per `docs/teams/messaging.md`; the owning team resolves it.

---

## Key Laws

- Rule 47 — Actionable Compiler Diagnostics: every frontend diagnostic carries an exact source location, a clear human-readable message, expected versus actual state, and a suggested fix or recovery action.
- Rule 48 — `[[nodiscard]]` on All Result Types: lexing and parsing entry points return result types that must be consumed; ignored parse failures do not compile.
- Rule 49 — No `#define` Macros for Logic: token kinds, keyword tables, and grammar decisions use `constexpr` functions and declarative tables, never logic macros.
- Rule 52 — Self-Contained, Reproducible Test Cases: lexer, parser, and corpus tests run identically on any machine; no network, no local paths, no nondeterministic state.
- Rule 57 — No Copy-Paste Code or Structural Duplication: token-kind tables, keyword maps, and repeated grammar productions use generators, `constexpr` helpers, or declarative tables.
- Rule 60 — No Untested or Unverified Code Paths: every error-recovery branch, every malformed-input path, and every lexer corner is exercised by explicit tests.
- Rule 62 — No Deletion-by-Avoidance ("Too Hard" Is Not a Valid Reason): hard JLS corners (unicode escapes before lexing, text-block indentation handling, ambiguous contextual keywords, cast-versus-lambda disambiguation) are implemented, not stubbed.
- Rule 63 — No Fragile Implementations: the frontend is resilient to malformed, truncated, hostile, and binary input — arbitrary source produces diagnostics, never a crash or out-of-bounds read.
- Rule 64 — No Documentation Debt: every AST node kind and every public header in `include/b2/frontend/` documents purpose, invariants, and edge cases at the point of definition.
- Rule 67 — Java SE/JVM Is the Semantic Oracle: acceptance of valid programs and rejection of invalid ones follows the Java SE specification's lexical and syntactic grammar; any divergence is documented, justified, and approved.
- Rule 124 — Compilation Must Be Deterministic and Replayable: the same source, compiler version, and flags produce an identical token stream, AST, and dump output; nondeterminism sources are logged.
- Rule 125 — Passes Must Not Use Hidden Global Mutable State: lexer and parser state is explicit; keyword and token tables are immutable configuration, with no hidden singletons.

---

## Deliverables

1. `SourceManager` — UTF-8 source loading, file identity, line/column mapping, and `SourceRange` construction (`include/b2/frontend/SourceManager.h`).
2. Diagnostics engine — severities, source locations, expected-versus-actual fields, suggested fixes, and a stable rendering (`include/b2/frontend/Diagnostics.h`), per Rule 47.
3. Token model and lexer — the full JLS lexical grammar including unicode escape processing and text blocks (`include/b2/frontend/Token.h`, `include/b2/frontend/Lexer.h`, `compiler/frontend/Lexer.cpp`).
4. Recursive-descent parser — declarations and statements (`compiler/frontend/Parser.cpp`, `compiler/frontend/ParserStmt.cpp`) and expressions with correct precedence and associativity (`compiler/frontend/ParserExpr.cpp`).
5. Lossless, position-annotated AST — every node carries a `SourceRange`; nothing is silently dropped or normalized away (`include/b2/frontend/ast/Ast.h`).
6. Error recovery — resynchronization at statement, declaration, and member boundaries; multiple diagnostics per file; a usable partial AST (`include/b2/frontend/Parser.h`).
7. `AstDumper` — deterministic, diff-stable textual AST form for golden tests and bug reports (`include/b2/frontend/AstDumper.h`, `compiler/frontend/AstDumper.cpp`).
8. `b2parse` driver — parse a `.java` file with `--dump-tokens` and `--dump-ast`, diagnostics to stderr, non-zero exit on error-severity diagnostics (`compiler/frontend/tools/b2parse.cpp`).
9. Test harness and suites — `TestHarness.h`, `TestMain.cpp`, `LexerTests.cpp`, `ParserTests.cpp`, `CorpusTest.cpp` (`tests/frontend/`).
10. Corpus — `tests/frontend/corpus/*.java` files covering current Java syntax plus malformed inputs.
11. Golden baselines — checked-in expected token and AST dumps for corpus files.
12. `docs/frontend_contract.md` — the v0 interface contract: inputs, outputs, guarantees, non-guarantees, future obligations.

---

## Interface Contract

### Consumes

- UTF-8-encoded Java source files (`.java`) — arbitrary and untrusted; content is never assumed well-formed.
- No other B-2 subsystem in v0: the frontend is the first stage of the source path and depends on no other team's output.

### Outputs

- Stable public headers in `include/b2/frontend/`: `SourceManager.h`, `Diagnostics.h`, `Token.h`, `Lexer.h`, `Parser.h`, `AstDumper.h`, `ast/Ast.h`.
- The token stream and the lossless, position-annotated AST, plus diagnostics, via the public API.
- The `b2parse` tool (`--dump-tokens`, `--dump-ast`) with a machine-checkable exit status.
- `docs/frontend_contract.md` — the published contract of this team.

### Promises

- Lossless source ranges: every AST node carries an exact `SourceRange`, and the token stream covers the entire input.
- Never crash on arbitrary input: malformed, truncated, or hostile source produces diagnostics (Rule 63), never a fault, hang, or out-of-bounds access.
- Deterministic output: same source, version, and flags produce byte-identical dumps (Rule 124).
- Error recovery: parsing continues after errors and reports multiple diagnostics per run (Rule 47).
- No hidden global mutable state (Rule 125).
- No semantic claims: the AST records syntax only; no binding or type-checking assertions are made.

---

## Testing Responsibilities

```text
tests/frontend/
```

- Lexer: every token kind and literal form; unicode escapes (including processing before tokenization); text blocks with incidental whitespace and indentation; contextual keywords; unterminated literals and comments; invalid characters and encoding errors.
- Parser: positive and negative tests per production; precedence and associativity; ambiguity cases (cast versus parenthesized expression, lambda versus parenthesized expression, switch expression versus statement, `yield` as identifier, generic-method-call versus comparison).
- Error recovery: multiple diagnostics per file; resynchronization points; partial-AST shape after errors.
- Losslessness: source ranges round-trip — every token is covered, and every node range matches the text it was parsed from.
- Determinism: repeated runs produce byte-identical token and AST dumps.
- Robustness: truncated, malformed, and binary inputs never crash the frontend.
- Corpus: every `tests/frontend/corpus/*.java` file is parsed by `CorpusTest.cpp` and pinned against golden baselines.

Test names must be self-documenting (Rule 41); bug fixes require five regression tests (Rule 34).

---

## When To Send Messages

- A public header in `include/b2/frontend/` is added, renamed, or deprecated — consumers must be advised.
- The diagnostics format or severity model changes — other teams may pin frontend output.
- The AST dump format or golden baselines change — downstream tests may reference them.
- The source-range or losslessness contract changes — this is a cross-team contract.
- AST-to-RBC lowering work is proposed — RFC first: the interpreter owns RBC execution semantics and the passes team parses RBC for graph building; both must review the lowering contract before any emission is designed.
- Binding or type-checking scope is proposed — RFC; it changes what the pipeline may assume about frontend output.
- The frontend finds a bug in another team's area while running the corpus — BUG message with a reproducer; no patching.

Bugs in frontend code are owned and fixed by this team regardless of who reports them; if a fix affects other teams, an ADVISORY accompanies it. The team never writes into another team's paths, regardless of severity.

---

## Reviewer Checklist

- [ ] New syntax support has positive tests, rejection tests with Rule 47-quality diagnostics, and error-recovery tests.
- [ ] Every AST node carries a `SourceRange`; losslessness round-trip tests pass.
- [ ] Arbitrary input never crashes the lexer or parser; malformed, truncated, and hostile cases are covered (Rule 63).
- [ ] Token stream, AST, and dumps are byte-identical across repeated runs (Rule 124).
- [ ] Diagnostics carry location, message, expected versus actual, and a suggested fix (Rule 47).
- [ ] Result-returning APIs are `[[nodiscard]]` (Rule 48); no logic macros (Rule 49).
- [ ] Token, keyword, and grammar tables are declarative, not copy-pasted (Rule 57).
- [ ] Nothing is stubbed as "too hard"; hard JLS corners are implemented or escalated (Rule 62).
- [ ] Every new AST node kind is documented (Rule 64) and covered by the dumper and golden baselines.
- [ ] Tests are self-contained and reproducible (Rule 52); names encode the bug or feature (Rule 41); fixes add five regression tests (Rule 34).
- [ ] No binding, RBC emission, IR construction, or codegen slipped into frontend scope.
- [ ] No file outside the owned write paths is modified; cross-team requests went through `messages/`.
