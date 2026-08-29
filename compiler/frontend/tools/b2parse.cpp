// b2parse - B-2 frontend driver.
//
//   b2parse [--dump-tokens] [--dump-ast] FILE.java...
//
// Exit status: 0 = parsed cleanly, 1 = diagnostics emitted, 2 = I/O error.

#include <algorithm>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "b2/frontend/AstDumper.h"
#include "b2/frontend/Diagnostics.h"
#include "b2/frontend/Lexer.h"
#include "b2/frontend/Parser.h"
#include "b2/frontend/SourceManager.h"
#include "b2/frontend/Token.h"

namespace {

int runOne(const std::string& path, bool dumpTokens, bool dumpAst) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::println(stderr, "b2parse: cannot open '{}'", path);
    return 2;
  }
  std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  b2::frontend::SourceManager sm;
  sm.load(path, std::move(src));
  b2::frontend::DiagnosticEngine diags(sm);

  auto tokens = b2::frontend::Lexer(sm, diags).lex();

  if (dumpTokens) {
    for (const auto& t : tokens) {
      const auto lc = sm.lineCol(t.offset);
      std::print("{}:{}:{} {:<18} off={} len={}", path, lc.line, lc.column,
                  b2::frontend::toString(t.kind), t.offset, t.length);
      if (t.kind == b2::frontend::Tok::Identifier) {
        std::print("  text='{}'", t.text);
      } else if (t.kind == b2::frontend::Tok::StringLiteral) {
        std::print("  value='{}'{}", t.text, t.isTextBlock ? " (text block)" : "");
      } else if (t.kind == b2::frontend::Tok::CharacterLiteral) {
        std::print("  value=U+{:04X}", static_cast<unsigned>(t.intValue));
      } else if (t.kind == b2::frontend::Tok::IntegerLiteral ||
                 t.kind == b2::frontend::Tok::LongLiteral) {
        std::print("  value={}", t.intValue);
      } else if (t.kind == b2::frontend::Tok::FloatLiteral ||
                 t.kind == b2::frontend::Tok::DoubleLiteral) {
        std::print("  value={}", t.floatValue);
      }
      std::println("");
    }
  }

  b2::frontend::Parser parser(std::move(tokens), sm, diags);
  auto unit = parser.parseCompilationUnit();

  if (dumpAst && unit) {
    b2::frontend::AstDumper dumper(sm);
    std::print("{}", dumper.dump(*unit));
  }

  const std::string all = diags.formatAll();
  if (!all.empty()) std::print(stderr, "{}", all);
  std::println(stderr, "{}: {} error(s), {} diagnostic(s) total", path, diags.errorCount(),
               diags.diagnostics().size());
  return diags.hasErrors() ? 1 : 0;
}

void usage() {
  std::println("usage: b2parse [--dump-tokens] [--dump-ast] FILE.java...");
}

}  // namespace

int main(int argc, char** argv) {
  bool dumpTokens = false;
  bool dumpAst = false;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--dump-tokens") {
      dumpTokens = true;
    } else if (a == "--dump-ast") {
      dumpAst = true;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      files.emplace_back(a);
    }
  }
  if (files.empty()) {
    usage();
    return 64;
  }
  int worst = 0;
  for (const auto& f : files) {
    worst = std::max(worst, runOne(f, dumpTokens, dumpAst));
  }
  return worst;
}
