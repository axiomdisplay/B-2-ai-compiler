// B-2 RBC text format: parser + deterministic printer (golden-test oracle).
//
// WHY THIS FILE EXISTS:
// docs/cpp26_standards.md "Golden IR Tests" makes the text round-trip the
// middle end's stability oracle: byte-stable print(parse(x)) catches
// unintended RBC changes before they become Java-visible, and the same text
// is the human debug surface for frontend lowering reviews. The in-memory
// model is frozen in include/b2/rbc/Rbc.h; this file owns only the text.
//
// The normative grammar lives in include/b2/rbc/RbcText.h. This
// implementation resolves the remaining ambiguities as follows (the parser
// is lenient, the printer canonical, so hand-written text stays convenient
// while machine output stays byte-stable):
//
//   1. TOKENS. Whitespace-separated; '#' starts a comment to end of line;
//      double-quoted strings with escapes \" \\ \n \t \r; decimal integers
//      with optional leading '-'; floats in the standard forms 1.5, -0.25,
//      1e9, .5, 5. (and 1e+09). ':', '=', '{', '}' and a lone '-' are
//      single-character punctuation. Everything else (including "(I)V",
//      "java/lang/String", ".method", "<init>", "r0") is one "word" token.
//      Line breaks are tracked per token solely to delimit labels.
//
//   2. LABELS are 'ident ":"' ALONE ON A LINE and bind to the next
//      instruction index; instructions themselves may flow across lines.
//      A label bound at code.size() is an error ("points past the end")
//      with ONE deliberate exception, see 6.
//
//   3. SWITCH CONSTANTS canonicalize to Const::ints as:
//        lookup: [N, defaultTarget, match1, target1, ..., matchN, targetN]
//                (matches strictly ascending, non-contiguous)
//        table:  [low, high, defaultTarget, target(low), ..., target(high)]
//                (dense ascending matches)
//      The parser sorts entries, rejects duplicate matches, requires a
//      'default' entry, and picks the table form iff the sorted matches are
//      contiguous. A tableswitch operand must reference a table-layout
//      constant and a lookupswitch operand a lookup-layout constant
//      (mismatched forms would be rejected by the verifier anyway; the
//      text parser rejects them early so the printer never has to guess).
//
//   4. OPERAND SPELLING is purely Sig-driven (info(op)):
//        registers  -> "r<N>"   (always; including Call's b=argCount field,
//                               which lives in Ins's b register slot)
//        slots      -> "l<N>"
//        constants  -> "c<N>"   (resolved after the whole method is read,
//                               forward references allowed)
//        branch ops -> label idents, resolved to instruction indices in imm
//        bare ints  -> only Sig fields that are not registers/slots/consts:
//                      iconst/fconst/iinc immediates (printed signed),
//                      newarray atype and quick field offsets (unsigned),
//                      deopt ids (guard/guard_class/deopt_trap, unsigned),
//                      invoke*_quick resolved ids (unsigned)
//
//   5. NON-FINITE float/double constants print and parse as the words
//      "inf", "-inf", "nan", "-nan" (std::to_chars/from_chars spelling).
//
//   6. .catch DIRECTIVE (grammar addition on top of RbcText.h):
//        ".catch" <class-c-id | "all"> "from" <label> "to" <label>
//                 "handler" <label>
//      Resolves to ExceptionHandler{start, end, handler, catchType} where
//      end is EXCLUSIVE and catchType is the pool index of a Class constant
//      (-1 for "all"). Directives may appear anywhere in the body. Because
//      [start,end) ranges routinely cover the method tail, a label bound at
//      code.size() is LEGAL when used exclusively as a .catch "to" target;
//      the printer emits that label after the last instruction.
//
//   7. GARBAGE POLICY. The parser accepts arbitrary bytes and returns
//      exactly one TextError (first error by byte offset wins; structural
//      errors abort immediately, deferred errors - label/const resolution -
//      are collected and the smallest offset is reported). The printer
//      never crashes on invalid Programs (op >= Op::_Count prints
//      "bad<N>" plus raw fields, switch payloads that fit neither layout
//      print as default-only switches) but such output may not re-parse:
//      validity is the verifier's contract, not the printer's.
//
//   8. METHOD HEADER. ".method" [flags...] name descriptor: the descriptor
//      is the first word starting with '(' (JVM descriptors always do),
//      the word before it is the name, and every word between ".method"
//      and the name must be a known flag ("unknown flag = error" per the
//      task contract).

