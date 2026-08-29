#pragma once
// B-2 Frontend - token kinds and the Token structure (JLS chapter 3).
//
// A token carries a RAW source range (offset/length into the on-disk
// buffer), so diagnostics always point at what the user wrote, even when
// unicode escapes were translated before lexing (JLS 3.3).

#include <cstdint>
#include <string>

#include "b2/frontend/SourceManager.h"

namespace b2::frontend {

enum class Tok : std::uint8_t {
  EndOfFile,
  Error,

  // ---- literals -----------------------------------------------------------
  IntegerLiteral,   // 42, 0xFF, 0b1010, 0755
  LongLiteral,      // 42L
  FloatLiteral,     // 1.5f, 0x1.8p3f
  DoubleLiteral,    // 1.5, 1., 1e10, 0x1.8p3
  CharacterLiteral, // 'a', '\n', '\u0041' (after translation)
  StringLiteral,    // "..." and text blocks
  TrueKeyword,
  FalseKeyword,
  NullKeyword,

  // Includes contextual keywords: var, record, sealed, permits, yield.
  // The parser disambiguates them by text.
  Identifier,

  // ---- reserved words -----------------------------------------------------
  AbstractKeyword,
  AssertKeyword,
  BooleanKeyword,
  BreakKeyword,
  ByteKeyword,
  CaseKeyword,
  CatchKeyword,
  CharKeyword,
  ClassKeyword,
  ConstKeyword,
  ContinueKeyword,
  DefaultKeyword,
  DoKeyword,
  DoubleKeyword,
  ElseKeyword,
  EnumKeyword,
  ExtendsKeyword,
  FinalKeyword,
  FinallyKeyword,
  FloatKeyword,
  ForKeyword,
  GotoKeyword,
  IfKeyword,
  ImplementsKeyword,
  ImportKeyword,
  InstanceOfKeyword,
  IntKeyword,
  InterfaceKeyword,
  LongKeyword,
  NativeKeyword,
  NewKeyword,
  PackageKeyword,
  PrivateKeyword,
  ProtectedKeyword,
  PublicKeyword,
  ReturnKeyword,
  ShortKeyword,
  StaticKeyword,
  StrictFpKeyword,
  SuperKeyword,
  SwitchKeyword,
  SynchronizedKeyword,
  ThisKeyword,
  ThrowKeyword,
  ThrowsKeyword,
  TransientKeyword,
  TryKeyword,
  VoidKeyword,
  VolatileKeyword,
  WhileKeyword,
  UnderscoreKeyword, // a lone '_'

  // ---- separators ---------------------------------------------------------
  LeftParen,
  RightParen,
  LeftBrace,
  RightBrace,
  LeftBracket,
  RightBracket,
  Semicolon,
  Comma,
  Dot,
  Ellipsis,
  At,
  Colon,
  ColonColon,
  Arrow, // ->  (lambda, switch rule)
  Question,

  // ---- operators ----------------------------------------------------------
  Amp,
  AmpAmp,
  AmpEqual,
  Bar,
  BarBar,
  BarEqual,
  Caret,
  CaretEqual,
  Plus,
  PlusPlus,
  PlusEqual,
  Minus,
  MinusMinus,
  MinusEqual,
  Star,
  StarEqual,
  Slash,
  SlashEqual,
  Percent,
  PercentEqual,
  Tilde,
  Bang,
  BangEqual,
  Equal,
  EqualEqual,
  Less,
  LessEqual,
  LessLess,
  LessLessEqual,
  Greater,
  GreaterEqual,
  GreaterGreater,
  GreaterGreaterEqual,
  GreaterGreaterGreater,
  GreaterGreaterGreaterEqual,
};

struct Token {
  Tok kind = Tok::EndOfFile;
  std::uint32_t offset = 0;  // raw byte offset
  std::uint32_t length = 0;  // raw byte length (0 for EOF)

  // Payload - which field is meaningful depends on `kind`:
  //   Identifier       -> text = the identifier
  //   StringLiteral    -> text = decoded value; isTextBlock marks """
  //   CharacterLiteral -> text = decoded char; intValue = code point
  //   number literals  -> text = spelling as written; intValue / floatValue
  std::string text;
  std::uint64_t intValue = 0;
  double floatValue = 0.0;
  bool isTextBlock = false;

  [[nodiscard]] SourceRange range() const noexcept { return {offset, length}; }
};

[[nodiscard]] const char* toString(Tok kind) noexcept;
[[nodiscard]] bool isKeyword(Tok kind) noexcept;
[[nodiscard]] bool isLiteral(Tok kind) noexcept;

// Keyword classification for one word; returns Tok::Identifier for
// non-reserved words (contextual keywords stay identifiers).
[[nodiscard]] Tok classifyWord(const std::string& text) noexcept;

}  // namespace b2::frontend
