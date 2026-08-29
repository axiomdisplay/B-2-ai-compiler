#pragma once
// B-2 Frontend - the AST: arbitrary Java syntax -> lossless tree.
//
// Design notes:
//   * Plain structs with public fields; children are owned via
//     std::unique_ptr ("Ptr<T>"). No arenas, no interning (yet) - clarity
//     first, the backend pipeline laws get their say later.
//   * Every node carries a SourceRange into the RAW source buffer, so any
//     later phase can produce exact diagnostics.
//   * Syntactic ambiguity that Java resolves semantically (is `a.b.c` a
//     package prefix, a type name, or a field access?) is preserved: we keep
//     the most general shape (Name / FieldAccess chains) and let the future
//     binding phase decide.
//   * Coverage target is Java 21-level syntax: classes, interfaces, enums,
//     records, sealed types, annotations, generics, lambdas, method
//     references, switch expressions, patterns, text blocks, var.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "b2/frontend/SourceManager.h"

namespace b2::frontend::ast {

enum class NodeKind : std::uint8_t {
  // types
  PrimitiveType,
  ClassType,
  ArrayType,
  WildcardType,
  IntersectionType,  // casts and type-parameter bounds only
  UnionType,         // multi-catch only

  // expressions
  Literal,
  Name,
  FieldAccess,
  MethodInvocation,
  ArrayAccess,
  ClassLiteral,
  ThisExpression,
  SuperAccess,
  ConstructorInvocation,  // this(...) / super(...)
  ClassInstanceCreation,
  ArrayCreation,
  ArrayInitializer,
  PrefixUnary,
  PostfixUnary,
  Binary,
  InstanceOf,
  Cast,
  Assignment,
  Conditional,
  Lambda,
  MethodReference,
  SwitchExpression,
  ParenExpression,
  AnnotationValue,  // nested annotation used as an element value

  // patterns
  TypePattern,
  RecordPattern,
  AnyPattern,

  // statements
  Block,
  EmptyStatement,
  LocalVariableDeclaration,
  ExpressionStatement,
  If,
  While,
  DoWhile,
  BasicFor,
  EnhancedFor,
  Return,
  Throw,
  BreakStatement,
  ContinueStatement,
  Yield,
  LabeledStatement,
  SwitchStatement,
  Try,
  Synchronized,
  AssertStatement,
  LocalTypeDeclaration,  // local class/interface/enum/record in a block,

  // declarations
  CompilationUnit,
  PackageDeclaration,
  ImportDeclaration,
  ClassDeclaration,
  InterfaceDeclaration,
  EnumDeclaration,
  RecordDeclaration,
  AnnotationTypeDeclaration,
  EnumConstant,
  FieldDeclaration,
  MethodDeclaration,
  ConstructorDeclaration,
  InitializerBlock,
};

struct Expr;
struct Stmt;
struct Decl;
struct Type;
struct Pattern;
struct Block;
struct CompilationUnit;
struct Annotation;

template <typename T>
using Ptr = std::unique_ptr<T>;

struct Node {
  NodeKind kind;
  SourceRange range;

 protected:
  Node(NodeKind k, SourceRange r) : kind(k), range(r) {}

 public:
  virtual ~Node() = default;
};

// ---------------------------------------------------------------- modifiers -

enum Mod : std::uint32_t {
  ModNone = 0,
  ModPublic = 1u << 0,
  ModProtected = 1u << 1,
  ModPrivate = 1u << 2,
  ModStatic = 1u << 3,
  ModFinal = 1u << 4,
  ModAbstract = 1u << 5,
  ModSealed = 1u << 6,   // contextual
  ModNonSealed = 1u << 7,
  ModTransient = 1u << 8,
  ModVolatile = 1u << 9,
  ModSynchronized = 1u << 10,
  ModNative = 1u << 11,
  ModStrictfp = 1u << 12,
  ModDefault = 1u << 13,  // interface methods
};

enum class PrimitiveKind : std::uint8_t {
  Boolean, Byte, Short, Char, Int, Long, Float, Double, Void
};

// -------------------------------------------------------------- annotations -

// An annotation use: @Name, @Name(v), @Name(k = v, ...).
// Element values are Expr, nested Annotation, or ArrayInitializer; all are
// Nodes, hence NodePtr.
struct Annotation {
  struct ElementValue {
    SourceRange range;
    std::string name;  // empty for the single-element form @A(v)
    Ptr<Node> value;   // Expr | Annotation | ArrayInitializer
  };