#include "b2/rbc/RbcText.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace b2::rbc {
namespace {

// ===========================================================================
// Tokenizer
// ===========================================================================
//
// WHY a flat token vector instead of an on-demand lexer: inputs are golden
// test files (tiny), a pre-scanned vector makes every peek(k) lookahead
// trivial, and the parser never has to re-synchronize byte positions. The
// tokenizer itself can never fail - malformed input just produces shorter
// or odder tokens that the parser reports with precise offsets.

enum class Tk : std::uint8_t {
  Word,  // mnemonic / directive / ident / "r0" / "(I)V" / ".method" ...
  Int,   // decimal integer, optional leading '-'
  Float, // 1.5 / -0.25 / 1e9 / .5 / 5. / 1e+09
  Str,   // double-quoted string, raw text includes the quotes
  Punct, // one of : = { } -
  End,   // sentinel appended after the last real token
};

struct Tok {
  Tk kind = Tk::End;
  std::uint32_t offset = 0; // byte offset of the first byte of the token
  std::uint32_t length = 0; // raw byte length
  bool lineStart = false;   // a '\n' preceded this token (first token: true)
  std::string_view text;    // raw slice into the source
};

[[nodiscard]] inline bool isWs(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}

[[nodiscard]] inline bool isDigit(char c) noexcept {
  return c >= '0' && c <= '9';
}

// WHY: word bytes are "everything that is not a delimiter". This keeps JVM
// descriptors ("(I)V", "[Ljava/lang/String;"), internal names and register
// spellings single tokens, which is what makes the grammar whitespace-based.
[[nodiscard]] inline bool isWordByte(char c) noexcept {
  return !isWs(c) && c != '#' && c != '"' && c != ':' && c != '=' &&
         c != '{' && c != '}';
}

[[nodiscard]] inline bool isIdentStart(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
         c == '$' || c == '.';
}

[[nodiscard]] inline bool isIdentByte(char c) noexcept {
  return isIdentStart(c) || isDigit(c);
}

[[nodiscard]] std::vector<Tok> lex(std::string_view src) {
  std::vector<Tok> toks;
  const std::size_t n = src.size();
  std::size_t i = 0;
  bool lineStart = true;
  while (i < n) {
    const char c = src[i];
    if (isWs(c)) {
      if (c == '\n') {
        lineStart = true;
      }
      ++i;
      continue;
    }
    if (c == '#') {
      // comment to end of line; the '\n' itself is left for the whitespace
      // branch so lineStart bookkeeping stays correct
      while (i < n && src[i] != '\n') {
        ++i;
      }
      continue;
    }

    Tok t;
    t.offset = static_cast<std::uint32_t>(i);
    t.lineStart = lineStart;
    lineStart = false;

    const auto digitAt = [&src, n](std::size_t k) noexcept {
      return k < n && isDigit(src[k]);
    };

    if (c == '"') {
      // String literal: scan to the closing quote; a backslash always
      // escapes the next byte (escape validity is checked at decode time,
      // not here, so the tokenizer stays a total function).
      ++i;
      while (i < n && src[i] != '"') {
        if (src[i] == '\\' && i + 1 < n) {
          i += 2;
        } else {
          ++i;
        }
      }
      if (i < n) {
        ++i; // consume the closing quote
      }
      t.kind = Tk::Str;
    } else if (isDigit(c) ||
               (c == '-' &&
                (digitAt(i + 1) ||
                 (i + 1 < n && src[i + 1] == '.' && digitAt(i + 2)))) ||
               (c == '.' && digitAt(i + 1))) {
      // Number: [-] digits [. digits] [e[+|-] digits]. A '.' followed by a
      // digit is a float; a '.' followed by anything else is a word
      // (directive). Byte consumption here is deliberately greedy over
      // number-ish bytes; from_chars re-validates the exact grammar and
      // errors point at the token start.
      std::size_t j = i;
      if (src[j] == '-') {
        ++j;
      }
      while (digitAt(j)) {
        ++j;
      }
      bool isFloat = false;
      if (j < n && src[j] == '.') {
        isFloat = true;
        ++j;
        while (digitAt(j)) {
          ++j;
        }
      }
      if (j < n && (src[j] == 'e' || src[j] == 'E')) {
        std::size_t k = j + 1;
        if (k < n && (src[k] == '+' || src[k] == '-')) {
          ++k;
        }
        if (digitAt(k)) {
          isFloat = true;
          j = k;
          while (digitAt(j)) {
            ++j;
          }
        }
      }
      i = j;
      t.kind = isFloat ? Tk::Float : Tk::Int;
    } else if (c == ':' || c == '=' || c == '{' || c == '}' || c == '-') {
      // single-character punctuation; '-' arrives here only when it does
      // not start a number (e.g. "-inf", or a stray minus)
      ++i;
      t.kind = Tk::Punct;
    } else {
      // word: maximal run of word bytes (also covers dot-led directives
      // like ".method" and lone dots)
      while (i < n && isWordByte(src[i])) {
        ++i;
      }
      t.kind = Tk::Word;
    }

    t.length = static_cast<std::uint32_t>(i - t.offset);
    t.text = src.substr(t.offset, t.length);
    toks.push_back(t);
  }

  Tok end;
  end.kind = Tk::End;
  end.offset = static_cast<std::uint32_t>(n);
  end.lineStart = true; // end-of-input counts as "next line" for label rules
  toks.push_back(end);
  return toks;
}

// ===========================================================================
// Shared tables and small helpers
// ===========================================================================

struct FlagWord {
  std::string_view word;
  std::uint16_t bit;
};

// Any order is accepted when parsing; this order is the canonical PRINT
// order (normative in the task contract).
inline constexpr FlagWord kFlags[] = {
    {"public", method_flags::Public},
    {"private", method_flags::Private},
    {"protected", method_flags::Protected},
    {"static", method_flags::Static},
    {"final", method_flags::Final},
    {"synchronized", method_flags::Synchronized},
    {"native", method_flags::Native},
    {"abstract", method_flags::Abstract},
    {"varargs", method_flags::Varargs},
};

[[nodiscard]] std::string tokDisplay(const Tok& t) {
  if (t.kind == Tk::End) {
    return "<end of input>";
  }
  std::string s(t.text);
  if (s.size() > 20) {
    s.resize(17);
    s += "...";
  }
  return s;
}

[[nodiscard]] TextError errAt(const Tok& t, std::string msg) {
  return TextError{t.offset, std::move(msg)};
}

// Reverse mnemonic index. WHY: the canonical mnemonics are owned by
// Opcode.cpp's opName() - the text parser must not carry a second spelling
// table that could drift. opName() is inverted once (the first opcode wins
// if a name ever repeats; "bad<N>" fallbacks are not parseable and are
// skipped).
[[nodiscard]] const std::vector<std::pair<std::string_view, Op>>& opIndex() {
  static const std::vector<std::pair<std::string_view, Op>> idx = [] {
    std::vector<std::pair<std::string_view, Op>> v;
    for (std::uint16_t i = 0; i < opCount(); ++i) {
      std::string_view nm = opName(static_cast<Op>(i));
      if (nm.starts_with("bad")) {
        continue;
      }
      if (std::none_of(v.begin(), v.end(),
                       [&nm](const auto& e) { return e.first == nm; })) {
        v.emplace_back(nm, static_cast<Op>(i));
      }
    }
    std::sort(v.begin(), v.end());
    return v;
  }();
  return idx;
}

[[nodiscard]] std::optional<Op> lookupOp(std::string_view w) {
  const auto& v = opIndex();
  const auto it = std::lower_bound(
      v.begin(), v.end(), w,
      [](const auto& e, std::string_view key) { return e.first < key; });
  if (it != v.end() && it->first == w) {
    return it->second;
  }
  return std::nullopt;
}

[[nodiscard]] bool isIdentShape(std::string_view w) {
  return !w.empty() && isIdentStart(w[0]) &&
         std::all_of(w.begin() + 1, w.end(),
                     [](char c) { return isIdentByte(c); });
}

// "r0" / "l12" / "c3" -> value. WHY a strict shape check: "r4x" or "r" must
// be rejected as what they are (bad spellings), not parsed as a truncated
// number.
[[nodiscard]] bool parsePrefixed(std::string_view text, char prefix,
                                 std::uint64_t& out) {
  if (text.size() < 2 || text[0] != prefix) {
    return false;
  }
  const std::string_view digits = text.substr(1);
  if (!std::all_of(digits.begin(), digits.end(),
                   [](char c) { return isDigit(c); })) {
    return false;
  }
  const char* begin = digits.data();
  const char* end = begin + digits.size();
  std::uint64_t v = 0;
  const auto res = std::from_chars(begin, end, v);
  if (res.ec != std::errc{} || res.ptr != end) {
    return false;
  }
  out = v;
  return true;
}

// Canonical switch layouts (see file header section 3).
[[nodiscard]] bool isTableLayout(const Const& c) noexcept {
  const std::vector<std::int32_t>& v = c.ints;
  if (v.size() < 4) {
    return false;
  }
  const std::int64_t low = v[0];
  const std::int64_t high = v[1];
  if (high < low) {
    return false;
  }
  return static_cast<std::uint64_t>(high - low) + 1 == v.size() - 3;
}

[[nodiscard]] bool isLookupLayout(const Const& c) noexcept {
  const std::vector<std::int32_t>& v = c.ints;
  if (v.size() < 2 || (v.size() - 2) % 2 != 0) {
    return false;
  }
  return v[0] >= 0 &&
         static_cast<std::uint64_t>(v[0]) == (v.size() - 2) / 2;
}

// ===========================================================================
// Parser
// ===========================================================================

class Parser {
public:
  explicit Parser(std::string_view text) : src_(text), toks_(lex(text)) {}

  [[nodiscard]] std::expected<Program, TextError> run() {
    Program p;
    bool seenClass = false;
    while (!atEnd()) {
      const Tok& t = peek();
      if (t.kind != Tk::Word) {
        return std::unexpected(errAt(t, "expected '.class' or '.method', "
                                         "found '" +
                                             tokDisplay(t) + "'"));
      }
      if (t.text == ".class") {
        if (seenClass) {
          return std::unexpected(errAt(t, "duplicate .class directive"));
        }
        if (!p.methods.empty()) {
          return std::unexpected(errAt(t, ".class must precede all methods"));
        }
        seenClass = true;
        advance();
        const Tok& name = peek();
        if (name.kind != Tk::Word || name.text.starts_with('.')) {
          return std::unexpected(errAt(name, "expected class internal name, "
                                             "found '" +
                                                 tokDisplay(name) + "'"));
        }
        p.className = std::string(name.text);
        advance();
      } else if (t.text == ".method") {
        auto m = parseMethod();
        if (!m) {
          return std::unexpected(m.error());
        }
        p.methods.push_back(std::move(*m));
      } else {
        return std::unexpected(errAt(t, "expected '.class' or '.method', "
                                         "found '" +
                                             tokDisplay(t) + "'"));
      }
    }
    return p;
  }

private:
  // ---- token access ------------------------------------------------------
  [[nodiscard]] const Tok& peek(std::size_t k = 0) const {
    const std::size_t i = pos_ + k;
    return i < toks_.size() ? toks_[i] : toks_.back();
  }
  [[nodiscard]] bool atEnd() const { return peek().kind == Tk::End; }
  void advance() {
    if (pos_ + 1 < toks_.size()) {
      ++pos_;
    }
  }
  [[nodiscard]] TextError errEof(std::string msg) const {
    std::uint32_t off = static_cast<std::uint32_t>(src_.size());
    if (src_.size() > 0xFFFFFFFFull) {
      off = 0xFFFFFFFFu;
    }
    return TextError{off, std::move(msg)};
  }

  // ---- method-scoped fixup bookkeeping -----------------------------------
  //
  // WHY deferred resolution: labels, constant ids and switch targets are
  // all legal forward references (a branch to a label defined later, a
  // tableswitch reading a .const that appears later in the body). The body
  // is therefore parsed into a Method plus these fixup lists, and a single
  // resolution pass at ".end" patches everything, collecting the earliest
  // (smallest-offset) error.
  struct LabelDef {
    std::string_view name;
    std::uint32_t offset;
    std::uint32_t pc;
    bool pastEnd;
  };
  struct Pending {
    std::string_view name;
    std::uint32_t offset;
  };
  struct BranchFix {
    std::uint32_t insn;
    std::uint32_t offset;
    std::string_view name;
  };
  struct SwitchFix {
    std::uint32_t constIdx;
    std::uint32_t pos; // index into Const::ints to patch
    std::uint32_t offset;
    std::string_view name;
  };
  struct InsnCpFix {
    std::uint32_t insn;
    std::uint32_t offset;
    std::uint64_t cid;
  };
  struct CatchLabelFix {
    std::uint32_t handler;
    int which; // 0 = from, 1 = to, 2 = handler
    std::uint32_t offset;
    std::string_view name;
  };
  struct CatchTypeFix {
    std::uint32_t handler;
    std::uint32_t offset;
    std::uint64_t cid;
  };
  struct ConstId {
    std::uint64_t cid;
    std::uint32_t index;
  };
  struct SwitchEntry {
    std::int32_t match;
    std::uint32_t matchOffset;
    std::string_view label;
    std::uint32_t labelOffset;
  };

