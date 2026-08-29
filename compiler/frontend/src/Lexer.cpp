// B-2 Frontend - lexer implementation (JLS chapter 3).
//
// Pipeline:
//   1. JLS 3.3 unicode-escape translation (with the even-backslash
//      eligibility rule) into a translated UTF-8 buffer plus a byte map
//      back to raw offsets.
//   2. Tokenization of the translated buffer; every token's offset/length
//      are mapped back to RAW source offsets.
//
// The translated buffer is what the JLS calls the input character stream:
// a \u000A written in a line comment terminates it, a \u0022 can open a
// string, and so on - all of that falls out of translating first.

#include "b2/frontend/Lexer.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "b2/frontend/Diagnostics.h"
#include "b2/frontend/SourceManager.h"
#include "b2/frontend/Token.h"

namespace b2::frontend {

namespace {

// ---------------------------------------------------------------- UTF-8 ----

// Decodes one code point; sets `len` to the bytes consumed. Invalid
// sequences decode to U+FFFD with len 1 (input is untrusted; we always
// make progress and never crash).
char32_t decodeUtf8(std::string_view s, std::uint32_t& len) {
  if (s.empty()) {
    len = 0;
    return 0;
  }
  const auto c0 = static_cast<unsigned char>(s[0]);
  const auto cont = [&](int i) {
    return i < static_cast<int>(s.size()) &&
           (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80;
  };
  if (c0 < 0x80) {
    len = 1;
    return c0;
  }
  if ((c0 & 0xE0) == 0xC0 && cont(1)) {
    len = 2;
    return (static_cast<char32_t>(c0 & 0x1F) << 6) |
           (static_cast<unsigned char>(s[1]) & 0x3F);
  }
  if ((c0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
    len = 3;
    return (static_cast<char32_t>(c0 & 0x0F) << 12) |
           (static_cast<char32_t>(static_cast<unsigned char>(s[1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(s[2]) & 0x3F);
  }
  if ((c0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
    len = 4;
    return (static_cast<char32_t>(c0 & 0x07) << 18) |
           (static_cast<char32_t>(static_cast<unsigned char>(s[1]) & 0x3F) << 12) |
           (static_cast<char32_t>(static_cast<unsigned char>(s[2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(s[3]) & 0x3F);
  }
  len = 1;
  return 0xFFFD;
}

void encodeUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// -------------------------------------------------------- char classes -----

bool isDigitCp(char32_t c) { return c >= '0' && c <= '9'; }
bool isOctalDigitCp(char32_t c) { return c >= '0' && c <= '7'; }
bool isBinaryDigitCp(char32_t c) { return c == '0' || c == '1'; }
bool isHexDigitCp(char32_t c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool isAsciiLetterCp(char32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Approximate Unicode identifier support.
//
// This stands in for Character.isJavaIdentifierStart / isJavaIdentifierPart
// without shipping ICU-sized tables: common script blocks (Latin-1 letters,
// Greek, Cyrillic, CJK, Hangul, fullwidth forms, ...) are accepted;
// punctuation, symbol, and emoji blocks are rejected. The approximation is
// documented in docs/frontend_contract.md; tightening it is future work.
bool isUnicodeIdentChar(char32_t c) {
  if (c < 0x80) return false;
  if (c == 0xFEFF) return false;
  const auto in = [&](char32_t a, char32_t b) { return c >= a && c <= b; };
  if (in(0x00AA, 0x00B5) || in(0x00BA, 0x02FF)) {
    return c != 0x00D7 && c != 0x00F7;  // multiplication / division signs
  }
  if (in(0x0370, 0x1FFF)) return true;   // Greek .. scripts
  if (in(0x2000, 0x206F)) return false;  // general punctuation
  if (in(0x2070, 0x2BFF)) return false;  // super/subscripts, arrows, math
  if (in(0x2C00, 0x2FFF)) return true;   // Latin Extended-C, CJK radicals
  if (in(0x3000, 0x303F)) return false;  // CJK punctuation
  if (in(0x3041, 0xD7FF)) return true;   // CJK, Hangul, other scripts
  if (in(0xF900, 0xFDCF)) return true;   // CJK compatibility ideographs
  if (in(0xFDD0, 0xFDEF)) return false;  // noncharacters
  if (in(0xFE10, 0xFE1F)) return false;  // vertical forms
  if (in(0xFE20, 0xFE4F)) return true;   // combining marks, compat forms
  if (in(0xFF01, 0xFF20)) return false;  // fullwidth punctuation
  if (in(0xFF21, 0xFF3A)) return true;   // fullwidth letters
  if (in(0xFF41, 0xFF5A)) return true;
  if (in(0xFF66, 0xFFDC)) return true;   // halfwidth katakana
  if (in(0xFFE0, 0xFFFF)) return false;  // fullwidth signs
  if (in(0x10000, 0x1D7FF)) return true;
  return false;  // symbols / emoji planes (approximation)
}

bool isIdentStartCp(char32_t c) {
  return isAsciiLetterCp(c) || c == '_' || c == '$' || isUnicodeIdentChar(c);
}
bool isIdentPartCp(char32_t c) { return isIdentStartCp(c) || isDigitCp(c); }

bool isWhitespaceCp(char32_t c) {
  return c == ' ' || c == '\t' || c == '\f' || c == '\n' || c == '\r' ||
         c == 0x2028 || c == 0x2029 || c == 0xFEFF;
}

// ------------------------------------------------------------- runner ------

constexpr char32_t kInvalidEscape = 0x110000;    // sentinel: diagnosed
constexpr char32_t kLineContinuation = 0x110001; // sentinel: \ <newline> in text block

class LexRunner {
 public:
  LexRunner(const SourceManager& sm, DiagnosticEngine& diags)
      : sm_(sm), diags_(diags) {}

  std::vector<Token> run();

 private:
  // ---- diagnostics (offsets are translated; mapped to raw) --------------
  std::uint32_t rawAt(std::uint32_t tpos) const {
    if (map_.empty()) return tpos;
    if (tpos >= map_.size()) return sm_.sourceSize();
    return map_[tpos];
  }
  void err(std::uint32_t tpos, std::string msg) {
    diags_.error(rawAt(tpos), std::move(msg));
  }

  // ---- cursor over the translated buffer ---------------------------------
  std::string_view view() const { return text_; }
  bool atEnd() const { return pos_ >= text_.size(); }
  char32_t cur() {
    if (atEnd()) return 0;
    std::uint32_t len = 0;
    return decodeUtf8(view().substr(pos_), len);
  }
  char32_t peekCp(int n) {
    std::uint32_t p = pos_;
    char32_t c = 0;
    for (int k = 0; k <= n; ++k) {
      if (p >= text_.size()) return 0;
      std::uint32_t len = 0;
      c = decodeUtf8(view().substr(p), len);
      p += len;
    }
    return c;
  }
  void bump() {
    if (atEnd()) return;
    std::uint32_t len = 0;
    decodeUtf8(view().substr(pos_), len);
    pos_ += len;
  }

  Token finish(Tok kind, std::uint32_t start) {
    Token t;
    t.kind = kind;
    t.offset = rawAt(start);
    t.length = rawAt(pos_) - rawAt(start);
    return t;
  }
  std::string spelling(std::uint32_t start) const {
    return text_.substr(start, pos_ - start);
  }

  // ---- JLS 3.3 translation ----------------------------------------------
  void translate();

  // ---- trivia ------------------------------------------------------------
  bool skipTrivia();

  // ---- tokens ------------------------------------------------------------
  Token lexOne();
  Token lexIdentifier();
  Token lexNumber();
  Token lexDecimal(std::uint32_t start);
  Token lexHex(std::uint32_t start);
  Token lexBinary(std::uint32_t start);
  Token lexOctal(std::uint32_t start);
  Token lexChar();
  Token lexString();
  Token lexTextBlock();

  // Scans a run of digits in `base`, skipping underscores (validating that
  // each underscore sits between digits). `prevDigit` tells whether a digit
  // was already consumed before this run (e.g. the '0' of '0_1').
  std::string scanDigits(int base, bool prevDigit);

  // Decodes one escape sequence at s[i] (s[i] == '\\'); advances i past it.
  // Returns the decoded code point, kInvalidEscape, or kLineContinuation.
  char32_t scanEscape(std::string_view s, std::uint32_t& i, bool inTextBlock,
                      std::uint32_t litStart);
  std::string decodeLineEscapes(const std::string& rawLine, std::uint32_t litStart,
                                bool& continuation);

  const SourceManager& sm_;
  DiagnosticEngine& diags_;

  std::string text_;                // translated UTF-8 buffer
  std::vector<std::uint32_t> map_;  // translated byte -> raw byte; empty == identity
  std::vector<Token> toks_;
  std::uint32_t pos_ = 0;
};

std::vector<Token> LexRunner::run() {
  translate();
  while (!atEnd()) {
    if (skipTrivia()) continue;
    if (atEnd()) break;
    toks_.push_back(lexOne());
  }
  Token eof;
  eof.kind = Tok::EndOfFile;
  eof.offset = rawAt(static_cast<std::uint32_t>(text_.size()));
  eof.length = 0;
  toks_.push_back(std::move(eof));
  return std::move(toks_);
}

void LexRunner::translate() {
  const std::string_view raw = sm_.source();
  std::string out;
  out.reserve(raw.size());
  std::vector<std::uint32_t> map;
  map.reserve(raw.size());

  auto emitRawRun = [&](std::size_t from, std::size_t n) {
    for (std::size_t q = 0; q < n; ++q) {
      out.push_back(raw[from + q]);
      map.push_back(static_cast<std::uint32_t>(from + q));
    }
  };
  const auto hexVal = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };

  std::size_t i = 0;
  bool escaped = false;
  while (i < raw.size()) {
    if (raw[i] != '\\') {
      out.push_back(raw[i]);
      map.push_back(static_cast<std::uint32_t>(i));
      ++i;
      continue;
    }
    // Run of backslashes. Only the last one can start a unicode escape, and
    // only when the run length is odd (JLS 3.3: an eligible '\' is preceded
    // by an even number of backslashes).
    std::size_t j = i;
    while (j < raw.size() && raw[j] == '\\') ++j;
    const std::size_t run = j - i;
    const bool followedByU = j < raw.size() && raw[j] == 'u';

    if (followedByU && (run % 2 == 1)) {
      emitRawRun(i, run - 1);  // the literal part of the run
      // Parse: '\' 'u'+ hex hex hex hex starting at raw[j-1].
      std::size_t p = j;
      while (p < raw.size() && raw[p] == 'u') ++p;
      std::uint32_t value = 0;
      bool ok = true;
      if (p + 4 > raw.size()) {
        ok = false;
      } else {
        for (int q = 0; q < 4; ++q) {
          const int v = hexVal(raw[p + q]);
          if (v < 0) {
            ok = false;
            break;
          }
          value = value * 16 + static_cast<std::uint32_t>(v);
        }
      }
      if (!ok) {
        diags_.error(static_cast<std::uint32_t>(j - 1), "invalid unicode escape");
        const std::size_t end = std::min(raw.size(), p + 4);
        emitRawRun(j - 1, end - (j - 1));
        i = end;
        escaped = true;
        continue;
      }
      const std::size_t before = out.size();
      encodeUtf8(out, static_cast<char32_t>(value));
      for (std::size_t q = before; q < out.size(); ++q)
        map.push_back(static_cast<std::uint32_t>(j - 1));
      i = p + 4;
      escaped = true;
      continue;
    }

    // No eligible escape: the whole run is literal. (When followed by 'u'
    // with an even run, the 'u' is handled as an ordinary character on the
    // next iteration.)
    emitRawRun(i, run);
    i = j;
    if (followedByU) escaped = true;
  }

  if (escaped) {
    text_ = std::move(out);
    map_ = std::move(map);
  } else {
    text_.assign(raw);
  }
}

bool LexRunner::skipTrivia() {
  bool consumed = false;
  while (!atEnd()) {
    const char32_t c = cur();
    if (isWhitespaceCp(c)) {
      bump();
      consumed = true;
      continue;
    }
    if (c == '/' && peekCp(1) == '/') {
      // A translated \u000A terminates the comment, which cur() reflects.
      bump();
      bump();
      while (!atEnd() && cur() != '\n' && cur() != '\r') bump();
      consumed = true;
      continue;
    }
    if (c == '/' && peekCp(1) == '*') {
      const std::uint32_t start = pos_;
      bump();
      bump();
      bool closed = false;
      while (!atEnd()) {
        if (cur() == '*' && peekCp(1) == '/') {
          bump();
          bump();
          closed = true;
          break;
        }
        bump();
      }
      if (!closed) err(start, "unterminated comment");
      consumed = true;
      continue;
    }
    break;
  }
  return consumed;
}

Token LexRunner::lexIdentifier() {
  const std::uint32_t start = pos_;
  std::string name;
  while (!atEnd()) {
    const char32_t c = cur();
    if (isIdentPartCp(c)) {
      encodeUtf8(name, c);
      bump();
    } else {
      break;
    }
  }
  Token t = finish(classifyWord(name), start);
  t.text = std::move(name);
  return t;
}

// ---- numbers --------------------------------------------------------------

std::string LexRunner::scanDigits(int base, bool prevDigit) {
  std::string digits;
  bool underscorePending = false;
  while (true) {
    const char32_t c = cur();
    if (c == '_') {
      if (!prevDigit && digits.empty())
        err(pos_, "misplaced '_' in numeric literal");
      underscorePending = true;
      bump();
      continue;
    }
    const bool isD = (base == 16)   ? isHexDigitCp(c)
                     : (base == 10) ? isDigitCp(c)
                     : (base == 8)  ? isOctalDigitCp(c)
                                    : isBinaryDigitCp(c);
    if (!isD) break;
    digits.push_back(static_cast<char>(c));
    prevDigit = true;
    underscorePending = false;
    bump();
  }
  if (underscorePending) err(pos_ - 1, "misplaced '_' in numeric literal");
  return digits;
}

Token LexRunner::lexNumber() {
  const std::uint32_t start = pos_;
  if (cur() == '0') {
    const char32_t n = peekCp(1);
    if (n == 'x' || n == 'X') {
      bump();
      bump();
      return lexHex(start);
    }
    if (n == 'b' || n == 'B') {
      bump();
      bump();
      return lexBinary(start);
    }
    if (isDigitCp(n)) return lexOctal(start);
  }
  return lexDecimal(start);
}

Token LexRunner::lexDecimal(std::uint32_t start) {
  const std::string intDigits = scanDigits(10, true);
  bool sawDot = false, sawExp = false, expMalformed = false;
  std::string fracDigits, expPart;

  if (cur() == '.') {
    sawDot = true;
    bump();
    fracDigits = scanDigits(10, false);
  }

  if (cur() == 'e' || cur() == 'E') {
    const char32_t n1 = peekCp(1);
    const char32_t n2 = peekCp(2);
    if (isDigitCp(n1) || ((n1 == '+' || n1 == '-') && isDigitCp(n2))) {
      sawExp = true;
      bump();  // 'e'
      expPart.push_back('e');
      if (cur() == '+' || cur() == '-') {
        expPart.push_back(static_cast<char>(cur()));
        bump();
      }
      expPart += scanDigits(10, false);
    } else {
      expMalformed = true;  // '1e' / '1ex' / '1e+' ...
    }
  }

  const bool isFloat = sawDot || sawExp || expMalformed || cur() == 'f' ||
                       cur() == 'F' || cur() == 'd' || cur() == 'D';
  char suffix = 0;
  if (cur() == 'f' || cur() == 'F') {
    suffix = 'f';
    bump();
  } else if (cur() == 'd' || cur() == 'D') {
    suffix = 'd';
    bump();
  } else if (!isFloat && (cur() == 'l' || cur() == 'L')) {
    suffix = 'l';
    bump();
  }

  if (expMalformed) {
    bump();  // the 'e'/'E'
    if (cur() == '+' || cur() == '-') bump();
    while (isIdentStartCp(cur()) || isDigitCp(cur())) bump();
    err(start, "malformed numeric literal");
    return finish(Tok::Error, start);
  }
  if (isIdentStartCp(cur()) || isDigitCp(cur())) {  // e.g. 12abc
    while (isIdentStartCp(cur()) || isDigitCp(cur())) bump();
    err(start, "malformed numeric literal");
    return finish(Tok::Error, start);
  }

  if (!isFloat) {
    std::uint64_t value = 0;
    const auto res =
        std::from_chars(intDigits.data(), intDigits.data() + intDigits.size(), value, 10);
    Token t = finish(suffix == 'l' ? Tok::LongLiteral : Tok::IntegerLiteral, start);
    if (res.ec == std::errc::result_out_of_range) {
      t.intValue = ~std::uint64_t{0};
      err(start, "integer literal too large");
    } else {
      t.intValue = value;
    }
    t.text = spelling(start);
    return t;
  }

  // Floating point.
  std::string cleaned = intDigits;
  if (sawDot) {
    cleaned += '.';
    cleaned += fracDigits;
    if (fracDigits.empty()) cleaned += '0';  // "1." -> "1.0"
  }
  cleaned += expPart;
  double d = 0.0;
  const auto res = std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), d);
  Token t = finish(suffix == 'f' ? Tok::FloatLiteral : Tok::DoubleLiteral, start);
  if (res.ec == std::errc::result_out_of_range) {
    err(start, "floating-point literal out of range");
  }
  t.floatValue = d;
  t.text = spelling(start);
  return t;
}

Token LexRunner::lexHex(std::uint32_t start) {
  const std::string mant = scanDigits(16, false);
  bool sawDot = false;
  std::string frac;
  if (cur() == '.') {
    sawDot = true;
    bump();
    frac = scanDigits(16, false);
  }
  bool sawExp = false;
  std::string expPart;
  if (cur() == 'p' || cur() == 'P') {
    const char32_t n1 = peekCp(1);
    const char32_t n2 = peekCp(2);
    if (isDigitCp(n1) || ((n1 == '+' || n1 == '-') && isDigitCp(n2))) {
      sawExp = true;
      bump();  // 'p'
      expPart.push_back('p');
      if (cur() == '+' || cur() == '-') {
        expPart.push_back(static_cast<char>(cur()));
        bump();
      }
      expPart += scanDigits(10, false);
    }
  }

  const bool isFloat = sawDot || sawExp;
  char suffix = 0;
  if (cur() == 'f' || cur() == 'F') {
    suffix = 'f';
    bump();
  } else if (cur() == 'd' || cur() == 'D') {
    suffix = 'd';
    bump();
  } else if (!isFloat && (cur() == 'l' || cur() == 'L')) {
    suffix = 'l';
    bump();
  }

  if (mant.empty() && frac.empty() && !sawExp) {  // e.g. `0x` / `0xg`
    while (isIdentStartCp(cur()) || isDigitCp(cur())) bump();
    err(start, "malformed hexadecimal literal");
    return finish(Tok::Error, start);
  }
  if (isIdentStartCp(cur()) || isDigitCp(cur())) {
    while (isIdentStartCp(cur()) || isDigitCp(cur())) bump();
    err(start, "malformed numeric literal");
    return finish(Tok::Error, start);
  }

  if (!isFloat) {
    std::uint64_t value = 0;
    const auto res = std::from_chars(mant.data(), mant.data() + mant.size(), value, 16);
    Token t = finish(suffix == 'l' ? Tok::LongLiteral : Tok::IntegerLiteral, start);
    if (res.ec == std::errc::result_out_of_range) {
      t.intValue = ~std::uint64_t{0};
      err(start, "integer literal too large");
    } else {
      t.intValue = value;
    }
    t.text = spelling(start);
    return t;
  }

  // Hexadecimal floating point: mantissa [ '.' fraction ] 'p' exponent.
  if (!sawExp) err(start, "hexadecimal floating literal requires a 'p' exponent");
  std::string cleaned;
  cleaned += mant.empty() ? "0" : mant;
  if (sawDot) {
    cleaned += '.';
    cleaned += frac.empty() ? "0" : frac;
  }
  cleaned += expPart.empty() ? "p0" : expPart;
  double d = 0.0;
  const auto res = std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), d,
                                   std::chars_format::hex);
  Token t = finish(suffix == 'f' ? Tok::FloatLiteral : Tok::DoubleLiteral, start);
  if (res.ec == std::errc::result_out_of_range) {
    err(start, "floating-point literal out of range");
  }
  t.floatValue = d;
  t.text = spelling(start);
  return t;
}

Token LexRunner::lexBinary(std::uint32_t start) {
  const std::string digits = scanDigits(2, false);
  char suffix = 0;
  if (cur() == 'l' || cur() == 'L') {
    suffix = static_cast<char>(cur());
    bump();
  }
  if (digits.empty() || isIdentStartCp(cur()) || isDigitCp(cur())) {
    while (isIdentStartCp(cur()) || isDigitCp(cur())) bump();
    err(start, "malformed binary literal");
    return finish(Tok::Error, start);
  }
  std::uint64_t value = 0;
  const auto res = std::from_chars(digits.data(), digits.data() + digits.size(), value, 2);
  Token t = finish(suffix ? Tok::LongLiteral : Tok::IntegerLiteral, start);
  if (res.ec == std::errc::result_out_of_range) {
    t.intValue = ~std::uint64_t{0};
    err(start, "integer literal too large");
  } else {
    t.intValue = value;
  }
  t.text = spelling(start);
  return t;
}

Token LexRunner::lexOctal(std::uint32_t start) {
  bump();  // leading '0'
  const std::string digits = scanDigits(8, true);
  // An 8 or 9 right after the octal digits is malformed (019, 09.5, ...).
  if (isDigitCp(cur()) || isIdentStartCp(cur())) {
    while (isIdentStartCp(cur()) || isDigitCp(cur())) bump();
    err(start, "malformed octal literal");
    return finish(Tok::Error, start);
  }
  char suffix = 0;
  if (cur() == 'l' || cur() == 'L') {
    suffix = static_cast<char>(cur());
    bump();
  }
  std::uint64_t value = 0;
  const auto res = std::from_chars(digits.data(), digits.data() + digits.size(), value, 8);
  Token t = finish(suffix ? Tok::LongLiteral : Tok::IntegerLiteral, start);
  if (res.ec == std::errc::result_out_of_range) {
    t.intValue = ~std::uint64_t{0};
    err(start, "integer literal too large");
  } else {
    t.intValue = value;
  }
  t.text = spelling(start);
  return t;
}

// ---- escapes ---------------------------------------------------------------

char32_t LexRunner::scanEscape(std::string_view s, std::uint32_t& i, bool inTextBlock,
                               std::uint32_t litStart) {
  // Precondition: s[i] == '\\'.
  std::uint32_t p = i + 1;
  const auto cpAt = [&](std::uint32_t q) -> char32_t {
    if (q >= s.size()) return 0;
    std::uint32_t len = 0;
    return decodeUtf8(s.substr(q), len);
  };
  const auto advance = [&]() {
    std::uint32_t len = 0;
    decodeUtf8(s.substr(p), len);
    p += len;
  };

  const char32_t c = cpAt(p);
  switch (c) {
    case 'b': advance(); i = p; return 8;
    case 't': advance(); i = p; return 9;
    case 'n': advance(); i = p; return 10;
    case 'f': advance(); i = p; return 12;
    case 'r': advance(); i = p; return 13;
    case 's': advance(); i = p; return 32;
    case '"': advance(); i = p; return '"';
    case '\'': advance(); i = p; return '\'';
    case '\\': advance(); i = p; return '\\';
    case '\n':
    case '\r': {
      if (inTextBlock) {
        // Line continuation: consume the whole terminator.
        if (c == '\r' && cpAt(p + 1) == '\n') advance();
        advance();
        i = p;
        return kLineContinuation;
      }
      err(litStart, "invalid escape sequence");
      i = p;
      return kInvalidEscape;
    }
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
      // Octal escape: \o, \oo, or \3oo (three digits only when the first
      // is <= 3). Value must fit in 8 bits.
      std::uint32_t v = 0;
      const std::uint32_t maxDigits = (c <= '3') ? 3 : 2;
      std::uint32_t n = 0;
      while (n < maxDigits && isOctalDigitCp(cpAt(p))) {
        v = v * 8 + (cpAt(p) - '0');
        advance();
        ++n;
      }
      i = p;
      if (v > 0xFF) {
        err(litStart, "octal escape out of range (maximum is \\377)");
        return kInvalidEscape;
      }
      return v;
    }
    default: {
      // Includes 'u': a \u reaching this scanner was shielded by an odd
      // number of backslashes, so it is NOT a unicode escape (JLS 3.3).
      err(litStart, "invalid escape sequence");
      if (p < s.size() && c != '\n' && c != '\r') advance();
      i = p;
      return kInvalidEscape;
    }
  }
}

std::string LexRunner::decodeLineEscapes(const std::string& rawLine, std::uint32_t litStart,
                                         bool& continuation) {
  std::string out;
  std::uint32_t i = 0;
  const std::string_view s = rawLine;
  while (i < s.size()) {
    if (s[i] == '\\') {
      if (i + 1 >= s.size()) {  // trailing '\' before the line terminator
        continuation = true;
        break;
      }
      std::uint32_t j = i;
      const char32_t e = scanEscape(s, j, /*inTextBlock=*/true, litStart);
      i = j;
      if (e == kInvalidEscape) continue;
      if (e == kLineContinuation) {
        continuation = true;
        break;
      }
      encodeUtf8(out, e);
      continue;
    }
    std::uint32_t len = 0;
    const char32_t cp = decodeUtf8(s.substr(i), len);
    encodeUtf8(out, cp);  // round-trips; invalid input became U+FFFD above
    i += len;
  }
  return out;
}

// ---- character / string literals -------------------------------------------

Token LexRunner::lexChar() {
  const std::uint32_t start = pos_;
  bump();  // opening quote
  if (cur() == '\'') {
    bump();
    err(start, "empty character literal");
    return finish(Tok::Error, start);
  }
  if (atEnd() || cur() == '\n' || cur() == '\r') {
    err(start, "unterminated character literal");
    return finish(Tok::Error, start);
  }

  char32_t cp = 0;
  bool ok = true;
  if (cur() == '\\') {
    std::uint32_t i = pos_;
    cp = scanEscape(view(), i, /*inTextBlock=*/false, start);
    pos_ = i;
    if (cp == kInvalidEscape || cp == kLineContinuation) ok = false;
  } else {
    cp = cur();
    bump();
  }

  if (ok && cur() == '\'') {
    bump();
    Token t = finish(Tok::CharacterLiteral, start);
    t.intValue = cp;
    encodeUtf8(t.text, cp);
    return t;
  }
  if (!ok || !atEnd()) {
    if (atEnd() || cur() == '\n' || cur() == '\r') {
      err(start, "unterminated character literal");
      return finish(Tok::Error, start);
    }
    // Too many characters: skip to the closing quote.
    while (!atEnd() && cur() != '\'' && cur() != '\n' && cur() != '\r') bump();
    if (cur() == '\'') bump();
    err(start, "character literal must contain exactly one character");
  }
  return finish(Tok::Error, start);
}

Token LexRunner::lexString() {
  const std::uint32_t start = pos_;
  bump();  // opening quote
  std::string value;
  while (true) {
    if (atEnd() || cur() == '\n' || cur() == '\r') {
      err(start, "unterminated string literal");
      return finish(Tok::Error, start);
    }
    if (cur() == '"') {
      bump();
      break;
    }
    if (cur() == '\\') {
      std::uint32_t i = pos_;
      const char32_t e = scanEscape(view(), i, /*inTextBlock=*/false, start);
      pos_ = i;
      if (e != kInvalidEscape && e != kLineContinuation) encodeUtf8(value, e);
      continue;
    }
    encodeUtf8(value, cur());
    bump();
  }
  Token t = finish(Tok::StringLiteral, start);
  t.text = std::move(value);
  return t;
}

// ---- text blocks (JLS 3.10.6) ----------------------------------------------
//
//   1. After the opening """, only whitespace and one line terminator may
//      appear before the content starts.
//   2. Content is collected raw (escape pairs are kept together so \"
//      cannot hide a closing delimiter) until the first """.
//   3. Incidental indentation = minimum leading whitespace over all
//      non-blank content lines AND the closing delimiter's line; blank
//      lines never widen the minimum.
//   4. Indentation is stripped, then escapes are processed (so an escaped
//      \s at line start survives; it is not treated as indentation).
//   5. A '\' at end of line is a line continuation (emits nothing).

Token LexRunner::lexTextBlock() {
  const std::uint32_t start = pos_;
  bump();
  bump();
  bump();  // opening """

  bool sawTerminator = false;
  while (!atEnd()) {
    const char32_t c = cur();
    if (c == '\n') {
      bump();
      sawTerminator = true;
      break;
    }
    if (c == '\r') {
      bump();
      if (cur() == '\n') bump();
      sawTerminator = true;
      break;
    }
    if (c == ' ' || c == '\t' || c == '\f') {
      bump();
      continue;
    }
    break;  // illegal character; diagnosed below
  }
  if (!sawTerminator && !atEnd()) {
    err(start, "illegal text block start: a line terminator must follow the opening \"\"\"");
    // Recover: keep scanning content from here.
  }

  std::vector<std::string> lines;
  std::string current;
  bool closed = false;
  while (!atEnd()) {
    const char32_t c = cur();
    if (c == '"') {
      if (peekCp(1) == '"' && peekCp(2) == '"') {
        bump();
        bump();
        bump();
        lines.push_back(std::move(current));
        closed = true;
        break;
      }
      current.push_back('"');
      bump();
      continue;
    }
    if (c == '\\') {
      // Keep the escape pair together so \" cannot hide a delimiter.
      current.push_back('\\');
      bump();
      if (!atEnd() && cur() != '\n' && cur() != '\r') {
        encodeUtf8(current, cur());
        bump();
      }
      continue;
    }
    if (c == '\n') {
      bump();
      lines.push_back(std::move(current));
      current.clear();
      continue;
    }
    if (c == '\r') {
      bump();
      if (cur() == '\n') bump();
      lines.push_back(std::move(current));
      current.clear();
      continue;
    }
    encodeUtf8(current, c);
    bump();
  }
  if (!closed) {
    lines.push_back(std::move(current));
    err(start, "unterminated text block");
    return finish(Tok::Error, start);
  }

  // Incidental whitespace.
  const auto indentWidth = [](const std::string& s) {
    std::size_t w = 0;
    while (w < s.size() && (s[w] == ' ' || s[w] == '\t')) ++w;
    return w;
  };
  std::size_t minIndent = static_cast<std::size_t>(-1);
  for (std::size_t li = 0; li < lines.size(); ++li) {
    const bool isClosing = (li + 1 == lines.size());
    if (!isClosing && indentWidth(lines[li]) == lines[li].size()) continue;  // blank
    minIndent = std::min(minIndent, indentWidth(lines[li]));
  }
  if (minIndent == static_cast<std::size_t>(-1)) minIndent = 0;

  std::string value;
  for (std::size_t li = 0; li < lines.size(); ++li) {
    std::string line = lines[li];
    line.erase(0, std::min(minIndent, indentWidth(line)));
    bool continuation = false;
    value += decodeLineEscapes(line, start, continuation);
    if (li + 1 < lines.size() && !continuation) value += '\n';
  }

  Token t = finish(Tok::StringLiteral, start);
  t.text = std::move(value);
  t.isTextBlock = true;
  return t;
}

// ---- everything else -------------------------------------------------------

Token LexRunner::lexOne() {
  const std::uint32_t start = pos_;
  const char32_t c = cur();

  if (isIdentStartCp(c)) return lexIdentifier();
  if (isDigitCp(c)) return lexNumber();

  switch (c) {
    case '"':
      if (peekCp(1) == '"' && peekCp(2) == '"') return lexTextBlock();
      return lexString();
    case '\'': return lexChar();

    case '(': bump(); return finish(Tok::LeftParen, start);
    case ')': bump(); return finish(Tok::RightParen, start);
    case '{': bump(); return finish(Tok::LeftBrace, start);
    case '}': bump(); return finish(Tok::RightBrace, start);
    case '[': bump(); return finish(Tok::LeftBracket, start);
    case ']': bump(); return finish(Tok::RightBracket, start);
    case ';': bump(); return finish(Tok::Semicolon, start);
    case ',': bump(); return finish(Tok::Comma, start);
    case '@': bump(); return finish(Tok::At, start);
    case '~': bump(); return finish(Tok::Tilde, start);
    case '?': bump(); return finish(Tok::Question, start);

    case '.':
      if (peekCp(1) == '.' && peekCp(2) == '.') {
        bump(); bump(); bump();
        return finish(Tok::Ellipsis, start);
      }
      bump();
      return finish(Tok::Dot, start);

    case ':':
      if (peekCp(1) == ':') {
        bump(); bump();
        return finish(Tok::ColonColon, start);
      }
      bump();
      return finish(Tok::Colon, start);

    case '-':
      if (peekCp(1) == '-') { bump(); bump(); return finish(Tok::MinusMinus, start); }
      if (peekCp(1) == '>') { bump(); bump(); return finish(Tok::Arrow, start); }
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::MinusEqual, start); }
      bump();
      return finish(Tok::Minus, start);

    case '+':
      if (peekCp(1) == '+') { bump(); bump(); return finish(Tok::PlusPlus, start); }
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::PlusEqual, start); }
      bump();
      return finish(Tok::Plus, start);

    case '&':
      if (peekCp(1) == '&') { bump(); bump(); return finish(Tok::AmpAmp, start); }
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::AmpEqual, start); }
      bump();
      return finish(Tok::Amp, start);

    case '|':
      if (peekCp(1) == '|') { bump(); bump(); return finish(Tok::BarBar, start); }
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::BarEqual, start); }
      bump();
      return finish(Tok::Bar, start);

