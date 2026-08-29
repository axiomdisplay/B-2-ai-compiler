// B-2 Frontend - parser statements: blocks, control flow, switch, try.

#include "b2/frontend/Parser.h"

namespace b2::frontend {

using namespace ast;

// ---------------------------------------------------------------- blocks ----

std::unique_ptr<Block> Parser::parseBlock() {
  const std::uint32_t start = cur().offset;
  const Token lb = expect(Tok::LeftBrace);
  auto b = std::make_unique<Block>(lb.range());
  while (!at(Tok::RightBrace) && !at(Tok::EndOfFile)) {
    const std::size_t before = pos_;
    auto s = parseStatement();
    if (s) b->statements.push_back(std::move(s));
    if (!ensureProgress(before)) break;
  }
  expect(Tok::RightBrace);
  b->range = rangeFrom(start);
  return b;
}

// ------------------------------------------------------- look-ahead checks --

bool Parser::looksLikeLocalVarDecl() {
  if (at(Tok::FinalKeyword) || at(Tok::At)) {
    // @Anno int x / final int x - but `@interface` starts a local type decl.
    if (at(Tok::At) && peek(1).kind == Tok::InterfaceKeyword) return false;
    return true;
  }
  PrimitiveKind pk;
  if (!primitiveTok(curKind(), pk)) {
    if (!at(Tok::Identifier)) return false;
    // `record`/`sealed`... contextual: `record` starts a local record decl,
    // handled elsewhere; anything else may be a type name.
  }
  return speculate([&]() -> bool {
    parseAnnotations();
    parseModifiers(false);
    auto t = parseType();
    if (!t || !at(Tok::Identifier)) return false;
    advance();  // the declarator name
    // Follower check (JLS 14.4 vs 14.8): `grid[row][col] = 7` must NOT be a
    // declaration. After the name only '=', ';', ',' or C-style `[]` dims
    // may follow.
    if (at(Tok::Equal) || at(Tok::Semicolon) || at(Tok::Comma)) return true;
    if (at(Tok::LeftBracket)) {
      while (at(Tok::LeftBracket) && peek(1).kind == Tok::RightBracket) {
        advance();
        advance();
      }
      return at(Tok::Equal) || at(Tok::Semicolon) || at(Tok::Comma);
    }
    return false;
  });
}

bool Parser::looksLikeLocalTypeDecl() {
  if (at(Tok::ClassKeyword) || at(Tok::InterfaceKeyword) || at(Tok::EnumKeyword))
    return true;
  if (atContextual("record") && peek(1).kind == Tok::Identifier) return true;
  if (at(Tok::At) && peek(1).kind == Tok::InterfaceKeyword) return true;
  if (at(Tok::At)) {  // maybe @Anno @Anno class C {}
    return speculate([&]() -> bool {
      parseAnnotations();
      parseModifiers(false);
      return at(Tok::ClassKeyword) || at(Tok::InterfaceKeyword) || at(Tok::EnumKeyword) ||
             (atContextual("record") && peek(1).kind == Tok::Identifier) ||
             (at(Tok::At) && peek(1).kind == Tok::InterfaceKeyword);
    });
  }
  return false;
}

std::unique_ptr<Stmt> Parser::parseLocalVariableDeclaration(bool consumeSemi) {
  const std::uint32_t start = cur().offset;
  auto d = std::make_unique<LocalVariableDeclaration>(rangeFrom(start));
  d->annotations = parseAnnotations();
  d->mods = parseModifiers(false);
  d->type = parseType();
  d->isVar = isVarType(d->type.get());
  parseVariableDeclaratorList(d->declarators);
  if (consumeSemi) expect(Tok::Semicolon);
  d->range = rangeFrom(start);
  return d;
}

// ------------------------------------------------------------ statements ----

std::unique_ptr<Stmt> Parser::parseStatement() {
  const std::uint32_t start = cur().offset;

  switch (curKind()) {
    case Tok::LeftBrace:
      return parseBlock();
    case Tok::Semicolon: {
      advance();
      return std::make_unique<EmptyStatement>(rangeFrom(start));
    }
    case Tok::IfKeyword:
      return parseIf();
    case Tok::WhileKeyword:
      return parseWhile();
    case Tok::DoKeyword:
      return parseDoWhile();
    case Tok::ForKeyword:
      return parseFor();
    case Tok::TryKeyword:
      return parseTry();
    case Tok::SwitchKeyword:
      return parseSwitchStatement();
    case Tok::ReturnKeyword: {
      advance();
      auto r = std::make_unique<Return>(rangeFrom(start));
      if (!at(Tok::Semicolon)) r->expression = parseExpression();
      expect(Tok::Semicolon);
      r->range = rangeFrom(start);
      return r;
    }
    case Tok::ThrowKeyword: {
      advance();
      auto t = std::make_unique<Throw>(rangeFrom(start), parseExpression());
      expect(Tok::Semicolon);
      t->range = rangeFrom(start);
      return t;
    }
    case Tok::BreakKeyword: {
      advance();
      auto b = std::make_unique<BreakStatement>(rangeFrom(start));
      if (at(Tok::Identifier)) {
        b->hasLabel = true;
        b->label = advance().text;
      }
      expect(Tok::Semicolon);
      b->range = rangeFrom(start);
      return b;
    }
    case Tok::ContinueKeyword: {
      advance();
      auto c = std::make_unique<ContinueStatement>(rangeFrom(start));
      if (at(Tok::Identifier)) {
        c->hasLabel = true;
        c->label = advance().text;
      }
      expect(Tok::Semicolon);
      c->range = rangeFrom(start);
      return c;
    }
    case Tok::SynchronizedKeyword: {
      advance();
      expect(Tok::LeftParen);
      auto s = std::make_unique<Synchronized>(rangeFrom(start));
      s->lock = parseExpression();
      expect(Tok::RightParen);
      s->body = parseBlock();
      s->range = rangeFrom(start);
      return s;
    }
    case Tok::AssertKeyword: {
      advance();
      auto a = std::make_unique<AssertStatement>(rangeFrom(start));
      a->condition = parseExpression();
      if (accept(Tok::Colon)) a->message = parseExpression();
      expect(Tok::Semicolon);
      a->range = rangeFrom(start);
      return a;
    }
    default:
      break;
  }

  // Local type declaration (class/interface/enum/record/@interface)?
  if (looksLikeLocalTypeDecl()) {
    const std::uint32_t startDecl = cur().offset;
    std::vector<Annotation> annos = parseAnnotations();
    std::uint32_t mods = parseModifiers(false);
    auto decl = parseTypeDeclCommon(startDecl, mods, std::move(annos), MemberContext{});
    return std::make_unique<LocalTypeDeclaration>(rangeFrom(startDecl), std::move(decl));
  }

  // yield (contextual, only inside a switch expression)?
  if (switchExprDepth_ > 0 && atContextual("yield") && canStartUnary(peek(1).kind)) {
    advance();  // yield
    auto y = std::make_unique<Yield>(rangeFrom(start), parseExpression());
    expect(Tok::Semicolon);
    y->range = rangeFrom(start);
    return y;
  }

  // Labeled statement: `name:`
  if (at(Tok::Identifier) && peek(1).kind == Tok::Colon) {
    const std::string label = advance().text;
    advance();  // ':'
    auto inner = parseStatement();
    auto l = std::make_unique<LabeledStatement>(rangeFrom(start), label, std::move(inner));
    l->range = rangeFrom(start);
    return l;
  }

  // Local variable declaration or expression statement.
  if (looksLikeLocalVarDecl()) {
    return parseLocalVariableDeclaration(true);
  }

  auto e = parseExpression();
  if (e) {
    expect(Tok::Semicolon);
    auto s = std::make_unique<ExpressionStatement>(rangeFrom(start), std::move(e));
    s->range = rangeFrom(start);
    return s;
  }

  // Could not parse anything: recover.
  if (!at(Tok::EndOfFile)) {
    if (!trial_) error("expected a statement");
    syncToStatementEnd();
  }
  return nullptr;
}

std::unique_ptr<Stmt> Parser::parseIf() {
  const std::uint32_t start = cur().offset;
  advance();  // if
  expect(Tok::LeftParen);
  auto s = std::make_unique<If>(rangeFrom(start));
  s->condition = parseExpression();
  expect(Tok::RightParen);
  s->thenStmt = parseStatement();
  if (accept(Tok::ElseKeyword)) s->elseStmt = parseStatement();
  s->range = rangeFrom(start);
  return s;
}

std::unique_ptr<Stmt> Parser::parseWhile() {
  const std::uint32_t start = cur().offset;
  advance();  // while
  expect(Tok::LeftParen);
  auto s = std::make_unique<While>(rangeFrom(start));
  s->condition = parseExpression();
  expect(Tok::RightParen);
  s->body = parseStatement();
  s->range = rangeFrom(start);
  return s;
}

std::unique_ptr<Stmt> Parser::parseDoWhile() {
  const std::uint32_t start = cur().offset;
  advance();  // do
  auto s = std::make_unique<DoWhile>(rangeFrom(start));
  s->body = parseStatement();
  expect(Tok::WhileKeyword);
  expect(Tok::LeftParen);
  s->condition = parseExpression();
  expect(Tok::RightParen);
  expect(Tok::Semicolon);
  s->range = rangeFrom(start);
  return s;
}

std::unique_ptr<Stmt> Parser::parseFor() {
  const std::uint32_t start = cur().offset;
  advance();  // for
  expect(Tok::LeftParen);

  // Enhanced for?  for ( [annos] [final] Type name : expr )
  const bool enhanced = speculate([&]() -> bool {
    parseAnnotations();
    parseModifiers(false);
    auto t = parseType();
    if (!t) return false;
    if (!at(Tok::Identifier)) return false;
    advance();
    return at(Tok::Colon);
  });

  if (enhanced) {
    auto ef = std::make_unique<EnhancedFor>(rangeFrom(start));
    ef->annotations = parseAnnotations();
    ef->mods = parseModifiers(false);
    ef->type = parseType();
    ef->isVar = isVarType(ef->type.get());
    if (at(Tok::Identifier)) {
      ef->nameRange = cur().range();
      ef->name = advance().text;
    } else if (!trial_) {
      error("expected a loop variable name");
    }
    expect(Tok::Colon);
    ef->iterable = parseExpression();
    expect(Tok::RightParen);
    ef->body = parseStatement();
    ef->range = rangeFrom(start);
    return ef;
  }

  auto f = std::make_unique<BasicFor>(rangeFrom(start));
  // Init: local var decl or expression list (or empty).
  if (!at(Tok::Semicolon)) {
    if (looksLikeLocalVarDecl()) {
      f->init.push_back(parseLocalVariableDeclaration(false));
    } else {
      while (true) {
        auto e = parseExpression();
        if (e) {
          f->init.push_back(std::make_unique<ExpressionStatement>(e->range, std::move(e)));
        }
        if (!accept(Tok::Comma)) break;
      }
    }
  }
  expect(Tok::Semicolon);
  if (!at(Tok::Semicolon)) f->condition = parseExpression();
  expect(Tok::Semicolon);
  if (!at(Tok::RightParen)) {
    while (true) {
      auto u = parseExpression();
      if (u) f->update.push_back(std::move(u));
      if (!accept(Tok::Comma)) break;
    }
  }
  expect(Tok::RightParen);
  f->body = parseStatement();
  f->range = rangeFrom(start);
  return f;
}

std::unique_ptr<Stmt> Parser::parseTry() {
  const std::uint32_t start = cur().offset;
  advance();  // try
  auto t = std::make_unique<Try>(rangeFrom(start));

  if (accept(Tok::LeftParen)) {  // resources
    while (!at(Tok::RightParen) && !at(Tok::EndOfFile)) {
      const std::size_t before = pos_;
      TryResource res;
      res.range = cur().range();
      if (looksLikeLocalVarDecl()) {
        res.isDeclaration = true;
        // parseLocalVariableDeclaration returns the Stmt base; cast down.
        res.decl.reset(
            static_cast<LocalVariableDeclaration*>(parseLocalVariableDeclaration(false).release()));
      } else {
        res.access = parseExpression();
      }
      t->resources.push_back(std::move(res));
      if (!accept(Tok::Semicolon)) break;
      if (!ensureProgress(before)) break;
    }
    expect(Tok::RightParen);
  }

  t->block = parseBlock();

  while (at(Tok::CatchKeyword)) {
    advance();
    expect(Tok::LeftParen);
    CatchClause cc;
    cc.range = cur().range();
    cc.annotations = parseAnnotations();
    cc.mods = parseModifiers(false);
    std::vector<std::unique_ptr<Type>> types;
    auto first = parseType();
    if (first) types.push_back(std::move(first));
    while (accept(Tok::Bar)) {
      auto u = parseType();
      if (u) types.push_back(std::move(u));
    }
    if (types.size() > 1) {
      cc.type = std::make_unique<UnionType>(rangeFrom(start), std::move(types));
    } else if (!types.empty()) {
      cc.type = std::move(types.front());
    }
    if (at(Tok::Identifier)) {
      cc.nameRange = cur().range();
      cc.name = advance().text;
    } else if (!trial_) {
      error("expected a catch parameter name");
    }
    expect(Tok::RightParen);
    cc.body = parseBlock();
    t->catches.push_back(std::move(cc));
  }

  if (accept(Tok::FinallyKeyword)) t->finallyBlock = parseBlock();
  t->range = rangeFrom(start);
  return t;
}

// ---------------------------------------------------------------- switch ----

std::unique_ptr<Stmt> Parser::parseSwitchStatement() {
  const std::uint32_t start = cur().offset;
  advance();  // switch
  expect(Tok::LeftParen);
  auto s = std::make_unique<SwitchStatement>(rangeFrom(start));
  s->selector = parseExpression();
  expect(Tok::RightParen);
  expect(Tok::LeftBrace);
  s->cases = parseSwitchCases(false);
  expect(Tok::RightBrace);
  s->range = rangeFrom(start);
  return s;
}

std::vector<SwitchCase> Parser::parseSwitchCases(bool inExpression) {
  std::vector<SwitchCase> out;
  if (inExpression) ++switchExprDepth_;  // `yield` is contextual (JLS 3.8)
  while (at(Tok::CaseKeyword) || at(Tok::DefaultKeyword)) {
    SwitchCase c;
    c.range = cur().range();
    const bool wasDefault = curKind() == Tok::DefaultKeyword;
    advance();  // case / default
    if (wasDefault) {
      SwitchLabel lbl;
      lbl.range = c.range;
      lbl.isDefault = true;
      c.labels.push_back(std::move(lbl));
    } else {
      while (true) {
        c.labels.push_back(parseSwitchLabel());
        if (!accept(Tok::Comma)) break;
      }
    }
    if (atContextual("when")) {
      advance();
      c.guard = parseExpression();
    }
    if (accept(Tok::Arrow)) {
      c.isRule = true;
      if (at(Tok::LeftBrace)) {
        c.ruleBody = parseBlock();
      } else if (at(Tok::ThrowKeyword)) {
        c.ruleBody = parseStatement();  // Throw (consumes its own ';')
      } else {
        c.ruleBody = parseExpression();
        expect(Tok::Semicolon);  // JLS 14.11: SwitchRule ends `-> Expression ;`
      }
    } else {
      expect(Tok::Colon);
      while (!at(Tok::CaseKeyword) && !at(Tok::DefaultKeyword) &&
             !at(Tok::RightBrace) && !at(Tok::EndOfFile)) {
        const std::size_t before = pos_;
        auto s = parseStatement();
        if (s) c.statements.push_back(std::move(s));
        if (!ensureProgress(before)) break;
      }
    }
    out.push_back(std::move(c));
  }
  if (inExpression) --switchExprDepth_;
  return out;
}

SwitchLabel Parser::parseSwitchLabel() {
  SwitchLabel lbl;
  lbl.range = cur().range();
  if (at(Tok::DefaultKeyword)) {
    advance();
    lbl.isDefault = true;
    return lbl;
  }
  if (at(Tok::NullKeyword)) {
    advance();
    lbl.isNull = true;
    return lbl;
  }
  if (looksLikePattern()) {
    lbl.pattern = parsePattern();
    return lbl;
  }
  lbl.constant = parseConditional();  // enum refs, literals, arithmetic
  return lbl;
}

// --------------------------------------------------------------- patterns ---

bool Parser::looksLikePattern() {
  if (at(Tok::UnderscoreKeyword)) return true;  // `case _`
  return speculate([&]() -> bool {
    parseAnnotations();
    auto t = parseType();
    if (!t) return false;
    return at(Tok::Identifier) || at(Tok::UnderscoreKeyword) || at(Tok::LeftParen);
  });
}

std::unique_ptr<Pattern> Parser::parsePattern() {
  const std::uint32_t start = cur().offset;
  if (at(Tok::UnderscoreKeyword)) {  // any pattern
    advance();
    return std::make_unique<AnyPattern>(rangeFrom(start));
  }
  parseAnnotations();  // annotations live on the type
  auto type = parseType();
  if (!type) return nullptr;

  if (at(Tok::LeftParen)) {  // record pattern: Type(Pattern, ...)
    advance();
    std::vector<std::unique_ptr<Pattern>> components;
    while (!at(Tok::RightParen) && !at(Tok::EndOfFile)) {
      const std::size_t before = pos_;
      auto p = parsePattern();
      if (p) components.push_back(std::move(p));
      if (!accept(Tok::Comma)) break;
      if (!ensureProgress(before)) break;
    }
    expect(Tok::RightParen);
    return std::make_unique<RecordPattern>(rangeFrom(start), std::move(type),
                                           std::move(components));
  }

  std::string binding;
  SourceRange bindingRange;
  if (at(Tok::Identifier)) {
    const Token id = advance();  // copy
    binding = id.text;
    bindingRange = id.range();
  } else if (at(Tok::UnderscoreKeyword)) {  // unnamed pattern variable
    bindingRange = advance().range();
    binding = "_";
  }
  return std::make_unique<TypePattern>(rangeFrom(start), std::move(type), binding,
                                       bindingRange);
}

}  // namespace b2::frontend
