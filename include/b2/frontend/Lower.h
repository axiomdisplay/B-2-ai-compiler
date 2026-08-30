#pragma once
// B-2 Frontend - AST -> RBC lowering: the source path's back half.
//
// WHY THIS FILE EXISTS:
// The frontend contract (docs/frontend_contract.md, Future Obligations) pins
// AST-to-RBC lowering as the stage that closes source -> execution: Java
// source enters via the lexer/parser, and this lowering emits the same
// Register Bytecode the classfile Loader/Verifier/Quickener path delivers,
// so all four tiers (T0 interpreter first) execute source-compiled programs
// unchanged. The lowering is the frontend team's stage and speaks ONLY to
// the frozen middle-end contracts: rbc::RbcBuilder (emission),
// rbc::Method/Program (shape), and the RBC verifier (gate).
//
// SCOPE (v1 of the lowering, "v0 lowering" in the contract doc):
//   Supported: one top-level class per compilation unit; fields (static and
//   instance, with initializers); methods and constructors (static and
//   instance); <clinit> synthesis from static initializers/blocks; default
//   constructor synthesis; instance field initializer prepending; the full
//   statement set (blocks, locals incl. `var`, if/while/do-while/basic-for,
//   enhanced-for over arrays, switch (colon form), try/catch/finally,
//   synchronized blocks and methods, throw, return, labeled break/continue);
//   the expression set (literals, names, arithmetic with JLS numeric
//   promotions, comparisons with NaN-correct float/double forms, short-
//   circuit && / ||, conditional ?:, casts (numeric + reference), compound
//   assignment with implicit narrowing, ++/--, array creation (1-D +
//   initializers), array access, instanceof, string literals, new,
//   calls: own static/instance methods, System.out/err println/print,
//   unknown-receiver virtual calls (dynamic resolution, JVM contract)).
//
//   Refused with diagnostics (honest refusals, never silent miscompiles):
//   lambdas / method references / switch expressions / patterns (need
//   invokedynamic + binding), string concatenation (needs runtime builtins),
//   enums / interfaces / records / nested / local / anonymous classes,
//   try-with-resources, assert, varargs call sites, multi-dimensional array
//   creation, String/Throwable method calls, boxed-type arithmetic, static
//   calls into classes other than the program class, `super(...)` with
//   arguments, abstract/native methods.
//
// KEY INVARIANTS:
// - Verified output is non-negotiable: every method the lowering emits must
//   pass rbc::verify (the tests enforce it; the pipeline runs it before any
//   tier). Refuse-and-diagnose beats emit-and-hope.
// - Local-slot type stability (rbc_spec.md SS5.3): every local slot is
//   default-initialized in the method prologue to its fixed family, so no
//   store ever changes a slot's verification type inside a protected range.
// - Exception delivery: catch variables live in REGISTERS (r0 -> amove),
//   never locals, so protected-range stability cannot be broken by handler
//   stores (a documented v0 rule; crossing a nested protected region is
//   refused).
// - Determinism (Rule 124): same AST -> byte-identical RBC text; methods in
//   declaration order, <clinit> first; constants interned first-use order.
// - Evaluation order is Java's: receiver, then arguments, left to right;
//   assignment targets are evaluated once (compound assignment included).
// - Diagnostics follow Rule 47: location, message, expected vs actual, and
//   a suggested fix; the first error stops the enclosing method's emission,
//   other methods still lower so one run surfaces many problems.

#include "b2/frontend/Diagnostics.h"
#include "b2/frontend/SourceManager.h"
#include "b2/frontend/ast/Ast.h"
#include "b2/rbc/Rbc.h"

namespace b2::frontend {

struct LoweredUnit {
  bool ok = false;          // false: at least one Error diagnostic; program
                            // is best-effort and must not be executed.
  rbc::Program program;     // className = internal name ("pkg/Foo").
};

// Lowers one parsed compilation unit. `unit` should come from an error-free
// parse (lowering a unit with parse errors is refused with one diagnostic).
// All lowering problems are reported through `diags`; the function never
// throws and never emits a method that would fail structural verification
// by construction (operand layout always via RbcBuilder factories).
[[nodiscard]] LoweredUnit lowerUnit(const ast::CompilationUnit& unit,
                                    const SourceManager& sm,
                                    DiagnosticEngine& diags);

}  // namespace b2::frontend
