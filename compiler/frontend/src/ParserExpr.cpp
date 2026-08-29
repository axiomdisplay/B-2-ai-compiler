// B-2 Frontend - parser expressions: precedence climbing, lambdas, casts,
// method references, switch expressions, creations.

#include "b2/frontend/Parser.h"

namespace b2::frontend {

using namespace ast;

// ------------------------------------------------------------ entry point --

std::unique_ptr<Expr> Parser::parseExpression() {
  // An Expression may be a lambda (JLS 15.2).
  if (looksLikeLambdaStart()) return parseLambda();

  auto lhs = parseConditional();
  if (!lhs) return nullptr;

  AssignOp op;
  if (assignOpFromTok(curKind(), op)) {
    advance();
    auto rhs = parseExpression();  // right-associative; lambda allowed
    if (!rhs) return nullptr;
    SourceRange r(lhs->range.offset,
                  rhs->range.offset + rhs->range.length - lhs->range.offset);
    return std::make_unique<Assignment>(r, op, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

std::unique_ptr<Expr> Parser::parseConditional() {
  auto cond = parseBinary(1);
  if (!cond) return nullptr;
  if (at(Tok::Question)) {
    advance();
    auto thenE = parseExpression();  // full Expression per JLS 15.25
    expect(Tok::Colon);
    std::unique_ptr<Expr> elseE;
    if (looksLikeLambdaStart()) {
      elseE = parseLambda();  // lambda is allowed in the tail
    } else {
      elseE = parseConditional();
    }
    if (!thenE || !elseE) return nullptr;
    SourceRange r(cond->range.offset,
                  elseE->range.offset + elseE->range.length - cond->range.offset);
    return std::make_unique<Conditional>(r, std::move(cond), std::move(thenE),
                                         std::move(elseE));
  }
  return cond;
}

std::unique_ptr<Expr> Parser::parseBinary(int minPrec) {
  auto lhs = parseUnary();
  while (lhs) {
    // instanceof sits at the relational precedence (JLS 15.20).
    if (at(Tok::InstanceOfKeyword)) {
      advance();
      auto res = std::make_unique<InstanceOf>(lhs->range);
      res->expression = std::move(lhs);
      if (looksLikePattern()) {
        res->pattern = parsePattern();
      } else {
        res->type = parseType();  // plain `x instanceof Type`
      }
      const std::uint32_t end = cur().offset + cur().length;
      res->range.length = end > res->range.offset ? end - res->range.offset : 0;
      lhs = std::move(res);
      continue;
    }
    BinaryOp op;
    int prec = 0;
    if (!binaryOpFromTok(curKind(), op, prec)) break;
    if (prec < minPrec) break;
    advance();
    auto rhs = parseBinary(prec + 1);  // left-associative
    if (!rhs) break;
    SourceRange r(lhs->range.offset,
                  rhs->range.offset + rhs->range.length - lhs->range.offset);
    lhs = std::make_unique<Binary>(r, op, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

std::unique_ptr<Expr> Parser::parseUnary() {
  const std::uint32_t start = cur().offset;
  UnaryOp op;
  switch (curKind()) {
    case Tok::Plus:
    case Tok::Minus:
    case Tok::Tilde:
    case Tok::Bang:
    case Tok::PlusPlus:
    case Tok::MinusMinus: {
      // Prefix operator (unaryOpFromTok inline).
      switch (curKind()) {
        case Tok::Plus: op = UnaryOp::Plus; break;
        case Tok::Minus: op = UnaryOp::Minus; break;
        case Tok::Tilde: op = UnaryOp::BitNot; break;
        case Tok::Bang: op = UnaryOp::LogicalNot; break;
        case Tok::PlusPlus: op = UnaryOp::PreInc; break;
        default: op = UnaryOp::PreDec; break;
      }
      advance();
      auto operand = parseUnary();  // right-associative
      if (!operand) return nullptr;
      return std::make_unique<PrefixUnary>(rangeFrom(start), op, std::move(operand));
    }
    case Tok::LeftParen:
      if (looksLikeCastAhead()) return parseCast();
      break;
    default:
      break;
  }
  return parsePostfix();
}

// ---------------------------------------------------------------- postfix --

std::unique_ptr<Expr> Parser::parsePostfix() {
  auto e = parsePrimary();
  while (e) {
    if (at(Tok::LeftBracket)) {
      if (peek(1).kind == Tok::RightBracket) {
        // Type-dimension context: `Foo[].class` or `Foo[]::new`.
        auto t = exprToType(std::move(e));
        while (at(Tok::LeftBracket) && peek(1).kind == Tok::RightBracket) {
          const std::uint32_t start = cur().offset;
          advance();
          advance();
          t = std::make_unique<ArrayType>(rangeFrom(start), std::move(t));
        }
        if (at(Tok::ColonColon)) {  // array constructor reference
          advance();
          e = finishMethodReference(std::move(t), {}, /*qualifierIsType=*/true);
          continue;
        }
        expect(Tok::Dot);
        expect(Tok::ClassKeyword);
        e = std::make_unique<ClassLiteral>(rangeFrom(t->range.offset), std::move(t));
        continue;
      }
      advance();
      auto idx = parseExpression();
      expect(Tok::RightBracket);
      if (!idx) break;
      SourceRange r = rangeFrom(e->range.offset);
      e = std::make_unique<ArrayAccess>(r, std::move(e), std::move(idx));
      continue;
    }
    if (at(Tok::PlusPlus) || at(Tok::MinusMinus)) {
      const PostfixOp op = at(Tok::PlusPlus) ? PostfixOp::Inc : PostfixOp::Dec;
      advance();
      e = std::make_unique<PostfixUnary>(rangeFrom(e->range.offset), op, std::move(e));
      continue;
    }
    if (at(Tok::ColonColon)) {
      advance();
      e = finishMethodReference(std::move(e), {});
      continue;
    }
    if (at(Tok::Dot)) {
      advance();
      // .class / .this / .super / .new
      if (at(Tok::ClassKeyword)) {
        advance();
        auto t = exprToType(std::move(e));
        e = std::make_unique<ClassLiteral>(rangeFrom(t->range.offset), std::move(t));
        continue;
      }
      if (at(Tok::ThisKeyword)) {
        advance();
        e = std::make_unique<ThisExpression>(rangeFrom(e->range.offset),
                                             exprToQualifiedName(e.get()));
        continue;
      }
      if (at(Tok::SuperKeyword)) {
        advance();
        e = std::make_unique<SuperAccess>(rangeFrom(e->range.offset),
                                          exprToQualifiedName(e.get()));
        continue;
      }
      if (at(Tok::NewKeyword)) {  // qualified anonymous creation: o.new Inner()
        advance();
        e = parseNew(std::move(e));
        continue;
      }
      if (at(Tok::Less)) {  // e.<T>m(...) - explicit type args after '.' (JLS 15.12)
        const bool ok = speculate([&]() -> bool {
          advance();  // '<'
          std::vector<std::unique_ptr<Type>> tmp;
          parseTypeArgumentsInto(tmp);
          expectGreater();  // closes the type arguments
          if (!at(Tok::Identifier)) return false;
          advance();
          return at(Tok::LeftParen);
        });
        if (ok) {
          advance();  // '<'
          std::vector<std::unique_ptr<Type>> targs;
          parseTypeArgumentsInto(targs);
          expectGreater();
          const Token id = advance();  // copy: the method name
          if (accept(Tok::LeftParen)) {
            auto call = std::make_unique<MethodInvocation>(rangeFrom(id.offset));
            call->target = std::move(e);
            call->hasExplicitTypeArgs = true;
            call->typeArgs = std::move(targs);
            call->name = id.text;
            call->nameRange = id.range();
            parseArgumentList(call->arguments);
            call->range = rangeFrom(id.offset);
            e = std::move(call);
            continue;
          }
        }
        if (!trial_) error("expected a member name after '.'");
        break;
      }
      if (at(Tok::Identifier)) {
        const Token id = advance();  // copy
        // Explicit type arguments: name.<T>m(...) or name.<T>::m
        if (at(Tok::Less)) {
          const bool ok = speculate([&]() -> bool {
            advance();  // '<'
            std::vector<std::unique_ptr<Type>> tmp;
            parseTypeArgumentsInto(tmp);
            expectGreater();  // closes the type arguments
            return at(Tok::LeftParen) || at(Tok::ColonColon);
          });
          if (ok) {
            advance();  // '<'
            std::vector<std::unique_ptr<Type>> targs;
            parseTypeArgumentsInto(targs);
            expectGreater();
            if (accept(Tok::LeftParen)) {
              auto call = std::make_unique<MethodInvocation>(rangeFrom(id.offset));
              call->target = std::move(e);
              call->hasExplicitTypeArgs = true;
              call->typeArgs = std::move(targs);
              call->name = id.text;
              call->nameRange = id.range();
              parseArgumentList(call->arguments);
              call->range = rangeFrom(call->target ? call->target->range.offset : id.offset);
              e = std::move(call);
              continue;
            }
            if (at(Tok::ColonColon)) {
              advance();
              // Build qualifier expression back: target.name
              auto qual = std::make_unique<FieldAccess>(id.range(), std::move(e), id.text,
                                                        id.range());
              e = finishMethodReference(std::move(qual), std::move(targs));
              continue;
            }
          }
        }
        if (at(Tok::LeftParen)) {
          advance();
          auto call = std::make_unique<MethodInvocation>(rangeFrom(id.offset));
          call->target = std::move(e);
          call->name = id.text;
          call->nameRange = id.range();
          parseArgumentList(call->arguments);
          call->range = rangeFrom(call->nameRange.offset);
          e = std::move(call);
          continue;
        }
        if (at(Tok::ColonColon)) {
          advance();
          auto qual = std::make_unique<FieldAccess>(id.range(), std::move(e), id.text,
                                                    id.range());
          e = finishMethodReference(std::move(qual), {});
          continue;
        }
        e = std::make_unique<FieldAccess>(rangeFrom(id.offset), std::move(e), id.text,
                                          id.range());
        continue;
      }
      if (!trial_) error("expected a member name after '.'");
      break;
    }
    break;
  }
  return e;
}

std::unique_ptr<Expr> Parser::finishMethodReference(std::unique_ptr<Node> qualifier,
                                                    std::vector<std::unique_ptr<Type>> typeArgs,
                                                    bool qualifierIsType) {
  auto mr = std::make_unique<MethodReference>(qualifier->range);
  mr->qualifier = std::move(qualifier);
  mr->qualifierIsType = qualifierIsType;
  if (!typeArgs.empty()) {
    mr->hasExplicitTypeArgs = true;
    mr->typeArgs = std::move(typeArgs);
  }
  if (at(Tok::Less)) {  // Type::<T>method - type args after '::' (JLS 15.13)
    advance();
    parseTypeArgumentsInto(mr->typeArgs);
    expectGreater();
    mr->hasExplicitTypeArgs = true;
  }
  if (at(Tok::NewKeyword)) {
    advance();
    mr->isConstructorRef = true;
    mr->name = "new";
  } else if (at(Tok::Identifier)) {
    const Token id = advance();  // copy
    mr->name = id.text;
  } else if (!trial_) {
    error("expected a method name after '::'");
  }
  mr->range = rangeFrom(mr->qualifier->range.offset);
  return mr;
}

// --------------------------------------------------------------- primary ---

std::unique_ptr<Expr> Parser::parsePrimary() {
  const std::uint32_t start = cur().offset;
  switch (curKind()) {
    case Tok::IntegerLiteral:
    case Tok::LongLiteral:
    case Tok::FloatLiteral:
    case Tok::DoubleLiteral:
    case Tok::CharacterLiteral:
    case Tok::StringLiteral:
    case Tok::TrueKeyword:
    case Tok::FalseKeyword:
    case Tok::NullKeyword: {
      const Token t = advance();  // copy
      auto lit = std::make_unique<Literal>(t.range(), LitKind::Null);
      switch (t.kind) {
        case Tok::IntegerLiteral: lit->lit = LitKind::Int; lit->intValue = t.intValue; break;
        case Tok::LongLiteral: lit->lit = LitKind::Long; lit->intValue = t.intValue; break;
        case Tok::FloatLiteral: lit->lit = LitKind::Float; lit->floatValue = t.floatValue; break;
        case Tok::DoubleLiteral: lit->lit = LitKind::Double; lit->floatValue = t.floatValue; break;
        case Tok::CharacterLiteral: lit->lit = LitKind::Char; lit->intValue = t.intValue; break;
        case Tok::StringLiteral: lit->lit = LitKind::String; lit->stringValue = t.text; break;
        case Tok::TrueKeyword: lit->lit = LitKind::Boolean; lit->intValue = 1; break;
        case Tok::FalseKeyword: lit->lit = LitKind::Boolean; lit->intValue = 0; break;
        default: break;  // null
      }
      return lit;
    }

    case Tok::ThisKeyword: {
      advance();
      if (at(Tok::LeftParen)) {  // this(...) constructor invocation
        advance();
        auto ci = std::make_unique<ConstructorInvocation>(rangeFrom(start), false);
        parseArgumentList(ci->arguments);
        ci->range = rangeFrom(start);
        return ci;
      }
      return std::make_unique<ThisExpression>(rangeFrom(start), std::vector<std::string>{});
    }

    case Tok::SuperKeyword: {
      advance();
      if (at(Tok::LeftParen)) {  // super(...) constructor invocation
        advance();
        auto ci = std::make_unique<ConstructorInvocation>(rangeFrom(start), true);
        parseArgumentList(ci->arguments);
        ci->range = rangeFrom(start);
        return ci;
      }
      return std::make_unique<SuperAccess>(rangeFrom(start), std::vector<std::string>{});
    }

    case Tok::LeftParen: {  // parenthesized expr, or a lambda `() -> ...`
      if (looksLikeLambdaStart()) return parseLambda();
      advance();
      auto inner = parseExpression();
      expect(Tok::RightParen);
      if (!inner) return nullptr;
      return std::make_unique<ParenExpression>(rangeFrom(start), std::move(inner));
    }

    case Tok::NewKeyword: {
      advance();
      return parseNew(nullptr);
    }

    case Tok::SwitchKeyword:
      return parseSwitchExpression();

    case Tok::Identifier:
    case Tok::UnderscoreKeyword: {  // `_` as an (unnamed) lambda param marker
      const Token id = advance();  // copy
      // Unqualified call: name(args), generic call: name<T>(args), ref: name::m
      if (at(Tok::LeftParen)) {
        advance();
        auto call = std::make_unique<MethodInvocation>(rangeFrom(start));
        call->name = id.text;
        call->nameRange = id.range();
        parseArgumentList(call->arguments);
        call->range = rangeFrom(start);
        return call;
      }
      if (at(Tok::Less)) {
        const bool ok = speculate([&]() -> bool {
          advance();  // '<'
          std::vector<std::unique_ptr<Type>> tmp;
          parseTypeArgumentsInto(tmp);
          expectGreater();  // closes the type arguments
          return at(Tok::LeftParen) || at(Tok::ColonColon);
        });
        if (ok) {
          advance();  // '<'
          std::vector<std::unique_ptr<Type>> targs;
          parseTypeArgumentsInto(targs);
          expectGreater();
          if (accept(Tok::LeftParen)) {
            auto call = std::make_unique<MethodInvocation>(rangeFrom(start));
            call->hasExplicitTypeArgs = true;
            call->typeArgs = std::move(targs);
            call->name = id.text;
            call->nameRange = id.range();
            parseArgumentList(call->arguments);
            call->range = rangeFrom(start);
            return call;
          }
          if (at(Tok::ColonColon)) {
            advance();
            auto qual = std::make_unique<Name>(id.range(), id.text);
            return finishMethodReference(std::move(qual), std::move(targs));
          }
        }
      }
      if (at(Tok::ColonColon)) {
        advance();
        auto qual = std::make_unique<Name>(id.range(), id.text);
        return finishMethodReference(std::move(qual), {});
      }
      return std::make_unique<Name>(rangeFrom(start), id.text);
    }

    default: {
      // int.class / int[].class / void.class
      PrimitiveKind pk;
      if (primitiveTok(curKind(), pk)) {
        advance();
        std::unique_ptr<Type> t = std::make_unique<PrimitiveType>(rangeFrom(start), pk);
        while (at(Tok::LeftBracket) && peek(1).kind == Tok::RightBracket) {
          advance();
          advance();
          t = std::make_unique<ArrayType>(rangeFrom(start), std::move(t));
        }
        if (at(Tok::ColonColon)) {  // int[]::new - array constructor reference
          advance();
          return finishMethodReference(std::move(t), {}, /*qualifierIsType=*/true);
        }
        expect(Tok::Dot);
        expect(Tok::ClassKeyword);
        return std::make_unique<ClassLiteral>(rangeFrom(start), std::move(t));
      }
      if (!trial_) error("expected an expression, found " + std::string(toString(curKind())));
      return nullptr;
    }
  }
}

// ------------------------------------------------------------------ new ----

std::unique_ptr<Expr> Parser::parseNew(std::unique_ptr<Expr> qualifier) {
  const std::uint32_t start = qualifier ? qualifier->range.offset : cur().offset;
  std::vector<std::unique_ptr<Type>> explicitTypeArgs;
  bool hasTypeArgs = false;
  if (at(Tok::Less)) {  // new <T> Foo(...)
    advance();
    hasTypeArgs = true;
    parseTypeArgumentsInto(explicitTypeArgs);
    expectGreater();
  }

  auto baseType = parseTypeNoArray();
  if (!baseType) return nullptr;

  if (at(Tok::LeftBracket)) {  // array creation: new int[3][] / new int[] {...}
    auto ac = std::make_unique<ArrayCreation>(rangeFrom(start));
    ac->elementType = std::move(baseType);
    while (at(Tok::LeftBracket)) {
      ArrayDim d;
      d.range = cur().range();
      advance();
      if (!at(Tok::RightBracket)) d.size = parseExpression();
      expect(Tok::RightBracket);
      ac->dimensions.push_back(std::move(d));
    }
    if (at(Tok::LeftBrace)) {
      ac->initializer = parseArrayInitializer();
    }
    ac->range = rangeFrom(start);
    return ac;
  }

  auto cre = std::make_unique<ClassInstanceCreation>(rangeFrom(start));
  cre->qualifier = std::move(qualifier);
  cre->hasExplicitTypeArgs = hasTypeArgs;
  cre->typeArgs = std::move(explicitTypeArgs);
  cre->type = std::move(baseType);
  expect(Tok::LeftParen);
  parseArgumentList(cre->arguments);
  if (at(Tok::LeftBrace)) {  // anonymous class body
    auto anon = std::make_unique<ClassDeclaration>(cur().range());
    const std::uint32_t bodyStart = cur().offset;
    expect(Tok::LeftBrace);
    MemberContext ctx;
    parseClassBodyInto(anon->members, ctx);
    expect(Tok::RightBrace);
    anon->range = rangeFrom(bodyStart);
    cre->anonymousBody = std::move(anon);
  }
  cre->range = rangeFrom(start);
  return cre;
}

// -------------------------------------------------------- switch expression -

std::unique_ptr<Expr> Parser::parseSwitchExpression() {
  const std::uint32_t start = cur().offset;
  advance();  // switch
  expect(Tok::LeftParen);
  auto se = std::make_unique<SwitchExpression>(rangeFrom(start));
  se->selector = parseExpression();
  expect(Tok::RightParen);
  expect(Tok::LeftBrace);
  se->cases = parseSwitchCases(true);
  expect(Tok::RightBrace);
  se->range = rangeFrom(start);
  return se;
}

// ----------------------------------------------------------------- lambda --

bool Parser::looksLikeLambdaStart() {
  if (at(Tok::Identifier) && peek(1).kind == Tok::Arrow) return true;
  if (at(Tok::UnderscoreKeyword) && peek(1).kind == Tok::Arrow) return true;
  if (at(Tok::LeftParen)) {
    // Scan to the matching ')'; lambda iff the next token is '->'.
    int depth = 0;
    for (std::size_t i = pos_; i < toks_.size(); ++i) {
      const Tok k = toks_[i].kind;
      if (k == Tok::LeftParen) {
        ++depth;
      } else if (k == Tok::RightParen) {
        --depth;
        if (depth == 0) {
          return i + 1 < toks_.size() && toks_[i + 1].kind == Tok::Arrow;
        }
      } else if (k == Tok::EndOfFile) {
        return false;
      }
    }
  }
  return false;
}

std::unique_ptr<Lambda> Parser::parseLambda() {
  const std::uint32_t start = cur().offset;
  auto l = std::make_unique<Lambda>(rangeFrom(start));

  if (at(Tok::Identifier) || at(Tok::UnderscoreKeyword)) {  // single implicit param
    LambdaParam p;
    p.range = cur().range();
    p.nameRange = cur().range();
    p.name = advance().text;
    l->parenthesized = false;
    l->parameters.push_back(std::move(p));
  } else {
    advance();  // '('
    if (!at(Tok::RightParen)) {
      while (true) {
        const std::size_t before = pos_;
        LambdaParam p;
        p.range = cur().range();
        p.annotations = parseAnnotations();
        p.mods = parseModifiers(false);
        if (at(Tok::Identifier) &&
            (peek(1).kind == Tok::Comma || peek(1).kind == Tok::RightParen)) {
          p.nameRange = cur().range();
          p.name = advance().text;  // inferred-type parameter
        } else {
          p.type = parseType();
          if (accept(Tok::Ellipsis)) { /* varargs lambda params are illegal; tolerate */ }
          if (at(Tok::Identifier)) {
            p.nameRange = cur().range();
            p.name = advance().text;
          } else if (at(Tok::UnderscoreKeyword)) {
            p.nameRange = cur().range();
            p.name = advance().text;
          } else if (!trial_) {
            error("expected a lambda parameter name");
            break;
          }
        }
        l->parameters.push_back(std::move(p));
        if (!accept(Tok::Comma)) break;
        if (!ensureProgress(before)) break;
      }
    }
    expect(Tok::RightParen);
  }

  expect(Tok::Arrow);
  if (at(Tok::LeftBrace)) {
    l->body = parseBlock();
  } else {
    l->body = parseExpression();
  }
  l->range = rangeFrom(start);
  return l;
}

// ------------------------------------------------------------------- cast --

bool Parser::looksLikeCastAhead() {
  return speculate([&]() -> bool {
    if (!at(Tok::LeftParen)) return false;
    advance();
    // A primitive cast is always a cast: (int) x.
    PrimitiveKind pk;
    if (primitiveTok(curKind(), pk)) {
      advance();
      while (at(Tok::LeftBracket)) {
        advance();
        if (!at(Tok::RightBracket)) return false;
        advance();
      }
      if (!at(Tok::RightParen)) return false;
      advance();
      return canStartUnary(curKind());
    }
    auto t = parseType();
    if (!t) return false;
    while (at(Tok::Amp)) {  // intersection cast: (A & B) x
      advance();
      t = parseType();
      if (!t) return false;
    }
    if (!at(Tok::RightParen)) return false;
    advance();
    // Reference casts must be followed by UnaryExpressionNotPlusMinus
    // (JLS 15.16): `(a) + b` is arithmetic, not a cast.
    return canStartUnaryNotPlusMinus(curKind());
  });
}

std::unique_ptr<Expr> Parser::parseCast() {
  const std::uint32_t start = cur().offset;
  advance();  // '('
  std::vector<std::unique_ptr<Type>> types;
  types.push_back(parseType());
  while (at(Tok::Amp)) {
    advance();
    types.push_back(parseType());
  }
  std::unique_ptr<Type> t;
  if (types.size() > 1) {
    t = std::make_unique<IntersectionType>(rangeFrom(start), std::move(types));
  } else {
    t = std::move(types.front());
  }
  expect(Tok::RightParen);
  auto e = parseUnary();
  if (!e) return nullptr;
  return std::make_unique<Cast>(rangeFrom(start), std::move(t), std::move(e));
}

// ---------------------------------------------------------------- helpers --

void Parser::parseArgumentList(std::vector<std::unique_ptr<Expr>>& out) {
  if (!at(Tok::RightParen)) {
    while (true) {
      const std::size_t before = pos_;
      auto a = parseExpression();
      if (a) out.push_back(std::move(a));
      if (!accept(Tok::Comma)) break;
      if (!ensureProgress(before)) break;
    }
  }
  expect(Tok::RightParen);
}

bool Parser::assignOpFromTok(Tok k, AssignOp& out) noexcept {
  switch (k) {
    case Tok::Equal: out = AssignOp::Assign; return true;
    case Tok::PlusEqual: out = AssignOp::AddA; return true;
    case Tok::MinusEqual: out = AssignOp::SubA; return true;
    case Tok::StarEqual: out = AssignOp::MulA; return true;
    case Tok::SlashEqual: out = AssignOp::DivA; return true;
    case Tok::PercentEqual: out = AssignOp::ModA; return true;
    case Tok::AmpEqual: out = AssignOp::AndA; return true;
    case Tok::BarEqual: out = AssignOp::OrA; return true;
    case Tok::CaretEqual: out = AssignOp::XorA; return true;
    case Tok::LessLessEqual: out = AssignOp::ShlA; return true;
    case Tok::GreaterGreaterEqual: out = AssignOp::ShrA; return true;
    case Tok::GreaterGreaterGreaterEqual: out = AssignOp::UShrA; return true;
    default: return false;
  }
}

bool Parser::binaryOpFromTok(Tok k, BinaryOp& out, int& prec) noexcept {
  switch (k) {
    case Tok::BarBar: out = BinaryOp::COr; prec = 1; return true;
    case Tok::AmpAmp: out = BinaryOp::CAnd; prec = 2; return true;
    case Tok::Bar: out = BinaryOp::Or; prec = 3; return true;
    case Tok::Caret: out = BinaryOp::Xor; prec = 4; return true;
    case Tok::Amp: out = BinaryOp::And; prec = 5; return true;
    case Tok::EqualEqual: out = BinaryOp::Eq; prec = 6; return true;
    case Tok::BangEqual: out = BinaryOp::Ne; prec = 6; return true;
    case Tok::Less: out = BinaryOp::Lt; prec = 7; return true;
    case Tok::Greater: out = BinaryOp::Gt; prec = 7; return true;
    case Tok::LessEqual: out = BinaryOp::Le; prec = 7; return true;
    case Tok::GreaterEqual: out = BinaryOp::Ge; prec = 7; return true;
    case Tok::LessLess: out = BinaryOp::Shl; prec = 8; return true;
    case Tok::GreaterGreater: out = BinaryOp::Shr; prec = 8; return true;
    case Tok::GreaterGreaterGreater: out = BinaryOp::UShr; prec = 8; return true;
    case Tok::Plus: out = BinaryOp::Add; prec = 9; return true;
    case Tok::Minus: out = BinaryOp::Sub; prec = 9; return true;
    case Tok::Star: out = BinaryOp::Mul; prec = 10; return true;
    case Tok::Slash: out = BinaryOp::Div; prec = 10; return true;
    case Tok::Percent: out = BinaryOp::Mod; prec = 10; return true;
    default: return false;
  }
}

bool Parser::canStartUnary(Tok k) noexcept {
  switch (k) {
    case Tok::IntegerLiteral:
    case Tok::LongLiteral:
    case Tok::FloatLiteral:
    case Tok::DoubleLiteral:
    case Tok::CharacterLiteral:
    case Tok::StringLiteral:
    case Tok::TrueKeyword:
    case Tok::FalseKeyword:
    case Tok::NullKeyword:
    case Tok::Identifier:
    case Tok::UnderscoreKeyword:
    case Tok::ThisKeyword:
    case Tok::SuperKeyword:
    case Tok::NewKeyword:
    case Tok::SwitchKeyword:
    case Tok::LeftParen:
    case Tok::Bang:
    case Tok::Tilde:
    case Tok::Plus:
    case Tok::Minus:
    case Tok::PlusPlus:
    case Tok::MinusMinus:
    case Tok::BooleanKeyword:
    case Tok::ByteKeyword:
    case Tok::ShortKeyword:
    case Tok::CharKeyword:
    case Tok::IntKeyword:
    case Tok::LongKeyword:
    case Tok::FloatKeyword:
    case Tok::DoubleKeyword:
      return true;
    default:
      return false;
  }
}

bool Parser::canStartUnaryNotPlusMinus(Tok k) noexcept {
  switch (k) {
    case Tok::Plus:
    case Tok::Minus:
    case Tok::PlusPlus:
    case Tok::MinusMinus:
      return false;  // (Type) + x is arithmetic, not a cast (JLS 15.16)
    default:
      return canStartUnary(k);
  }
}

bool Parser::isVarType(const Type* t) noexcept {
  if (!t || t->kind != NodeKind::ClassType) return false;
  const auto* ct = static_cast<const ClassType*>(t);
  return ct->segments.size() == 1 && !ct->segments[0].hasTypeArgs &&
         ct->segments[0].name == "var";
}

std::unique_ptr<Type> Parser::exprToType(std::unique_ptr<Expr> e) {
  std::vector<ClassType::Segment> segments;
  std::vector<std::string> parts = exprToQualifiedName(e.get());
  for (auto& p : parts) {
    ClassType::Segment seg;
    seg.name = std::move(p);
    seg.range = e->range;
    segments.push_back(std::move(seg));
  }
  if (segments.empty()) {  // not a plain name chain; keep a placeholder
    ClassType::Segment seg;
    seg.range = e->range;
    segments.push_back(std::move(seg));
  }
  SourceRange r = e->range;
  auto ct = std::make_unique<ClassType>(r, std::move(segments));
  return ct;
}

std::vector<std::string> Parser::exprToQualifiedName(const Expr* e) {
  std::vector<std::string> parts;
  while (e) {
    if (e->kind == NodeKind::Name) {
      parts.insert(parts.begin(), static_cast<const Name*>(e)->identifier);
      return parts;
    }
    if (e->kind == NodeKind::FieldAccess) {
      const auto* fa = static_cast<const FieldAccess*>(e);
      parts.insert(parts.begin(), fa->name);
      e = fa->target.get();
      continue;
    }
    return {};  // not a plain qualified name
  }
  return parts;
}

}  // namespace b2::frontend
