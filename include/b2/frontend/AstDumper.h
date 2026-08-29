#pragma once
// B-2 Frontend - AST pretty-printer for debugging, tests, and goldens.

#include <string>

#include "b2/frontend/ast/Ast.h"

namespace b2::frontend {

class SourceManager;

// Renders a CompilationUnit as a deterministic, indented tree - one node per
// line, two spaces per depth level, every node kind covered. Output is
// stable across runs and platforms (goldens depend on it).
class AstDumper {
 public:
  explicit AstDumper(const SourceManager& sm) : sm_(&sm) {}

  [[nodiscard]] std::string dump(const ast::CompilationUnit& unit);
  void dump(const ast::CompilationUnit& unit, std::string& out);

 private:
  const SourceManager* sm_;
};

}  // namespace b2::frontend
