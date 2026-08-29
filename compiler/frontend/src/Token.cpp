#include "b2/frontend/Token.h"

#include <string_view>
#include <unordered_map>

namespace b2::frontend {

namespace {

const std::unordered_map<std::string_view, Tok>& keywordTable() noexcept {
  static const std::unordered_map<std::string_view, Tok> table = {
      {"abstract", Tok::AbstractKeyword},     {"assert", Tok::AssertKeyword},
      {"boolean", Tok::BooleanKeyword},       {"break", Tok::BreakKeyword},
      {"byte", Tok::ByteKeyword},             {"case", Tok::CaseKeyword},
      {"catch", Tok::CatchKeyword},           {"char", Tok::CharKeyword},
      {"class", Tok::ClassKeyword},           {"const", Tok::ConstKeyword},
      {"continue", Tok::ContinueKeyword},     {"default", Tok::DefaultKeyword},
      {"do", Tok::DoKeyword},                 {"double", Tok::DoubleKeyword},
      {"else", Tok::ElseKeyword},             {"enum", Tok::EnumKeyword},
      {"extends", Tok::ExtendsKeyword},       {"final", Tok::FinalKeyword},
      {"finally", Tok::FinallyKeyword},       {"float", Tok::FloatKeyword},
      {"for", Tok::ForKeyword},               {"goto", Tok::GotoKeyword},
      {"if", Tok::IfKeyword},                 {"implements", Tok::ImplementsKeyword},
      {"import", Tok::ImportKeyword},         {"instanceof", Tok::InstanceOfKeyword},
      {"int", Tok::IntKeyword},               {"interface", Tok::InterfaceKeyword},
      {"long", Tok::LongKeyword},             {"native", Tok::NativeKeyword},
      {"new", Tok::NewKeyword},               {"package", Tok::PackageKeyword},
      {"private", Tok::PrivateKeyword},       {"protected", Tok::ProtectedKeyword},
      {"public", Tok::PublicKeyword},         {"return", Tok::ReturnKeyword},
      {"short", Tok::ShortKeyword},           {"static", Tok::StaticKeyword},
      {"strictfp", Tok::StrictFpKeyword},     {"super", Tok::SuperKeyword},
      {"switch", Tok::SwitchKeyword},         {"synchronized", Tok::SynchronizedKeyword},
      {"this", Tok::ThisKeyword},             {"throw", Tok::ThrowKeyword},
      {"throws", Tok::ThrowsKeyword},         {"transient", Tok::TransientKeyword},
      {"try", Tok::TryKeyword},               {"void", Tok::VoidKeyword},
      {"volatile", Tok::VolatileKeyword},     {"while", Tok::WhileKeyword},
      {"true", Tok::TrueKeyword},             {"false", Tok::FalseKeyword},
      {"null", Tok::NullKeyword},
  };
  return table;
}

}  // namespace

const char* toString(Tok kind) noexcept {
  switch (kind) {
    case Tok::EndOfFile: return "end of file";
    case Tok::Error: return "error token";

    case Tok::IntegerLiteral: return "integer literal";
    case Tok::LongLiteral: return "long literal";
    case Tok::FloatLiteral: return "float literal";
    case Tok::DoubleLiteral: return "double literal";
    case Tok::CharacterLiteral: return "character literal";
    case Tok::StringLiteral: return "string literal";
    case Tok::TrueKeyword: return "'true'";
    case Tok::FalseKeyword: return "'false'";
    case Tok::NullKeyword: return "'null'";
    case Tok::Identifier: return "identifier";

    case Tok::AbstractKeyword: return "'abstract'";
    case Tok::AssertKeyword: return "'assert'";
    case Tok::BooleanKeyword: return "'boolean'";
    case Tok::BreakKeyword: return "'break'";
    case Tok::ByteKeyword: return "'byte'";
    case Tok::CaseKeyword: return "'case'";
    case Tok::CatchKeyword: return "'catch'";
    case Tok::CharKeyword: return "'char'";
    case Tok::ClassKeyword: return "'class'";
    case Tok::ConstKeyword: return "'const'";
    case Tok::ContinueKeyword: return "'continue'";
    case Tok::DefaultKeyword: return "'default'";
    case Tok::DoKeyword: return "'do'";
    case Tok::DoubleKeyword: return "'double'";
    case Tok::ElseKeyword: return "'else'";
    case Tok::EnumKeyword: return "'enum'";
    case Tok::ExtendsKeyword: return "'extends'";
    case Tok::FinalKeyword: return "'final'";
    case Tok::FinallyKeyword: return "'finally'";
    case Tok::FloatKeyword: return "'float'";
    case Tok::ForKeyword: return "'for'";
    case Tok::GotoKeyword: return "'goto'";
    case Tok::IfKeyword: return "'if'";
    case Tok::ImplementsKeyword: return "'implements'";
    case Tok::ImportKeyword: return "'import'";
    case Tok::InstanceOfKeyword: return "'instanceof'";
    case Tok::IntKeyword: return "'int'";
    case Tok::InterfaceKeyword: return "'interface'";
    case Tok::LongKeyword: return "'long'";
    case Tok::NativeKeyword: return "'native'";
    case Tok::NewKeyword: return "'new'";
    case Tok::PackageKeyword: return "'package'";
    case Tok::PrivateKeyword: return "'private'";
    case Tok::ProtectedKeyword: return "'protected'";
    case Tok::PublicKeyword: return "'public'";
    case Tok::ReturnKeyword: return "'return'";
    case Tok::ShortKeyword: return "'short'";
    case Tok::StaticKeyword: return "'static'";
    case Tok::StrictFpKeyword: return "'strictfp'";
    case Tok::SuperKeyword: return "'super'";
    case Tok::SwitchKeyword: return "'switch'";
    case Tok::SynchronizedKeyword: return "'synchronized'";
    case Tok::ThisKeyword: return "'this'";
    case Tok::ThrowKeyword: return "'throw'";
    case Tok::ThrowsKeyword: return "'throws'";
    case Tok::TransientKeyword: return "'transient'";
    case Tok::TryKeyword: return "'try'";
    case Tok::VoidKeyword: return "'void'";
    case Tok::VolatileKeyword: return "'volatile'";
    case Tok::WhileKeyword: return "'while'";
    case Tok::UnderscoreKeyword: return "'_'";

    case Tok::LeftParen: return "'('";
    case Tok::RightParen: return "')'";
    case Tok::LeftBrace: return "'{'";
    case Tok::RightBrace: return "'}'";
    case Tok::LeftBracket: return "'['";
    case Tok::RightBracket: return "']'";
    case Tok::Semicolon: return "';'";
    case Tok::Comma: return "','";
    case Tok::Dot: return "'.'";
    case Tok::Ellipsis: return "'...'";
    case Tok::At: return "'@'";
    case Tok::Colon: return "':'";
    case Tok::ColonColon: return "'::'";
    case Tok::Arrow: return "'->'";
    case Tok::Question: return "'?'";

    case Tok::Amp: return "'&'";
    case Tok::AmpAmp: return "'&&'";
    case Tok::AmpEqual: return "'&='";
    case Tok::Bar: return "'|'";
    case Tok::BarBar: return "'||'";
    case Tok::BarEqual: return "'|='";
    case Tok::Caret: return "'^'";
    case Tok::CaretEqual: return "'^='";
    case Tok::Plus: return "'+'";
    case Tok::PlusPlus: return "'++'";
    case Tok::PlusEqual: return "'+='";
    case Tok::Minus: return "'-'";
    case Tok::MinusMinus: return "'--'";
    case Tok::MinusEqual: return "'-='";
    case Tok::Star: return "'*'";
    case Tok::StarEqual: return "'*='";
    case Tok::Slash: return "'/'";
    case Tok::SlashEqual: return "'/='";
    case Tok::Percent: return "'%'";
    case Tok::PercentEqual: return "'%='";
    case Tok::Tilde: return "'~'";
    case Tok::Bang: return "'!'";
    case Tok::BangEqual: return "'!='";
    case Tok::Equal: return "'='";
    case Tok::EqualEqual: return "'=='";
    case Tok::Less: return "'<'";
    case Tok::LessEqual: return "'<='";
    case Tok::LessLess: return "'<<'";
    case Tok::LessLessEqual: return "'<<='";
    case Tok::Greater: return "'>'";
    case Tok::GreaterEqual: return "'>='";
    case Tok::GreaterGreater: return "'>>'";
    case Tok::GreaterGreaterEqual: return "'>>='";
    case Tok::GreaterGreaterGreater: return "'>>>'";
    case Tok::GreaterGreaterGreaterEqual: return "'>>>='";
  }
  return "<unknown>";
}

bool isKeyword(Tok kind) noexcept {
  const int k = static_cast<int>(kind);
  return (k >= static_cast<int>(Tok::TrueKeyword) &&
          k <= static_cast<int>(Tok::UnderscoreKeyword));
}

bool isLiteral(Tok kind) noexcept {
  const int k = static_cast<int>(kind);
  return (k >= static_cast<int>(Tok::IntegerLiteral) &&
          k <= static_cast<int>(Tok::NullKeyword));
}

Tok classifyWord(const std::string& text) noexcept {
  const auto& table = keywordTable();
  const auto it = table.find(std::string_view(text));
  if (it != table.end()) return it->second;
  if (text == "_") return Tok::UnderscoreKeyword;
  return Tok::Identifier;
}

}  // namespace b2::frontend
