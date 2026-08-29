// B-2 Frontend - parser core: token utilities, speculation, recovery,
// annotations / modifiers / types, and the compilation-unit skeleton.

#include "b2/frontend/Parser.h"

#include <utility>

#include "b2/frontend/Diagnostics.h"
#include "b2/frontend/SourceManager.h"

namespace b2::frontend {

using namespace ast;

Parser::Parser(std::vector<Token> tokens, const SourceManager& sm, DiagnosticEngine& diags)
    : toks_(std::move(tokens)), sm_(sm), diags_(diags) {
  if (toks_.empty()) {
    Token eof;
    eof.kind = Tok::EndOfFile;
    toks_.push_back(std::move(eof));
  }
}

// ------------------------------------------------------------ token stream -

const Token& Parser::peek(std::size_t n) const noexcept {
  const std::size_t i = pos_ + n;
  return toks_[i < toks_.size() ? i : toks_.size() - 1];
}

bool Parser::atContextual(std::string_view text) const noexcept {
  return curKind() == Tok::Identifier && cur().text == text;
}

Token Parser::advance() noexcept {
  const std::size_t i = pos_;
  if (toks_[pos_].kind != Tok::EndOfFile) ++pos_;
  return toks_[i];
}

bool Parser::accept(Tok k) noexcept {
  if (at(k)) {
    advance();
    return true;
  }
  return false;
}

Token Parser::expect(Tok k) {
  if (at(k)) return advance();
  if (!trial_) {
    error("expected " + std::string(toString(k)) + " but found " +
          std::string(toString(curKind())));
  }
  return cur();
}

SourceRange Parser::rangeFrom(std::uint32_t startOffset) const noexcept {
  const std::uint32_t end = cur().offset + cur().length;
  return SourceRange(startOffset, end > startOffset ? end - startOffset : 0);
}

Token Parser::expectGreater() {
  auto makeTok = [](Tok kind, std::uint32_t offset, std::uint32_t length) {
    Token t;
    t.kind = kind;
    t.offset = offset;
    t.length = length;
    return t;
  };
  Token first;
  Token rest;
  switch (curKind()) {
    case Tok::Greater:
      return advance();
    case Tok::GreaterGreater:
      first = makeTok(Tok::Greater, cur().offset, 1);
      rest = makeTok(Tok::Greater, cur().offset + 1, 1);
      break;
    case Tok::GreaterGreaterEqual:
      first = makeTok(Tok::Greater, cur().offset, 1);
      rest = makeTok(Tok::GreaterEqual, cur().offset + 1, 2);
      break;
    case Tok::GreaterGreaterGreater:
      first = makeTok(Tok::Greater, cur().offset, 1);
      rest = makeTok(Tok::GreaterGreater, cur().offset + 1, 2);
      break;
    case Tok::GreaterGreaterGreaterEqual:
      first = makeTok(Tok::Greater, cur().offset, 1);
      rest = makeTok(Tok::GreaterGreaterEqual, cur().offset + 1, 3);
      break;
    case Tok::GreaterEqual:
      first = makeTok(Tok::Greater, cur().offset, 1);
      rest = makeTok(Tok::Equal, cur().offset + 1, 1);
      break;
    default:
      if (!trial_) {
        error("expected '>' but found " + std::string(toString(curKind())));
      }
      return cur();
  }
  toks_[pos_] = std::move(rest);
  toks_.insert(toks_.begin() + static_cast<std::ptrdiff_t>(pos_), std::move(first));
  return advance();  // consumes the inserted '>'
}

// ------------------------------------------------------------- diagnostics -

void Parser::error(const Token& atToken, std::string msg) {
  if (trial_) return;
  diags_.error(atToken.offset, std::move(msg));
}

void Parser::error(std::string msg) { error(cur(), std::move(msg)); }

void Parser::reset(Checkpoint cp) noexcept {
  pos_ = cp.pos;
  if (toks_.size() > cp.tokenCount) toks_.resize(cp.tokenCount);
}

// ---------------------------------------------------------- error recovery -

void Parser::syncToStatementEnd() {
  while (!at(Tok::EndOfFile)) {
    const Tok k = curKind();
    if (k == Tok::Semicolon) {
      advance();
      return;
    }
    if (k == Tok::RightBrace || k == Tok::CaseKeyword || k == Tok::DefaultKeyword) return;
    advance();
  }
}

void Parser::syncToClassMember() {
  while (!at(Tok::EndOfFile)) {
    const Tok k = curKind();
    if (k == Tok::Semicolon) {
      advance();
      return;
    }
    if (k == Tok::RightBrace) return;
    // Heuristic: a modifier/keyword that starts a member stops the skip.
    if (k == Tok::PublicKeyword || k == Tok::PrivateKeyword || k == Tok::ProtectedKeyword ||
        k == Tok::StaticKeyword || k == Tok::FinalKeyword || k == Tok::AbstractKeyword ||
        k == Tok::ClassKeyword || k == Tok::InterfaceKeyword || k == Tok::EnumKeyword ||
        k == Tok::LeftBrace) {
      return;
    }
    advance();
  }
}

bool Parser::ensureProgress(std::size_t startPos) {
  if (pos_ > startPos) return true;
  if (at(Tok::EndOfFile)) return false;
  if (!trial_) error("skipping unexpected " + std::string(toString(curKind())));
  advance();
  return true;
}

// ---------------------------------------------------------- shared syntax -

std::vector<Annotation> Parser::parseAnnotations() {
  std::vector<Annotation> out;
  while (at(Tok::At) && peek(1).kind != Tok::InterfaceKeyword) {
    out.push_back(parseAnnotation());
  }
  return out;
}

Annotation Parser::parseAnnotation() {
  const Token atTok = advance();  // @ (copy: parsing may split tokens)
  Annotation a;
  a.range = atTok.range();
  a.name = parseQualifiedName("annotation name");
  if (accept(Tok::LeftParen)) {
    a.hasArguments = true;
    if (!at(Tok::RightParen)) {
      if (at(Tok::Identifier) && peek(1).kind == Tok::Equal) {
        while (true) {
          Annotation::ElementValue ev;
          ev.range = cur().range();
          ev.name = advance().text;
          expect(Tok::Equal);
          ev.value = parseAnnotationElementValue();
          a.values.push_back(std::move(ev));
          if (!accept(Tok::Comma)) break;
        }
      } else {
        a.singleElement = true;
        Annotation::ElementValue ev;
        ev.range = cur().range();
        ev.value = parseAnnotationElementValue();
        a.values.push_back(std::move(ev));
      }
    }
    expect(Tok::RightParen);
  }
  const std::uint32_t end = cur().offset + cur().length;
  a.range.length = end > a.range.offset ? end - a.range.offset : a.range.length;
  return a;
}

std::unique_ptr<Node> Parser::parseAnnotationElementValue() {
  if (at(Tok::At)) {
    Annotation inner = parseAnnotation();
    return std::make_unique<AnnotationValue>(inner.range, std::move(inner));
  }
  if (at(Tok::LeftBrace)) {
    return parseArrayInitializer();
  }
  return parseConditional();
}

std::uint32_t Parser::parseModifiers(bool /*inInterface*/) {
  std::uint32_t mods = ModNone;
  while (true) {
    std::uint32_t m = 0;
    bool contextualMulti = false;  // non-sealed eats three tokens
    switch (curKind()) {
      case Tok::PublicKeyword: m = ModPublic; break;
      case Tok::ProtectedKeyword: m = ModProtected; break;
      case Tok::PrivateKeyword: m = ModPrivate; break;
      case Tok::StaticKeyword: m = ModStatic; break;
      case Tok::FinalKeyword: m = ModFinal; break;
      case Tok::AbstractKeyword: m = ModAbstract; break;
      case Tok::TransientKeyword: m = ModTransient; break;
      case Tok::VolatileKeyword: m = ModVolatile; break;
      case Tok::SynchronizedKeyword: m = ModSynchronized; break;
      case Tok::NativeKeyword: m = ModNative; break;
      case Tok::StrictFpKeyword: m = ModStrictfp; break;
      case Tok::DefaultKeyword: m = ModDefault; break;
      case Tok::Identifier:
        if (cur().text == "sealed") {
          // Contextual: only a modifier when a type declaration follows.
          const Tok n = peek(1).kind;
          if (n == Tok::ClassKeyword || n == Tok::InterfaceKeyword || n == Tok::EnumKeyword ||
              (n == Tok::Identifier && peek(1).text == "record")) {
            m = ModSealed;
          }
        } else if (cur().text == "non" && peek(1).kind == Tok::Minus &&
                   peek(2).kind == Tok::Identifier && peek(2).text == "sealed") {
          m = ModNonSealed;
          contextualMulti = true;
        }
        break;
      default:
        break;
    }
    if (m == 0) break;
    mods |= m;
    if (contextualMulti) {
      advance();  // 'non'
      advance();  // '-'
      advance();  // 'sealed'
    } else {
      advance();
    }
  }
  return mods;
}

bool Parser::primitiveTok(Tok k, PrimitiveKind& out) noexcept {
  switch (k) {
    case Tok::BooleanKeyword: out = PrimitiveKind::Boolean; return true;
    case Tok::ByteKeyword: out = PrimitiveKind::Byte; return true;
    case Tok::ShortKeyword: out = PrimitiveKind::Short; return true;
    case Tok::CharKeyword: out = PrimitiveKind::Char; return true;
    case Tok::IntKeyword: out = PrimitiveKind::Int; return true;
    case Tok::LongKeyword: out = PrimitiveKind::Long; return true;
    case Tok::FloatKeyword: out = PrimitiveKind::Float; return true;
    case Tok::DoubleKeyword: out = PrimitiveKind::Double; return true;
    case Tok::VoidKeyword: out = PrimitiveKind::Void; return true;
    default: return false;
  }
}

std::unique_ptr<Type> Parser::parseType() {
  const std::uint32_t start = cur().offset;
  auto base = parseTypeNoArray();
  if (!base) return nullptr;
  // Type-position dimensions are always EMPTY brackets; sized brackets only
  // occur in `new` expressions (parseNew handles those itself). Requiring
  // the closing ']' immediately keeps `grid[row][col] = 7` from being
  // misread as an array-typed declaration.
  while (at(Tok::LeftBracket) && peek(1).kind == Tok::RightBracket) {
    advance();
    advance();
    base = std::make_unique<ArrayType>(rangeFrom(start), std::move(base));
  }
  return base;
}

std::unique_ptr<Type> Parser::parseTypeNoArray() {
  const std::uint32_t start = cur().offset;
  std::vector<Annotation> annos = parseAnnotations();

  PrimitiveKind pk;
  if (primitiveTok(curKind(), pk)) {
    advance();
    auto t = std::make_unique<PrimitiveType>(rangeFrom(start), pk);
    t->annotations = std::move(annos);
    return t;
  }

  if (at(Tok::Identifier)) {
    std::vector<ClassType::Segment> segments;
    while (at(Tok::Identifier)) {
      const Token id = advance();  // copy (type args may split tokens)
      ClassType::Segment seg;
      seg.name = id.text;
      seg.range = id.range();
      if (at(Tok::Less)) {
        advance();  // '<'
        seg.hasTypeArgs = true;
        parseTypeArgumentsInto(seg.args);
        expectGreater();
      }
      segments.push_back(std::move(seg));
      if (at(Tok::Dot) && peek(1).kind == Tok::Identifier) {
        advance();
        continue;
      }
      break;
    }
    auto t = std::make_unique<ClassType>(rangeFrom(start), std::move(segments));
    t->annotations = std::move(annos);
    return t;
  }

  if (!trial_) error("expected a type");
  return nullptr;
}

bool Parser::parseTypeArgumentsInto(std::vector<std::unique_ptr<Type>>& out) {
  if (at(Tok::Greater)) return true;  // diamond <>
  while (true) {
    if (at(Tok::Question)) {
      advance();
      auto w = std::make_unique<WildcardType>(rangeFrom(0));
      if (at(Tok::ExtendsKeyword)) {
        advance();
        w->bound = parseType();
      } else if (at(Tok::SuperKeyword)) {
        advance();
        w->superBound = true;
        w->bound = parseType();
      }
      out.push_back(std::move(w));
    } else {
      auto t = parseType();
      if (!t) return false;
      out.push_back(std::move(t));
    }
    if (accept(Tok::Comma)) continue;
    break;
  }
  return true;
}

std::vector<TypeParameter> Parser::parseTypeParameters() {
  std::vector<TypeParameter> out;
  expect(Tok::Less);
  while (!at(Tok::Greater) && !at(Tok::EndOfFile)) {
    TypeParameter tp;
    tp.range = cur().range();
    tp.annotations = parseAnnotations();
    if (!at(Tok::Identifier)) {
      if (!trial_) error("expected a type parameter name");
      break;
    }
    tp.name = advance().text;
    if (accept(Tok::ExtendsKeyword)) {
      auto first = parseType();
      if (first) tp.bounds.push_back(std::move(first));
      while (at(Tok::Amp)) {
        advance();
        auto b = parseType();
        if (b) tp.bounds.push_back(std::move(b));
      }
    }
    out.push_back(std::move(tp));
    if (!accept(Tok::Comma)) break;
  }
  expectGreater();
  return out;
}

std::vector<std::string> Parser::parseQualifiedName(const char* what) {
  std::vector<std::string> parts;
  if (!at(Tok::Identifier)) {
    if (!trial_) error(std::string("expected ") + what);
    return parts;
  }
  parts.push_back(advance().text);
  while (at(Tok::Dot) && peek(1).kind == Tok::Identifier) {
    advance();
    parts.push_back(advance().text);
  }
  return parts;
}

// ------------------------------------------------- compilation unit skeleton -

std::unique_ptr<CompilationUnit> Parser::parseCompilationUnit() {
  const std::uint32_t start = cur().offset;
  auto unit = std::make_unique<CompilationUnit>(cur().range());

  // Optional annotated package declaration.
  if (at(Tok::PackageKeyword)) {
    unit->package = parsePackageDecl();
  } else if (at(Tok::At)) {
    // @Anno package p;  vs  @Anno public class C ...
    const bool isPackage = speculate([&] {
      parseAnnotations();
      return at(Tok::PackageKeyword);
    });
    if (isPackage) {
      auto pkg = std::make_unique<PackageDeclaration>(cur().range());
      pkg->annotations = parseAnnotations();
      expect(Tok::PackageKeyword);
      pkg->name = parseQualifiedName("package name");
      expect(Tok::Semicolon);
      unit->package = std::move(pkg);
    }
  }

  while (at(Tok::ImportKeyword)) {
    unit->imports.push_back(parseImportDecl());
  }

  while (!at(Tok::EndOfFile)) {
    const std::size_t before = pos_;
    if (accept(Tok::Semicolon)) continue;  // stray semicolons are legal
    auto decl = parseTopLevelTypeDecl();
    if (decl) unit->types.push_back(std::move(decl));
    if (!ensureProgress(before)) break;
  }

  unit->range = rangeFrom(start);
  return unit;
}

std::unique_ptr<PackageDeclaration> Parser::parsePackageDecl() {
  auto pkg = std::make_unique<PackageDeclaration>(cur().range());
  expect(Tok::PackageKeyword);
  pkg->name = parseQualifiedName("package name");
  expect(Tok::Semicolon);
  pkg->range = rangeFrom(pkg->range.offset);
  return pkg;
}

std::unique_ptr<ImportDeclaration> Parser::parseImportDecl() {
  auto imp = std::make_unique<ImportDeclaration>(cur().range());
  expect(Tok::ImportKeyword);
  if (accept(Tok::StaticKeyword)) imp->isStatic = true;
  imp->name = parseQualifiedName("import name");
  if (accept(Tok::Dot)) {
    if (accept(Tok::Star)) {  // '*' lexes as Star
      imp->isOnDemand = true;
    } else if (!trial_) {
      error("expected '*' or a type name in import");
    }
  }
  expect(Tok::Semicolon);
  imp->range = rangeFrom(imp->range.offset);
  return imp;
}

std::unique_ptr<Decl> Parser::parseTopLevelTypeDecl() {
  const std::uint32_t start = cur().offset;
  std::vector<Annotation> annos = parseAnnotations();
  const std::uint32_t mods = parseModifiers(false);
  return parseTypeDeclCommon(start, mods, std::move(annos), MemberContext{});
}

std::unique_ptr<Decl> Parser::parseTypeDeclCommon(std::uint32_t start, std::uint32_t mods,
                                                  std::vector<Annotation> annos,
                                                  const MemberContext& ctx) {
  (void)ctx;
  if (at(Tok::ClassKeyword)) return parseClassDeclaration(start, mods, std::move(annos));
  if (at(Tok::InterfaceKeyword)) return parseInterfaceDeclaration(start, mods, std::move(annos));
  if (at(Tok::EnumKeyword)) return parseEnumDeclaration(start, mods, std::move(annos));
  if (atContextual("record")) return parseRecordDeclaration(start, mods, std::move(annos));
  if (at(Tok::At) && peek(1).kind == Tok::InterfaceKeyword)
    return parseAnnotationTypeDeclaration(start, mods, std::move(annos));
  if (!trial_) error("expected a type declaration (class, interface, enum, record, or @interface)");
  syncToClassMember();
  return nullptr;
}

// ---- type declarations -----------------------------------------------------

std::unique_ptr<Decl> Parser::parseClassDeclaration(std::uint32_t start, std::uint32_t mods,
                                                    std::vector<Annotation> annos) {
  expect(Tok::ClassKeyword);
  auto cd = std::make_unique<ClassDeclaration>(rangeFrom(start));
  cd->annotations = std::move(annos);
  cd->mods = mods;
  if (at(Tok::Identifier)) {
    cd->nameRange = cur().range();
    cd->name = advance().text;
  } else if (!trial_) {
    error("expected a class name");
  }
  if (at(Tok::Less)) cd->typeParameters = parseTypeParameters();
  if (accept(Tok::ExtendsKeyword)) cd->extendsType = parseType();
  if (accept(Tok::ImplementsKeyword)) {
    do {
      auto t = parseType();
      if (t) cd->implements.push_back(std::move(t));
    } while (accept(Tok::Comma));
  }
  if (atContextual("permits")) {
    advance();
    do {
      auto t = parseType();
      if (t) cd->permits.push_back(std::move(t));
    } while (accept(Tok::Comma));
  }
  if (expect(Tok::LeftBrace).kind != Tok::LeftBrace) {
    syncToClassMember();
    cd->range = rangeFrom(start);
    return cd;
  }
  MemberContext ctx;
  ctx.typeName = cd->name;
  parseClassBodyInto(cd->members, ctx);
  expect(Tok::RightBrace);
  cd->range = rangeFrom(start);
  return cd;
}

std::unique_ptr<Decl> Parser::parseInterfaceDeclaration(std::uint32_t start, std::uint32_t mods,
                                                        std::vector<Annotation> annos) {
  expect(Tok::InterfaceKeyword);
  auto id = std::make_unique<InterfaceDeclaration>(rangeFrom(start));
  id->annotations = std::move(annos);
  id->mods = mods;
  if (at(Tok::Identifier)) {
    id->nameRange = cur().range();
    id->name = advance().text;
  } else if (!trial_) {
    error("expected an interface name");
  }
  if (at(Tok::Less)) id->typeParameters = parseTypeParameters();
  if (accept(Tok::ExtendsKeyword)) {
    do {
      auto t = parseType();
      if (t) id->extends.push_back(std::move(t));
    } while (accept(Tok::Comma));
  }
  if (atContextual("permits")) {
    advance();
    do {
      auto t = parseType();
      if (t) id->permits.push_back(std::move(t));
    } while (accept(Tok::Comma));
  }
  if (expect(Tok::LeftBrace).kind != Tok::LeftBrace) {
    syncToClassMember();
    id->range = rangeFrom(start);
    return id;
  }
  MemberContext ctx;
  ctx.inInterface = true;
  ctx.typeName = id->name;
  parseClassBodyInto(id->members, ctx);
  expect(Tok::RightBrace);
  id->range = rangeFrom(start);
  return id;
}

std::unique_ptr<Decl> Parser::parseEnumDeclaration(std::uint32_t start, std::uint32_t mods,
                                                   std::vector<Annotation> annos) {
  expect(Tok::EnumKeyword);
  auto ed = std::make_unique<EnumDeclaration>(rangeFrom(start));
  ed->annotations = std::move(annos);
  ed->mods = mods;
  if (at(Tok::Identifier)) {
    ed->nameRange = cur().range();
    ed->name = advance().text;
  } else if (!trial_) {
    error("expected an enum name");
  }
  if (accept(Tok::ImplementsKeyword)) {
    do {
      auto t = parseType();
      if (t) ed->implements.push_back(std::move(t));
    } while (accept(Tok::Comma));
  }
  if (expect(Tok::LeftBrace).kind != Tok::LeftBrace) {
    ed->range = rangeFrom(start);
    return ed;
  }
  MemberContext ctx;
  ctx.typeName = ed->name;

  // Enum constants until ';' or '}'.
  while (!at(Tok::RightBrace) && !at(Tok::Semicolon) && !at(Tok::EndOfFile)) {
    const std::size_t before = pos_;
    EnumConstant c;
    c.range = cur().range();
    c.annotations = parseAnnotations();
    if (at(Tok::Identifier)) {
      c.nameRange = cur().range();
      c.name = advance().text;
    } else if (!trial_) {
      error("expected an enum constant");
    }
    if (accept(Tok::LeftParen)) {
      parseArgumentList(c.arguments);
    }
    if (at(Tok::LeftBrace)) {  // constant-specific body
      auto body = std::make_unique<ClassDeclaration>(cur().range());
      expect(Tok::LeftBrace);
      MemberContext bctx = ctx;
      bctx.typeName = c.name;
      parseClassBodyInto(body->members, bctx);
      expect(Tok::RightBrace);
      body->range = rangeFrom(body->range.offset);
      c.body = std::move(body);
    }
    ed->constants.push_back(std::move(c));
    if (!accept(Tok::Comma)) break;
    if (!ensureProgress(before)) break;
  }
  accept(Tok::Semicolon);  // constants end; members follow
  parseClassBodyInto(ed->members, ctx);
  expect(Tok::RightBrace);
  ed->range = rangeFrom(start);
  return ed;
}

std::unique_ptr<Decl> Parser::parseRecordDeclaration(std::uint32_t start, std::uint32_t mods,
                                                     std::vector<Annotation> annos) {
  advance();  // contextual 'record'
  auto rd = std::make_unique<RecordDeclaration>(rangeFrom(start));
  rd->annotations = std::move(annos);
  rd->mods = mods;
  if (at(Tok::Identifier)) {
    rd->nameRange = cur().range();
    rd->name = advance().text;
  } else if (!trial_) {
    error("expected a record name");
  }
  if (at(Tok::Less)) rd->typeParameters = parseTypeParameters();
  expect(Tok::LeftParen);
  while (!at(Tok::RightParen) && !at(Tok::EndOfFile)) {
    const std::size_t before = pos_;
    RecordComponent rc;
    rc.range = cur().range();
    rc.annotations = parseAnnotations();
    rc.type = parseType();
    if (at(Tok::Identifier)) {
      rc.nameRange = cur().range();
      rc.name = advance().text;
    } else if (!trial_) {
      error("expected a record component name");
    }
    rd->components.push_back(std::move(rc));
    if (!accept(Tok::Comma)) break;
    if (!ensureProgress(before)) break;
  }
  expect(Tok::RightParen);
  if (accept(Tok::ImplementsKeyword)) {
    do {
      auto t = parseType();
      if (t) rd->implements.push_back(std::move(t));
    } while (accept(Tok::Comma));
  }
  if (expect(Tok::LeftBrace).kind != Tok::LeftBrace) {
    rd->range = rangeFrom(start);
    return rd;
  }
  MemberContext ctx;
  ctx.isRecord = true;
  ctx.typeName = rd->name;
  parseClassBodyInto(rd->members, ctx);
  expect(Tok::RightBrace);
  rd->range = rangeFrom(start);
  return rd;
}

std::unique_ptr<Decl> Parser::parseAnnotationTypeDeclaration(std::uint32_t start,
                                                             std::uint32_t mods,
                                                             std::vector<Annotation> annos) {
  expect(Tok::At);
  expect(Tok::InterfaceKeyword);
  auto ad = std::make_unique<AnnotationTypeDeclaration>(rangeFrom(start));
  ad->annotations = std::move(annos);
  ad->mods = mods;
  if (at(Tok::Identifier)) {
    ad->nameRange = cur().range();
    ad->name = advance().text;
  } else if (!trial_) {
    error("expected an annotation type name");
  }
  if (expect(Tok::LeftBrace).kind != Tok::LeftBrace) {
    ad->range = rangeFrom(start);
    return ad;
  }
  MemberContext ctx;
  ctx.isAnnotationType = true;
  ctx.typeName = ad->name;
  parseClassBodyInto(ad->members, ctx);
  expect(Tok::RightBrace);
  ad->range = rangeFrom(start);
  return ad;
}

void Parser::parseClassBodyInto(std::vector<std::unique_ptr<Decl>>& members,
                                const MemberContext& ctx) {
  while (!at(Tok::RightBrace) && !at(Tok::EndOfFile)) {
    const std::size_t before = pos_;
    auto m = parseClassMember(ctx);
    if (m) members.push_back(std::move(m));
    if (!ensureProgress(before)) break;
  }
}

std::unique_ptr<Decl> Parser::parseClassMember(const MemberContext& ctx) {
  const std::uint32_t start = cur().offset;
  std::vector<Annotation> annos = parseAnnotations();
  const std::uint32_t mods = parseModifiers(ctx.inInterface);

  if (at(Tok::LeftBrace)) {  // initializer block
    auto ib = std::make_unique<InitializerBlock>(rangeFrom(start));
    ib->isStatic = (mods & ModStatic) != 0;
    ib->body = parseBlock();
    ib->range = rangeFrom(start);
    return ib;
  }
  if (accept(Tok::Semicolon)) return nullptr;  // stray semicolon

  if (at(Tok::ClassKeyword)) return parseClassDeclaration(start, mods, std::move(annos));
  if (at(Tok::InterfaceKeyword)) return parseInterfaceDeclaration(start, mods, std::move(annos));
  if (at(Tok::EnumKeyword)) return parseEnumDeclaration(start, mods, std::move(annos));
  if (atContextual("record") && (peek(1).kind == Tok::Identifier || peek(1).kind == Tok::Less))
    return parseRecordDeclaration(start, mods, std::move(annos));
  if (at(Tok::At) && peek(1).kind == Tok::InterfaceKeyword)
    return parseAnnotationTypeDeclaration(start, mods, std::move(annos));

  // Generic methods / constructors introduce type parameters first.
  std::vector<TypeParameter> tps;
  if (at(Tok::Less)) tps = parseTypeParameters();

  // Constructor: `Name(` or (records) the compact form `Name {`.
  if (at(Tok::Identifier)) {
    const bool ctorParen = peek(1).kind == Tok::LeftParen;
    const bool compact = ctx.isRecord && peek(1).kind == Tok::LeftBrace && cur().text == ctx.typeName;
    if (ctorParen || compact) return parseConstructorDeclaration(start, mods, std::move(annos), std::move(tps), ctx);
  }

  // Method or field: Type name ...
  auto type = parseType();
  if (!type) {
    if (!trial_) error("expected a member");
    syncToClassMember();
    return nullptr;
  }
  if (!at(Tok::Identifier)) {
    if (!trial_) error("expected a member name");
    syncToClassMember();
    return nullptr;
  }
  const Token nameTok = advance();
  if (at(Tok::LeftParen)) {
    return parseMethodDeclaration(start, mods, std::move(annos), std::move(tps), std::move(type),
                                  nameTok, ctx);
  }
  return parseFieldDeclaration(start, mods, std::move(annos), std::move(type), nameTok);
}

std::unique_ptr<Decl> Parser::parseMethodDeclaration(std::uint32_t start, std::uint32_t mods,
                                                     std::vector<Annotation> annos,
                                                     std::vector<TypeParameter> tps,
                                                     std::unique_ptr<Type> retType,
                                                     const Token& nameTok,
                                                     const MemberContext& ctx) {
  auto md = std::make_unique<MethodDeclaration>(rangeFrom(start));
  md->annotations = std::move(annos);
  md->mods = mods;
  md->typeParameters = std::move(tps);
  md->returnType = std::move(retType);
  md->name = nameTok.text;
  md->nameRange = nameTok.range();
  expect(Tok::LeftParen);
  md->parameters = parseFormalParameters();
  expect(Tok::RightParen);
  if (at(Tok::ThrowsKeyword)) md->throws = parseThrowsClause();
  if (ctx.isAnnotationType && accept(Tok::DefaultKeyword)) {
    md->defaultValue = parseAnnotationElementValue();
  }
  if (at(Tok::LeftBrace)) {
    md->body = parseBlock();
  } else {
    expect(Tok::Semicolon);  // abstract / native / interface method
  }
  md->range = rangeFrom(start);
  return md;
}

std::unique_ptr<Decl> Parser::parseConstructorDeclaration(std::uint32_t start, std::uint32_t mods,
                                                          std::vector<Annotation> annos,
                                                          std::vector<TypeParameter> tps,
                                                          const MemberContext& ctx) {
  auto cd = std::make_unique<ConstructorDeclaration>(rangeFrom(start));
  cd->annotations = std::move(annos);
  cd->mods = mods;
  cd->typeParameters = std::move(tps);
  if (at(Tok::Identifier)) {
    cd->nameRange = cur().range();
    cd->name = advance().text;
  } else if (!trial_) {
    error("expected a constructor name");
  }
  if (accept(Tok::LeftParen)) {  // compact record ctor has no parameter list
    cd->parameters = parseFormalParameters();
    expect(Tok::RightParen);
    if (at(Tok::ThrowsKeyword)) cd->throws = parseThrowsClause();
  } else {
    cd->isCompact = ctx.isRecord;
    if (at(Tok::ThrowsKeyword)) cd->throws = parseThrowsClause();
  }
  cd->body = parseBlock();
  cd->range = rangeFrom(start);
  return cd;
}

std::unique_ptr<Decl> Parser::parseFieldDeclaration(std::uint32_t start, std::uint32_t mods,
                                                    std::vector<Annotation> annos,
                                                    std::unique_ptr<Type> type,
                                                    const Token& firstName) {
  auto fd = std::make_unique<FieldDeclaration>(rangeFrom(start));
  fd->annotations = std::move(annos);
  fd->mods = mods;
  fd->type = std::move(type);
  // Re-inject the already-consumed first declarator name.
  VariableDeclarator first;
  first.range = firstName.range();
  first.name = firstName.text;
  first.nameRange = firstName.range();
  while (at(Tok::LeftBracket)) {  // C-style dims: int a[];
    advance();
    expect(Tok::RightBracket);
    first.extraDims.push_back(rangeFrom(first.range.offset));
  }
  if (accept(Tok::Equal)) first.initializer = parseVariableInitializer();
  fd->declarators.push_back(std::move(first));
  if (accept(Tok::Comma)) parseVariableDeclaratorList(fd->declarators);
  expect(Tok::Semicolon);
  fd->range = rangeFrom(start);
  return fd;
}

std::vector<FormalParameter> Parser::parseFormalParameters() {
  std::vector<FormalParameter> out;
  if (at(Tok::RightParen)) return out;
  while (true) {
    const std::size_t before = pos_;
    FormalParameter p;
    p.range = cur().range();
    p.annotations = parseAnnotations();
    p.mods = parseModifiers(false);  // `final` legally
    p.type = parseType();
    if (!p.type) {
      if (!trial_) error("expected a parameter type");
      break;
    }
    if (accept(Tok::Ellipsis)) p.isVarArgs = true;
    if (at(Tok::ThisKeyword)) {  // receiver parameter: void m(Foo this)
      p.nameRange = cur().range();
      p.name = "this";
      advance();
      p.isReceiver = true;
    } else if (at(Tok::Identifier)) {
      p.nameRange = cur().range();
      p.name = advance().text;
    } else if (!trial_) {
      error("expected a parameter name");
    }
    out.push_back(std::move(p));
    if (!accept(Tok::Comma)) break;
    if (!ensureProgress(before)) break;
  }
  return out;
}

void Parser::parseVariableDeclaratorList(std::vector<VariableDeclarator>& out) {
  while (true) {
    VariableDeclarator d;
    if (!at(Tok::Identifier)) {
      if (!trial_) error("expected a variable name");
      return;
    }
    const Token nameTok = advance();  // copy (token vector may grow)
    d.range = nameTok.range();
    d.name = nameTok.text;
    d.nameRange = nameTok.range();
    while (at(Tok::LeftBracket)) {
      advance();
      expect(Tok::RightBracket);
      d.extraDims.push_back(rangeFrom(d.range.offset));
    }
    if (accept(Tok::Equal)) d.initializer = parseVariableInitializer();
    out.push_back(std::move(d));
    if (!at(Tok::Comma)) return;
    advance();  // ','; loop for the next declarator
  }
}

std::unique_ptr<Expr> Parser::parseVariableInitializer() {
  if (at(Tok::LeftBrace)) return parseArrayInitializer();
  return parseExpression();
}

std::unique_ptr<ArrayInitializer> Parser::parseArrayInitializer() {
  const std::uint32_t start = cur().offset;
  expect(Tok::LeftBrace);
  auto ai = std::make_unique<ArrayInitializer>(rangeFrom(start));
  while (!at(Tok::RightBrace) && !at(Tok::EndOfFile)) {
    const std::size_t before = pos_;
    auto v = parseVariableInitializer();
    if (v) ai->values.push_back(std::move(v));
    if (!accept(Tok::Comma)) break;
    if (!ensureProgress(before)) break;
  }
  expect(Tok::RightBrace);
  ai->range = rangeFrom(start);
  return ai;
}

std::vector<std::unique_ptr<Type>> Parser::parseThrowsClause() {
  std::vector<std::unique_ptr<Type>> out;
  expect(Tok::ThrowsKeyword);
  do {
    auto t = parseType();
    if (t) out.push_back(std::move(t));
  } while (accept(Tok::Comma));
  return out;
}

}  // namespace b2::frontend
