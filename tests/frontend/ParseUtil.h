#pragma once
// B-2 Frontend tests - shared parse helper.

#include <memory>
#include <optional>
#include <string>

#include "b2/frontend/Diagnostics.h"
#include "b2/frontend/Lexer.h"
#include "b2/frontend/Parser.h"
#include "b2/frontend/SourceManager.h"
#include "b2/frontend/ast/Ast.h"

namespace b2::test {

// Runs the full frontend over one source string. Lives in place (the
// DiagnosticEngine references the SourceManager); do not move it around.
struct ParseSession {
  b2::frontend::SourceManager sm;
  std::optional<b2::frontend::DiagnosticEngine> diags;
  std::unique_ptr<b2::frontend::ast::CompilationUnit> unit;

  void run(const std::string& name, const std::string& source) {
    sm.load(name, source);
    diags.emplace(sm);
    auto tokens = b2::frontend::Lexer(sm, *diags).lex();
    b2::frontend::Parser parser(std::move(tokens), sm, *diags);
    unit = parser.parseCompilationUnit();
  }

  [[nodiscard]] bool hasErrors() const { return diags && diags->hasErrors(); }

  // All diagnostics formatted, joined by newlines.
  [[nodiscard]] std::string diagnosticsText() const {
    if (!diags) return {};
    return diags->formatAll();
  }
};

}  // namespace b2::test
