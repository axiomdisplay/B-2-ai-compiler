#pragma once
// B-2 Frontend - recursive-descent parser: tokens -> AST.
//
// Strategy notes:
//   * Arbitrary Java (Java 21-level grammar): classes, interfaces, enums,
//     records, sealed types, annotations, generics, lambdas, method
//     references, switch expressions, patterns, text blocks, var.
//   * Classic ambiguities are resolved by bounded speculation: the parser
//     checkpoints the token cursor (and the token vector, because `>>` is
//     split when closing nested type arguments), re-parses in a silent
//     trial mode, and rolls back. Diagnostics are suppressed while
//     speculating, so users never see guesses.
//   * Error recovery: malformed members/statements are skipped to the next
//     plausible boundary; parsing always makes progress and never crashes.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "b2/frontend/Diagnostics.h"
#include "b2/frontend/SourceManager.h"
#include "b2/frontend/Token.h"
#include "b2/frontend/ast/Ast.h"

namespace b2::frontend {

class Parser {
 public:
  Parser(std::vector<Token> tokens, const SourceManager& sm, DiagnosticEngine& diags);

  // Parses a whole compilation unit. Never returns null: on hard failure an
  // (empty) unit comes back and the errors live in the diagnostic engine.
  [[nodiscard]] std::unique_ptr<ast::CompilationUnit> parseCompilationUnit();

  // ---------------------------------------------------------------- context
  struct MemberContext {
    bool inInterface = false;
    bool isRecord = false;          // compact constructors allowed
    bool isAnnotationType = false;  // element default values allowed
    std::string typeName;           // enclosing type name (compact ctor)
  };

 private:
  // ------------------------------------------------------------ token stream
  std::vector<Token> toks_;
  std::size_t pos_ = 0;
  const SourceManager& sm_;
  DiagnosticEngine& diags_;
  bool trial_ = false;               // speculative mode: no diagnostics
  std::uint32_t switchExprDepth_ = 0;  // for contextual `yield`

  const Token& cur() const noexcept { return toks_[pos_]; }
  const Token& peek(std::size_t n) const noexcept;
  [[nodiscard]] Tok curKind() const noexcept { return toks_[pos_].kind; }
  [[nodiscard]] bool at(Tok k) const noexcept { return curKind() == k; }
  [[nodiscard]] bool atContextual(std::string_view text) const noexcept;
  // Returns tokens BY VALUE: expectGreater() may split `>>` tokens, which
  // inserts into the token vector and can reallocate it - held references
  // would dangle.
  Token advance() noexcept;
  bool accept(Tok k) noexcept;
  Token expect(Tok k);
  [[nodiscard]] SourceRange rangeFrom(std::uint32_t startOffset) const noexcept;

  // `>` that closes type arguments may be glued into `>>`, `>>>`, `>=` ...
  // by the lexer; split the current token when needed.
  Token expectGreater();

  // ------------------------------------------------------------- diagnostics
  void error(const Token& atToken, std::string msg);
  void error(std::string msg);

  struct Checkpoint {
    std::size_t pos;
    std::size_t tokenCount;
  };
  [[nodiscard]] Checkpoint mark() const noexcept { return {pos_, toks_.size()}; }
  void reset(Checkpoint cp) noexcept;

  // Runs f() in trial mode with rollback; returns f()'s result.
  template <typename F>
  auto speculate(F&& f) -> decltype(f()) {
    const Checkpoint cp = mark();
    const bool wasTrial = trial_;
    trial_ = true;
    auto result = f();
    trial_ = wasTrial;
    reset(cp);
    return result;
  }

  // ------------------------------------------------------- error recovery
  void syncToStatementEnd();
  void syncToClassMember();
  // Returns true when the cursor moved past startPos; otherwise diagnoses
  // and skips one token so loops always terminate.
  bool ensureProgress(std::size_t startPos);

  // ------------------------------------------------------------ shared syntax
  std::vector<ast::Annotation> parseAnnotations();
  ast::Annotation parseAnnotation();
  std::unique_ptr<ast::Node> parseAnnotationElementValue();
  std::uint32_t parseModifiers(bool inInterface);
  std::unique_ptr<ast::Type> parseType();
  std::unique_ptr<ast::Type> parseTypeNoArray();  // no trailing []
  bool parseTypeArgumentsInto(std::vector<std::unique_ptr<ast::Type>>& out);
  std::vector<ast::TypeParameter> parseTypeParameters();
  std::vector<std::string> parseQualifiedName(const char* what);