  // ---- operand parsers ---------------------------------------------------
  [[nodiscard]] std::expected<std::uint32_t, TextError> parseReg() {
    const Tok& t = peek();
    std::uint64_t v = 0;
    if (t.kind != Tk::Word || !parsePrefixed(t.text, 'r', v)) {
      return std::unexpected(errAt(
          t, "expected register 'r<N>', found '" + tokDisplay(t) + "'"));
    }
    if (v > 0xFFFFu) {
      return std::unexpected(errAt(t,
                                   "register number too large (max 65535)"));
    }
    advance();
    return static_cast<std::uint32_t>(v);
  }

  [[nodiscard]] std::expected<std::uint32_t, TextError> parseSlot() {
    const Tok& t = peek();
    std::uint64_t v = 0;
    if (t.kind != Tk::Word || !parsePrefixed(t.text, 'l', v)) {
      return std::unexpected(errAt(
          t, "expected local slot 'l<N>', found '" + tokDisplay(t) + "'"));
    }
    if (v > 0xFFFFFFFFull) {
      return std::unexpected(errAt(t, "local slot number too large"));
    }
    advance();
    return static_cast<std::uint32_t>(v);
  }

  // Constant references are recorded, not resolved (forward refs allowed).
  [[nodiscard]] std::expected<std::uint64_t, TextError> parseCid() {
    const Tok& t = peek();
    std::uint64_t v = 0;
    if (t.kind != Tk::Word || !parsePrefixed(t.text, 'c', v)) {
      return std::unexpected(errAt(t, "expected constant reference 'c<N>', "
                                      "found '" +
                                          tokDisplay(t) + "'"));
    }
    advance();
    return v;
  }

  [[nodiscard]] std::expected<std::string_view, TextError>
  parseLabelIdent() {
    const Tok& t = peek();
    if (t.kind != Tk::Word || !isIdentShape(t.text)) {
      return std::unexpected(errAt(
          t, "expected label identifier, found '" + tokDisplay(t) + "'"));
    }
    advance();
    return t.text;
  }

  // Bare integers: any Sig field that is an immediate rather than a
  // register/slot/constant. The accepted range is the union of signed and
  // unsigned 32-bit so both spellings survive; the bits are stored verbatim
  // in the target field and the verifier judges the semantics.
  [[nodiscard]] std::expected<std::uint32_t, TextError> parseBareInt() {
    const Tok& t = peek();
    if (t.kind != Tk::Int) {
      return std::unexpected(
          errAt(t, "expected integer, found '" + tokDisplay(t) + "'"));
    }
    advance();
    std::int64_t v = 0;
    const char* begin = t.text.data();
    const char* end = begin + t.text.size();
    const auto res = std::from_chars(begin, end, v);
    if (res.ec == std::errc::result_out_of_range ||
        (res.ec == std::errc{} && res.ptr == end &&
         (v < -2147483648ll || v > 4294967295ll))) {
      return std::unexpected(errAt(
          t, "immediate out of 32-bit range: '" + tokDisplay(t) + "'"));
    }
    if (res.ec != std::errc{} || res.ptr != end) {
      return std::unexpected(
          errAt(t, "invalid integer '" + tokDisplay(t) + "'"));
    }
    return static_cast<std::uint32_t>(v);
  }

  [[nodiscard]] std::expected<std::int64_t, TextError> parseIntToken(
      const Tok& t) const {
    std::int64_t v = 0;
    const char* begin = t.text.data();
    const char* end = begin + t.text.size();
    const auto res = std::from_chars(begin, end, v);
    if (res.ec != std::errc{} || res.ptr != end) {
      return std::unexpected(errAt(t, "invalid or out-of-range integer '" +
                                          tokDisplay(t) + "'"));
    }
    return v;
  }

  [[nodiscard]] std::expected<void, TextError> expectPunct(
      char p, std::string_view ctx) {
    const Tok& t = peek();
    if (t.kind != Tk::Punct || t.text.size() != 1 || t.text[0] != p) {
      return std::unexpected(errAt(t, "expected '" + std::string(1, p) +
                                          "' " + std::string(ctx) +
                                          ", found '" + tokDisplay(t) + "'"));
    }
    advance();
    return {};
  }

  [[nodiscard]] std::expected<void, TextError> expectWord(
      std::string_view w) {
    const Tok& t = peek();
    if (t.kind != Tk::Word || t.text != w) {
      return std::unexpected(errAt(t, "expected '" + std::string(w) +
                                          "', found '" + tokDisplay(t) +
                                          "'"));
    }
    advance();
    return {};
  }

  [[nodiscard]] std::expected<std::string, TextError> parseStringPayload() {
    const Tok& t = peek();
    if (t.kind != Tk::Str) {
      return std::unexpected(errAt(
          t, "expected string literal, found '" + tokDisplay(t) + "'"));
    }
    const std::string_view raw = t.text;
    if (raw.size() < 2 || raw.back() != '"') {
      return std::unexpected(errAt(t, "unterminated string literal"));
    }
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 1; i + 1 < raw.size();) {
      const char c = raw[i];
      if (c == '\\') {
        if (i + 1 >= raw.size() - 1) {
          return std::unexpected(
              errAt(t, "unterminated escape in string literal"));
        }
        switch (raw[i + 1]) {
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case 'n':
          out += '\n';
          break;
        case 't':
          out += '\t';
          break;
        case 'r':
          out += '\r';
          break;
        default:
          return std::unexpected(errAt(
              t, std::string("unknown escape '\\") + raw[i + 1] +
                     "' in string literal"));
        }
        i += 2;
      } else {
        out += c;
        ++i;
      }
    }
    advance();
    return out;
  }

  // Float/double payloads: Int and Float tokens go through from_chars
  // (which accepts 5, 5., .5, 1e+09 and the bare words inf/nan); a leading
  // '-' punct is folded in so "-inf"/"-nan" work (the tokenizer only makes
  // '-' part of a number when digits follow).
  template <typename F>
  [[nodiscard]] std::expected<F, TextError> parseFloatPayload() {
    const Tok& t = peek();
    bool negate = false;
    std::string_view text = t.text;
    if (t.kind == Tk::Punct && t.text == "-") {
      negate = true;
      advance();
      const Tok& w = peek();
      if (w.kind != Tk::Word || (w.text != "inf" && w.text != "nan")) {
        return std::unexpected(errAt(w, "expected floating-point constant, "
                                        "found '" +
                                            tokDisplay(w) + "'"));
      }
      text = w.text;
    } else if (t.kind != Tk::Int && t.kind != Tk::Float &&
               !(t.kind == Tk::Word && (t.text == "inf" ||
                                        t.text == "nan"))) {
      return std::unexpected(errAt(t, "expected floating-point constant, "
                                      "found '" +
                                          tokDisplay(t) + "'"));
    }
    advance();
    F v = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto res = std::from_chars(begin, end, v);
    if (res.ec == std::errc::result_out_of_range) {
      return std::unexpected(errAt(t, "floating-point constant out of "
                                      "range: '" +
                                          std::string(text) + "'"));
    }
    if (res.ec != std::errc{} || res.ptr != end) {
      return std::unexpected(errAt(t, "invalid floating-point constant '" +
                                          std::string(text) + "'"));
    }
    if (negate) {
      v = std::copysign(v, static_cast<F>(-1.0));
    }
    return v;
  }