  SourceRange range;
  std::vector<std::string> name;  // qualified: {"java", "lang", "Override"}
  bool hasArguments = false;      // @A vs @A(...)
  bool singleElement = false;     // @A(v) vs @A(k = v)
  std::vector<ElementValue> values;
};

// Wraps a nested annotation so it can sit in an ElementValue (a Node slot).
struct AnnotationValue : Node {
  Annotation annotation;
  AnnotationValue(SourceRange r, Annotation a)
      : Node(NodeKind::AnnotationValue, r), annotation(std::move(a)) {}
};

// -------------------------------------------------------------------- types -

struct Type : Node {
  // Annotations written on the type (@NotNull String, @A int @B []).
  std::vector<Annotation> annotations;

 protected:
  Type(NodeKind k, SourceRange r) : Node(k, r) {}
};

struct PrimitiveType : Type {
  PrimitiveKind primitive;
  PrimitiveType(SourceRange r, PrimitiveKind p) : Type(NodeKind::PrimitiveType, r), primitive(p) {}
};

struct ClassType : Type {
  // A dotted type with optional type arguments per segment: a.B<C>.D<E>.
  struct Segment {
    std::string name;
    SourceRange range;
    std::vector<Ptr<Type>> args;  // empty when no <...>
    bool hasTypeArgs = false;
  };
  std::vector<Segment> segments;
  ClassType(SourceRange r, std::vector<Segment> s)
      : Type(NodeKind::ClassType, r), segments(std::move(s)) {}
};

struct ArrayType : Type {
  Ptr<Type> elementType;  // nested arrays stack: int[][] == Array(int[])
  ArrayType(SourceRange r, Ptr<Type> e)
      : Type(NodeKind::ArrayType, r), elementType(std::move(e)) {}
};

struct WildcardType : Type {
  bool superBound = false;  // ? super X  vs  ? extends X / ?
  Ptr<Type> bound;          // null for bare ?
  WildcardType(SourceRange r) : Type(NodeKind::WildcardType, r) {}
};

struct IntersectionType : Type {  // (A & B) casts, <T extends A & B>
  std::vector<Ptr<Type>> types;
  IntersectionType(SourceRange r, std::vector<Ptr<Type>> t)
      : Type(NodeKind::IntersectionType, r), types(std::move(t)) {}
};

struct UnionType : Type {  // catch (A | B e)
  std::vector<Ptr<Type>> types;
  UnionType(SourceRange r, std::vector<Ptr<Type>> t)
      : Type(NodeKind::UnionType, r), types(std::move(t)) {}
};

struct TypeParameter {
  SourceRange range;
  std::vector<Annotation> annotations;
  std::string name;
  std::vector<Ptr<Type>> bounds;  // after `extends`; IntersectionType when > 1
};

// -------------------------------------------------------------- expressions -

enum class LitKind : std::uint8_t {
  Int, Long, Float, Double, Char, Boolean, String, Null
};

enum class UnaryOp : std::uint8_t { Plus, Minus, BitNot, LogicalNot, PreInc, PreDec };
enum class PostfixOp : std::uint8_t { Inc, Dec };
enum class BinaryOp : std::uint8_t {
  Mul, Div, Mod, Add, Sub, Shl, Shr, UShr,
  Lt, Gt, Le, Ge, Eq, Ne,
  And, Xor, Or, CAnd, COr
};
enum class AssignOp : std::uint8_t {
  Assign, AddA, SubA, MulA, DivA, ModA,
  AndA, OrA, XorA, ShlA, ShrA, UShrA
};

struct Expr : Node {
 protected:
  Expr(NodeKind k, SourceRange r) : Node(k, r) {}
};

struct Literal : Expr {
  LitKind lit;
  std::uint64_t intValue = 0;   // Int, Long, Char (code point), Boolean (0/1)
  double floatValue = 0.0;      // Float, Double
  std::string stringValue;      // String (decoded), display for Char
  Literal(SourceRange r, LitKind k) : Expr(NodeKind::Literal, r), lit(k) {}
};

// A simple name in expression position; could be a local, field, or type.
struct Name : Expr {
  std::string identifier;
  Name(SourceRange r, std::string id)
      : Expr(NodeKind::Name, r), identifier(std::move(id)) {}
};

struct FieldAccess : Expr {  // target.name (target may be a Name chain)
  Ptr<Expr> target;
  std::string name;
  SourceRange nameRange;
  FieldAccess(SourceRange r, Ptr<Expr> t, std::string n, SourceRange nr)
      : Expr(NodeKind::FieldAccess, r), target(std::move(t)), name(std::move(n)), nameRange(nr) {}
};

struct MethodInvocation : Expr {
  Ptr<Expr> target;  // null = unqualified call m(...)
  bool hasExplicitTypeArgs = false;
  std::vector<Ptr<Type>> typeArgs;
  std::string name;
  SourceRange nameRange;
  std::vector<Ptr<Expr>> arguments;
  MethodInvocation(SourceRange r) : Expr(NodeKind::MethodInvocation, r) {}
};

struct ArrayAccess : Expr {
  Ptr<Expr> array;
  Ptr<Expr> index;
  ArrayAccess(SourceRange r, Ptr<Expr> a, Ptr<Expr> i)
      : Expr(NodeKind::ArrayAccess, r), array(std::move(a)), index(std::move(i)) {}
};

struct ClassLiteral : Expr {  // int.class, Foo[].class
  Ptr<Type> type;
  ClassLiteral(SourceRange r, Ptr<Type> t)
      : Expr(NodeKind::ClassLiteral, r), type(std::move(t)) {}
};

struct ThisExpression : Expr {  // this / Outer.this
  std::vector<std::string> qualifier;
  ThisExpression(SourceRange r, std::vector<std::string> q)
      : Expr(NodeKind::ThisExpression, r), qualifier(std::move(q)) {}
};

struct SuperAccess : Expr {  // super.field / super.m() target; T.super...
  std::vector<std::string> qualifier;
  SuperAccess(SourceRange r, std::vector<std::string> q)
      : Expr(NodeKind::SuperAccess, r), qualifier(std::move(q)) {}
};

struct ConstructorInvocation : Expr {  // this(...) / super(...)
  bool isSuper;
  std::vector<std::string> qualifier;  // for Outer.this(...) forms
  std::vector<Ptr<Expr>> arguments;
  ConstructorInvocation(SourceRange r, bool sup)
      : Expr(NodeKind::ConstructorInvocation, r), isSuper(sup) {}
};

struct ClassInstanceCreation : Expr {  // new Foo(...) [anonymous body]
  Ptr<Expr> qualifier;  // Outer.new Inner(...); null usually
  bool hasExplicitTypeArgs = false;
  std::vector<Ptr<Type>> typeArgs;  // new <T> Foo(...)
  Ptr<Type> type;                   // ClassType (or ArrayType never here)
  std::vector<Ptr<Expr>> arguments;
  Ptr<struct ClassDeclaration> anonymousBody;  // null when not anonymous
  ClassInstanceCreation(SourceRange r) : Expr(NodeKind::ClassInstanceCreation, r) {}
};

// One dimension of an array creation: new int[3][] -> [{3}, {}].
struct ArrayDim {
  Ptr<Expr> size;  // null = empty dimension
  SourceRange range;
};

struct ArrayInitializer : Expr {  // {1, 2, 3}; also annotation arrays
  std::vector<Ptr<Expr>> values;
  ArrayInitializer(SourceRange r) : Expr(NodeKind::ArrayInitializer, r) {}
};

struct ArrayCreation : Expr {  // new int[3][4] / new int[] {1,2}
  Ptr<Type> elementType;
  std::vector<ArrayDim> dimensions;
  Ptr<ArrayInitializer> initializer;  // null unless new int[] {...}
  ArrayCreation(SourceRange r) : Expr(NodeKind::ArrayCreation, r) {}
};

struct PrefixUnary : Expr {
  UnaryOp op;
  Ptr<Expr> operand;
  PrefixUnary(SourceRange r, UnaryOp o, Ptr<Expr> e)
      : Expr(NodeKind::PrefixUnary, r), op(o), operand(std::move(e)) {}
};

struct PostfixUnary : Expr {
  PostfixOp op;
  Ptr<Expr> operand;
  PostfixUnary(SourceRange r, PostfixOp o, Ptr<Expr> e)
      : Expr(NodeKind::PostfixUnary, r), op(o), operand(std::move(e)) {}
};

struct Binary : Expr {
  BinaryOp op;
  Ptr<Expr> lhs;
  Ptr<Expr> rhs;
  Binary(SourceRange r, BinaryOp o, Ptr<Expr> l, Ptr<Expr> h)
      : Expr(NodeKind::Binary, r), op(o), lhs(std::move(l)), rhs(std::move(h)) {}
};

struct InstanceOf : Expr {
  Ptr<Expr> expression;
  Ptr<Type> type;    // null when a record pattern carries the type
  Ptr<Pattern> pattern;  // null for plain `x instanceof T`
  InstanceOf(SourceRange r) : Expr(NodeKind::InstanceOf, r) {}
};

struct Cast : Expr {
  Ptr<Type> type;  // may be IntersectionType
  Ptr<Expr> expression;
  Cast(SourceRange r, Ptr<Type> t, Ptr<Expr> e)
      : Expr(NodeKind::Cast, r), type(std::move(t)), expression(std::move(e)) {}
};

struct Assignment : Expr {
  AssignOp op;
  Ptr<Expr> target;
  Ptr<Expr> value;
  Assignment(SourceRange r, AssignOp o, Ptr<Expr> t, Ptr<Expr> v)
      : Expr(NodeKind::Assignment, r), op(o), target(std::move(t)), value(std::move(v)) {}
};

struct Conditional : Expr {
  Ptr<Expr> condition;
  Ptr<Expr> thenExpr;
  Ptr<Expr> elseExpr;
  Conditional(SourceRange r, Ptr<Expr> c, Ptr<Expr> t, Ptr<Expr> e)
      : Expr(NodeKind::Conditional, r), condition(std::move(c)), thenExpr(std::move(t)),
        elseExpr(std::move(e)) {}
};

struct LambdaParam {
  SourceRange range;
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  Ptr<Type> type;  // null = inferred (`x -> ...`)
  std::string name;
  SourceRange nameRange;
};

struct Lambda : Expr {
  std::vector<LambdaParam> parameters;
  bool parenthesized = true;   // `x ->` vs `(x) ->`
  Ptr<Node> body;              // Expr (expression body) | Block
  Lambda(SourceRange r) : Expr(NodeKind::Lambda, r) {}
};

struct MethodReference : Expr {  // Type::m, expr::m, Type::new, int[]::new
  Ptr<Node> qualifier;   // Expr (Name chain, this, super) or Type
  bool qualifierIsType = false;
  bool hasExplicitTypeArgs = false;
  std::vector<Ptr<Type>> typeArgs;
  std::string name;  // "new" when isConstructorRef
  bool isConstructorRef = false;
  MethodReference(SourceRange r) : Expr(NodeKind::MethodReference, r) {}
};

struct ParenExpression : Expr {
  Ptr<Expr> inner;
  ParenExpression(SourceRange r, Ptr<Expr> e)
      : Expr(NodeKind::ParenExpression, r), inner(std::move(e)) {}
};

// ----------------------------------------------------------------- patterns -

struct Pattern : Node {
 protected:
  Pattern(NodeKind k, SourceRange r) : Node(k, r) {}
};

struct TypePattern : Pattern {  // Type name
  Ptr<Type> type;
  std::string binding;  // empty only when used as `case Type` (not bound)
  SourceRange bindingRange;
  TypePattern(SourceRange r, Ptr<Type> t, std::string b, SourceRange br)
      : Pattern(NodeKind::TypePattern, r), type(std::move(t)), binding(std::move(b)),
        bindingRange(br) {}
};

struct RecordPattern : Pattern {  // Type(Pattern, Pattern, ...)
  Ptr<Type> type;
  std::vector<Ptr<Pattern>> components;
  RecordPattern(SourceRange r, Ptr<Type> t, std::vector<Ptr<Pattern>> c)
      : Pattern(NodeKind::RecordPattern, r), type(std::move(t)), components(std::move(c)) {}
};

struct AnyPattern : Pattern {  // `_` (unnamed)
  AnyPattern(SourceRange r) : Pattern(NodeKind::AnyPattern, r) {}
};

// --------------------------------------------------------------- statements -

struct Stmt : Node {
 protected:
  Stmt(NodeKind k, SourceRange r) : Node(k, r) {}
};

struct Block : Stmt {
  std::vector<Ptr<Stmt>> statements;
  explicit Block(SourceRange r) : Stmt(NodeKind::Block, r) {}
};

struct EmptyStatement : Stmt {
  explicit EmptyStatement(SourceRange r) : Stmt(NodeKind::EmptyStatement, r) {}
};

// `int a, b[] = {1,2};` - the declarator's extraDims apply on top of `type`.
struct VariableDeclarator {
  SourceRange range;
  std::string name;
  SourceRange nameRange;
  std::vector<SourceRange> extraDims;  // C-style `int a[]`
  Ptr<Expr> initializer;
};

struct LocalVariableDeclaration : Stmt {
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  Ptr<Type> type;
  std::vector<VariableDeclarator> declarators;
  bool isVar = false;  // declared with contextual `var`
  LocalVariableDeclaration(SourceRange r) : Stmt(NodeKind::LocalVariableDeclaration, r) {}
};

struct ExpressionStatement : Stmt {
  Ptr<Expr> expression;
  ExpressionStatement(SourceRange r, Ptr<Expr> e)
      : Stmt(NodeKind::ExpressionStatement, r), expression(std::move(e)) {}
};

struct If : Stmt {
  Ptr<Expr> condition;
  Ptr<Stmt> thenStmt;
  Ptr<Stmt> elseStmt;  // null
  If(SourceRange r) : Stmt(NodeKind::If, r) {}
};

struct While : Stmt {
  Ptr<Expr> condition;
  Ptr<Stmt> body;
  While(SourceRange r) : Stmt(NodeKind::While, r) {}
};

struct DoWhile : Stmt {
  Ptr<Stmt> body;
  Ptr<Expr> condition;
  DoWhile(SourceRange r) : Stmt(NodeKind::DoWhile, r) {}
};

struct BasicFor : Stmt {
  std::vector<Ptr<Stmt>> init;  // LocalVariableDeclaration | ExpressionStatement
  Ptr<Expr> condition;          // null = empty
  std::vector<Ptr<Expr>> update;
  Ptr<Stmt> body;
  BasicFor(SourceRange r) : Stmt(NodeKind::BasicFor, r) {}
};

struct EnhancedFor : Stmt {
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  Ptr<Type> type;
  bool isVar = false;
  std::string name;
  SourceRange nameRange;
  Ptr<Expr> iterable;
  Ptr<Stmt> body;
  EnhancedFor(SourceRange r) : Stmt(NodeKind::EnhancedFor, r) {}
};

struct Return : Stmt {
  Ptr<Expr> expression;  // null
  Return(SourceRange r) : Stmt(NodeKind::Return, r) {}
};

struct Throw : Stmt {
  Ptr<Expr> expression;
  Throw(SourceRange r, Ptr<Expr> e) : Stmt(NodeKind::Throw, r), expression(std::move(e)) {}
};

struct BreakStatement : Stmt {
  std::string label;
  bool hasLabel = false;
  BreakStatement(SourceRange r) : Stmt(NodeKind::BreakStatement, r) {}
};

struct ContinueStatement : Stmt {
  std::string label;
  bool hasLabel = false;
  ContinueStatement(SourceRange r) : Stmt(NodeKind::ContinueStatement, r) {}
};

struct Yield : Stmt {  // switch-expression result: yield e;
  Ptr<Expr> value;
  Yield(SourceRange r, Ptr<Expr> e) : Stmt(NodeKind::Yield, r), value(std::move(e)) {}
};

struct LabeledStatement : Stmt {
  std::string label;
  Ptr<Stmt> statement;
  LabeledStatement(SourceRange r, std::string l, Ptr<Stmt> s)
      : Stmt(NodeKind::LabeledStatement, r), label(std::move(l)), statement(std::move(s)) {}
};

// One `case` label element: default | null | constant expression | pattern.
struct SwitchLabel {
  SourceRange range;
  bool isDefault = false;
  bool isNull = false;          // case null
  Ptr<Expr> constant;           // case A, case 1+2, case Season.SPRING
  Ptr<Pattern> pattern;         // case Type t / case Rec(...) / case _
};

// One case group:  case A, B when g -> body   |   case A: stmts...
struct SwitchCase {
  SourceRange range;
  bool isRule = false;                 // `->` form
  std::vector<SwitchLabel> labels;
  Ptr<Expr> guard;                     // `when expr` (rule form only)
  Ptr<Node> ruleBody;                  // rule form: Expr | Block | Throw
  std::vector<Ptr<Stmt>> statements;   // colon form
};

struct SwitchStatement : Stmt {
  Ptr<Expr> selector;
  std::vector<SwitchCase> cases;
  SwitchStatement(SourceRange r) : Stmt(NodeKind::SwitchStatement, r) {}
};

struct SwitchExpression : Expr {
  Ptr<Expr> selector;
  std::vector<SwitchCase> cases;
  SwitchExpression(SourceRange r) : Expr(NodeKind::SwitchExpression, r) {}
};

struct CatchClause {
  SourceRange range;
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  Ptr<Type> type;  // ClassType or UnionType
  std::string name;
  SourceRange nameRange;
  Ptr<Block> body;
};

struct TryResource {
  SourceRange range;
  bool isDeclaration = false;
  Ptr<LocalVariableDeclaration> decl;  // valid when isDeclaration
  Ptr<Expr> access;                    // otherwise: Name / FieldAccess (final var)
};

struct Try : Stmt {
  std::vector<TryResource> resources;
  Ptr<Block> block;
  std::vector<CatchClause> catches;
  Ptr<Block> finallyBlock;  // null
  Try(SourceRange r) : Stmt(NodeKind::Try, r) {}
};

struct Synchronized : Stmt {
  Ptr<Expr> lock;
  Ptr<Block> body;
  Synchronized(SourceRange r) : Stmt(NodeKind::Synchronized, r) {}
};

struct AssertStatement : Stmt {
  Ptr<Expr> condition;
  Ptr<Expr> message;  // null
  AssertStatement(SourceRange r) : Stmt(NodeKind::AssertStatement, r) {}
};

// A local class/interface/enum/record declared inside a block or method.
struct LocalTypeDeclaration : Stmt {
  Ptr<Decl> decl;
  LocalTypeDeclaration(SourceRange r, Ptr<Decl> d)
      : Stmt(NodeKind::LocalTypeDeclaration, r), decl(std::move(d)) {}
};

// -------------------------------------------------------------- declarations -

struct Decl : Node {
 protected:
  Decl(NodeKind k, SourceRange r) : Node(k, r) {}
};

struct PackageDeclaration : Decl {
  std::vector<Annotation> annotations;
  std::vector<std::string> name;
  PackageDeclaration(SourceRange r) : Decl(NodeKind::PackageDeclaration, r) {}
};

struct ImportDeclaration : Decl {
  bool isStatic = false;
  bool isOnDemand = false;  // ends with .*
  std::vector<std::string> name;
  ImportDeclaration(SourceRange r) : Decl(NodeKind::ImportDeclaration, r) {}
};

struct FormalParameter {
  SourceRange range;
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  Ptr<Type> type;
  std::string name;
  SourceRange nameRange;
  bool isVarArgs = false;   // last parameter: Type...
  bool isReceiver = false;  // Type this
};

struct FieldDeclaration : Decl {
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  Ptr<Type> type;
  std::vector<VariableDeclarator> declarators;
  FieldDeclaration(SourceRange r) : Decl(NodeKind::FieldDeclaration, r) {}
};

struct MethodDeclaration : Decl {
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  std::vector<TypeParameter> typeParameters;
  Ptr<Type> returnType;  // PrimitiveType(Void) for void
  std::string name;
  SourceRange nameRange;
  std::vector<FormalParameter> parameters;
  std::vector<Ptr<Type>> throws;  // ClassTypes
  Ptr<Block> body;                 // null: abstract / native
  Ptr<Node> defaultValue;          // annotation elements only: Expr | Annotation
  MethodDeclaration(SourceRange r) : Decl(NodeKind::MethodDeclaration, r) {}
};

struct ConstructorDeclaration : Decl {
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  std::vector<TypeParameter> typeParameters;
  std::string name;
  SourceRange nameRange;
  bool isCompact = false;  // record compact constructor: R { ... }
  std::vector<FormalParameter> parameters;
  std::vector<Ptr<Type>> throws;
  Ptr<Block> body;
  ConstructorDeclaration(SourceRange r) : Decl(NodeKind::ConstructorDeclaration, r) {}
};

struct InitializerBlock : Decl {  // static { ... } / { ... }
  bool isStatic = false;
  Ptr<Block> body;
  InitializerBlock(SourceRange r) : Decl(NodeKind::InitializerBlock, r) {}
};

struct EnumConstant {
  SourceRange range;
  std::vector<Annotation> annotations;
  std::string name;
  SourceRange nameRange;
  std::vector<Ptr<Expr>> arguments;
  Ptr<struct ClassDeclaration> body;  // constant-specific body; null
};

struct ClassDeclaration : Decl {
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  std::string name;
  SourceRange nameRange;
  std::vector<TypeParameter> typeParameters;
  Ptr<Type> extendsType;                 // null
  std::vector<Ptr<Type>> implements;     // ClassTypes
  std::vector<Ptr<Type>> permits;        // ClassTypes (sealed)
  std::vector<Ptr<Decl>> members;        // Field|Method|Ctor|Init|nested TypeDecl
  ClassDeclaration(SourceRange r) : Decl(NodeKind::ClassDeclaration, r) {}
};

struct InterfaceDeclaration : Decl {
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  std::string name;
  SourceRange nameRange;
  std::vector<TypeParameter> typeParameters;
  std::vector<Ptr<Type>> extends;  // multiple interfaces
  std::vector<Ptr<Type>> permits;
  std::vector<Ptr<Decl>> members;
  InterfaceDeclaration(SourceRange r) : Decl(NodeKind::InterfaceDeclaration, r) {}
};

struct EnumDeclaration : Decl {
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  std::string name;
  SourceRange nameRange;
  std::vector<Ptr<Type>> implements;
  std::vector<EnumConstant> constants;  // may be empty
  std::vector<Ptr<Decl>> members;       // after the optional ';'
  EnumDeclaration(SourceRange r) : Decl(NodeKind::EnumDeclaration, r) {}
};

struct RecordComponent {
  SourceRange range;
  std::vector<Annotation> annotations;
  Ptr<Type> type;
  std::string name;
  SourceRange nameRange;
};

struct RecordDeclaration : Decl {
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  std::string name;
  SourceRange nameRange;
  std::vector<TypeParameter> typeParameters;
  std::vector<RecordComponent> components;
  std::vector<Ptr<Type>> implements;
  std::vector<Ptr<Decl>> members;
  RecordDeclaration(SourceRange r) : Decl(NodeKind::RecordDeclaration, r) {}
};

struct AnnotationTypeDeclaration : Decl {
  std::vector<Annotation> annotations;
  std::uint32_t mods = ModNone;
  std::string name;
  SourceRange nameRange;
  std::vector<Ptr<Decl>> members;  // MethodDeclaration (elements), nested types
  AnnotationTypeDeclaration(SourceRange r) : Decl(NodeKind::AnnotationTypeDeclaration, r) {}
};

struct CompilationUnit : Decl {
  Ptr<PackageDeclaration> package;  // null
  std::vector<Ptr<ImportDeclaration>> imports;
  std::vector<Ptr<Decl>> types;     // Class/Interface/Enum/Record/Annotation decls
  CompilationUnit(SourceRange r) : Decl(NodeKind::CompilationUnit, r) {}
};

}  // namespace b2::frontend::ast