    case '^':
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::CaretEqual, start); }
      bump();
      return finish(Tok::Caret, start);

    case '!':
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::BangEqual, start); }
      bump();
      return finish(Tok::Bang, start);

    case '=':
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::EqualEqual, start); }
      bump();
      return finish(Tok::Equal, start);

    case '*':
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::StarEqual, start); }
      bump();
      return finish(Tok::Star, start);

    case '/':
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::SlashEqual, start); }
      bump();
      return finish(Tok::Slash, start);

    case '%':
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::PercentEqual, start); }
      bump();
      return finish(Tok::Percent, start);

    case '<':
      if (peekCp(1) == '<') {
        if (peekCp(2) == '=') { bump(); bump(); bump(); return finish(Tok::LessLessEqual, start); }
        bump(); bump();
        return finish(Tok::LessLess, start);
      }
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::LessEqual, start); }
      bump();
      return finish(Tok::Less, start);

    case '>':
      if (peekCp(1) == '>') {
        if (peekCp(2) == '>') {
          if (peekCp(3) == '=') { bump(); bump(); bump(); bump(); return finish(Tok::GreaterGreaterGreaterEqual, start); }
          bump(); bump(); bump();
          return finish(Tok::GreaterGreaterGreater, start);
        }
        if (peekCp(2) == '=') { bump(); bump(); bump(); return finish(Tok::GreaterGreaterEqual, start); }
        bump(); bump();
        return finish(Tok::GreaterGreater, start);
      }
      if (peekCp(1) == '=') { bump(); bump(); return finish(Tok::GreaterEqual, start); }
      bump();
      return finish(Tok::Greater, start);

    default: {
      std::string msg = "invalid character '";
      if (c >= 0x20 && c < 0x7F) {
        msg += static_cast<char>(c);
      } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "U+%04X", static_cast<unsigned>(c));
        msg += buf;
      }
      msg += "'";
      bump();
      err(start, std::move(msg));
      return finish(Tok::Error, start);
    }
  }
}

}  // namespace

std::vector<Token> Lexer::lex() {
  LexRunner runner(sm_, diags_);
  return runner.run();
}

}  // namespace b2::frontend