  // ---- .const ------------------------------------------------------------
  [[nodiscard]] std::expected<void, TextError> parseConst(Method& m) {
    const Tok& cidTok = peek();
    std::uint64_t cid = 0;
    if (cidTok.kind != Tk::Word || !parsePrefixed(cidTok.text, 'c', cid)) {
      return std::unexpected(errAt(cidTok, "expected constant id 'c<N>', "
                                           "found '" +
                                               tokDisplay(cidTok) + "'"));
    }
    advance();
    if (auto e = expectPunct('=', "after constant id"); !e) {
      return std::unexpected(e.error());
    }
    const Tok& kindTok = peek();
    if (kindTok.kind != Tk::Word) {
      return std::unexpected(errAt(kindTok, "expected const kind, found '" +
                                                tokDisplay(kindTok) + "'"));
    }
    advance();

    if (findConst(cid)) {
      return std::unexpected(errAt(cidTok, "redefinition of constant 'c" +
                                               std::to_string(cid) + "'"));
    }

    Const c;
    const std::string_view k = kindTok.text;
    if (k == "int") {
      const Tok& t = peek();
      if (t.kind != Tk::Int) {
        return std::unexpected(errAt(
            t, "expected 32-bit integer, found '" + tokDisplay(t) + "'"));
      }
      advance();
      auto v = parseIntToken(t);
      if (!v) {
        return std::unexpected(v.error());
      }
      if (*v < -2147483648ll || *v > 2147483647ll) {
        return std::unexpected(errAt(t, "int constant out of 32-bit range"));
      }
      c.kind = Const::Kind::Int32;
      c.i32 = static_cast<std::int32_t>(*v);
    } else if (k == "long") {
      const Tok& t = peek();
      if (t.kind != Tk::Int) {
        return std::unexpected(errAt(
            t, "expected 64-bit integer, found '" + tokDisplay(t) + "'"));
      }
      advance();
      auto v = parseIntToken(t);
      if (!v) {
        return std::unexpected(v.error());
      }
      c.kind = Const::Kind::Int64;
      c.i64 = *v;
    } else if (k == "float") {
      auto v = parseFloatPayload<float>();
      if (!v) {
        return std::unexpected(v.error());
      }
      c.kind = Const::Kind::Float;
      c.f32 = *v;
    } else if (k == "double") {
      auto v = parseFloatPayload<double>();
      if (!v) {
        return std::unexpected(v.error());
      }
      c.kind = Const::Kind::Double;
      c.f64 = *v;
    } else if (k == "utf8" || k == "string" || k == "class") {
      auto s = parseStringPayload();
      if (!s) {
        return std::unexpected(s.error());
      }
      c.kind = k == "utf8"    ? Const::Kind::Utf8
               : k == "string" ? Const::Kind::String
                               : Const::Kind::Class;
      c.str = std::move(*s);
    } else if (k == "nametype" || k == "methodhandle" || k == "indy") {
      // nametype <name> <desc> / methodhandle <kind> <ref> / indy <name>
      // <desc>: two word payloads.
      const Tok& a = peek();
      if (a.kind != Tk::Word || a.text.starts_with('.')) {
        return std::unexpected(errAt(a, "expected word operand, found '" +
                                            tokDisplay(a) + "'"));
      }
      advance();
      const Tok& b = peek();
      if (b.kind != Tk::Word || b.text.starts_with('.')) {
        return std::unexpected(errAt(b, "expected word operand, found '" +
                                            tokDisplay(b) + "'"));
      }
      advance();
      c.kind = k == "nametype"      ? Const::Kind::NameType
               : k == "methodhandle" ? Const::Kind::MethodHandle
                                     : Const::Kind::InvokeDynamic;
      c.str = std::string(a.text);
      c.str2 = std::string(b.text);
    } else if (k == "methodtype") {
      const Tok& t = peek();
      if (t.kind != Tk::Word || t.text.starts_with('.')) {
        return std::unexpected(errAt(t, "expected descriptor, found '" +
                                            tokDisplay(t) + "'"));
      }
      advance();
      c.kind = Const::Kind::MethodType;
      c.str = std::string(t.text);
    } else if (k == "field" || k == "method" || k == "imethod") {
      // field/method/imethod <class> <name> <desc>: three word payloads.
      Tok words[3] = {};
      for (Tok& w : words) {
        const Tok& t = peek();
        if (t.kind != Tk::Word || t.text.starts_with('.')) {
          return std::unexpected(errAt(t, "expected word operand, found '" +
                                              tokDisplay(t) + "'"));
        }
        w = t;
        advance();
      }
      c.kind = k == "field"    ? Const::Kind::FieldRef
               : k == "method" ? Const::Kind::MethodRef
                               : Const::Kind::InterfaceMethodRef;
      c.str = std::string(words[0].text);
      c.str2 = std::string(words[1].text);
      c.str3 = std::string(words[2].text);
    } else if (k == "switch") {
      auto sc = parseSwitchPayload(m, kindTok);
      if (!sc) {
        return std::unexpected(sc.error());
      }
      c = std::move(*sc);
    } else {
      return std::unexpected(
          errAt(kindTok, "unknown const kind '" + std::string(k) + "'"));
    }

    constIds_.push_back(
        ConstId{cid, static_cast<std::uint32_t>(m.cp.size())});
    m.cp.push_back(std::move(c));
    return {};
  }

  // Switch payload: '{' { int ':' label | "default" ':' label } '}'.
  //
  // WHY canonicalization: the human-facing form is an unordered match/label
  // map plus a mandatory default; the machine form (Const::ints) is either
  // a dense table [low, high, default, targets...] or a sorted lookup
  // [N, default, match/target pairs]. Canonicalizing here makes printed
  // output independent of how the cases were written, so golden diffs
  // ignore case ordering. The const's future pool index is m.cp.size()
  // because parseConst appends it immediately afterwards.
  [[nodiscard]] std::expected<Const, TextError> parseSwitchPayload(
      const Method& m, const Tok& kindTok) {
    if (auto e = expectPunct('{', "to open switch payload"); !e) {
      return std::unexpected(e.error());
    }
    bool haveDefault = false;
    std::uint32_t defaultOffset = 0;
    std::string_view defaultLabel{};
    std::vector<SwitchEntry> entries;
    while (true) {
      const Tok& k = peek();
      if (k.kind == Tk::End) {
        return std::unexpected(
            errEof("unexpected end of input inside switch payload"));
      }
      if (k.kind == Tk::Punct && k.text == "}") {
        advance();
        break;
      }
      if (k.kind == Tk::Word && k.text == "default") {
        advance();
        if (auto e = expectPunct(':', "after 'default'"); !e) {
          return std::unexpected(e.error());
        }
        auto lab = parseLabelIdent();
        if (!lab) {
          return std::unexpected(lab.error());
        }
        if (haveDefault) {
          return std::unexpected(
              errAt(k, "duplicate 'default' entry in switch constant"));
        }
        haveDefault = true;
        defaultOffset = k.offset;
        defaultLabel = *lab;
      } else if (k.kind == Tk::Int) {
        auto v = parseIntToken(k);
        if (!v) {
          return std::unexpected(v.error());
        }
        if (*v < -2147483648ll || *v > 2147483647ll) {
          return std::unexpected(
              errAt(k, "switch match value out of 32-bit range"));
        }
        advance();
        if (auto e = expectPunct(':', "after switch match value"); !e) {
          return std::unexpected(e.error());
        }
        const Tok& labTok = peek();
        auto lab = parseLabelIdent();
        if (!lab) {
          return std::unexpected(lab.error());
        }
        entries.push_back(SwitchEntry{static_cast<std::int32_t>(*v), k.offset,
                                      *lab, labTok.offset});
      } else {
        return std::unexpected(errAt(
            k, "expected match value or 'default' in switch payload, found "
               "'" +
                   tokDisplay(k) + "'"));
      }
    }
    if (!haveDefault) {
      return std::unexpected(
          errAt(kindTok, "switch constant requires a 'default' entry"));
    }

    std::sort(entries.begin(), entries.end(),
              [](const SwitchEntry& a, const SwitchEntry& b) {
                return a.match < b.match;
              });
    for (std::size_t i = 1; i < entries.size(); ++i) {
      if (entries[i].match == entries[i - 1].match) {
        return std::unexpected(TextError{
            entries[i].matchOffset,
            "duplicate switch match value " +
                std::to_string(entries[i].match)});
      }
    }
    bool dense = !entries.empty();
    for (std::size_t i = 1; i < entries.size(); ++i) {
      if (entries[i].match != entries[i - 1].match + 1) {
        dense = false;
      }
    }

    Const c;
    c.kind = Const::Kind::SwitchTable;
    const auto constIdx = static_cast<std::uint32_t>(m.cp.size());
    if (dense) {
      // table: [low, high, default, target(low..high)]
      c.ints.push_back(entries.front().match);
      c.ints.push_back(entries.back().match);
      c.ints.push_back(0);
      for (std::size_t d = 0; d < entries.size(); ++d) {
        c.ints.push_back(0);
      }
      switchFixes_.push_back(SwitchFix{constIdx, 2, defaultOffset,
                                       defaultLabel});
      for (std::size_t d = 0; d < entries.size(); ++d) {
        switchFixes_.push_back(SwitchFix{
            constIdx, static_cast<std::uint32_t>(3 + d),
            entries[d].labelOffset, entries[d].label});
      }
    } else {
      // lookup: [N, default, match1, target1, ..., matchN, targetN]
      c.ints.push_back(static_cast<std::int32_t>(entries.size()));
      c.ints.push_back(0);
      for (const SwitchEntry& e : entries) {
        c.ints.push_back(e.match);
        c.ints.push_back(0);
      }
      switchFixes_.push_back(
          SwitchFix{constIdx, 1, defaultOffset, defaultLabel});
      for (std::size_t d = 0; d < entries.size(); ++d) {
        switchFixes_.push_back(SwitchFix{
            constIdx, static_cast<std::uint32_t>(3 + 2 * d),
            entries[d].labelOffset, entries[d].label});
      }
    }
    return c;
  }

