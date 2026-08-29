// B-2 Frontend unit tests - lexer (JLS chapter 3).
//
// Each test drives the real SourceManager -> DiagnosticEngine -> Lexer stack
// over a small snippet and asserts on the produced token stream: kind
// sequences, decoded literal payloads (values, not just spellings), raw-range
// fidelity across the JLS 3.3 unicode-escape translation, and diagnostics for
// malformed input. Expected values (literal values, decoded escapes, text
// block contents) were cross-checked against the JDK 21 reference compiler.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "TestHarness.h"

#include "b2/frontend/Diagnostics.h"
#include "b2/frontend/Lexer.h"
#include "b2/frontend/SourceManager.h"
#include "b2/frontend/Token.h"

namespace {

using b2::frontend::DiagnosticEngine;
using b2::frontend::Lexer;
using b2::frontend::SourceManager;
using b2::frontend::Tok;
using b2::frontend::Token;

// Lexes one source string in place. The DiagnosticEngine references the
// SourceManager, so the session must not be moved after run().
struct LexSession {
  SourceManager sm;
  std::optional<DiagnosticEngine> diags;
  std::vector<Token> toks;

  void run(const std::string& fileName, const std::string& source) {
    sm.load(fileName, source);
    diags.emplace(sm);
    toks = Lexer(sm, *diags).lex();
  }

  [[nodiscard]] bool hasErrors() const { return diags && diags->hasErrors(); }
  [[nodiscard]] std::uint32_t errorCount() const { return diags ? diags->errorCount() : 0; }
  [[nodiscard]] std::string diagnosticsText() const {
    return diags ? diags->formatAll() : std::string();
  }
};

// Kind-sequence comparison; the stream always ends with EndOfFile.
bool kindsAre(const std::vector<Token>& toks, const std::vector<Tok>& want) {
  if (toks.size() != want.size()) return false;
  for (std::size_t i = 0; i < want.size(); ++i) {
    if (toks[i].kind != want[i]) return false;
  }
  return true;
}

std::string describeKinds(const std::vector<Token>& toks) {
  std::string out;
  for (const Token& t : toks) {
    if (!out.empty()) out += ' ';
    out += b2::frontend::toString(t.kind);
  }
  return out;
}

// Renders a decoded literal value with escapes visible (for failure text).
std::string describeText(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\\') {
      out += "\\\\";
    } else {
      out += c;
    }
  }
  return out;
}

// The literal tokens in order (skips keywords, identifiers, punctuation).
std::vector<const Token*> literalsOf(const std::vector<Token>& toks) {
  std::vector<const Token*> out;
  for (const Token& t : toks) {
    switch (t.kind) {
      case Tok::IntegerLiteral:
      case Tok::LongLiteral:
      case Tok::FloatLiteral:
      case Tok::DoubleLiteral:
      case Tok::CharacterLiteral:
      case Tok::StringLiteral:
        out.push_back(&t);
        break;
      default:
        break;
    }
  }
  return out;
}

// First token whose text matches exactly (nullptr when absent).
const Token* findToken(const std::vector<Token>& toks, const std::string& text) {
  for (const Token& t : toks) {
    if (t.text == text) return &t;
  }
  return nullptr;
}

std::size_t countKind(const std::vector<Token>& toks, Tok kind) {
  std::size_t n = 0;
  for (const Token& t : toks) {
    if (t.kind == kind) ++n;
  }
  return n;
}

// ------------------------------------------------------------- token stream -

B2_TEST(lexer_token_kinds_for_small_class) {
  LexSession s;
  s.run("Small.java", "class A { int x = 1; }\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  const std::vector<Tok> want = {Tok::ClassKeyword,  Tok::Identifier, Tok::LeftBrace,
                                 Tok::IntKeyword,    Tok::Identifier, Tok::Equal,
                                 Tok::IntegerLiteral, Tok::Semicolon, Tok::RightBrace,
                                 Tok::EndOfFile};
  CHECK_MSG(kindsAre(s.toks, want), "unexpected token stream: " + describeKinds(s.toks));
  CHECK_MSG(s.toks.size() == 10, "10 tokens expected (9 + EOF)");
  CHECK_MSG(s.toks[1].text == "A", "class name should be the identifier A");
  CHECK_MSG(s.toks[4].text == "x", "field name should be the identifier x");
  CHECK_MSG(s.toks[6].intValue == 1u, "the literal 1 should decode to 1");
  CHECK_MSG(s.toks.back().kind == Tok::EndOfFile, "stream must end with EndOfFile");
  CHECK_MSG(s.toks.back().length == 0u, "EndOfFile has zero length");
}

