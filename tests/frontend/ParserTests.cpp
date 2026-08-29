// B-2 Frontend unit tests - parser: token stream -> AST shape (JLS chapters
// 7-19).
//
// Each test parses a minimal, complete Java program through the full
// lexer+parser stack (b2::test::ParseSession) and asserts the AST structure:
// node kinds, names, modifiers, and the classic ambiguity resolutions (cast
// vs parenthesized expression, lambda vs expression, generic call vs
// less-than, patterns). The parser is syntax-only, so unknown types
// (java.util.function.Function, Runnable, ...) are fine.

#include <string>
#include <vector>

#include "ParseUtil.h"
#include "TestHarness.h"

namespace {

namespace ast = b2::frontend::ast;
using b2::test::ParseSession;

// ------------------------------------------------------- downcast helpers --

bool isKind(const ast::Node* n, ast::NodeKind k) { return n != nullptr && n->kind == k; }

// One overload per AST node type used in these tests; as<T> checks the node
// kind before the static_cast so a mismatch yields nullptr instead of a bad
// cast.
constexpr ast::NodeKind kindOf(const ast::ArrayCreation*) { return ast::NodeKind::ArrayCreation; }
constexpr ast::NodeKind kindOf(const ast::ArrayAccess*) { return ast::NodeKind::ArrayAccess; }
constexpr ast::NodeKind kindOf(const ast::ArrayType*) { return ast::NodeKind::ArrayType; }
constexpr ast::NodeKind kindOf(const ast::Assignment*) { return ast::NodeKind::Assignment; }
constexpr ast::NodeKind kindOf(const ast::Binary*) { return ast::NodeKind::Binary; }
constexpr ast::NodeKind kindOf(const ast::Block*) { return ast::NodeKind::Block; }
constexpr ast::NodeKind kindOf(const ast::BreakStatement*) { return ast::NodeKind::BreakStatement; }
constexpr ast::NodeKind kindOf(const ast::Cast*) { return ast::NodeKind::Cast; }
constexpr ast::NodeKind kindOf(const ast::ClassDeclaration*) { return ast::NodeKind::ClassDeclaration; }
constexpr ast::NodeKind kindOf(const ast::ClassInstanceCreation*) {
  return ast::NodeKind::ClassInstanceCreation;
}
constexpr ast::NodeKind kindOf(const ast::ClassType*) { return ast::NodeKind::ClassType; }
constexpr ast::NodeKind kindOf(const ast::ConstructorDeclaration*) {
  return ast::NodeKind::ConstructorDeclaration;
}
constexpr ast::NodeKind kindOf(const ast::ContinueStatement*) { return ast::NodeKind::ContinueStatement; }
constexpr ast::NodeKind kindOf(const ast::EnhancedFor*) { return ast::NodeKind::EnhancedFor; }
constexpr ast::NodeKind kindOf(const ast::EnumDeclaration*) { return ast::NodeKind::EnumDeclaration; }
constexpr ast::NodeKind kindOf(const ast::ExpressionStatement*) {
  return ast::NodeKind::ExpressionStatement;
}
constexpr ast::NodeKind kindOf(const ast::FieldAccess*) { return ast::NodeKind::FieldAccess; }
constexpr ast::NodeKind kindOf(const ast::FieldDeclaration*) { return ast::NodeKind::FieldDeclaration; }
constexpr ast::NodeKind kindOf(const ast::If*) { return ast::NodeKind::If; }
constexpr ast::NodeKind kindOf(const ast::InstanceOf*) { return ast::NodeKind::InstanceOf; }
constexpr ast::NodeKind kindOf(const ast::InterfaceDeclaration*) {
  return ast::NodeKind::InterfaceDeclaration;
}
constexpr ast::NodeKind kindOf(const ast::LabeledStatement*) { return ast::NodeKind::LabeledStatement; }
constexpr ast::NodeKind kindOf(const ast::Lambda*) { return ast::NodeKind::Lambda; }
constexpr ast::NodeKind kindOf(const ast::Literal*) { return ast::NodeKind::Literal; }
constexpr ast::NodeKind kindOf(const ast::LocalTypeDeclaration*) {
  return ast::NodeKind::LocalTypeDeclaration;
}
constexpr ast::NodeKind kindOf(const ast::LocalVariableDeclaration*) {
  return ast::NodeKind::LocalVariableDeclaration;
}
constexpr ast::NodeKind kindOf(const ast::MethodDeclaration*) { return ast::NodeKind::MethodDeclaration; }
constexpr ast::NodeKind kindOf(const ast::MethodInvocation*) { return ast::NodeKind::MethodInvocation; }
constexpr ast::NodeKind kindOf(const ast::MethodReference*) { return ast::NodeKind::MethodReference; }
constexpr ast::NodeKind kindOf(const ast::Name*) { return ast::NodeKind::Name; }
constexpr ast::NodeKind kindOf(const ast::ParenExpression*) { return ast::NodeKind::ParenExpression; }
constexpr ast::NodeKind kindOf(const ast::PrimitiveType*) { return ast::NodeKind::PrimitiveType; }
constexpr ast::NodeKind kindOf(const ast::RecordDeclaration*) { return ast::NodeKind::RecordDeclaration; }
constexpr ast::NodeKind kindOf(const ast::RecordPattern*) { return ast::NodeKind::RecordPattern; }
constexpr ast::NodeKind kindOf(const ast::Return*) { return ast::NodeKind::Return; }
constexpr ast::NodeKind kindOf(const ast::SwitchExpression*) { return ast::NodeKind::SwitchExpression; }
constexpr ast::NodeKind kindOf(const ast::SwitchStatement*) { return ast::NodeKind::SwitchStatement; }
constexpr ast::NodeKind kindOf(const ast::Try*) { return ast::NodeKind::Try; }
constexpr ast::NodeKind kindOf(const ast::TypePattern*) { return ast::NodeKind::TypePattern; }
constexpr ast::NodeKind kindOf(const ast::UnionType*) { return ast::NodeKind::UnionType; }
constexpr ast::NodeKind kindOf(const ast::Yield*) { return ast::NodeKind::Yield; }

template <typename T>
T* as(ast::Node* n) {
  if (n == nullptr || n->kind != kindOf(static_cast<T*>(nullptr))) return nullptr;
  return static_cast<T*>(n);
}

// ------------------------------------------------------- navigation helpers -

ast::ClassDeclaration* classAt(ParseSession& s, std::size_t i) {
  if (!s.unit || s.unit->types.size() <= i) return nullptr;
  return as<ast::ClassDeclaration>(s.unit->types[i].get());
}

ast::MethodDeclaration* methodNamed(ast::ClassDeclaration* cd, const std::string& name) {
  if (cd == nullptr) return nullptr;
  for (auto& m : cd->members) {
    if (auto* md = as<ast::MethodDeclaration>(m.get()); md != nullptr && md->name == name) {
      return md;
    }
  }
  return nullptr;
}

ast::FieldDeclaration* fieldNamed(ast::ClassDeclaration* cd, const std::string& name) {
  if (cd == nullptr) return nullptr;
  for (auto& m : cd->members) {
    if (auto* fd = as<ast::FieldDeclaration>(m.get()); fd != nullptr && !fd->declarators.empty() &&
        fd->declarators[0].name == name) {
      return fd;
    }
  }
  return nullptr;
}

ast::Block* methodBody(ast::MethodDeclaration* md) { return md ? md->body.get() : nullptr; }

// First initializer of a field / local declaration (nullptr-safe).
ast::Expr* firstInit(ast::FieldDeclaration* fd) {
  if (fd == nullptr || fd->declarators.empty()) return nullptr;
  return fd->declarators[0].initializer.get();
}

ast::Expr* firstInit(ast::LocalVariableDeclaration* ld) {
  if (ld == nullptr || ld->declarators.empty()) return nullptr;
  return ld->declarators[0].initializer.get();
}

// --------------------------------------------------------------- the tests --

B2_TEST(parser_hello_world_shape) {
  ParseSession s;
  s.run("HelloWorld.java",
        "class HelloWorld {\n"
        "    public static void main(String[] args) {\n"
        "        System.out.println(\"Hello, World!\");\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  CHECK_MSG(cd != nullptr, "expected one top-level ClassDeclaration");
  if (cd == nullptr) return;
  CHECK_MSG(cd->name == "HelloWorld", "class name should be HelloWorld");
  CHECK_MSG(cd->members.size() == 1, "one member expected");
  auto* md = as<ast::MethodDeclaration>(cd->members[0].get());
  CHECK_MSG(md != nullptr, "member should be a MethodDeclaration");
  if (md == nullptr) return;
  CHECK_MSG(md->name == "main", "method name should be main");
  CHECK_MSG((md->mods & ast::ModPublic) != 0, "main should be public");
  CHECK_MSG((md->mods & ast::ModStatic) != 0, "main should be static");
  CHECK_MSG(md->parameters.size() == 1, "main takes one parameter");
  CHECK_MSG(isKind(md->parameters[0].type.get(), ast::NodeKind::ArrayType),
            "String[] parameter should be an ArrayType");
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "method body should be a Block");
  if (block == nullptr) return;
  CHECK_MSG(block->statements.size() == 1, "body has one statement");
  auto* es = as<ast::ExpressionStatement>(block->statements[0].get());
  CHECK_MSG(es != nullptr, "expected an ExpressionStatement");
  if (es == nullptr) return;
  auto* call = as<ast::MethodInvocation>(es->expression.get());
  CHECK_MSG(call != nullptr, "expected a MethodInvocation, got kind " +
                                 std::to_string(static_cast<int>(es->expression->kind)));
  if (call == nullptr) return;
  CHECK_MSG(call->name == "println", "called method should be println");
  CHECK_MSG(call->arguments.size() == 1, "println takes one argument");
  auto* fa = as<ast::FieldAccess>(call->target.get());
  CHECK_MSG(fa != nullptr, "System.out should be a FieldAccess");
  if (fa != nullptr) {
    CHECK_MSG(fa->name == "out", "field should be out");
    auto* sys = as<ast::Name>(fa->target.get());
    CHECK_MSG(sys != nullptr, "System should be a Name");
    if (sys != nullptr) CHECK_MSG(sys->identifier == "System", "target should be System");
  }
  auto* lit = as<ast::Literal>(call->arguments[0].get());
  CHECK_MSG(lit != nullptr, "argument should be a Literal");
  if (lit != nullptr) {
    CHECK_MSG(lit->lit == ast::LitKind::String, "argument should be a string literal");
    CHECK_MSG(lit->stringValue == "Hello, World!", "argument text mismatch");
  }
}

B2_TEST(parser_binary_precedence_and_parens) {
  ParseSession s;
  s.run("Prec.java", "class A { int x = 1 + 2 * 3; int y = (1 + 2) * 3; }\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  CHECK_MSG(cd != nullptr, "expected a ClassDeclaration");
  if (cd == nullptr) return;

  auto* add = as<ast::Binary>(firstInit(fieldNamed(cd, "x")));
  CHECK_MSG(add != nullptr, "1 + 2 * 3 should be a Binary node");
  if (add != nullptr) {
    CHECK_MSG(add->op == ast::BinaryOp::Add, "root operator should be Add");
    auto* one = as<ast::Literal>(add->lhs.get());
    CHECK_MSG(one != nullptr && one->intValue == 1u, "lhs should be the literal 1");
    auto* mul = as<ast::Binary>(add->rhs.get());
    CHECK_MSG(mul != nullptr, "2 * 3 should bind as its own Binary node");
    if (mul != nullptr) {
      CHECK_MSG(mul->op == ast::BinaryOp::Mul, "nested operator should be Mul");
      auto* two = as<ast::Literal>(mul->lhs.get());
      CHECK_MSG(two != nullptr && two->intValue == 2u, "Mul lhs should be 2");
      auto* three = as<ast::Literal>(mul->rhs.get());
      CHECK_MSG(three != nullptr && three->intValue == 3u, "Mul rhs should be 3");
    }
  }

  auto* mul2 = as<ast::Binary>(firstInit(fieldNamed(cd, "y")));
  CHECK_MSG(mul2 != nullptr, "(1 + 2) * 3 should be a Binary node");
  if (mul2 != nullptr) {
    CHECK_MSG(mul2->op == ast::BinaryOp::Mul, "root operator should be Mul");
    auto* paren = as<ast::ParenExpression>(mul2->lhs.get());
    CHECK_MSG(paren != nullptr, "lhs should be a ParenExpression");
    auto* inner = paren != nullptr ? as<ast::Binary>(paren->inner.get()) : nullptr;
    CHECK_MSG(inner != nullptr && inner->op == ast::BinaryOp::Add,
              "inside the parens should be Binary(Add)");
  }
}

B2_TEST(parser_cast_disambiguation) {
  ParseSession s;
  s.run("Cast.java",
        "class A {\n"
        "    void m() {\n"
        "        int a = 1, b = 2;\n"
        "        int c = (a) + b;\n"
        "        long d = (long) (a + b);\n"
        "    }\n"
        "    void n(Object o2) { Object o = (Object) o2; }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  CHECK_MSG(cd != nullptr, "expected a ClassDeclaration");
  if (cd == nullptr) return;
  auto* md = methodNamed(cd, "m");
  CHECK_MSG(md != nullptr, "method m missing");
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "m body should be a Block");
  CHECK_MSG(block != nullptr && block->statements.size() == 3,
            "m body should have 3 statements (a, c, d)");
  if (block == nullptr || block->statements.size() != 3) return;

  // (a) + b: the parenthesized name is a PRIMARY, not a cast target.
  auto* add = as<ast::Binary>(firstInit(as<ast::LocalVariableDeclaration>(block->statements[1].get())));
  CHECK_MSG(add != nullptr, "(a) + b should be a Binary(Add)");
  if (add != nullptr) {
    CHECK_MSG(add->op == ast::BinaryOp::Add, "(a) + b should be an Add");
    auto* paren = as<ast::ParenExpression>(add->lhs.get());
    CHECK_MSG(paren != nullptr, "(a) must parse as a ParenExpression, not a Cast");
    auto* nameA = paren != nullptr ? as<ast::Name>(paren->inner.get()) : nullptr;
    CHECK_MSG(nameA != nullptr && nameA->identifier == "a", "inside the parens is the name a");
  }

  // (long) (a + b): a primitive cast followed by a parenthesized expression.
  auto* cast = as<ast::Cast>(firstInit(as<ast::LocalVariableDeclaration>(block->statements[2].get())));
  CHECK_MSG(cast != nullptr, "(long) (a + b) should be a Cast");
  if (cast != nullptr) {
    auto* pt = as<ast::PrimitiveType>(cast->type.get());
    CHECK_MSG(pt != nullptr, "cast type should be a PrimitiveType");
    CHECK_MSG(pt != nullptr && pt->primitive == ast::PrimitiveKind::Long, "cast to long");
    auto* paren = as<ast::ParenExpression>(cast->expression.get());
    CHECK_MSG(paren != nullptr, "cast operand should be a ParenExpression");
    auto* sum = paren != nullptr ? as<ast::Binary>(paren->inner.get()) : nullptr;
    CHECK_MSG(sum != nullptr && sum->op == ast::BinaryOp::Add,
              "inside the parens should be Binary(Add)");
  }

  // (Object) o2: a reference-type cast of a simple name.
  auto* nd = methodNamed(cd, "n");
  auto* nblock = methodBody(nd);
  CHECK_MSG(isKind(nblock, ast::NodeKind::Block), "n body should be a Block");
  if (nblock == nullptr || nblock->statements.empty()) return;
  auto* cast2 = as<ast::Cast>(firstInit(as<ast::LocalVariableDeclaration>(nblock->statements[0].get())));
  CHECK_MSG(cast2 != nullptr, "(Object) o2 should be a Cast");
  if (cast2 != nullptr) {
    auto* ct = as<ast::ClassType>(cast2->type.get());
    CHECK_MSG(ct != nullptr, "cast type should be a ClassType");
    CHECK_MSG(ct != nullptr && !ct->segments.empty() && ct->segments[0].name == "Object",
              "cast type should be Object");
    auto* operand = as<ast::Name>(cast2->expression.get());
    CHECK_MSG(operand != nullptr && operand->identifier == "o2", "cast operand should be o2");
  }
}

B2_TEST(parser_lambda_forms) {
  ParseSession s;
  s.run("Lambdas.java",
        "class L {\n"
        "    Runnable r = () -> {};\n"
        "    java.util.function.Function<Integer, Integer> f = x -> x + 1;\n"
        "    interface Op { int apply(int a, int b); }\n"
        "    Op op = (int a, int b) -> a;\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  CHECK_MSG(cd != nullptr, "expected a ClassDeclaration");
  if (cd == nullptr) return;

  // () -> {} : zero parameters, block body.
  auto* l0 = as<ast::Lambda>(firstInit(fieldNamed(cd, "r")));
  CHECK_MSG(l0 != nullptr, "Runnable initializer should be a Lambda");
  if (l0 != nullptr) {
    CHECK_MSG(l0->parameters.empty(), "() -> has zero parameters");
    CHECK_MSG(l0->parenthesized, "the parameter list is written with parens");
    CHECK_MSG(isKind(l0->body.get(), ast::NodeKind::Block), "body should be a Block");
  }

  // The declared field type is a qualified generic ClassType.
  auto* ft = fieldNamed(cd, "f");
  CHECK_MSG(ft != nullptr, "field f missing");
  auto* fct = ft != nullptr ? as<ast::ClassType>(ft->type.get()) : nullptr;
  CHECK_MSG(fct != nullptr, "field type should be a ClassType");
  if (fct != nullptr) {
    CHECK_MSG(fct->segments.size() == 4, "java.util.function.Function has 4 segments");
    if (fct->segments.size() == 4) {
      CHECK_MSG(fct->segments[3].name == "Function", "last segment should be Function");
      CHECK_MSG(fct->segments[3].hasTypeArgs, "Function should carry type arguments");
      CHECK_MSG(fct->segments[3].args.size() == 2, "Function has two type arguments");
    }
  }

  // x -> x + 1 : one unparenthesized inferred parameter, expression body.
  auto* l1 = as<ast::Lambda>(firstInit(fieldNamed(cd, "f")));
  CHECK_MSG(l1 != nullptr, "Function initializer should be a Lambda");
  if (l1 != nullptr) {
    CHECK_MSG(l1->parameters.size() == 1, "x -> has one parameter");
    CHECK_MSG(!l1->parenthesized, "x -> is written without parens");
    CHECK_MSG(l1->parameters[0].type == nullptr, "parameter type is inferred");
    CHECK_MSG(l1->parameters[0].name == "x", "parameter should be named x");
    auto* body = as<ast::Binary>(l1->body.get());
    CHECK_MSG(body != nullptr, "body should be a Binary expression");
    CHECK_MSG(body != nullptr && body->op == ast::BinaryOp::Add, "body should be an Add");
  }

  // (int a, int b) -> a : parenthesized, explicitly typed parameters.
  auto* l2 = as<ast::Lambda>(firstInit(fieldNamed(cd, "op")));
  CHECK_MSG(l2 != nullptr, "Op initializer should be a Lambda");
  if (l2 != nullptr) {
    CHECK_MSG(l2->parenthesized, "parameter list is written with parens");
    CHECK_MSG(l2->parameters.size() == 2, "two parameters expected");
    if (l2->parameters.size() == 2) {
      auto* ta = as<ast::PrimitiveType>(l2->parameters[0].type.get());
      CHECK_MSG(ta != nullptr && ta->primitive == ast::PrimitiveKind::Int,
                "first parameter should be typed int");
      CHECK_MSG(l2->parameters[0].name == "a", "first parameter should be a");
      CHECK_MSG(l2->parameters[1].name == "b", "second parameter should be b");
    }
    auto* bodyName = as<ast::Name>(l2->body.get());
    CHECK_MSG(bodyName != nullptr && bodyName->identifier == "a",
              "expression body should be the name a");
  }
}

B2_TEST(parser_method_references) {
  ParseSession s;
  s.run("MethodRefs.java",
        "class M {\n"
        "    Runnable r = System.out::println;\n"
        "    java.util.function.Supplier<ArrayList<String>> s = ArrayList::new;\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  CHECK_MSG(cd != nullptr, "expected a ClassDeclaration");
  if (cd == nullptr) return;

  // System.out::println : expression qualifier, plain method name.
  auto* mr = as<ast::MethodReference>(firstInit(fieldNamed(cd, "r")));
  CHECK_MSG(mr != nullptr, "System.out::println should be a MethodReference");
  if (mr != nullptr) {
    CHECK_MSG(mr->name == "println", "referenced method should be println");
    CHECK_MSG(!mr->isConstructorRef, "println is not a constructor reference");
    auto* fa = as<ast::FieldAccess>(mr->qualifier.get());
    CHECK_MSG(fa != nullptr, "qualifier System.out should be a FieldAccess");
    CHECK_MSG(fa != nullptr && fa->name == "out", "field should be out");
  }

  // ArrayList::new : constructor reference.
  auto* cr = as<ast::MethodReference>(firstInit(fieldNamed(cd, "s")));
  CHECK_MSG(cr != nullptr, "ArrayList::new should be a MethodReference");
  if (cr != nullptr) {
    CHECK_MSG(cr->isConstructorRef, "ArrayList::new is a constructor reference");
    CHECK_MSG(cr->name == "new", "constructor references are named new");
    auto* qual = as<ast::Name>(cr->qualifier.get());
    CHECK_MSG(qual != nullptr && qual->identifier == "ArrayList",
              "qualifier should be the name ArrayList");
  }
}

// int[]::new - an array constructor reference. NOTE: this pins the JLS
// grammar; at the time of writing the parser mis-lexes the path as a class
// literal and emits spurious "expected '.'" diagnostics (see the FE-6 worklog
// entry). The structural assertions hold; the no-error assertion is the
// regression signal for the fix.
B2_TEST(parser_array_constructor_reference) {
  ParseSession s;
  s.run("ArrayCtorRef.java",
        "class AC {\n"
        "    java.util.function.Function<int[], int[]> g = int[]::new;\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "int[]::new must parse without diagnostics, got:\n" +
                                s.diagnosticsText());
  auto* cd = classAt(s, 0);
  CHECK_MSG(cd != nullptr, "expected a ClassDeclaration");
  if (cd == nullptr) return;
  auto* mr = as<ast::MethodReference>(firstInit(fieldNamed(cd, "g")));
  CHECK_MSG(mr != nullptr, "int[]::new should be a MethodReference");
  if (mr != nullptr) {
    CHECK_MSG(mr->isConstructorRef, "int[]::new is a constructor reference");
    CHECK_MSG(mr->name == "new", "constructor references are named new");
  }
}

B2_TEST(parser_less_than_is_not_a_generic_call) {
  ParseSession s;
  s.run("LessThan.java",
        "class G {\n"
        "    void m(int a, int c) {\n"
        "        boolean b = a < c;\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  CHECK_MSG(cd != nullptr, "expected a ClassDeclaration");
  auto* md = cd != nullptr ? methodNamed(cd, "m") : nullptr;
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "m body should be a Block");
  if (block == nullptr || block->statements.empty()) return;
  auto* ld = as<ast::LocalVariableDeclaration>(block->statements[0].get());
  CHECK_MSG(ld != nullptr, "statement should be a local variable declaration");
  auto* lt = as<ast::Binary>(firstInit(ld));
  CHECK_MSG(lt != nullptr, "a < c should be a Binary node");
  if (lt != nullptr) {
    CHECK_MSG(lt->op == ast::BinaryOp::Lt, "operator should be less-than");
    auto* lhs = as<ast::Name>(lt->lhs.get());
    CHECK_MSG(lhs != nullptr && lhs->identifier == "a", "lhs should be a");
    auto* rhs = as<ast::Name>(lt->rhs.get());
    CHECK_MSG(rhs != nullptr && rhs->identifier == "c", "rhs should be c");
  }
}

// Collections.<String>emptyList() - explicit type arguments on a qualified
// method invocation. NOTE: pins the JLS 15.12 grammar; at the time of writing
// the parser rejects `<` right after `.` (see the FE-6 worklog entry).
B2_TEST(parser_explicit_type_arguments_on_call) {
  ParseSession s;
  s.run("GenericCall.java",
        "class G {\n"
        "    void m() {\n"
        "        java.util.Collections.<String>emptyList();\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "a qualified explicit-type-argument call must parse, got:\n" +
                                s.diagnosticsText());
  auto* cd = classAt(s, 0);
  CHECK_MSG(cd != nullptr, "expected a ClassDeclaration");
  auto* md = cd != nullptr ? methodNamed(cd, "m") : nullptr;
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "m body should be a Block");
  CHECK_MSG(block != nullptr && !block->statements.empty(),
            "m body should have a statement");
  if (block == nullptr || block->statements.empty()) return;
  auto* es = as<ast::ExpressionStatement>(block->statements[0].get());
  CHECK_MSG(es != nullptr, "statement should be an ExpressionStatement");
  auto* call = es != nullptr ? as<ast::MethodInvocation>(es->expression.get()) : nullptr;
  CHECK_MSG(call != nullptr, "expected a MethodInvocation, got kind " +
                                 std::to_string(static_cast<int>(es->expression->kind)));
  if (call == nullptr) return;
  CHECK_MSG(call->name == "emptyList", "called method should be emptyList");
  CHECK_MSG(call->hasExplicitTypeArgs, "the call carries explicit type arguments");
  CHECK_MSG(call->typeArgs.size() == 1, "one type argument expected");
  if (call->typeArgs.size() == 1) {
    auto* targ = as<ast::ClassType>(call->typeArgs[0].get());
    CHECK_MSG(targ != nullptr && !targ->segments.empty() && targ->segments[0].name == "String",
              "type argument should be the ClassType String");
  }
  auto* target = as<ast::FieldAccess>(call->target.get());
  CHECK_MSG(target != nullptr, "java.util.Collections should be a FieldAccess chain");
}

B2_TEST(parser_record_declaration_and_compact_constructor) {
  ParseSession s;
  s.run("Point.java",
        "record Point(int x, int y) {\n"
        "    Point {\n"
        "        int z = 0;\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* rd = s.unit != nullptr ? as<ast::RecordDeclaration>(s.unit->types[0].get()) : nullptr;
  CHECK_MSG(rd != nullptr, "expected a RecordDeclaration");
  if (rd == nullptr) return;
  CHECK_MSG(rd->name == "Point", "record name should be Point");
  CHECK_MSG(rd->components.size() == 2, "Point has two record components");
  if (rd->components.size() == 2) {
    CHECK_MSG(rd->components[0].name == "x", "first component should be x");
    CHECK_MSG(rd->components[1].name == "y", "second component should be y");
    auto* xt = as<ast::PrimitiveType>(rd->components[0].type.get());
    CHECK_MSG(xt != nullptr && xt->primitive == ast::PrimitiveKind::Int,
              "component x should be typed int");
  }
  CHECK_MSG(rd->members.size() == 1, "one member expected");
  if (rd->members.empty()) return;
  auto* ctor = as<ast::ConstructorDeclaration>(rd->members[0].get());
  CHECK_MSG(ctor != nullptr, "member should be a ConstructorDeclaration");
  if (ctor != nullptr) {
    CHECK_MSG(ctor->isCompact, "Point { ... } is a compact constructor");
    CHECK_MSG(ctor->name == "Point", "compact constructor is named after the record");
    CHECK_MSG(ctor->parameters.empty(), "compact constructors have no parameter list");
    CHECK_MSG(isKind(ctor->body.get(), ast::NodeKind::Block), "compact ctor has a body");
    auto* body = ctor->body.get();
    if (body != nullptr) {
      CHECK_MSG(body->statements.size() == 1, "compact ctor body has one statement");
    }
  }
}

B2_TEST(parser_sealed_and_non_sealed) {
  ParseSession s;
  s.run("Shapes.java",
        "sealed interface Shape permits Circle, Square {}\n"
        "non-sealed class Square implements Shape {}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  CHECK_MSG(s.unit != nullptr && s.unit->types.size() == 2, "two top-level types expected");
  if (s.unit == nullptr || s.unit->types.size() != 2) return;

  auto* shape = as<ast::InterfaceDeclaration>(s.unit->types[0].get());
  CHECK_MSG(shape != nullptr, "first type should be an InterfaceDeclaration");
  if (shape != nullptr) {
    CHECK_MSG(shape->name == "Shape", "interface name should be Shape");
    CHECK_MSG((shape->mods & ast::ModSealed) != 0, "Shape should carry ModSealed");
    CHECK_MSG(shape->permits.size() == 2, "permits lists two types");
    if (shape->permits.size() == 2) {
      auto* c = as<ast::ClassType>(shape->permits[0].get());
      CHECK_MSG(c != nullptr && !c->segments.empty() && c->segments[0].name == "Circle",
                "first permit should be Circle");
      auto* sq = as<ast::ClassType>(shape->permits[1].get());
      CHECK_MSG(sq != nullptr && !sq->segments.empty() && sq->segments[0].name == "Square",
                "second permit should be Square");
    }
  }

  auto* square = as<ast::ClassDeclaration>(s.unit->types[1].get());
  CHECK_MSG(square != nullptr, "second type should be a ClassDeclaration");
  if (square != nullptr) {
    CHECK_MSG(square->name == "Square", "class name should be Square");
    CHECK_MSG((square->mods & ast::ModNonSealed) != 0, "Square should carry ModNonSealed");
    CHECK_MSG(square->implements.size() == 1, "Square implements one type");
  }
}

B2_TEST(parser_switch_expression_rules) {
  ParseSession s;
  s.run("SwitchExpr.java",
        "class S {\n"
        "    String f(Object x) {\n"
        "        return switch (x) {\n"
        "            case 1 -> \"one\";\n"
        "            case Integer i when i > 10 -> \"big\";\n"
        "            case null, default -> \"other\";\n"
        "        };\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  auto* md = cd != nullptr ? methodNamed(cd, "f") : nullptr;
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "f body should be a Block");
  CHECK_MSG(block != nullptr && !block->statements.empty(),
            "f body should have a statement");
  if (block == nullptr || block->statements.empty()) return;
  auto* ret = as<ast::Return>(block->statements[0].get());
  CHECK_MSG(ret != nullptr, "body should be a return statement");
  auto* se = ret != nullptr ? as<ast::SwitchExpression>(ret->expression.get()) : nullptr;
  CHECK_MSG(se != nullptr, "returned expression should be a SwitchExpression");
  if (se == nullptr) return;
  auto* selector = as<ast::Name>(se->selector.get());
  CHECK_MSG(selector != nullptr && selector->identifier == "x", "selector should be x");
  CHECK_MSG(se->cases.size() == 3, "three case groups expected");
  if (se->cases.size() != 3) return;

  // case 1 -> "one"
  const auto& c0 = se->cases[0];
  CHECK_MSG(c0.isRule, "case 1 uses the rule (arrow) form");
  CHECK_MSG(c0.labels.size() == 1, "single label");
  if (c0.labels.empty()) return;
  CHECK_MSG(c0.labels[0].constant != nullptr, "label should be a constant");
  auto* one = c0.labels[0].constant != nullptr ? as<ast::Literal>(c0.labels[0].constant.get())
                                              : nullptr;
  CHECK_MSG(one != nullptr && one->intValue == 1u, "constant should be 1");
  CHECK_MSG(c0.guard == nullptr, "no guard on the first case");
  auto* body0 = as<ast::Literal>(c0.ruleBody.get());
  CHECK_MSG(body0 != nullptr && body0->lit == ast::LitKind::String &&
                body0->stringValue == "one",
            "rule body should be the string one");

  // case Integer i when i > 10 -> "big"
  const auto& c1 = se->cases[1];
  CHECK_MSG(c1.isRule, "arrow form");
  CHECK_MSG(c1.labels.size() == 1, "single label");
  if (c1.labels.empty()) return;
  auto* pat = as<ast::TypePattern>(c1.labels[0].pattern.get());
  CHECK_MSG(pat != nullptr, "label should be a TypePattern");
  if (pat != nullptr) {
    CHECK_MSG(pat->binding == "i", "pattern binds i");
    auto* intType = as<ast::ClassType>(pat->type.get());
    CHECK_MSG(intType != nullptr && !intType->segments.empty() &&
                  intType->segments[0].name == "Integer",
              "pattern type should be Integer");
  }
  auto* guard = as<ast::Binary>(c1.guard.get());
  CHECK_MSG(guard != nullptr && guard->op == ast::BinaryOp::Gt, "guard should be i > 10");

  // case null, default -> "other"
  const auto& c2 = se->cases[2];
  CHECK_MSG(c2.isRule, "arrow form");
  CHECK_MSG(c2.labels.size() == 2, "null and default share one case");
  if (c2.labels.size() < 2) return;
  CHECK_MSG(c2.labels[0].isNull, "first label should be the null case");
  CHECK_MSG(!c2.labels[0].isDefault, "first label is not default");
  CHECK_MSG(c2.labels[1].isDefault, "second label should be default");
  auto* body2 = as<ast::Literal>(c2.ruleBody.get());
  CHECK_MSG(body2 != nullptr && body2->stringValue == "other", "rule body should be other");
}

B2_TEST(parser_classic_switch_statement_colon_groups) {
  ParseSession s;
  s.run("SwitchStmt.java",
        "class C {\n"
        "    void f(int code) {\n"
        "        int bucket = 0;\n"
        "        switch (code) {\n"
        "            case 1:\n"
        "                bucket += 1;\n"
        "            case 2:\n"
        "                bucket += 2;\n"
        "                break;\n"
        "            default:\n"
        "                bucket = -1;\n"
        "        }\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  auto* md = cd != nullptr ? methodNamed(cd, "f") : nullptr;
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "f body should be a Block");
  CHECK_MSG(block != nullptr && block->statements.size() == 2,
            "f body should have 2 statements");
  if (block == nullptr || block->statements.size() != 2) return;
  auto* sw = as<ast::SwitchStatement>(block->statements[1].get());
  CHECK_MSG(sw != nullptr, "second statement should be a SwitchStatement");
  if (sw == nullptr) return;
  auto* selector = as<ast::Name>(sw->selector.get());
  CHECK_MSG(selector != nullptr && selector->identifier == "code", "selector should be code");
  CHECK_MSG(sw->cases.size() == 3, "three case groups expected");
  if (sw->cases.size() != 3) return;

  const auto& c0 = sw->cases[0];
  CHECK_MSG(!c0.isRule, "colon cases are not rules");
  CHECK_MSG(c0.ruleBody == nullptr, "colon cases have no rule body");
  CHECK_MSG(c0.guard == nullptr, "colon cases have no guard");
  CHECK_MSG(c0.statements.size() == 1, "case 1 falls through with one statement");
  CHECK_MSG(!c0.labels.empty() && c0.labels[0].constant != nullptr,
            "case 1 label is a constant");
  auto* k1 = !c0.labels.empty() && c0.labels[0].constant != nullptr
                 ? as<ast::Literal>(c0.labels[0].constant.get())
                 : nullptr;
  CHECK_MSG(k1 != nullptr && k1->intValue == 1u, "label constant should be 1");

  const auto& c1 = sw->cases[1];
  CHECK_MSG(!c1.isRule, "colon case");
  CHECK_MSG(c1.statements.size() == 2, "case 2 has a statement and a break");
  if (c1.statements.size() == 2) {
    CHECK_MSG(isKind(c1.statements[1].get(), ast::NodeKind::BreakStatement),
              "second statement should be a break");
  }

  const auto& c2 = sw->cases[2];
  CHECK_MSG(!c2.isRule, "colon case");
  CHECK_MSG(!c2.labels.empty() && c2.labels[0].isDefault, "default label");
  CHECK_MSG(c2.statements.size() == 1, "default has one statement");
}

B2_TEST(parser_instanceof_patterns) {
  ParseSession s;
  s.run("Instanceof.java",
        "class I {\n"
        "    void f(Object o, Object p) {\n"
        "        if (o instanceof String s) {}\n"
        "        if (p instanceof Point(int x, int y)) {}\n"
        "    }\n"
        "}\n"
        "record Point(int x, int y) {}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  auto* md = cd != nullptr ? methodNamed(cd, "f") : nullptr;
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "f body should be a Block");
  CHECK_MSG(block != nullptr && block->statements.size() == 2,
            "f body should have 2 statements");
  if (block == nullptr || block->statements.size() != 2) return;

  auto* if0 = as<ast::If>(block->statements[0].get());
  CHECK_MSG(if0 != nullptr, "first statement should be an If");
  auto* io0 = if0 != nullptr ? as<ast::InstanceOf>(if0->condition.get()) : nullptr;
  CHECK_MSG(io0 != nullptr, "condition should be an InstanceOf");
  if (io0 != nullptr) {
    auto* tp = as<ast::TypePattern>(io0->pattern.get());
    CHECK_MSG(tp != nullptr, "pattern should be a TypePattern");
    if (tp != nullptr) {
      CHECK_MSG(tp->binding == "s", "pattern should bind s");
      auto* strType = as<ast::ClassType>(tp->type.get());
      CHECK_MSG(strType != nullptr && !strType->segments.empty() &&
                    strType->segments[0].name == "String",
                "pattern type should be String");
    }
    CHECK_MSG(io0->type == nullptr, "type is carried by the pattern");
  }
  CHECK_MSG(isKind(if0->thenStmt.get(), ast::NodeKind::Block), "then branch is an empty block");

  auto* if1 = as<ast::If>(block->statements[1].get());
  CHECK_MSG(if1 != nullptr, "second statement should be an If");
  auto* io1 = if1 != nullptr ? as<ast::InstanceOf>(if1->condition.get()) : nullptr;
  CHECK_MSG(io1 != nullptr, "condition should be an InstanceOf");
  if (io1 != nullptr) {
    auto* rp = as<ast::RecordPattern>(io1->pattern.get());
    CHECK_MSG(rp != nullptr, "pattern should be a RecordPattern");
    if (rp != nullptr) {
      auto* pointType = as<ast::ClassType>(rp->type.get());
      CHECK_MSG(pointType != nullptr && !pointType->segments.empty() &&
                    pointType->segments[0].name == "Point",
                "record pattern type should be Point");
      CHECK_MSG(rp->components.size() == 2, "Point pattern has two components");
      if (rp->components.size() == 2) {
        auto* px = as<ast::TypePattern>(rp->components[0].get());
        CHECK_MSG(px != nullptr && px->binding == "x", "first component binds x");
        auto* py = as<ast::TypePattern>(rp->components[1].get());
        CHECK_MSG(py != nullptr && py->binding == "y", "second component binds y");
      }
    }
  }
}

B2_TEST(parser_try_with_resources_union_catch_finally) {
  ParseSession s;
  s.run("TryRes.java",
        "class T {\n"
        "    void f() throws Exception {\n"
        "        try (var r = open(); final java.io.Reader r2 = make()) {\n"
        "        } catch (java.io.IOException | java.sql.SQLException e) {\n"
        "        } finally {\n"
        "        }\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  auto* md = cd != nullptr ? methodNamed(cd, "f") : nullptr;
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "f body should be a Block");
  CHECK_MSG(block != nullptr && !block->statements.empty(),
            "f body should have a statement");
  if (block == nullptr || block->statements.empty()) return;
  auto* tr = as<ast::Try>(block->statements[0].get());
  CHECK_MSG(tr != nullptr, "statement should be a Try");
  if (tr == nullptr) return;

  CHECK_MSG(tr->resources.size() == 2, "two resources expected");
  if (tr->resources.size() == 2) {
    const auto& r0 = tr->resources[0];
    CHECK_MSG(r0.isDeclaration, "var r = open() declares a resource");
    CHECK_MSG(r0.decl != nullptr && r0.decl->isVar, "first resource uses var");
    const auto& r1 = tr->resources[1];
    CHECK_MSG(r1.isDeclaration, "final Reader r2 = make() declares a resource");
    if (r1.decl != nullptr) {
      CHECK_MSG((r1.decl->mods & ast::ModFinal) != 0, "second resource is final");
      auto* rt = as<ast::ClassType>(r1.decl->type.get());
      CHECK_MSG(rt != nullptr && rt->segments.size() == 3, "java.io.Reader has 3 segments");
    }
  }

  CHECK_MSG(tr->catches.size() == 1, "one catch clause expected");
  if (!tr->catches.empty()) {
    const auto& cc = tr->catches[0];
    CHECK_MSG(cc.name == "e", "catch parameter should be e");
    auto* ut = as<ast::UnionType>(cc.type.get());
    CHECK_MSG(ut != nullptr, "multi-catch type should be a UnionType");
    CHECK_MSG(ut != nullptr && ut->types.size() == 2, "union of two exception types");
  }
  CHECK_MSG(tr->finallyBlock != nullptr, "finally block should be present");
}

B2_TEST(parser_labeled_enhanced_for_and_continue) {
  ParseSession s;
  s.run("Labels.java",
        "class E {\n"
        "    void f(int[][] grid) {\n"
        "        outer: for (int[] row : grid) {\n"
        "            for (var v : row) {\n"
        "                continue outer;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  auto* md = cd != nullptr ? methodNamed(cd, "f") : nullptr;
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "f body should be a Block");
  CHECK_MSG(block != nullptr && !block->statements.empty(),
            "f body should have a statement");
  if (block == nullptr || block->statements.empty()) return;
  auto* labeled = as<ast::LabeledStatement>(block->statements[0].get());
  CHECK_MSG(labeled != nullptr, "statement should be a LabeledStatement");
  if (labeled == nullptr) return;
  CHECK_MSG(labeled->label == "outer", "label should be outer");
  auto* outerFor = as<ast::EnhancedFor>(labeled->statement.get());
  CHECK_MSG(outerFor != nullptr, "labeled statement should be an EnhancedFor");
  if (outerFor == nullptr) return;
  CHECK_MSG(outerFor->name == "row", "loop variable should be row");
  CHECK_MSG(isKind(outerFor->type.get(), ast::NodeKind::ArrayType),
            "int[] loop variable type should be an ArrayType");
  auto* grid = as<ast::Name>(outerFor->iterable.get());
  CHECK_MSG(grid != nullptr && grid->identifier == "grid", "iterable should be grid");
  auto* outerBody = as<ast::Block>(outerFor->body.get());
  CHECK_MSG(outerBody != nullptr, "outer loop body should be a Block");
  if (outerBody == nullptr || outerBody->statements.empty()) return;

  auto* innerFor = as<ast::EnhancedFor>(outerBody->statements[0].get());
  CHECK_MSG(innerFor != nullptr, "nested statement should be an EnhancedFor");
  if (innerFor == nullptr) return;
  CHECK_MSG(innerFor->isVar, "inner loop uses var");
  CHECK_MSG(innerFor->name == "v", "inner loop variable should be v");
  auto* rowName = as<ast::Name>(innerFor->iterable.get());
  CHECK_MSG(rowName != nullptr && rowName->identifier == "row", "inner iterable should be row");
  auto* innerBody = as<ast::Block>(innerFor->body.get());
  CHECK_MSG(innerBody != nullptr, "inner loop body should be a Block");
  if (innerBody == nullptr || innerBody->statements.empty()) return;
  auto* cont = as<ast::ContinueStatement>(innerBody->statements[0].get());
  CHECK_MSG(cont != nullptr, "statement should be a ContinueStatement");
  if (cont != nullptr) {
    CHECK_MSG(cont->hasLabel, "continue carries a label");
    CHECK_MSG(cont->label == "outer", "continue target should be outer");
  }
}

B2_TEST(parser_local_declarations_and_c_style_dims) {
  ParseSession s;
  s.run("Locals.java",
        "class A {\n"
        "    void m() {\n"
        "        int a[] = null, b;\n"
        "        record R(int q) {}\n"
        "        class C {}\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  auto* md = cd != nullptr ? methodNamed(cd, "m") : nullptr;
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "m body should be a Block");
  CHECK_MSG(block != nullptr && block->statements.size() == 3,
            "m body should have 3 statements");
  if (block == nullptr || block->statements.size() != 3) return;

  auto* ld = as<ast::LocalVariableDeclaration>(block->statements[0].get());
  CHECK_MSG(ld != nullptr, "first statement should be a LocalVariableDeclaration");
  if (ld != nullptr) {
    CHECK_MSG(ld->declarators.size() == 2, "two declarators: a and b");
    if (ld->declarators.size() == 2) {
      CHECK_MSG(ld->declarators[0].name == "a", "first declarator is a");
      CHECK_MSG(ld->declarators[0].extraDims.size() == 1, "a has one C-style dimension");
      auto* initA = as<ast::Literal>(ld->declarators[0].initializer.get());
      CHECK_MSG(initA != nullptr && initA->lit == ast::LitKind::Null, "a is null-initialized");
      CHECK_MSG(ld->declarators[1].name == "b", "second declarator is b");
      CHECK_MSG(ld->declarators[1].extraDims.empty(), "b has no extra dimensions");
      CHECK_MSG(ld->declarators[1].initializer == nullptr, "b has no initializer");
    }
  }

  auto* ltdR = as<ast::LocalTypeDeclaration>(block->statements[1].get());
  CHECK_MSG(ltdR != nullptr, "second statement should be a LocalTypeDeclaration");
  auto* recR = ltdR != nullptr ? as<ast::RecordDeclaration>(ltdR->decl.get()) : nullptr;
  CHECK_MSG(recR != nullptr, "local type should be a RecordDeclaration");
  if (recR != nullptr) {
    CHECK_MSG(recR->name == "R", "local record should be R");
    CHECK_MSG(recR->components.size() == 1, "R has one component");
  }

  auto* ltdC = as<ast::LocalTypeDeclaration>(block->statements[2].get());
  CHECK_MSG(ltdC != nullptr, "third statement should be a LocalTypeDeclaration");
  auto* clsC = ltdC != nullptr ? as<ast::ClassDeclaration>(ltdC->decl.get()) : nullptr;
  CHECK_MSG(clsC != nullptr, "local type should be a ClassDeclaration");
  if (clsC != nullptr) CHECK_MSG(clsC->name == "C", "local class should be C");
}

B2_TEST(parser_enum_constants_and_members) {
  ParseSession s;
  s.run("E.java",
        "enum E {\n"
        "    A, B(1) { void f() {} }, C;\n"
        "    void g() {}\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* ed = s.unit != nullptr ? as<ast::EnumDeclaration>(s.unit->types[0].get()) : nullptr;
  CHECK_MSG(ed != nullptr, "expected an EnumDeclaration");
  if (ed == nullptr) return;
  CHECK_MSG(ed->name == "E", "enum name should be E");
  CHECK_MSG(ed->constants.size() == 3, "three constants expected");
  if (ed->constants.size() != 3) return;

  CHECK_MSG(ed->constants[0].name == "A", "first constant should be A");
  CHECK_MSG(ed->constants[0].arguments.empty(), "A has no arguments");
  CHECK_MSG(ed->constants[0].body == nullptr, "A has no body");

  CHECK_MSG(ed->constants[1].name == "B", "second constant should be B");
  CHECK_MSG(ed->constants[1].arguments.size() == 1, "B has one argument");
  auto* bodyB = ed->constants[1].body.get();
  CHECK_MSG(bodyB != nullptr, "B should have a constant-specific body");
  if (bodyB != nullptr) {
    CHECK_MSG(bodyB->members.size() == 1, "B's body has one member");
    auto* f = as<ast::MethodDeclaration>(bodyB->members[0].get());
    CHECK_MSG(f != nullptr && f->name == "f", "B's body declares f");
  }

  CHECK_MSG(ed->constants[2].name == "C", "third constant should be C");

  CHECK_MSG(ed->members.size() == 1, "one member after the constants");
  if (ed->members.empty()) return;
  auto* g = as<ast::MethodDeclaration>(ed->members[0].get());
  CHECK_MSG(g != nullptr && g->name == "g", "the member should be method g");
}

B2_TEST(parser_field_modifiers_and_declarator_dims) {
  ParseSession s;
  s.run("Fields.java",
        "class A {\n"
        "    static final int X = 1;\n"
        "    transient String s;\n"
        "    int a[], b;\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  CHECK_MSG(cd != nullptr, "expected a ClassDeclaration");
  if (cd == nullptr) return;
  CHECK_MSG(cd->members.size() == 3, "three fields expected");
  if (cd->members.size() != 3) return;

  auto* fx = as<ast::FieldDeclaration>(cd->members[0].get());
  CHECK_MSG(fx != nullptr, "first member should be a FieldDeclaration");
  if (fx != nullptr) {
    CHECK_MSG((fx->mods & ast::ModStatic) != 0, "X should be static");
    CHECK_MSG((fx->mods & ast::ModFinal) != 0, "X should be final");
    CHECK_MSG((fx->mods & ast::ModTransient) == 0, "X should not be transient");
    auto* init = as<ast::Literal>(firstInit(fx));
    CHECK_MSG(init != nullptr && init->intValue == 1u, "X is initialized to 1");
  }

  auto* fs = as<ast::FieldDeclaration>(cd->members[1].get());
  CHECK_MSG(fs != nullptr, "second member should be a FieldDeclaration");
  if (fs != nullptr) {
    CHECK_MSG((fs->mods & ast::ModTransient) != 0, "s should be transient");
    auto* st = as<ast::ClassType>(fs->type.get());
    CHECK_MSG(st != nullptr && !st->segments.empty() && st->segments[0].name == "String",
              "s should be typed String");
  }

  auto* fa = as<ast::FieldDeclaration>(cd->members[2].get());
  CHECK_MSG(fa != nullptr, "third member should be a FieldDeclaration");
  if (fa != nullptr) {
    CHECK_MSG(fa->declarators.size() == 2, "two declarators: a and b");
    if (fa->declarators.size() == 2) {
      CHECK_MSG(fa->declarators[0].name == "a", "first declarator is a");
      CHECK_MSG(fa->declarators[0].extraDims.size() == 1, "a has one C-style dimension");
      CHECK_MSG(fa->declarators[1].name == "b", "second declarator is b");
      CHECK_MSG(fa->declarators[1].extraDims.empty(), "b has no extra dimensions");
    }
  }
}

B2_TEST(parser_new_forms_array_and_anonymous_class) {
  ParseSession s;
  s.run("NewForms.java",
        "class N {\n"
        "    void f() {\n"
        "        int[] arr = new int[3];\n"
        "        int[][] m2 = new int[2][];\n"
        "        int[] ini = new int[] {1, 2};\n"
        "        Object o = new Foo(1) {\n"
        "            public String toString() { return \"\"; }\n"
        "        };\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  auto* md = cd != nullptr ? methodNamed(cd, "f") : nullptr;
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "f body should be a Block");
  CHECK_MSG(block != nullptr && block->statements.size() == 4,
            "f body should have 4 statements");
  if (block == nullptr || block->statements.size() != 4) return;

  // new int[3]
  auto* ac0 = as<ast::ArrayCreation>(
      firstInit(as<ast::LocalVariableDeclaration>(block->statements[0].get())));
  CHECK_MSG(ac0 != nullptr, "new int[3] should be an ArrayCreation");
  if (ac0 != nullptr) {
    CHECK_MSG(isKind(ac0->elementType.get(), ast::NodeKind::PrimitiveType),
              "element type should be int");
    CHECK_MSG(ac0->dimensions.size() == 1, "one dimension");
    auto* size = ac0->dimensions.size() == 1 && ac0->dimensions[0].size != nullptr
                     ? as<ast::Literal>(ac0->dimensions[0].size.get())
                     : nullptr;
    CHECK_MSG(size != nullptr && size->intValue == 3u, "dimension size should be 3");
    CHECK_MSG(ac0->initializer == nullptr, "no array initializer");
  }

  // new int[2][]
  auto* ac1 = as<ast::ArrayCreation>(
      firstInit(as<ast::LocalVariableDeclaration>(block->statements[1].get())));
  CHECK_MSG(ac1 != nullptr, "new int[2][] should be an ArrayCreation");
  if (ac1 != nullptr) {
    CHECK_MSG(ac1->dimensions.size() == 2, "two dimensions");
    CHECK_MSG(ac1->dimensions[0].size != nullptr, "first dimension has a size");
    CHECK_MSG(ac1->dimensions[1].size == nullptr, "second dimension is empty");
    CHECK_MSG(ac1->initializer == nullptr, "no array initializer");
  }

  // new int[] {1, 2}
  auto* ac2 = as<ast::ArrayCreation>(
      firstInit(as<ast::LocalVariableDeclaration>(block->statements[2].get())));
  CHECK_MSG(ac2 != nullptr, "new int[] {1, 2} should be an ArrayCreation");
  if (ac2 != nullptr && ac2->dimensions.size() != 1) {
    CHECK_MSG(false, "one (empty) dimension expected");
    ac2 = nullptr;
  }
  if (ac2 != nullptr) {
    CHECK_MSG(ac2->dimensions[0].size == nullptr, "the dimension is empty");
    CHECK_MSG(ac2->initializer != nullptr, "initializer present");
    if (ac2->initializer != nullptr) {
      CHECK_MSG(ac2->initializer->values.size() == 2, "two initial values");
    }
  }

  // new Foo(1) { ... }
  auto* cre = as<ast::ClassInstanceCreation>(
      firstInit(as<ast::LocalVariableDeclaration>(block->statements[3].get())));
  CHECK_MSG(cre != nullptr, "new Foo(1) {...} should be a ClassInstanceCreation");
  if (cre != nullptr) {
    auto* ft = as<ast::ClassType>(cre->type.get());
    CHECK_MSG(ft != nullptr && !ft->segments.empty() && ft->segments[0].name == "Foo",
              "constructed type should be Foo");
    CHECK_MSG(cre->arguments.size() == 1, "one constructor argument");
    auto* anon = cre->anonymousBody.get();
    CHECK_MSG(anon != nullptr, "anonymous class body should be present");
    if (anon != nullptr) {
      CHECK_MSG(anon->members.size() == 1, "anonymous body has one member");
      auto* ts = as<ast::MethodDeclaration>(anon->members[0].get());
      CHECK_MSG(ts != nullptr && ts->name == "toString", "member should be toString");
      if (ts != nullptr) {
        CHECK_MSG((ts->mods & ast::ModPublic) != 0, "toString is public");
        auto* tsBody = ts->body.get();
        CHECK_MSG(isKind(tsBody, ast::NodeKind::Block), "toString has a body");
        if (tsBody != nullptr && tsBody->statements.size() == 1) {
          auto* ret = as<ast::Return>(tsBody->statements[0].get());
          CHECK_MSG(ret != nullptr, "toString body should return");
          auto* empty = ret != nullptr ? as<ast::Literal>(ret->expression.get()) : nullptr;
          CHECK_MSG(empty != nullptr && empty->lit == ast::LitKind::String &&
                        empty->stringValue.empty(),
                    "returned value should be the empty string");
        }
      }
    }
  }
}

B2_TEST(parser_yield_in_switch_expression) {
  ParseSession s;
  s.run("Yield.java",
        "class Y {\n"
        "    int f(int x) {\n"
        "        int r = switch (x) {\n"
        "            default -> { yield 1; }\n"
        "        };\n"
        "        int yield = 2;\n"
        "        return r + yield;\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "expected a clean parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  auto* md = cd != nullptr ? methodNamed(cd, "f") : nullptr;
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "f body should be a Block");
  CHECK_MSG(block != nullptr && block->statements.size() == 3,
            "f body should have 3 statements");
  if (block == nullptr || block->statements.size() != 3) return;

  auto* ld = as<ast::LocalVariableDeclaration>(block->statements[0].get());
  CHECK_MSG(ld != nullptr, "first statement should be a local declaration");
  auto* se = as<ast::SwitchExpression>(firstInit(ld));
  CHECK_MSG(se != nullptr, "initializer should be a SwitchExpression");
  if (se == nullptr) return;
  CHECK_MSG(se->cases.size() == 1, "one case group");
  if (se->cases.empty()) return;
  const auto& c = se->cases[0];
  CHECK_MSG(c.isRule, "default -> uses the rule form");
  CHECK_MSG(!c.labels.empty() && c.labels[0].isDefault, "default label");
  auto* ruleBody = as<ast::Block>(c.ruleBody.get());
  CHECK_MSG(ruleBody != nullptr, "rule body should be a Block");
  if (ruleBody == nullptr || ruleBody->statements.empty()) return;
  auto* y = as<ast::Yield>(ruleBody->statements[0].get());
  CHECK_MSG(y != nullptr, "block body should contain a Yield statement");
  auto* one = y != nullptr ? as<ast::Literal>(y->value.get()) : nullptr;
  CHECK_MSG(one != nullptr && one->intValue == 1u, "yield value should be 1");

  // Outside a switch expression, `yield` is an ordinary identifier.
  auto* ldy = as<ast::LocalVariableDeclaration>(block->statements[1].get());
  CHECK_MSG(ldy != nullptr, "int yield = 2; should parse as a local declaration");
  if (ldy != nullptr && !ldy->declarators.empty()) {
    CHECK_MSG(ldy->declarators[0].name == "yield", "the variable is named yield");
  }
  auto* ret = as<ast::Return>(block->statements[2].get());
  CHECK_MSG(ret != nullptr, "last statement should be a return");
  auto* sum = ret != nullptr ? as<ast::Binary>(ret->expression.get()) : nullptr;
  CHECK_MSG(sum != nullptr && sum->op == ast::BinaryOp::Add,
            "r + yield should be a Binary(Add)");
}

// grid[row][col] = 7 - an array-access assignment (JLS 15.13 / 14.8), not a
// local variable declaration: `grid[]`-as-type + `row`-as-declarator-name
// must not win the disambiguation. NOTE: pins JLS behavior; at the time of
// writing the parser misclassifies this statement (see the FE-6 worklog).
B2_TEST(parser_array_access_assignment_is_not_local_decl) {
  ParseSession s;
  s.run("ArrayAssign.java",
        "class AA {\n"
        "    void f(int[][] grid) {\n"
        "        int row = 0, col = 1;\n"
        "        grid[row][col] = 7;\n"
        "    }\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "array-access assignment must parse, got:\n" +
                                s.diagnosticsText());
  auto* cd = classAt(s, 0);
  auto* md = cd != nullptr ? methodNamed(cd, "f") : nullptr;
  auto* block = methodBody(md);
  CHECK_MSG(isKind(block, ast::NodeKind::Block), "f body should be a Block");
  CHECK_MSG(block != nullptr && block->statements.size() == 2,
            "f body should have 2 statements");
  if (block == nullptr || block->statements.size() != 2) return;

  auto* lvd = as<ast::LocalVariableDeclaration>(block->statements[0].get());
  CHECK_MSG(lvd != nullptr, "first statement should declare row and col");

  auto* es = as<ast::ExpressionStatement>(block->statements[1].get());
  CHECK_MSG(es != nullptr,
            "grid[row][col] = 7 must be an ExpressionStatement, not a local "
            "variable declaration");
  auto* assign = es != nullptr ? as<ast::Assignment>(es->expression.get()) : nullptr;
  CHECK_MSG(assign != nullptr, "the statement should be an Assignment");
  if (assign == nullptr) return;
  CHECK_MSG(assign->op == ast::AssignOp::Assign, "plain assignment");
  auto* outer = as<ast::ArrayAccess>(assign->target.get());
  CHECK_MSG(outer != nullptr, "assignment target should be an ArrayAccess");
  if (outer != nullptr) {
    auto* inner = as<ast::ArrayAccess>(outer->array.get());
    CHECK_MSG(inner != nullptr, "nested target should also be an ArrayAccess");
    if (inner != nullptr) {
      auto* grid = as<ast::Name>(inner->array.get());
      CHECK_MSG(grid != nullptr && grid->identifier == "grid", "array should be grid");
      auto* rowIdx = as<ast::Name>(inner->index.get());
      CHECK_MSG(rowIdx != nullptr && rowIdx->identifier == "row", "first index should be row");
    }
    auto* colIdx = as<ast::Name>(outer->index.get());
    CHECK_MSG(colIdx != nullptr && colIdx->identifier == "col", "second index should be col");
  }
  auto* seven = as<ast::Literal>(assign->value.get());
  CHECK_MSG(seven != nullptr && seven->intValue == 7u, "assigned value should be 7");
}

// (Runnable) () -> {} - a cast whose operand is a lambda (JLS 15.16).
// NOTE: pins JLS behavior; at the time of writing the parser fails on the
// lambda operand (see the FE-6 worklog).
B2_TEST(parser_cast_lambda_operand) {
  ParseSession s;
  s.run("CastLambda.java",
        "class CL {\n"
        "    Runnable cast = (Runnable) () -> {};\n"
        "}\n");
  CHECK_MSG(!s.hasErrors(), "a cast of a lambda must parse, got:\n" + s.diagnosticsText());
  auto* cd = classAt(s, 0);
  CHECK_MSG(cd != nullptr, "expected a ClassDeclaration");
  if (cd == nullptr) return;
  auto* cast = as<ast::Cast>(firstInit(fieldNamed(cd, "cast")));
  CHECK_MSG(cast != nullptr, "initializer should be a Cast");
  if (cast == nullptr) return;
  auto* rt = as<ast::ClassType>(cast->type.get());
  CHECK_MSG(rt != nullptr && !rt->segments.empty() && rt->segments[0].name == "Runnable",
            "cast type should be Runnable");
  auto* lambda = as<ast::Lambda>(cast->expression.get());
  CHECK_MSG(lambda != nullptr, "cast operand should be a Lambda");
  if (lambda != nullptr) {
    CHECK_MSG(lambda->parameters.empty(), "the lambda takes no parameters");
    CHECK_MSG(lambda->parenthesized, "the lambda writes its (empty) parameter list");
    CHECK_MSG(isKind(lambda->body.get(), ast::NodeKind::Block), "lambda body should be a Block");
  }
}

// ------------------------------------------------------- error diagnostics -

B2_TEST(parser_missing_semicolon_diagnostic) {
  ParseSession s;
  s.run("MissingSemi.java", "class A { int x = 1 }\n");
  CHECK_MSG(s.hasErrors(), "a missing ';' must be diagnosed");
  CHECK_MSG(s.diagnosticsText().find("expected ';'") != std::string::npos,
            "diagnostic should mention expected ';', got:\n" + s.diagnosticsText());
  CHECK_MSG(s.unit != nullptr && !s.unit->types.empty(),
            "a unit with a type declaration still comes back");
}

B2_TEST(parser_unclosed_brace_diagnostic) {
  ParseSession s;
  s.run("UnclosedBrace.java", "class A {\n");
  CHECK_MSG(s.hasErrors(), "a missing '}' must be diagnosed");
  CHECK_MSG(s.diagnosticsText().find("expected '}'") != std::string::npos,
            "diagnostic should mention expected '}', got:\n" + s.diagnosticsText());
  CHECK_MSG(s.unit != nullptr, "parsing still returns a unit (never null)");
}

}  // namespace