  // ---- .catch ------------------------------------------------------------
  [[nodiscard]] std::expected<void, TextError> parseCatch(Method& m) {
    const auto hidx = static_cast<std::uint32_t>(m.handlers.size());
    m.handlers.push_back(ExceptionHandler{});
    const Tok& ct = peek();
    if (ct.kind == Tk::Word && ct.text == "all") {
      advance(); // catchType stays -1
    } else {
      std::uint64_t cid = 0;
      if (ct.kind != Tk::Word || !parsePrefixed(ct.text, 'c', cid)) {
        return std::unexpected(
            errAt(ct, "expected 'all' or constant id 'c<N>' after .catch, "
                      "found '" +
                          tokDisplay(ct) + "'"));
      }
      advance();
      catchTypeFixes_.push_back(CatchTypeFix{hidx, ct.offset, cid});
    }
    static constexpr int kWhich[3] = {0, 1, 2};
    const std::string_view keywords[3] = {"from", "to", "handler"};
    for (int i = 0; i < 3; ++i) {
      if (auto e = expectWord(keywords[static_cast<std::size_t>(i)]); !e) {
        return std::unexpected(e.error());
      }
      const Tok& t = peek();
      auto lab = parseLabelIdent();
      if (!lab) {
        return std::unexpected(lab.error());
      }
      catchLabelFixes_.push_back(
          CatchLabelFix{hidx, kWhich[static_cast<std::size_t>(i)], t.offset,
                        *lab});
    }
    return {};
  }

  // ---- instructions ------------------------------------------------------
  [[nodiscard]] std::expected<void, TextError> parseInsn(Method& m) {
    const Tok& mn = peek();
    const std::optional<Op> op = lookupOp(mn.text);
    if (!op) {
      return std::unexpected(
          errAt(mn, "unknown mnemonic '" + std::string(mn.text) + "'"));
    }
    if ((m.flags & (method_flags::Abstract | method_flags::Native)) != 0) {
      return std::unexpected(errAt(
          mn, "abstract or native method cannot contain instructions"));
    }
    advance();
    const Sig sig = info(*op).sig;

    // Bind pending labels to this instruction's index. WHY here and not at
    // label-parse time: a label binds to the NEXT instruction, which is
    // only known once the instruction actually starts.
    for (const Pending& p : pending_) {
      labels_.push_back(LabelDef{
          p.name, p.offset, static_cast<std::uint32_t>(m.code.size()),
          false});
    }
    pending_.clear();

    Ins ins(*op, 0, 0, 0, 0);
    const auto insnIdx = static_cast<std::uint32_t>(m.code.size());

    const auto reg = [&](std::uint16_t& field)
        -> std::expected<void, TextError> {
      auto r = parseReg();
      if (!r) {
        return std::unexpected(r.error());
      }
      field = static_cast<std::uint16_t>(*r);
      return {};
    };
    const auto slot = [&](std::uint32_t& field)
        -> std::expected<void, TextError> {
      auto r = parseSlot();
      if (!r) {
        return std::unexpected(r.error());
      }
      field = *r;
      return {};
    };
    const auto bare = [&](std::uint32_t& field)
        -> std::expected<void, TextError> {
      auto r = parseBareInt();
      if (!r) {
        return std::unexpected(r.error());
      }
      field = *r;
      return {};
    };
    // GuardCp's deopt id lives in Ins::b, which is a 16-bit field.
    const auto bare16 = [&](std::uint16_t& field)
        -> std::expected<void, TextError> {
      const Tok& t = peek();
      auto r = parseBareInt();
      if (!r) {
        return std::unexpected(r.error());
      }
      if (*r > 0xFFFFu) {
        return std::unexpected(errAt(t, "value too large for a 16-bit deopt "
                                        "id: '" +
                                            std::to_string(*r) + "'"));
      }
      field = static_cast<std::uint16_t>(*r);
      return {};
    };
    const auto cid = [&]() -> std::expected<void, TextError> {
      const Tok& t = peek();
      auto r = parseCid();
      if (!r) {
        return std::unexpected(r.error());
      }
      insnCpFixes_.push_back(InsnCpFix{insnIdx, t.offset, *r});
      return {};
    };
    const auto label = [&]() -> std::expected<void, TextError> {
      const Tok& t = peek();
      auto r = parseLabelIdent();
      if (!r) {
        return std::unexpected(r.error());
      }
      branchFixes_.push_back(BranchFix{insnIdx, t.offset, *r});
      return {};
    };

    std::expected<void, TextError> e = std::expected<void, TextError>{};
    switch (sig) {
    case Sig::None:
      break;
    case Sig::Reg:
      e = reg(ins.a);
      break;
    case Sig::RegImm: // iconst/fconst/iinc: imm is int32 semantics
      e = reg(ins.dst);
      if (e) {
        e = bare(ins.imm);
      }
      break;
    case Sig::RegCp:
      e = reg(ins.dst);
      if (e) {
        e = cid();
      }
      break;
    case Sig::RegSlot:
      e = reg(ins.dst);
      if (e) {
        e = slot(ins.imm);
      }
      break;
    case Sig::SlotReg:
      e = reg(ins.a);
      if (e) {
        e = slot(ins.imm);
      }
      break;
    case Sig::RegReg:
      e = reg(ins.dst);
      if (e) {
        e = reg(ins.a);
      }
      break;
    case Sig::RegRegReg:
      e = reg(ins.dst);
      if (e) {
        e = reg(ins.a);
      }
      if (e) {
        e = reg(ins.b);
      }
      break;
    case Sig::RegRegImm: // newarray atype, quick field offsets
      e = reg(ins.dst);
      if (e) {
        e = reg(ins.a);
      }
      if (e) {
        e = bare(ins.imm);
      }
      break;
    case Sig::RegRegCp:
      e = reg(ins.dst);
      if (e) {
        e = reg(ins.a);
      }
      if (e) {
        e = cid();
      }
      break;
    case Sig::RegRegRegCp:
      e = reg(ins.dst);
      if (e) {
        e = reg(ins.a);
      }
      if (e) {
        e = reg(ins.b);
      }
      if (e) {
        e = cid();
      }
      break;
    case Sig::Branch:
      e = label();
      break;
    case Sig::RegBranch:
      e = reg(ins.a);
      if (e) {
        e = label();
      }
      break;
    case Sig::RegRegBranch:
      e = reg(ins.a);
      if (e) {
        e = reg(ins.b);
      }
      if (e) {
        e = label();
      }
      break;
    case Sig::RegCpBranch:
      e = reg(ins.a);
      if (e) {
        e = cid();
      }
      break;
    case Sig::Call: // (dst, a=argBase, b=argCount, imm=cp)
      e = reg(ins.dst);
      if (e) {
        e = reg(ins.a);
      }
      if (e) {
        e = reg(ins.b);
      }
      if (e) {
        e = cid();
      }
      break;
    case Sig::CallQuick: // (dst, a, b, imm=resolved id)
      e = reg(ins.dst);
      if (e) {
        e = reg(ins.a);
      }
      if (e) {
        e = reg(ins.b);
      }
      if (e) {
        e = bare(ins.imm);
      }
      break;
    case Sig::Guard: // (a, imm=DeoptId)
      e = reg(ins.a);
      if (e) {
        e = bare(ins.imm);
      }
      break;
    case Sig::GuardCp: // (a, b=DeoptId, imm=cp)
      e = reg(ins.a);
      if (e) {
        e = bare16(ins.b);
      }
      if (e) {
        e = cid();
      }
      break;
    case Sig::Trap: // (imm=DeoptId)
      e = bare(ins.imm);
      break;
    }
    if (!e) {
      return std::unexpected(e.error());
    }
    m.code.push_back(ins);
    return {};
  }