// ---------------------------------------------------------------- integers -

B2_TEST(lexer_integer_literal_forms_and_values) {
  LexSession s;
  s.run("Ints.java",
        "class Ints {\n"
        "    int dec = 42;\n"
        "    int hex = 0xFF;\n"
        "    int bin = 0b1010;\n"
        "    int oct = 0755;\n"
        "    int und = 1_000_000;\n"
        "    long big = 9999999999L;\n"
        "    long hexUnd = 0xCAFE_F00DL;\n"
        "    int binUnd = 0b1010_1010;\n"
        "    long maxLong = 0x7FFF_FFFF_FFFF_FFFFL;\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  const std::vector<const Token*> lits = literalsOf(s.toks);
  CHECK_MSG(lits.size() == 9, "expected 9 integer literals");
  if (lits.size() != 9) return;

  CHECK_MSG(lits[0]->kind == Tok::IntegerLiteral, "42 should be an IntegerLiteral");
  CHECK_MSG(lits[0]->intValue == 42u, "42 should decode to 42");
  CHECK_MSG(lits[0]->text == "42", "spelling should be preserved");

  CHECK_MSG(lits[1]->kind == Tok::IntegerLiteral, "0xFF should be an IntegerLiteral");
  CHECK_MSG(lits[1]->intValue == 255u, "0xFF should decode to 255");
  CHECK_MSG(lits[1]->text == "0xFF", "spelling should be preserved");

  CHECK_MSG(lits[2]->kind == Tok::IntegerLiteral, "0b1010 should be an IntegerLiteral");
  CHECK_MSG(lits[2]->intValue == 10u, "0b1010 should decode to 10");

  CHECK_MSG(lits[3]->kind == Tok::IntegerLiteral, "0755 should be an IntegerLiteral");
  CHECK_MSG(lits[3]->intValue == 493u, "0755 (octal) should decode to 493");

  CHECK_MSG(lits[4]->kind == Tok::IntegerLiteral, "1_000_000 should be an IntegerLiteral");
  CHECK_MSG(lits[4]->intValue == 1000000u, "1_000_000 should decode to 1000000");
  CHECK_MSG(lits[4]->text == "1_000_000", "spelling should keep the underscores");

  CHECK_MSG(lits[5]->kind == Tok::LongLiteral, "9999999999L should be a LongLiteral");
  CHECK_MSG(lits[5]->intValue == 9999999999ULL, "9999999999L should decode to 9999999999");
  CHECK_MSG(lits[5]->text == "9999999999L", "spelling should keep the L suffix");

  CHECK_MSG(lits[6]->kind == Tok::LongLiteral, "0xCAFE_F00DL should be a LongLiteral");
  CHECK_MSG(lits[6]->intValue == 3405705229ULL, "0xCAFE_F00D should decode to 3405705229");

  CHECK_MSG(lits[7]->kind == Tok::IntegerLiteral, "0b1010_1010 should be an IntegerLiteral");
  CHECK_MSG(lits[7]->intValue == 170u, "0b1010_1010 should decode to 170");

  CHECK_MSG(lits[8]->kind == Tok::LongLiteral,
            "0x7FFF_FFFF_FFFF_FFFFL should be a LongLiteral");
  CHECK_MSG(lits[8]->intValue == 9223372036854775807ULL,
            "0x7FFF_FFFF_FFFF_FFFFL should decode to Long.MAX_VALUE");
}

// --------------------------------------------------------------- floatings -

B2_TEST(lexer_floating_literal_forms_and_values) {
  LexSession s;
  s.run("Floats.java",
        "class Floats {\n"
        "    double d1 = 1.5;\n"
        "    double d2 = 1.;\n"
        "    double d3 = 1e10;\n"
        "    double d4 = 1.5E-3;\n"
        "    float f1 = 1e-3f;\n"
        "    double d5 = 2.5d;\n"
        "    double d6 = 0x1.8p3;\n"
        "    float f2 = 0x1.8p3f;\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  const std::vector<const Token*> lits = literalsOf(s.toks);
  CHECK_MSG(lits.size() == 8, "expected 8 floating-point literals");
  if (lits.size() != 8) return;

  CHECK_MSG(lits[0]->kind == Tok::DoubleLiteral, "1.5 should be a DoubleLiteral");
  CHECK_MSG(lits[0]->floatValue == 1.5, "1.5 should decode to 1.5");

  CHECK_MSG(lits[1]->kind == Tok::DoubleLiteral, "1. should be a DoubleLiteral");
  CHECK_MSG(lits[1]->floatValue == 1.0, "1. should decode to 1.0");

  CHECK_MSG(lits[2]->kind == Tok::DoubleLiteral, "1e10 should be a DoubleLiteral");
  CHECK_MSG(lits[2]->floatValue == 1e10, "1e10 should decode to 1e10");

  CHECK_MSG(lits[3]->kind == Tok::DoubleLiteral, "1.5E-3 should be a DoubleLiteral");
  CHECK_MSG(lits[3]->floatValue == 0.0015, "1.5E-3 should decode to 0.0015");

  CHECK_MSG(lits[4]->kind == Tok::FloatLiteral, "1e-3f should be a FloatLiteral");
  CHECK_MSG(lits[4]->floatValue == 0.001, "1e-3f should decode to 0.001");

  CHECK_MSG(lits[5]->kind == Tok::DoubleLiteral, "2.5d should be a DoubleLiteral");
  CHECK_MSG(lits[5]->floatValue == 2.5, "2.5d should decode to 2.5");

  CHECK_MSG(lits[6]->kind == Tok::DoubleLiteral, "0x1.8p3 should be a DoubleLiteral");
  CHECK_MSG(lits[6]->floatValue == 12.0, "0x1.8p3 (1.5 * 2^3) should decode to 12.0");

  CHECK_MSG(lits[7]->kind == Tok::FloatLiteral, "0x1.8p3f should be a FloatLiteral");
  CHECK_MSG(lits[7]->floatValue == 12.0, "0x1.8p3f (1.5 * 2^3) should decode to 12.0");
}

// ------------------------------------------------------------- characters --

B2_TEST(lexer_character_literal_values) {
  LexSession s;
  s.run("Chars.java",
        "class Chars {\n"
        "    char a = 'a';\n"
        "    char b = '\\n';\n"
        "    char c = '\\\\';\n"
        "    char d = '\\47';\n"
        "    char e = '\\u0041';\n"  // raw bytes: backslash u 0 0 4 1
        "    char f = '\\s';\n"
        "    char g = '\\101';\n"
        "    char h = '\\377';\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  const std::vector<const Token*> lits = literalsOf(s.toks);
  CHECK_MSG(lits.size() == 8, "expected 8 character literals");
  if (lits.size() != 8) return;

  CHECK_MSG(lits[0]->kind == Tok::CharacterLiteral, "'a' should be a CharacterLiteral");
  CHECK_MSG(lits[0]->intValue == 97u, "'a' should decode to 97");
  CHECK_MSG(lits[0]->text == "a", "'a' decoded text should be a");

  CHECK_MSG(lits[1]->intValue == 10u, "'\\n' should decode to 10");
  CHECK_MSG(lits[2]->intValue == 92u, "'\\\\' should decode to 92");

  CHECK_MSG(lits[3]->intValue == 39u, "'\\47' (octal) should decode to 39");

  // The JLS 3.3 translation runs before tokenizing: '\u0041' is the letter A.
  CHECK_MSG(lits[4]->intValue == 65u, "'\\u0041' should decode to 65 via translation");
  CHECK_MSG(lits[4]->text == "A", "'\\u0041' decoded text should be A");

  CHECK_MSG(lits[5]->intValue == 32u, "'\\s' should decode to 32");
  CHECK_MSG(lits[6]->intValue == 65u, "'\\101' (octal) should decode to 65");
  CHECK_MSG(lits[7]->intValue == 255u, "'\\377' (octal) should decode to 255");
}

// ---------------------------------------------------------------- strings --

B2_TEST(lexer_string_escapes_decoded) {
  LexSession s;
  s.run("Strs.java",
        "class Strs {\n"
        "    String a = \"a\\tb\\n\\\"q\\\"\\\\\";\n"
        "    String b = \"\\101\\u0042\";\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  const std::vector<const Token*> lits = literalsOf(s.toks);
  CHECK_MSG(lits.size() == 2, "expected 2 string literals");
  if (lits.size() != 2) return;

  // a TAB b NL " q " backslash
  CHECK_MSG(lits[0]->kind == Tok::StringLiteral, "expected a StringLiteral");
  CHECK_MSG(lits[0]->text == "a\tb\n\"q\"\\", "escape decoding mismatch");
  CHECK_MSG(!lits[0]->isTextBlock, "plain string is not a text block");

  // '\101' is an octal escape (A); \u0042 is translated before lexing (B).
  CHECK_MSG(lits[1]->text == "AB", "octal escape + unicode escape should decode to AB");
}

// The even-backslash rule of JLS 3.3: in "\\u0041" the escaped backslash
// shields the escape, so the literal value keeps all six characters; in
// "\u0041" the escape fires and the value is a single A.
B2_TEST(lexer_unicode_escape_backslash_parity) {
  LexSession s;
  s.run("Parity.java",
        "class Parity {\n"
        "    String s = \"\\\\u0041\";\n"  // Java source: "\\u0041"
        "    String t = \"\\u0041\";\n"   // Java source: "\u0041"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  const std::vector<const Token*> lits = literalsOf(s.toks);
  CHECK_MSG(lits.size() == 2, "expected 2 string literals");
  if (lits.size() != 2) return;

  CHECK_MSG(lits[0]->text == "\\u0041",
            "shielded escape should decode to backslash + u0041 (6 chars)");
  CHECK_MSG(lits[0]->text.size() == 6, "shielded escape value has 6 characters");
  CHECK_MSG(lits[1]->text == "A", "unshielded \\u0041 should decode to A");
}

// A \u000A is a real line terminator (JLS 3.3): it terminates a line comment.
B2_TEST(lexer_unicode_escape_ends_line_comment) {
  LexSession s;
  s.run("Comment.java", "class C { // hi \\u000A int x = 1; }\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  const std::vector<Tok> want = {Tok::ClassKeyword, Tok::Identifier, Tok::LeftBrace,
                                 Tok::IntKeyword,    Tok::Identifier, Tok::Equal,
                                 Tok::IntegerLiteral, Tok::Semicolon, Tok::RightBrace,
                                 Tok::EndOfFile};
  CHECK_MSG(kindsAre(s.toks, want),
            "\\u000A must end the comment and 'int x = 1;' must lex; got: " +
                describeKinds(s.toks));
}

// ------------------------------------------------------------ text blocks --

B2_TEST(lexer_text_block_incidental_indentation) {
  LexSession s;
  s.run("TB.java", R"java(class TB {
    String a = """
        alpha
        beta
        gamma
        """;
    String b = """
        a

        """;
}
)java");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  const std::vector<const Token*> lits = literalsOf(s.toks);
  CHECK_MSG(lits.size() == 2, "expected 2 text blocks");
  if (lits.size() != 2) return;

  CHECK_MSG(lits[0]->kind == Tok::StringLiteral, "text blocks are StringLiterals");
  CHECK_MSG(lits[0]->isTextBlock, "isTextBlock must be set");
  CHECK_MSG(lits[0]->text == "alpha\nbeta\ngamma\n",
            "8/8/8 indent + closing at 8: no leading spaces, lines joined with "
            "\\n, final newline before the closing delimiter kept; got: " +
                describeText(lits[0]->text));

  CHECK_MSG(lits[1]->isTextBlock, "isTextBlock must be set");
  CHECK_MSG(lits[1]->text == "a\n\n",
            "a blank content line before the closing delimiter yields a trailing "
            "empty line; got: " + describeText(lits[1]->text));
}

B2_TEST(lexer_text_block_continuation_and_escaped_space) {
  LexSession s;
  s.run("TB2.java", R"java(class TB2 {
    String c = """
        one two \
        three
        """;
    String d = """
        \sindented
        """;
}
)java");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  const std::vector<const Token*> lits = literalsOf(s.toks);
  CHECK_MSG(lits.size() == 2, "expected 2 text blocks");
  if (lits.size() != 2) return;

  // A trailing backslash joins the lines without a newline.
  CHECK_MSG(lits[0]->text == "one two three\n",
            "line continuation should drop the backslash-newline; got: " +
                describeText(lits[0]->text));

  // An escaped \s is processed after indentation stripping, so the space it
  // produces survives at the start of the line.
  CHECK_MSG(lits[1]->text == " indented\n",
            "\\s at line start must not be stripped as indentation; got: " +
                describeText(lits[1]->text));
}

// -------------------------------------------------------------- operators --

B2_TEST(lexer_operator_longest_match) {
  LexSession s;
  s.run("Ops.java",
        "class Ops {\n"
        "    void f(int... xs) {}\n"
        "    void m(int a, int b, int x) {\n"
        "        int u = a >>> b;\n"
        "        a >>= b;\n"
        "        int w = x < -1;\n"
        "        Runnable r = () -> {};\n"
        "        Runnable q = O::m;\n"
        "        boolean z = a > b ? true : false;\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  CHECK_MSG(countKind(s.toks, Tok::GreaterGreaterGreater) == 1, "a >>> b is >>>");
  CHECK_MSG(countKind(s.toks, Tok::GreaterGreaterEqual) == 1, "a >>= b is >>=");
  CHECK_MSG(countKind(s.toks, Tok::GreaterGreaterGreaterEqual) == 0, "no >>>= here");
  CHECK_MSG(countKind(s.toks, Tok::Ellipsis) == 1, "int... is an Ellipsis");
  CHECK_MSG(countKind(s.toks, Tok::Arrow) == 1, "() -> is an Arrow");
  CHECK_MSG(countKind(s.toks, Tok::ColonColon) == 1, "O::m is a ColonColon");
  CHECK_MSG(countKind(s.toks, Tok::Question) == 1, "ternary uses Question");

  // x < -1 lexes as Less Minus, never as a glued operator.
  CHECK_MSG(countKind(s.toks, Tok::Less) == 1, "exactly one '<'");
  bool lessFollowedByMinus = false;
  for (std::size_t i = 0; i + 1 < s.toks.size(); ++i) {
    if (s.toks[i].kind == Tok::Less && s.toks[i + 1].kind == Tok::Minus) {
      lessFollowedByMinus = true;
    }
  }
  CHECK_MSG(lessFollowedByMinus, "'x < -1' must lex as '<' followed by '-'");
  CHECK_MSG(countKind(s.toks, Tok::Error) == 0, "no error tokens expected");
}

// ------------------------------------------------- words, keywords, names --

B2_TEST(lexer_contextual_keywords_and_underscore) {
  LexSession s;
  s.run("Words.java", "class Words { record sealed var yield permits instanceof _ _x }");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());

  const Token* rec = findToken(s.toks, "record");
  CHECK_MSG(rec != nullptr, "'record' should lex as one token");
  if (rec) CHECK_MSG(rec->kind == Tok::Identifier, "'record' is a contextual keyword");
  const Token* sealed = findToken(s.toks, "sealed");
  CHECK_MSG(sealed != nullptr, "'sealed' should lex as one token");
  if (sealed) CHECK_MSG(sealed->kind == Tok::Identifier, "'sealed' is contextual");
  const Token* var = findToken(s.toks, "var");
  CHECK_MSG(var != nullptr, "'var' should lex as one token");
  if (var) CHECK_MSG(var->kind == Tok::Identifier, "'var' is contextual");
  const Token* yld = findToken(s.toks, "yield");
  CHECK_MSG(yld != nullptr, "'yield' should lex as one token");
  if (yld) CHECK_MSG(yld->kind == Tok::Identifier, "'yield' is contextual");
  const Token* permits = findToken(s.toks, "permits");
  CHECK_MSG(permits != nullptr, "'permits' should lex as one token");
  if (permits) CHECK_MSG(permits->kind == Tok::Identifier, "'permits' is contextual");

  const Token* cls = findToken(s.toks, "class");
  CHECK_MSG(cls != nullptr, "'class' should lex as one token");
  if (cls) CHECK_MSG(cls->kind == Tok::ClassKeyword, "'class' is reserved");
  const Token* io = findToken(s.toks, "instanceof");
  CHECK_MSG(io != nullptr, "'instanceof' should lex as one token");
  if (io) CHECK_MSG(io->kind == Tok::InstanceOfKeyword, "'instanceof' is reserved");

  const Token* us = findToken(s.toks, "_");
  CHECK_MSG(us != nullptr, "a lone '_' should lex as one token");
  if (us) CHECK_MSG(us->kind == Tok::UnderscoreKeyword, "a lone '_' is UnderscoreKeyword");
  const Token* usx = findToken(s.toks, "_x");
  CHECK_MSG(usx != nullptr, "'_x' should lex as one token");
  if (usx) CHECK_MSG(usx->kind == Tok::Identifier, "'_x' is an identifier");
}

B2_TEST(lexer_non_ascii_identifiers) {
  LexSession s;
  s.run("UnicodeIds.java", "class U {\n    int 名前 = 1;\n    String über$ = null;\n}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  CHECK_MSG(s.toks.size() == 15,
            "15 tokens expected: both non-ASCII names must be single tokens; got " +
                std::to_string(s.toks.size()));
  const Token* jp = findToken(s.toks, "名前");
  CHECK_MSG(jp != nullptr, "'名前' should lex as one token");
  if (jp) CHECK_MSG(jp->kind == Tok::Identifier, "'名前' is an Identifier");
  const Token* de = findToken(s.toks, "über$");
  CHECK_MSG(de != nullptr, "'über$' should lex as one token");
  if (de) CHECK_MSG(de->kind == Tok::Identifier, "'über$' is an Identifier");
  CHECK_MSG(countKind(s.toks, Tok::Error) == 0, "no error tokens expected");
}

// ------------------------------------------------------- lossless ranges ---

B2_TEST(lexer_raw_ranges_survive_unicode_translation) {
  LexSession s;
  // Java source: int \u0078 = 1;   (\u0078 is the letter x)
  s.run("Offsets.java", "int \\u0078 = 1;\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  CHECK_MSG(s.toks.size() == 6, "int, x, =, 1, ;, EOF");
  CHECK_MSG(s.toks[1].kind == Tok::Identifier, "translated identifier expected");
  CHECK_MSG(s.toks[1].text == "x", "translated text is x");
  CHECK_MSG(s.toks[1].offset == 4u, "raw offset points at the backslash");
  CHECK_MSG(s.toks[1].length == 6u, "raw length spans \\u0078");
  CHECK_MSG(s.toks[2].kind == Tok::Equal, "'=' follows the escape");
  CHECK_MSG(s.toks[2].offset == 11u, "'=' sits after the six-byte escape");
  CHECK_MSG(s.toks.back().kind == Tok::EndOfFile, "stream ends with EndOfFile");
  CHECK_MSG(s.toks.back().offset == 16u, "EOF offset equals the raw buffer size");
}

// ---------------------------------------------------------------- comments -

B2_TEST(lexer_skips_line_block_javadoc_comments) {
  LexSession s;
  s.run("Comments.java",
        "// line comment\n"
        "/* block\n"
        "   continues */\n"
        "/** javadoc */\n"
        "class C { int x = 1; }\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean lex, got:\n" + s.diagnosticsText());
  const std::vector<Tok> want = {Tok::ClassKeyword,  Tok::Identifier, Tok::LeftBrace,
                                 Tok::IntKeyword,    Tok::Identifier, Tok::Equal,
                                 Tok::IntegerLiteral, Tok::Semicolon, Tok::RightBrace,
                                 Tok::EndOfFile};
  CHECK_MSG(kindsAre(s.toks, want), "comments must not produce tokens; got: " +
                                        describeKinds(s.toks));
}

// ------------------------------------------------------------ diagnostics --

B2_TEST(lexer_unterminated_string_diagnostic) {
  LexSession s;
  s.run("BadString.java", "class B { String s = \"oops;\n}\n");
  CHECK_MSG(s.hasErrors(), "an unterminated string must be an error");
  CHECK_MSG(s.diagnosticsText().find("unterminated string literal") != std::string::npos,
            "diagnostic should mention 'unterminated string literal', got:\n" +
                s.diagnosticsText());
  CHECK_MSG(s.toks.back().kind == Tok::EndOfFile, "lexing still reaches EndOfFile");
}

B2_TEST(lexer_invalid_escape_diagnostic) {
  LexSession s;
  s.run("BadEscapeLit.java", "class B { char c = '\\q'; }\n");
  CHECK_MSG(s.hasErrors(), "an invalid escape must be an error");
  CHECK_MSG(s.diagnosticsText().find("invalid escape sequence") != std::string::npos,
            "diagnostic should mention 'invalid escape sequence', got:\n" +
                s.diagnosticsText());
  CHECK_MSG(s.toks.back().kind == Tok::EndOfFile, "lexing still reaches EndOfFile");
}

B2_TEST(lexer_stray_character_diagnostic) {
  LexSession s;
  s.run("BadCharLit.java", "class B { int x = 5 # 3; }\n");
  CHECK_MSG(s.hasErrors(), "a stray '#' must be an error");
  CHECK_MSG(s.diagnosticsText().find("invalid character") != std::string::npos,
            "diagnostic should mention 'invalid character', got:\n" +
                s.diagnosticsText());
  CHECK_MSG(countKind(s.toks, Tok::Error) == 1, "the '#' becomes one Error token");
  CHECK_MSG(s.toks.back().kind == Tok::EndOfFile, "lexing still reaches EndOfFile");
}

B2_TEST(lexer_malformed_numeric_literal_diagnostic) {
  LexSession s;
  s.run("BadNum.java", "class B { int x = 12abc; }\n");
  CHECK_MSG(s.hasErrors(), "12abc must be an error");
  CHECK_MSG(s.diagnosticsText().find("malformed numeric literal") != std::string::npos,
            "diagnostic should mention 'malformed numeric literal', got:\n" +
                s.diagnosticsText());
  CHECK_MSG(countKind(s.toks, Tok::Error) == 1, "12abc becomes one Error token");
  CHECK_MSG(s.toks.back().kind == Tok::EndOfFile, "lexing still reaches EndOfFile");
}

B2_TEST(lexer_empty_character_literal_diagnostic) {
  LexSession s;
  s.run("EmptyChar.java", "class B { char c = ''; }\n");
  CHECK_MSG(s.hasErrors(), "'' must be an error");
  CHECK_MSG(s.diagnosticsText().find("empty character literal") != std::string::npos,
            "diagnostic should mention 'empty character literal', got:\n" +
                s.diagnosticsText());
  CHECK_MSG(s.toks.back().kind == Tok::EndOfFile, "lexing still reaches EndOfFile");
}

B2_TEST(lexer_unterminated_comment_diagnostic) {
  LexSession s;
  s.run("BadComment.java", "class B { /* never closes\n}\n");
  CHECK_MSG(s.hasErrors(), "an unterminated comment must be an error");
  CHECK_MSG(s.diagnosticsText().find("unterminated comment") != std::string::npos,
            "diagnostic should mention 'unterminated comment', got:\n" +
                s.diagnosticsText());
  const std::vector<Tok> want = {Tok::ClassKeyword, Tok::Identifier, Tok::LeftBrace,
                                 Tok::EndOfFile};
  CHECK_MSG(kindsAre(s.toks, want), "the comment swallows the rest; got: " +
                                        describeKinds(s.toks));
}

// Error limit / progress: arbitrary garbage must not wedge the lexer. The
// engine caps errors (default limit 100) while the lexer keeps producing
// tokens and terminates with EndOfFile.
B2_TEST(lexer_error_limit_and_progress) {
  std::string src;
  for (int i = 0; i < 300; ++i) {
    if (i > 0) src += ' ';
    src += '#';
  }
  LexSession s;
  s.run("Garbage.java", src);
  CHECK_MSG(s.toks.back().kind == Tok::EndOfFile, "lexing must complete with EndOfFile");
  CHECK_MSG(countKind(s.toks, Tok::Error) == 300,
            "every '#' becomes an Error token (tokens are not capped)");
  CHECK_MSG(s.errorCount() <= 100u, "error count must not exceed the engine limit");
  CHECK_MSG(s.errorCount() == 100u, "the default error limit is 100");
  CHECK_MSG(s.diagnosticsText().find("too many errors") != std::string::npos,
            "the engine should announce the suppressed remainder");
}

}  // namespace
