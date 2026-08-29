#pragma once
// B-2 Frontend - lexer (JLS chapter 3).

#include <cstdint>
#include <string>
#include <vector>

#include "b2/frontend/Token.h"

namespace b2::frontend {

class SourceManager;
class DiagnosticEngine;

// Lexes one source file.
//
// The lexer first performs the JLS 3.3 unicode-escape translation (with the
// even-backslash eligibility rule), then tokenizes the translated buffer.
// Token offsets are mapped back to RAW source offsets, so diagnostics point
// at what the user wrote. All the classic JLS consequences fall out of
// translating first: a \u000A in a line comment terminates it, a \u0022 can
// open a string, and so on.
//
// The lexer never throws and never loops forever on malformed input: it
// emits Error tokens and keeps making progress.
class Lexer {
 public:
  Lexer(const SourceManager& sm, DiagnosticEngine& diags) : sm_(sm), diags_(diags) {}

  // Tokenize the whole file. The result is always terminated by an
  // EndOfFile token.
  [[nodiscard]] std::vector<Token> lex();

 private:
  const SourceManager& sm_;
  DiagnosticEngine& diags_;
};

}  // namespace b2::frontend