  // ---- method ------------------------------------------------------------
  [[nodiscard]] std::expected<Method, TextError> parseMethod() {
    resetMethodState();
    Method m;
    advance(); // ".method"

    // Method header: [flags...] name descriptor.
    //
    // WHY scan-to-descriptor instead of greedy flags: a typo'd flag
    // ("statik") is indistinguishable from a method name while walking
    // left-to-right. JVM method descriptors always start with '(', so the
    // first '('-led word identifies the descriptor, the word before it the
    // name, and everything between ".method" and the name must then be a
    // known flag - which makes "unknown flag = error" (task contract)
    // actually diagnosable. The scan stops at the first non-word or
    // end-of-input, which is where a missing descriptor is reported.
    std::size_t d = 0; // relative offset from pos_ to the descriptor
    while (true) {
      const Tok& t = peek(d);
      if (t.kind != Tk::Word || t.text.starts_with('(')) {
        break;
      }
      ++d;
    }
    const Tok& desc = peek(d);
    if (desc.kind != Tk::Word || !desc.text.starts_with('(')) {
      return std::unexpected(errAt(desc, "expected method descriptor "
                                         "(must start with '('), found '" +
                                             tokDisplay(desc) + "'"));
    }
    if (d == 0) {
      return std::unexpected(errAt(desc, "expected method name before "
                                         "descriptor, found '" +
                                             tokDisplay(desc) + "'"));
    }
    const Tok& name = peek(d - 1);
    m.name = std::string(name.text);
    m.descriptor = std::string(desc.text);

    bool seen[9] = {};
    for (std::size_t fpos = 0; fpos + 1 < d; ++fpos) {
      const Tok& t = peek(fpos);
      const FlagWord* f = nullptr;
      for (const FlagWord& cand : kFlags) {
        if (cand.word == t.text) {
          f = &cand;
          break;
        }
      }
      if (f == nullptr) {
        return std::unexpected(errAt(
            t, "unknown method flag '" + std::string(t.text) + "'"));
      }
      const std::size_t idx = static_cast<std::size_t>(f - kFlags);
      if (seen[idx]) {
        return std::unexpected(
            errAt(t, "duplicate method flag '" + std::string(t.text) + "'"));
      }
      seen[idx] = true;
      m.flags |= f->bit;
    }
    for (std::size_t k = 0; k <= d; ++k) {
      advance();
    }

    bool seenRegs = false;
    bool seenLocals = false;
    std::uint32_t firstInsnOff = 0;
    std::uint32_t endOff = 0;

    while (true) {
      const Tok& t = peek();
      if (t.kind == Tk::End) {
        return std::unexpected(errEof(
            "unexpected end of input inside method (missing .end)"));
      }
      if (t.kind != Tk::Word) {
        return std::unexpected(errAt(t, "expected directive, label, or "
                                         "instruction, found '" +
                                             tokDisplay(t) + "'"));
      }
      if (t.text == ".end") {
        endOff = t.offset;
        advance();
        break;
      }
      if (t.text == ".regs" || t.text == ".locals") {
        const bool isRegs = t.text == ".regs";
        if (isRegs ? seenRegs : seenLocals) {
          return std::unexpected(errAt(t, "duplicate " +
                                              std::string(t.text) +
                                              " directive"));
        }
        advance();
        const Tok& num = peek();
        if (num.kind != Tk::Int) {
          return std::unexpected(errAt(
              num, "expected non-negative integer after " +
                       std::string(t.text) + ", found '" + tokDisplay(num) +
                       "'"));
        }
        advance();
        auto v = parseIntToken(num);
        if (!v) {
          return std::unexpected(v.error());
        }
        if (*v < 0 || *v > 4294967295ll) {
          return std::unexpected(
              errAt(num, "register/slot count out of range"));
        }
        if (isRegs) {
          seenRegs = true;
          m.numRegs = static_cast<std::uint32_t>(*v);
        } else {
          seenLocals = true;
          m.numLocals = static_cast<std::uint32_t>(*v);
        }
        continue;
      }
      if (t.text == ".const") {
        advance();
        if (auto e = parseConst(m); !e) {
          return std::unexpected(e.error());
        }
        continue;
      }
      if (t.text == ".catch") {
        advance();
        if (auto e = parseCatch(m); !e) {
          return std::unexpected(e.error());
        }
        continue;
      }
      if (t.text.starts_with('.')) {
        return std::unexpected(
            errAt(t, "unknown directive '" + std::string(t.text) + "'"));
      }

      // Label definition: ident ':' with nothing else on the line.
      // WHY the "own line" rule: it visually binds 'L1:' to the
      // instruction that follows and disambiguates from branch operands,
      // which are bare idents without ':'.
      if (peek(1).kind == Tk::Punct && peek(1).text == ":" &&
          !peek(1).lineStart) {
        if (!isIdentShape(t.text)) {
          return std::unexpected(errAt(
              t, "invalid label identifier '" + tokDisplay(t) + "'"));
        }
        if (peek(2).kind != Tk::End && !peek(2).lineStart) {
          return std::unexpected(errAt(
              t, "label '" + std::string(t.text) +
                     "' must be on its own line"));
        }
        if (labelDefined(t.text)) {
          return std::unexpected(
              errAt(t, "duplicate label '" + std::string(t.text) + "'"));
        }
        pending_.push_back(Pending{t.text, t.offset});
        advance();
        advance();
        continue;
      }

      if (m.code.empty()) {
        firstInsnOff = t.offset;
      }
      if (auto e = parseInsn(m); !e) {
        return std::unexpected(e.error());
      }
    }

    return finishMethod(std::move(m), endOff, firstInsnOff);
  }

  // ---- resolution --------------------------------------------------------
  [[nodiscard]] std::expected<Method, TextError> finishMethod(
      Method&& m, std::uint32_t endOff, std::uint32_t firstInsnOff) {
    // Labels still pending at ".end" bind to code.size() ("past the end").
    // WHY allowed at all: exception ranges are half-open [start,end) and a
    // handler covering the method tail needs end == code.size(); the label
    // for that pc can only be written after the last instruction. Every
    // other use of a past-end label is an error (checked below).
    for (const Pending& p : pending_) {
      labels_.push_back(LabelDef{
          p.name, p.offset, static_cast<std::uint32_t>(m.code.size()), true});
    }
    pending_.clear();

    const bool absNat =
        (m.flags & (method_flags::Abstract | method_flags::Native)) != 0;
    if (absNat && !m.code.empty()) {
      return std::unexpected(
          TextError{firstInsnOff,
                    "abstract or native method cannot contain instructions"});
    }
    if (!absNat && m.code.empty()) {
      return std::unexpected(TextError{
          endOff, "method must contain at least one instruction (or be "
                  "declared abstract/native)"});
    }

    // WHY one deferred pass: labels and constants may be referenced before
    // they are defined (forward branches, .const after .catch, ...). All
    // fixups are applied now and every failure is collected; the error
    // with the smallest byte offset wins so the diagnostic points at the
    // earliest offending token, matching "first error wins".
    std::vector<TextError> errs;

    for (const InsnCpFix& f : insnCpFixes_) {
      const std::optional<std::uint32_t> idx = findConst(f.cid);
      if (!idx) {
        errs.push_back(TextError{
            f.offset, "undefined constant 'c" + std::to_string(f.cid) + "'"});
        continue;
      }
      m.code[f.insn].imm = *idx;
      const Op op = m.code[f.insn].opcode();
      if (op == Op::Tableswitch || op == Op::Lookupswitch) {
        const Const& c = m.cp[*idx];
        if (c.kind != Const::Kind::SwitchTable) {
          errs.push_back(TextError{
              f.offset,
              op == Op::Tableswitch
                  ? "tableswitch operand must reference a switch constant"
                  : "lookupswitch operand must reference a switch constant"});
        } else if (op == Op::Tableswitch && !isTableLayout(c)) {
          errs.push_back(TextError{
              f.offset,
              "tableswitch matches must be contiguous ascending"});
        } else if (op == Op::Lookupswitch && !isLookupLayout(c)) {
          errs.push_back(TextError{
              f.offset, "lookupswitch matches must not be contiguous (use "
                        "tableswitch)"});
        }
      }
    }

    for (const BranchFix& f : branchFixes_) {
      const LabelDef* d = findLabel(f.name);
      if (d == nullptr) {
        errs.push_back(TextError{f.offset,
                                 "undefined label '" +
                                     std::string(f.name) + "'"});
        continue;
      }
      if (d->pastEnd) {
        errs.push_back(TextError{
            d->offset, "label '" + std::string(f.name) +
                           "' points past the end of the method"});
        continue;
      }
      m.code[f.insn].imm = d->pc;
    }

    for (const SwitchFix& f : switchFixes_) {
      const LabelDef* d = findLabel(f.name);
      if (d == nullptr) {
        errs.push_back(TextError{f.offset,
                                 "undefined label '" +
                                     std::string(f.name) + "'"});
        continue;
      }
      if (d->pastEnd) {
        errs.push_back(TextError{
            d->offset, "label '" + std::string(f.name) +
                           "' points past the end of the method"});
        continue;
      }
      m.cp[f.constIdx].ints[f.pos] = static_cast<std::int32_t>(d->pc);
    }

    for (const CatchLabelFix& f : catchLabelFixes_) {
      const LabelDef* d = findLabel(f.name);
      if (d == nullptr) {
        errs.push_back(TextError{f.offset,
                                 "undefined label '" +
                                     std::string(f.name) + "'"});
        continue;
      }
      if (d->pastEnd && f.which != 1) {
        errs.push_back(TextError{
            d->offset, "label '" + std::string(f.name) +
                           "' points past the end of the method"});
        continue;
      }
      const std::uint32_t pc =
          d->pastEnd ? static_cast<std::uint32_t>(m.code.size()) : d->pc;
      ExceptionHandler& h = m.handlers[f.handler];
      switch (f.which) {
      case 0:
        h.start = pc;
        break;
      case 1:
        h.end = pc;
        break;
      default:
        h.handler = pc;
        break;
      }
    }

    for (const CatchTypeFix& f : catchTypeFixes_) {
      const std::optional<std::uint32_t> idx = findConst(f.cid);
      if (!idx) {
        errs.push_back(TextError{
            f.offset, "undefined constant 'c" + std::to_string(f.cid) + "'"});
        continue;
      }
      if (m.cp[*idx].kind != Const::Kind::Class) {
        errs.push_back(TextError{
            f.offset, ".catch type must reference a class constant ('c" +
                          std::to_string(f.cid) + "' is not a class)"});
        continue;
      }
      m.handlers[f.handler].catchType = static_cast<std::int32_t>(*idx);
    }

    for (const LabelDef& d : labels_) {
      if (!d.pastEnd) {
        continue;
      }
      const bool usedAsCatchTo = std::any_of(
          catchLabelFixes_.begin(), catchLabelFixes_.end(),
          [&d](const CatchLabelFix& f) {
            return f.which == 1 && f.name == d.name;
          });
      if (!usedAsCatchTo) {
        errs.push_back(TextError{
            d.offset, "label '" + std::string(d.name) +
                          "' points past the end of the method"});
      }
    }

    if (!errs.empty()) {
      const TextError* best = &errs.front();
      for (std::size_t i = 1; i < errs.size(); ++i) {
        if (errs[i].offset < best->offset) {
          best = &errs[i];
        }
      }
      return std::unexpected(*best);
    }
    return std::move(m);
  }