  // ----------------------------------------------------------- declarations
  std::unique_ptr<ast::PackageDeclaration> parsePackageDecl();
  std::unique_ptr<ast::ImportDeclaration> parseImportDecl();
  std::unique_ptr<ast::Decl> parseTopLevelTypeDecl();
  std::unique_ptr<ast::Decl> parseTypeDeclCommon(std::uint32_t start,
                                                 std::uint32_t mods,
                                                 std::vector<ast::Annotation> annos,
                                                 const MemberContext& ctx);
  std::unique_ptr<ast::Decl> parseClassDeclaration(std::uint32_t start, std::uint32_t mods,
                                                   std::vector<ast::Annotation> annos);
  std::unique_ptr<ast::Decl> parseInterfaceDeclaration(std::uint32_t start, std::uint32_t mods,
                                                       std::vector<ast::Annotation> annos);
  std::unique_ptr<ast::Decl> parseEnumDeclaration(std::uint32_t start, std::uint32_t mods,
                                                  std::vector<ast::Annotation> annos);
  std::unique_ptr<ast::Decl> parseRecordDeclaration(std::uint32_t start, std::uint32_t mods,
                                                    std::vector<ast::Annotation> annos);
  std::unique_ptr<ast::Decl> parseAnnotationTypeDeclaration(std::uint32_t start, std::uint32_t mods,
                                                            std::vector<ast::Annotation> annos);
  void parseClassBodyInto(std::vector<std::unique_ptr<ast::Decl>>& members,
                          const MemberContext& ctx);
  std::unique_ptr<ast::Decl> parseClassMember(const MemberContext& ctx);
  std::unique_ptr<ast::Decl> parseMethodDeclaration(std::uint32_t start, std::uint32_t mods,
                                                    std::vector<ast::Annotation> annos,
                                                    std::vector<ast::TypeParameter> tps,
                                                    std::unique_ptr<ast::Type> retType,
                                                    const Token& nameTok,
                                                    const MemberContext& ctx);
  std::unique_ptr<ast::Decl> parseConstructorDeclaration(std::uint32_t start, std::uint32_t mods,
                                                         std::vector<ast::Annotation> annos,
                                                         std::vector<ast::TypeParameter> tps,
                                                         const MemberContext& ctx);
  std::unique_ptr<ast::Decl> parseFieldDeclaration(std::uint32_t start, std::uint32_t mods,
                                                   std::vector<ast::Annotation> annos,
                                                   std::unique_ptr<ast::Type> type,
                                                   const Token& firstName);
  std::vector<ast::FormalParameter> parseFormalParameters();
  void parseVariableDeclaratorList(std::vector<ast::VariableDeclarator>& out);
  std::unique_ptr<ast::Expr> parseVariableInitializer();
  std::unique_ptr<ast::ArrayInitializer> parseArrayInitializer();
  std::vector<std::unique_ptr<ast::Type>> parseThrowsClause();

  // ------------------------------------------------------------- statements
  std::unique_ptr<ast::Stmt> parseStatement();
  std::unique_ptr<ast::Block> parseBlock();
  std::unique_ptr<ast::Stmt> parseLocalVariableDeclaration(bool consumeSemi);
  [[nodiscard]] bool looksLikeLocalVarDecl();
  [[nodiscard]] bool looksLikeLocalTypeDecl();
  std::unique_ptr<ast::Stmt> parseIf();
  std::unique_ptr<ast::Stmt> parseWhile();
  std::unique_ptr<ast::Stmt> parseDoWhile();
  std::unique_ptr<ast::Stmt> parseFor();
  std::unique_ptr<ast::Stmt> parseTry();
  std::unique_ptr<ast::Stmt> parseSwitchStatement();
  std::vector<ast::SwitchCase> parseSwitchCases(bool inExpression);
  ast::SwitchLabel parseSwitchLabel();
  std::unique_ptr<ast::Pattern> parsePattern();
  [[nodiscard]] bool looksLikePattern();

  // ------------------------------------------------------------ expressions
  std::unique_ptr<ast::Expr> parseExpression();
  std::unique_ptr<ast::Expr> parseConditional();
  std::unique_ptr<ast::Expr> parseBinary(int minPrec);
  std::unique_ptr<ast::Expr> parseUnary();
  std::unique_ptr<ast::Expr> parsePostfix();
  std::unique_ptr<ast::Expr> parsePrimary();
  std::unique_ptr<ast::Lambda> parseLambda();
  [[nodiscard]] bool looksLikeLambdaStart();
  [[nodiscard]] bool looksLikeCastAhead();
  std::unique_ptr<ast::Expr> parseCast();
  std::unique_ptr<ast::Expr> parseNew(std::unique_ptr<ast::Expr> qualifier);
  std::unique_ptr<ast::Expr> parseSwitchExpression();
  void parseArgumentList(std::vector<std::unique_ptr<ast::Expr>>& out);
  std::unique_ptr<ast::Expr> finishMethodReference(std::unique_ptr<ast::Node> qualifier,
                                                   std::vector<std::unique_ptr<ast::Type>> typeArgs,
                                                   bool qualifierIsType = false);

  // ------------------------------------------------------------- utilities
  [[nodiscard]] static bool primitiveTok(Tok k, ast::PrimitiveKind& out) noexcept;
  [[nodiscard]] static bool assignOpFromTok(Tok k, ast::AssignOp& out) noexcept;
  [[nodiscard]] static bool binaryOpFromTok(Tok k, ast::BinaryOp& out, int& prec) noexcept;
  [[nodiscard]] static bool canStartUnary(Tok k) noexcept;
  [[nodiscard]] static bool canStartUnaryNotPlusMinus(Tok k) noexcept;
  [[nodiscard]] static bool isVarType(const ast::Type* t) noexcept;
  [[nodiscard]] static std::unique_ptr<ast::Type> exprToType(std::unique_ptr<ast::Expr> e);
  [[nodiscard]] static std::vector<std::string> exprToQualifiedName(const ast::Expr* e);
};

}  // namespace b2::frontend