  // ---- state helpers -----------------------------------------------------
  void resetMethodState() {
    labels_.clear();
    pending_.clear();
    branchFixes_.clear();
    switchFixes_.clear();
    insnCpFixes_.clear();
    catchLabelFixes_.clear();
    catchTypeFixes_.clear();
    constIds_.clear();
  }

  [[nodiscard]] const LabelDef* findLabel(std::string_view name) const {
    for (const LabelDef& d : labels_) {
      if (d.name == name) {
        return &d;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool labelDefined(std::string_view name) const {
    return findLabel(name) != nullptr ||
           std::any_of(pending_.begin(), pending_.end(),
                       [name](const Pending& p) { return p.name == name; });
  }

  [[nodiscard]] std::optional<std::uint32_t> findConst(
      std::uint64_t cid) const {
    for (const ConstId& c : constIds_) {
      if (c.cid == cid) {
        return c.index;
      }
    }
    return std::nullopt;
  }

  std::string_view src_;
  std::vector<Tok> toks_;
  std::size_t pos_ = 0;

  std::vector<LabelDef> labels_;
  std::vector<Pending> pending_;
  std::vector<BranchFix> branchFixes_;
  std::vector<SwitchFix> switchFixes_;
  std::vector<InsnCpFix> insnCpFixes_;
  std::vector<CatchLabelFix> catchLabelFixes_;
  std::vector<CatchTypeFix> catchTypeFixes_;
  std::vector<ConstId> constIds_;
};

// ===========================================================================
// Printer
// ===========================================================================

void appendU(std::string& out, std::uint64_t v) { out += std::to_string(v); }

void appendI(std::string& out, std::int64_t v) { out += std::to_string(v); }

// WHY shortest round-trip formatting: std::to_chars without a format
// argument emits the fewest digits that re-parse bit-exactly, so golden
// files stay minimal AND print(parse(print(x))) == print(x) holds for
// every finite value. Non-finite values use the from_chars-compatible
// words inf/-inf/nan/-nan.
template <typename F>
void appendFloat(std::string& out, F v) {
  if (std::isnan(v)) {
    out += std::signbit(v) ? "-nan" : "nan";
    return;
  }
  if (std::isinf(v)) {
    out += std::signbit(v) ? "-inf" : "inf";
    return;
  }
  char buf[64];
  const auto res = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, static_cast<std::size_t>(res.ptr - buf));
}

[[nodiscard]] std::string quoted(std::string_view s) {
  std::string out;
  out += '"';
  for (const char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\r':
      out += "\\r";
      break;
    default:
      out += c;
      break;
    }
  }
  out += '"';
  return out;
}

struct SwData {
  bool table = false;
  std::vector<std::int32_t> matches; // ascending
  std::vector<std::uint32_t> targets;
  std::uint32_t def = 0;
};

// Decode a SwitchTable const back into (matches, targets, default).
//
// WHY opcode-driven: the two layouts are not always shape-distinguishable
// (a canonical lookup [N, def, m1, t1, ...] can satisfy the table shape
// [low, high, def, targets...] and vice versa), so the referencing
// instruction is the only authority. 'form' is 1 for tableswitch, 2 for
// lookupswitch, 0 for unreferenced consts. For unreferenced consts the
// table shape is tried first: a canonical lookup const that happens to fit
// the table shape re-parses to the identical ints vector (the table
// canonicalization of the printed matches reproduces [low, high, def,
// targets...] byte for byte), so the heuristic never breaks
// byte-stability. Consts that fit neither shape are garbage; they print as
// a default-only switch so the output stays tokenizable.
[[nodiscard]] SwData decodeSwitch(const Const& c, int form) {
  SwData d;
  const std::vector<std::int32_t>& v = c.ints;

  const auto tryTable = [&]() {
    if (v.size() < 4) {
      return false;
    }
    const std::int64_t low = v[0];
    const std::int64_t high = v[1];
    if (high < low) {
      return false;
    }
    if (static_cast<std::uint64_t>(high - low) + 1 != v.size() - 3) {
      return false;
    }
    d.table = true;
    d.def = static_cast<std::uint32_t>(v[2]);
    d.matches.clear();
    d.targets.clear();
    for (std::size_t k = 3; k < v.size(); ++k) {
      d.matches.push_back(static_cast<std::int32_t>(
          low + static_cast<std::int64_t>(k - 3)));
      d.targets.push_back(static_cast<std::uint32_t>(v[k]));
    }
    return true;
  };
  const auto tryLookup = [&]() {
    if (v.size() < 2 || (v.size() - 2) % 2 != 0) {
      return false;
    }
    const std::uint64_t n = (v.size() - 2) / 2;
    if (v[0] < 0 || static_cast<std::uint64_t>(v[0]) != n) {
      return false;
    }
    d.table = false;
    d.def = static_cast<std::uint32_t>(v[1]);
    d.matches.clear();
    d.targets.clear();
    for (std::uint64_t k = 0; k < n; ++k) {
      d.matches.push_back(v[static_cast<std::size_t>(2 + 2 * k)]);
      d.targets.push_back(
          static_cast<std::uint32_t>(v[static_cast<std::size_t>(3 + 2 * k)]));
    }
    return true;
  };

  const bool ok =
      (form == 2) ? (tryLookup() || tryTable()) : (tryTable() || tryLookup());
  if (!ok) {
    // GIGO fallback: neither layout fits.
    d.table = false;
    d.def = v.empty() ? 0 : static_cast<std::uint32_t>(v.back());
    d.matches.clear();
    d.targets.clear();
  }
  return d;
}

void printConstInto(std::string& out, const Const& c, const SwData& sw) {
  switch (c.kind) {
  case Const::Kind::Int32:
    out += "int ";
    appendI(out, c.i32);
    return;
  case Const::Kind::Int64:
    out += "long ";
    appendI(out, c.i64);
    return;
  case Const::Kind::Float:
    out += "float ";
    appendFloat(out, c.f32);
    return;
  case Const::Kind::Double:
    out += "double ";
    appendFloat(out, c.f64);
    return;
  case Const::Kind::Utf8:
    out += "utf8 ";
    out += quoted(c.str);
    return;
  case Const::Kind::String:
    out += "string ";
    out += quoted(c.str);
    return;
  case Const::Kind::Class:
    out += "class ";
    out += quoted(c.str);
    return;
  case Const::Kind::NameType:
    out += "nametype ";
    out += c.str;
    out += ' ';
    out += c.str2;
    return;
  case Const::Kind::MethodType:
    out += "methodtype ";
    out += c.str;
    return;
  case Const::Kind::MethodHandle:
    out += "methodhandle ";
    out += c.str;
    out += ' ';
    out += c.str2;
    return;
  case Const::Kind::InvokeDynamic:
    out += "indy ";
    out += c.str;
    out += ' ';
    out += c.str2;
    return;
  case Const::Kind::SwitchTable: {
    out += "switch {";
    for (std::size_t i = 0; i < sw.matches.size(); ++i) {
      out += ' ';
      appendI(out, sw.matches[i]);
      out += " : L";
      appendU(out, sw.targets[i]);
    }
    out += " default : L";
    appendU(out, sw.def);
    out += " }";
    return;
  }
  case Const::Kind::FieldRef:
  case Const::Kind::MethodRef:
  case Const::Kind::InterfaceMethodRef:
    break; // three word payloads, shared tail below
  }
  out += c.kind == Const::Kind::FieldRef    ? "field "
         : c.kind == Const::Kind::MethodRef ? "method "
                                            : "imethod ";
  out += c.str;
  out += ' ';
  out += c.str2;
  out += ' ';
  out += c.str3;
}

void printInsnInto(std::string& out, const Ins& ins) {
  out += opName(ins.opcode()); // safe: falls back to "bad<N>"
  if (ins.op >= opCount()) {
    // WHY the guard: info() is only defined for valid op codes; invalid
    // programs print their raw fields instead of crashing.
    out += " r";
    appendU(out, ins.dst);
    out += " r";
    appendU(out, ins.a);
    out += " r";
    appendU(out, ins.b);
    out += ' ';
    appendU(out, ins.imm);
    return;
  }
  const auto r = [&](std::uint16_t v) {
    out += " r";
    appendU(out, v);
  };
  const auto l = [&](std::uint32_t v) {
    out += " l";
    appendU(out, v);
  };
  const auto cidx = [&](std::uint32_t v) {
    out += " c";
    appendU(out, v);
  };
  const auto lab = [&](std::uint32_t v) {
    out += " L";
    appendU(out, v);
  };
  const auto immS = [&](std::uint32_t v) {
    out += ' ';
    appendI(out, static_cast<std::int32_t>(v));
  };
  const auto immU = [&](std::uint32_t v) {
    out += ' ';
    appendU(out, v);
  };
  switch (info(ins.opcode()).sig) {
  case Sig::None:
    break;
  case Sig::Reg:
    r(ins.a);
    break;
  case Sig::RegImm: // iconst/fconst/iinc immediates are int32 semantics
    r(ins.dst);
    immS(ins.imm);
    break;
  case Sig::RegCp:
    r(ins.dst);
    cidx(ins.imm);
    break;
  case Sig::RegSlot:
    r(ins.dst);
    l(ins.imm);
    break;
  case Sig::SlotReg:
    r(ins.a);
    l(ins.imm);
    break;
  case Sig::RegReg:
    r(ins.dst);
    r(ins.a);
    break;
  case Sig::RegRegReg:
    r(ins.dst);
    r(ins.a);
    r(ins.b);
    break;
  case Sig::RegRegImm: // newarray atype, quick field offsets
    r(ins.dst);
    r(ins.a);
    immU(ins.imm);
    break;
  case Sig::RegRegCp:
    r(ins.dst);
    r(ins.a);
    cidx(ins.imm);
    break;
  case Sig::RegRegRegCp:
    r(ins.dst);
    r(ins.a);
    r(ins.b);
    cidx(ins.imm);
    break;
  case Sig::Branch:
    lab(ins.imm);
    break;
  case Sig::RegBranch:
    r(ins.a);
    lab(ins.imm);
    break;
  case Sig::RegRegBranch:
    r(ins.a);
    r(ins.b);
    lab(ins.imm);
    break;
  case Sig::RegCpBranch:
    r(ins.a);
    cidx(ins.imm);
    break;
  case Sig::Call: // b (argCount) lives in Ins::b, hence r<N> form
    r(ins.dst);
    r(ins.a);
    r(ins.b);
    cidx(ins.imm);
    break;
  case Sig::CallQuick: // resolved id
    r(ins.dst);
    r(ins.a);
    r(ins.b);
    immU(ins.imm);
    break;
  case Sig::Guard: // deopt id
    r(ins.a);
    immU(ins.imm);
    break;
  case Sig::GuardCp: // a, b = deopt id (bare int), imm = cp
    r(ins.a);
    immU(ins.b);
    cidx(ins.imm);
    break;
  case Sig::Trap: // deopt id
    immU(ins.imm);
    break;
  }
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================

std::expected<Program, TextError> parseRbcText(std::string_view text) {
  return Parser(text).run();
}

std::string printRbcText(const Method& m) {
  std::string out;
  out += ".method";
  for (const FlagWord& f : kFlags) {
    if ((m.flags & f.bit) != 0) {
      out += ' ';
      out += f.word;
    }
  }
  out += ' ';
  out += m.name;
  out += ' ';
  out += m.descriptor;
  out += '\n';
  out += ".regs ";
  appendU(out, m.numRegs);
  out += '\n';
  out += ".locals ";
  appendU(out, m.numLocals);
  out += '\n';

  // Switch layout per const: decided by the first switch instruction that
  // references it (tableswitch -> table, lookupswitch -> lookup); 0 for
  // unreferenced consts means "try table, then lookup" (see decodeSwitch).
  std::vector<std::uint8_t> form(m.cp.size(), 0);
  for (const Ins& ins : m.code) {
    if (ins.imm < form.size() && form[ins.imm] == 0) {
      if (ins.opcode() == Op::Tableswitch) {
        form[ins.imm] = 1;
      } else if (ins.opcode() == Op::Lookupswitch) {
        form[ins.imm] = 2;
      }
    }
  }
  std::vector<SwData> sw(m.cp.size());
  for (std::size_t i = 0; i < m.cp.size(); ++i) {
    if (m.cp[i].kind == Const::Kind::SwitchTable) {
      sw[i] = decodeSwitch(m.cp[i], static_cast<int>(form[i]));
    }
  }

  for (std::size_t i = 0; i < m.cp.size(); ++i) {
    out += ".const c";
    appendU(out, i);
    out += " = ";
    printConstInto(out, m.cp[i], sw[i]);
    out += '\n';
  }

  // Labels are emitted only for pcs that are actually referenced: branch
  // targets, switch targets/defaults, and exception handler boundaries.
  // The name is "L<pc>" so label identity is a pure function of position -
  // that is what makes the printer deterministic across runs and
  // independent of any map iteration order (vectors only).
  std::vector<char> isTarget(m.code.size() + 1, 0);
  const auto mark = [&](std::uint32_t target) {
    if (target < isTarget.size()) { // WHY clamp: invalid programs stay
      isTarget[target] = 1;         // printable instead of crashing
    }
  };
  for (const Ins& ins : m.code) {
    if (ins.op >= opCount()) {
      continue;
    }
    switch (info(ins.opcode()).sig) {
    case Sig::Branch:
    case Sig::RegBranch:
    case Sig::RegRegBranch:
      mark(ins.imm);
      break;
    default:
      break;
    }
  }
  for (const SwData& s : sw) {
    mark(s.def);
    for (const std::uint32_t t : s.targets) {
      mark(t);
    }
  }
  for (const ExceptionHandler& h : m.handlers) {
    mark(h.start);
    mark(h.end);
    mark(h.handler);
  }

  for (std::size_t pc = 0; pc < m.code.size(); ++pc) {
    if (isTarget[pc] != 0) {
      out += 'L';
      appendU(out, pc);
      out += ":\n";
    }
    printInsnInto(out, m.code[pc]);
    out += '\n';
  }
  // A handler whose [start,end) range ends at the method tail needs a label
  // at code.size(); it is emitted after the last instruction and is legal
  // only as a .catch "to" target (see the parser's past-end label rule).
  if (isTarget[m.code.size()] != 0) {
    out += 'L';
    appendU(out, m.code.size());
    out += ":\n";
  }

  for (const ExceptionHandler& h : m.handlers) {
    out += ".catch ";
    if (h.catchType < 0) {
      out += "all";
    } else {
      out += 'c';
      appendU(out, static_cast<std::uint32_t>(h.catchType));
    }
    out += " from L";
    appendU(out, h.start);
    out += " to L";
    appendU(out, h.end);
    out += " handler L";
    appendU(out, h.handler);
    out += '\n';
  }

  out += ".end\n";
  return out;
}

std::string printRbcText(const Program& p) {
  std::string out;
  out += ".class ";
  out += p.className;
  out += '\n';
  for (const Method& m : p.methods) {
    out += '\n';
    out += printRbcText(m);
  }
  return out;
}

} // namespace b2::rbc
