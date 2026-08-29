// B-2 Frontend - AST pretty-printer implementation (see AstDumper.h).
//
// Output contract (deterministic; golden tests depend on every choice):
//   * One node per line; two spaces of indentation per AST depth level.
//   * Line = "[Kind]" + " key=value" fields + " @offset+length", where the
//     range is the node's RAW byte offset and length and always closes the
//     line. Children follow on the next lines, one level deeper.
//   * Null pointers and empty vectors print nothing.
//
// Documented decisions (applied uniformly, recorded once here):
//   * Numbers: int/long literals print as unsigned decimal (value=42);
//     float/double literals print with printf "%f" - e.g. value=1.500000 -
//     the ONE floating rendering used everywhere. inf/-inf/nan are spelled
//     out so output stays stable across libm versions.
//   * Quoted payloads (names, strings, chars, labels, qualifiers) are
//     single-quoted by escapeQuoted(): backslash, quote, newline, tab and
//     CR render as \\ \' \n \t \r; other C0/DEL bytes render as \xNN.
//   * Char literals: Ast.h's Literal keeps the code point in intValue (the
//     parser fills stringValue only for strings), so the char payload is
//     UTF-8-encoded from intValue when stringValue is empty.
//   * Text blocks: Ast.h's Literal has no isTextBlock flag (the lexer's
//     Token has one, the AST node does not), so textBlock=true is recovered
//     from the raw source - a String literal whose range starts with three
//     double quotes. The SourceManager is used only for that sniff.
//   * Field order: name= (when the node has one), mods= (when non-empty),
//     then node-specific flags; @range is always last.
//   * Booleans: structural flags the spec pins as true/false print both
//     ways - super (WildcardType), rule (SwitchCase), declared (Resource),
//     inferred (Param), isSuper (ConstructorInvocation), qualifierIsType
//     and explicitTypeArgs (MethodReference, MethodInvocation,
//     ClassInstanceCreation). Other booleans print only when true (static,
//     onDemand, textBlock, singleElement, var, varargs, receiver, compact,
//     ctor, label), except Lambda's parenthesized, which prints only when
//     false.
//   * mods=[...] lists modifiers in the fixed order public protected
//     private static final abstract sealed non-sealed transient volatile
//     synchronized native strictfp default; the key is omitted when empty.
//   * Pseudo-nodes - AST structs that are not Node subclasses - get the
//     same line shape (bracket, fields, @range): [TypeParameter],
//     [FormalParameter], [RecordComponent], [EnumConstant], [ElementValue],
//     [SwitchCase], [Label], [Dim], [Resource], [Catch], [Declarator] and
//     [Param] (the spec's short name for lambda parameters).
//   * dump(unit, out) APPENDS the rendered tree to out.

#include "b2/frontend/AstDumper.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "b2/frontend/SourceManager.h"

namespace b2::frontend {
namespace {

using namespace ast;

// --------------------------------------------------------- spelling helpers -

const char* primitiveKindName(PrimitiveKind k) {
  switch (k) {
    case PrimitiveKind::Boolean: return "boolean";
    case PrimitiveKind::Byte: return "byte";
    case PrimitiveKind::Short: return "short";
    case PrimitiveKind::Char: return "char";
    case PrimitiveKind::Int: return "int";
    case PrimitiveKind::Long: return "long";
    case PrimitiveKind::Float: return "float";
    case PrimitiveKind::Double: return "double";
    case PrimitiveKind::Void: return "void";
  }
  return "?";  // only reachable with a corrupted AST
}

const char* litKindName(LitKind k) {
  switch (k) {
    case LitKind::Int: return "int";
    case LitKind::Long: return "long";
    case LitKind::Float: return "float";
    case LitKind::Double: return "double";
    case LitKind::Char: return "char";
    case LitKind::Boolean: return "boolean";
    case LitKind::String: return "string";
    case LitKind::Null: return "null";
  }
  return "?";
}

const char* unaryOpName(UnaryOp op) {
  switch (op) {
    case UnaryOp::Plus: return "+";
    case UnaryOp::Minus: return "-";
    case UnaryOp::BitNot: return "~";
    case UnaryOp::LogicalNot: return "!";
    case UnaryOp::PreInc: return "++";
    case UnaryOp::PreDec: return "--";
  }
  return "?";
}

const char* postfixOpName(PostfixOp op) {
  switch (op) {
    case PostfixOp::Inc: return "++";
    case PostfixOp::Dec: return "--";
  }
  return "?";
}

const char* binaryOpName(BinaryOp op) {
  switch (op) {
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Mod: return "%";
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Shl: return "<<";
    case BinaryOp::Shr: return ">>";
    case BinaryOp::UShr: return ">>>";
    case BinaryOp::Lt: return "<";
    case BinaryOp::Gt: return ">";
    case BinaryOp::Le: return "<=";
    case BinaryOp::Ge: return ">=";
    case BinaryOp::Eq: return "==";
    case BinaryOp::Ne: return "!=";
    case BinaryOp::And: return "&";
    case BinaryOp::Xor: return "^";
    case BinaryOp::Or: return "|";
    case BinaryOp::CAnd: return "&&";
    case BinaryOp::COr: return "||";
  }
  return "?";
}

const char* assignOpName(AssignOp op) {
  switch (op) {
    case AssignOp::Assign: return "=";
    case AssignOp::AddA: return "+=";
    case AssignOp::SubA: return "-=";
    case AssignOp::MulA: return "*=";
    case AssignOp::DivA: return "/=";
    case AssignOp::ModA: return "%=";
    case AssignOp::AndA: return "&=";
    case AssignOp::OrA: return "|=";
    case AssignOp::XorA: return "^=";
    case AssignOp::ShlA: return "<<=";
    case AssignOp::ShrA: return ">>=";
    case AssignOp::UShrA: return ">>>=";
  }
  return "?";
}

// One modifier bit -> its Java spelling.
const char* modName(Mod m) {
  switch (m) {
    case ModPublic: return "public";
    case ModProtected: return "protected";
    case ModPrivate: return "private";
    case ModStatic: return "static";
    case ModFinal: return "final";
    case ModAbstract: return "abstract";
    case ModSealed: return "sealed";
    case ModNonSealed: return "non-sealed";
    case ModTransient: return "transient";
    case ModVolatile: return "volatile";
    case ModSynchronized: return "synchronized";
    case ModNative: return "native";
    case ModStrictfp: return "strictfp";
    case ModDefault: return "default";
    case ModNone: break;
  }
  return "";  // unknown bits (corrupted AST) contribute nothing
}

// Modifier bits -> "public static", in the fixed order above. Empty string
// when no bit is set; callers wrap it in mods=[...].
std::string modsToString(std::uint32_t mods) {
  static const Mod kOrder[] = {
      ModPublic,  ModProtected,    ModPrivate,     ModStatic,
      ModFinal,   ModAbstract,     ModSealed,      ModNonSealed,
      ModTransient, ModVolatile,  ModSynchronized, ModNative,
      ModStrictfp, ModDefault,
  };
  std::string text;
  for (const Mod m : kOrder) {
    if ((mods & m) != 0) {
      if (!text.empty()) text.push_back(' ');
      text += modName(m);
    }
  }
  return text;
}

// ----------------------------------------------------------- value helpers -

// Single-quoted payload with a small escaper: \\ \' \n \t \r, other C0/DEL
// control bytes as \xNN, everything else (UTF-8 included) passes through.
std::string escapeQuoted(const std::string& raw) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string quoted;
  quoted.reserve(raw.size() + 2);
  quoted.push_back('\'');
  for (const char ch : raw) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '\\': quoted += "\\\\"; break;
      case '\'': quoted += "\\'"; break;
      case '\n': quoted += "\\n"; break;
      case '\t': quoted += "\\t"; break;
      case '\r': quoted += "\\r"; break;
      default:
        if (c < 0x20 || c == 0x7F) {
          quoted += "\\x";
          quoted.push_back(kHex[c >> 4]);
          quoted.push_back(kHex[c & 0x0F]);
        } else {
          quoted.push_back(ch);
        }
    }
  }
  quoted.push_back('\'');
  return quoted;
}

// {"java", "util", "List"} -> "java.util.List"; {} -> "".
std::string dotted(const std::vector<std::string>& parts) {
  std::string text;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) text.push_back('.');
    text += parts[i];
  }
  return text;
}

// The one floating rendering: printf "%f" (six fractional digits), with
// inf/-inf/nan spelled out for cross-platform stability.
std::string doubleText(double v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
  char buf[512];  // large enough for %f of DBL_MAX (~309 digits + ".000000")
  std::snprintf(buf, sizeof buf, "%f", v);
  return std::string(buf);
}

// Appends one code point as UTF-8; out-of-range values (corrupt AST) are
// dropped rather than crashing.
void appendCodePoint(std::string& out, char32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// ----------------------------------------------------------------- dumper -

struct Dumper {
  const SourceManager* sm;
  std::string out;
  int depth = 0;

  explicit Dumper(const SourceManager* sourceManager) : sm(sourceManager) {}

  // ---- line emission ----

  void open(const char* bracket) {
    out.append(2 * static_cast<std::size_t>(depth), ' ');
    out.push_back('[');
    out.append(bracket);
    out.push_back(']');
  }

  void field(const char* key, const std::string& value) {
    out.push_back(' ');
    out.append(key);
    out.push_back('=');
    out.append(value);
  }

  // Bare word (no '='), used by the [Label] and [Dim] markers.
  void word(const char* marker) {
    out.push_back(' ');
    out.append(marker);
  }

  void range(SourceRange r) {
    out += " @";
    out += std::to_string(r.offset);
    out.push_back('+');
    out += std::to_string(r.length);
  }

  // Ends the current node line with its source range.
  void closeLine(SourceRange r) {
    range(r);
    out.push_back('\n');
  }

  void boolField(const char* key, bool value) {
    field(key, value ? "true" : "false");
  }

  void modsField(std::uint32_t mods) {
    if (mods != 0) field("mods", "[" + modsToString(mods) + "]");
  }

  void child(const Node* n) {
    if (n == nullptr) return;
    ++depth;
    dispatch(*n);
    --depth;
  }

  // No default case: -Wswitch turns a forgotten NodeKind into a compile
  // error, which is this file's completeness guarantee.
  void dispatch(const Node& n) {
    switch (n.kind) {
      case NodeKind::PrimitiveType: primitiveType(static_cast<const PrimitiveType&>(n)); break;
      case NodeKind::ClassType: classType(static_cast<const ClassType&>(n)); break;
      case NodeKind::ArrayType: arrayType(static_cast<const ArrayType&>(n)); break;
      case NodeKind::WildcardType: wildcardType(static_cast<const WildcardType&>(n)); break;
      case NodeKind::IntersectionType: intersectionType(static_cast<const IntersectionType&>(n)); break;
      case NodeKind::UnionType: unionType(static_cast<const UnionType&>(n)); break;
      case NodeKind::Literal: literal(static_cast<const Literal&>(n)); break;
      case NodeKind::Name: nameExpr(static_cast<const Name&>(n)); break;
      case NodeKind::FieldAccess: fieldAccess(static_cast<const FieldAccess&>(n)); break;
      case NodeKind::MethodInvocation: methodInvocation(static_cast<const MethodInvocation&>(n)); break;
      case NodeKind::ArrayAccess: arrayAccess(static_cast<const ArrayAccess&>(n)); break;
      case NodeKind::ClassLiteral: classLiteral(static_cast<const ClassLiteral&>(n)); break;
      case NodeKind::ThisExpression: thisExpression(static_cast<const ThisExpression&>(n)); break;
      case NodeKind::SuperAccess: superAccess(static_cast<const SuperAccess&>(n)); break;
      case NodeKind::ConstructorInvocation: constructorInvocation(static_cast<const ConstructorInvocation&>(n)); break;
      case NodeKind::ClassInstanceCreation: classInstanceCreation(static_cast<const ClassInstanceCreation&>(n)); break;
      case NodeKind::ArrayCreation: arrayCreation(static_cast<const ArrayCreation&>(n)); break;
      case NodeKind::ArrayInitializer: arrayInitializer(static_cast<const ArrayInitializer&>(n)); break;
      case NodeKind::PrefixUnary: prefixUnary(static_cast<const PrefixUnary&>(n)); break;
      case NodeKind::PostfixUnary: postfixUnary(static_cast<const PostfixUnary&>(n)); break;
      case NodeKind::Binary: binary(static_cast<const Binary&>(n)); break;
      case NodeKind::InstanceOf: instanceOf(static_cast<const InstanceOf&>(n)); break;
      case NodeKind::Cast: cast(static_cast<const Cast&>(n)); break;
      case NodeKind::Assignment: assignment(static_cast<const Assignment&>(n)); break;
      case NodeKind::Conditional: conditional(static_cast<const Conditional&>(n)); break;
      case NodeKind::Lambda: lambda(static_cast<const Lambda&>(n)); break;
      case NodeKind::MethodReference: methodReference(static_cast<const MethodReference&>(n)); break;
      case NodeKind::SwitchExpression: switchExpression(static_cast<const SwitchExpression&>(n)); break;
      case NodeKind::ParenExpression: parenExpression(static_cast<const ParenExpression&>(n)); break;
      case NodeKind::AnnotationValue: annotationValue(static_cast<const AnnotationValue&>(n)); break;
      case NodeKind::TypePattern: typePattern(static_cast<const TypePattern&>(n)); break;
      case NodeKind::RecordPattern: recordPattern(static_cast<const RecordPattern&>(n)); break;
      case NodeKind::AnyPattern: anyPattern(static_cast<const AnyPattern&>(n)); break;
      case NodeKind::Block: block(static_cast<const Block&>(n)); break;
      case NodeKind::EmptyStatement: emptyStatement(static_cast<const EmptyStatement&>(n)); break;
      case NodeKind::LocalVariableDeclaration: localVariableDeclaration(static_cast<const LocalVariableDeclaration&>(n)); break;
      case NodeKind::ExpressionStatement: expressionStatement(static_cast<const ExpressionStatement&>(n)); break;
      case NodeKind::If: ifStmt(static_cast<const If&>(n)); break;
      case NodeKind::While: whileStmt(static_cast<const While&>(n)); break;
      case NodeKind::DoWhile: doWhile(static_cast<const DoWhile&>(n)); break;
      case NodeKind::BasicFor: basicFor(static_cast<const BasicFor&>(n)); break;
      case NodeKind::EnhancedFor: enhancedFor(static_cast<const EnhancedFor&>(n)); break;
      case NodeKind::Return: returnStmt(static_cast<const Return&>(n)); break;
      case NodeKind::Throw: throwStmt(static_cast<const Throw&>(n)); break;
      case NodeKind::BreakStatement: breakStatement(static_cast<const BreakStatement&>(n)); break;
      case NodeKind::ContinueStatement: continueStatement(static_cast<const ContinueStatement&>(n)); break;
      case NodeKind::Yield: yield(static_cast<const Yield&>(n)); break;
      case NodeKind::LabeledStatement: labeledStatement(static_cast<const LabeledStatement&>(n)); break;
      case NodeKind::SwitchStatement: switchStatement(static_cast<const SwitchStatement&>(n)); break;
      case NodeKind::Try: tryStmt(static_cast<const Try&>(n)); break;
      case NodeKind::Synchronized: synchronized(static_cast<const Synchronized&>(n)); break;
      case NodeKind::AssertStatement: assertStatement(static_cast<const AssertStatement&>(n)); break;
      case NodeKind::LocalTypeDeclaration: localTypeDeclaration(static_cast<const LocalTypeDeclaration&>(n)); break;
      case NodeKind::CompilationUnit: compilationUnit(static_cast<const CompilationUnit&>(n)); break;
      case NodeKind::PackageDeclaration: packageDeclaration(static_cast<const PackageDeclaration&>(n)); break;
      case NodeKind::ImportDeclaration: importDeclaration(static_cast<const ImportDeclaration&>(n)); break;
      case NodeKind::ClassDeclaration: classDeclaration(static_cast<const ClassDeclaration&>(n)); break;
      case NodeKind::InterfaceDeclaration: interfaceDeclaration(static_cast<const InterfaceDeclaration&>(n)); break;
      case NodeKind::EnumDeclaration: enumDeclaration(static_cast<const EnumDeclaration&>(n)); break;
      case NodeKind::RecordDeclaration: recordDeclaration(static_cast<const RecordDeclaration&>(n)); break;
      case NodeKind::AnnotationTypeDeclaration: annotationTypeDeclaration(static_cast<const AnnotationTypeDeclaration&>(n)); break;
      case NodeKind::EnumConstant:
        // EnumConstant is a plain struct, not a Node: EnumDeclaration emits
        // the [EnumConstant] lines itself. Unreachable through dispatch.
        break;
      case NodeKind::FieldDeclaration: fieldDeclaration(static_cast<const FieldDeclaration&>(n)); break;
      case NodeKind::MethodDeclaration: methodDeclaration(static_cast<const MethodDeclaration&>(n)); break;
      case NodeKind::ConstructorDeclaration: constructorDeclaration(static_cast<const ConstructorDeclaration&>(n)); break;
      case NodeKind::InitializerBlock: initializerBlock(static_cast<const InitializerBlock&>(n)); break;
    }
  }

  // ---- shared pseudo-structure helpers ----

  void annotations(const std::vector<Annotation>& anns) {
    for (const Annotation& a : anns) {
      ++depth;
      annotation(a);
      --depth;
    }
  }

  void annotation(const Annotation& a) {
    open("Annotation"); field("name", escapeQuoted(dotted(a.name)));
    if (a.singleElement) boolField("singleElement", true);
    closeLine(a.range);
    for (const Annotation::ElementValue& ev : a.values) {
      ++depth;
      open("ElementValue"); field("name", escapeQuoted(ev.name));
      closeLine(ev.range);
      child(ev.value.get());
      --depth;
    }
  }

  void typeParams(const std::vector<TypeParameter>& tps) {
    for (const TypeParameter& tp : tps) {
      ++depth;
      open("TypeParameter"); field("name", escapeQuoted(tp.name));
      closeLine(tp.range);
      annotations(tp.annotations);
      for (const auto& bound : tp.bounds) child(bound.get());
      --depth;
    }
  }

  void formalParameters(const std::vector<FormalParameter>& ps) {
    for (const FormalParameter& p : ps) {
      ++depth;
      open("FormalParameter"); field("name", escapeQuoted(p.name));
      modsField(p.mods);
      if (p.isVarArgs) boolField("varargs", true);
      if (p.isReceiver) boolField("receiver", true);
      closeLine(p.range);
      annotations(p.annotations);
      child(p.type.get());
      --depth;
    }
  }

  void declarators(const std::vector<VariableDeclarator>& ds) {
    for (const VariableDeclarator& d : ds) {
      ++depth;
      open("Declarator"); field("name", escapeQuoted(d.name));
      field("extraDims", std::to_string(d.extraDims.size()));
      closeLine(d.range);
      child(d.initializer.get());
      --depth;
    }
  }

  void switchCases(const std::vector<SwitchCase>& cases) {
    for (const SwitchCase& c : cases) {
      ++depth;
      open("SwitchCase"); boolField("rule", c.isRule);
      closeLine(c.range);
      child(c.guard.get());
      for (const SwitchLabel& l : c.labels) {
        ++depth;
        open("Label");
        if (l.isDefault) word("default");
        else if (l.isNull) word("null");
        else if (l.constant != nullptr) word("constant");
        else if (l.pattern != nullptr) word("pattern");
        closeLine(l.range);
        child(l.constant.get());
        child(l.pattern.get());
        --depth;
      }
      child(c.ruleBody.get());
      for (const auto& st : c.statements) child(st.get());
      --depth;
    }
  }

  // ---------------------------------------------------------------- types -

  void primitiveType(const PrimitiveType& t) {
    open("PrimitiveType"); field("pk", primitiveKindName(t.primitive));
    closeLine(t.range);
    annotations(t.annotations);
  }

  void classType(const ClassType& t) {
    std::string dottedName;
    for (std::size_t i = 0; i < t.segments.size(); ++i) {
      if (i != 0) dottedName.push_back('.');
      dottedName += t.segments[i].name;
    }
    open("ClassType"); field("name", escapeQuoted(dottedName));
    closeLine(t.range);
    annotations(t.annotations);
    for (const ClassType::Segment& seg : t.segments) {
      for (const auto& arg : seg.args) child(arg.get());
    }
  }

  void arrayType(const ArrayType& t) {
    open("ArrayType"); closeLine(t.range);
    annotations(t.annotations);
    child(t.elementType.get());
  }

  void wildcardType(const WildcardType& t) {
    open("WildcardType"); boolField("super", t.superBound);
    closeLine(t.range);
    annotations(t.annotations);
    child(t.bound.get());
  }

  void intersectionType(const IntersectionType& t) {
    open("IntersectionType"); closeLine(t.range);
    annotations(t.annotations);
    for (const auto& m : t.types) child(m.get());
  }

  void unionType(const UnionType& t) {
    open("UnionType"); closeLine(t.range);
    annotations(t.annotations);
    for (const auto& m : t.types) child(m.get());
  }

  // ----------------------------------------------------------- expressions -

  // A String literal is a text block iff its raw source starts with """.
  bool isTextBlock(const Literal& lit) const {
    if (sm == nullptr || lit.lit != LitKind::String || lit.range.length < 3) {
      return false;
    }
    const std::string_view src = sm->source();
    const SourceRange r = lit.range;
    if (static_cast<std::size_t>(r.offset) + 3 > src.size()) return false;
    return src[r.offset] == '"' && src[r.offset + 1] == '"' && src[r.offset + 2] == '"';
  }

  // Char payload: stringValue when present, else the intValue code point
  // encoded as UTF-8 (that is what the parser produces).
  std::string charDisplay(const Literal& lit) {
    if (!lit.stringValue.empty()) return lit.stringValue;
    std::string text;
    appendCodePoint(text, static_cast<char32_t>(lit.intValue));
    return text;
  }

  void literal(const Literal& lit) {
    open("Literal"); field("lit", litKindName(lit.lit));
    switch (lit.lit) {
      case LitKind::Int:
      case LitKind::Long:
        field("value", std::to_string(lit.intValue));
        break;
      case LitKind::Float:
      case LitKind::Double:
        field("value", doubleText(lit.floatValue));
        break;
      case LitKind::Char:
        field("value", escapeQuoted(charDisplay(lit)));
        break;
      case LitKind::Boolean:
        boolField("value", lit.intValue != 0);
        break;
      case LitKind::String:
        field("value", escapeQuoted(lit.stringValue));
        if (isTextBlock(lit)) boolField("textBlock", true);
        break;
      case LitKind::Null:
        field("value", "null");
        break;
    }
    closeLine(lit.range);
  }

  void nameExpr(const Name& n) {
    open("Name"); field("name", escapeQuoted(n.identifier));
    closeLine(n.range);
  }

  void fieldAccess(const FieldAccess& f) {
    open("FieldAccess"); field("name", escapeQuoted(f.name));
    closeLine(f.range);
    child(f.target.get());
  }

  void methodInvocation(const MethodInvocation& m) {
    open("MethodInvocation"); field("name", escapeQuoted(m.name));
    boolField("explicitTypeArgs", m.hasExplicitTypeArgs);
    closeLine(m.range);
    for (const auto& ta : m.typeArgs) child(ta.get());
    child(m.target.get());
    for (const auto& a : m.arguments) child(a.get());
  }

  void arrayAccess(const ArrayAccess& a) {
    open("ArrayAccess"); closeLine(a.range);
    child(a.array.get());
    child(a.index.get());
  }

  void classLiteral(const ClassLiteral& c) {
    open("ClassLiteral"); closeLine(c.range);
    child(c.type.get());
  }

  void thisExpression(const ThisExpression& e) {
    open("ThisExpression");
    if (!e.qualifier.empty()) field("qualifier", escapeQuoted(dotted(e.qualifier)));
    closeLine(e.range);
  }

  void superAccess(const SuperAccess& e) {
    open("SuperAccess");
    if (!e.qualifier.empty()) field("qualifier", escapeQuoted(dotted(e.qualifier)));
    closeLine(e.range);
  }

  void constructorInvocation(const ConstructorInvocation& c) {
    open("ConstructorInvocation"); boolField("isSuper", c.isSuper);
    if (!c.qualifier.empty()) field("qualifier", escapeQuoted(dotted(c.qualifier)));
    closeLine(c.range);
    for (const auto& a : c.arguments) child(a.get());
  }

  void classInstanceCreation(const ClassInstanceCreation& c) {
    open("ClassInstanceCreation"); boolField("explicitTypeArgs", c.hasExplicitTypeArgs);
    closeLine(c.range);
    child(c.qualifier.get());
    for (const auto& ta : c.typeArgs) child(ta.get());
    child(c.type.get());
    for (const auto& a : c.arguments) child(a.get());
    child(c.anonymousBody.get());
  }

  void arrayCreation(const ArrayCreation& c) {
    open("ArrayCreation"); closeLine(c.range);
    child(c.elementType.get());
    for (const ArrayDim& d : c.dimensions) {
      ++depth;
      open("Dim"); word(d.size != nullptr ? "size" : "empty");
      closeLine(d.range);
      child(d.size.get());
      --depth;
    }
    child(c.initializer.get());
  }

  void arrayInitializer(const ArrayInitializer& a) {
    open("ArrayInitializer"); closeLine(a.range);
    for (const auto& v : a.values) child(v.get());
  }

  void prefixUnary(const PrefixUnary& u) {
    open("PrefixUnary"); field("op", unaryOpName(u.op));
    closeLine(u.range);
    child(u.operand.get());
  }

  void postfixUnary(const PostfixUnary& u) {
    open("PostfixUnary"); field("op", postfixOpName(u.op));
    closeLine(u.range);
    child(u.operand.get());
  }

  void binary(const Binary& b) {
    open("Binary"); field("op", binaryOpName(b.op));
    closeLine(b.range);
    child(b.lhs.get());
    child(b.rhs.get());
  }

  void instanceOf(const InstanceOf& e) {
    open("InstanceOf"); closeLine(e.range);
    child(e.expression.get());
    child(e.type.get());
    child(e.pattern.get());
  }

  void cast(const Cast& c) {
    open("Cast"); closeLine(c.range);
    child(c.type.get());
    child(c.expression.get());
  }

  void assignment(const Assignment& a) {
    open("Assignment"); field("op", assignOpName(a.op));
    closeLine(a.range);
    child(a.target.get());
    child(a.value.get());
  }

  void conditional(const Conditional& c) {
    open("Conditional"); closeLine(c.range);
    child(c.condition.get());
    child(c.thenExpr.get());
    child(c.elseExpr.get());
  }

  void lambda(const Lambda& l) {
    open("Lambda");
    if (!l.parenthesized) boolField("parenthesized", false);
    closeLine(l.range);
    for (const LambdaParam& p : l.parameters) {
      ++depth;
      open("Param"); field("name", escapeQuoted(p.name));
      modsField(p.mods);
      boolField("inferred", p.type == nullptr);
      closeLine(p.range);
      annotations(p.annotations);
      child(p.type.get());
      --depth;
    }
    child(l.body.get());
  }

  void methodReference(const MethodReference& m) {
    open("MethodReference"); boolField("qualifierIsType", m.qualifierIsType);
    field("name", escapeQuoted(m.name));
    boolField("explicitTypeArgs", m.hasExplicitTypeArgs);
    if (m.isConstructorRef) boolField("ctor", true);
    closeLine(m.range);
    child(m.qualifier.get());
    for (const auto& ta : m.typeArgs) child(ta.get());
  }

  void switchExpression(const SwitchExpression& e) {
    open("SwitchExpression"); closeLine(e.range);
    child(e.selector.get());
    switchCases(e.cases);
  }

  void parenExpression(const ParenExpression& e) {
    open("ParenExpression"); closeLine(e.range);
    child(e.inner.get());
  }

  void annotationValue(const AnnotationValue& v) {
    open("AnnotationValue"); closeLine(v.range);
    ++depth;
    annotation(v.annotation);
    --depth;
  }

  // -------------------------------------------------------------- patterns -

  void typePattern(const TypePattern& p) {
    open("TypePattern"); field("binding", escapeQuoted(p.binding));
    closeLine(p.range);
    child(p.type.get());
  }

  void recordPattern(const RecordPattern& p) {
    open("RecordPattern"); closeLine(p.range);
    child(p.type.get());
    for (const auto& c : p.components) child(c.get());
  }

  void anyPattern(const AnyPattern& p) {
    open("AnyPattern"); closeLine(p.range);
  }

  // ------------------------------------------------------------ statements -

  void block(const Block& b) {
    open("Block"); closeLine(b.range);
    for (const auto& s : b.statements) child(s.get());
  }

  void emptyStatement(const EmptyStatement& s) {
    open("EmptyStatement"); closeLine(s.range);
  }

  void localVariableDeclaration(const LocalVariableDeclaration& s) {
    open("LocalVariableDeclaration");
    modsField(s.mods);
    if (s.isVar) boolField("var", true);
    closeLine(s.range);
    annotations(s.annotations);
    child(s.type.get());
    declarators(s.declarators);
  }

  void expressionStatement(const ExpressionStatement& s) {
    open("ExpressionStatement"); closeLine(s.range);
    child(s.expression.get());
  }

  void ifStmt(const If& s) {
    open("If"); closeLine(s.range);
    child(s.condition.get());
    child(s.thenStmt.get());
    child(s.elseStmt.get());
  }

  void whileStmt(const While& s) {
    open("While"); closeLine(s.range);
    child(s.condition.get());
    child(s.body.get());
  }

  void doWhile(const DoWhile& s) {
    open("DoWhile"); closeLine(s.range);
    child(s.body.get());
    child(s.condition.get());
  }

  void basicFor(const BasicFor& s) {
    open("BasicFor"); closeLine(s.range);
    for (const auto& i : s.init) child(i.get());
    child(s.condition.get());
    for (const auto& u : s.update) child(u.get());
    child(s.body.get());
  }

  void enhancedFor(const EnhancedFor& s) {
    open("EnhancedFor"); field("name", escapeQuoted(s.name));
    modsField(s.mods);
    if (s.isVar) boolField("var", true);
    closeLine(s.range);
    annotations(s.annotations);
    child(s.type.get());
    child(s.iterable.get());
    child(s.body.get());
  }

  void returnStmt(const Return& s) {
    open("Return"); closeLine(s.range);
    child(s.expression.get());
  }

  void throwStmt(const Throw& s) {
    open("Throw"); closeLine(s.range);
    child(s.expression.get());
  }

  void breakStatement(const BreakStatement& s) {
    open("BreakStatement");
    if (s.hasLabel) field("label", escapeQuoted(s.label));
    closeLine(s.range);
  }

  void continueStatement(const ContinueStatement& s) {
    open("ContinueStatement");
    if (s.hasLabel) field("label", escapeQuoted(s.label));
    closeLine(s.range);
  }

  void yield(const Yield& s) {
    open("Yield"); closeLine(s.range);
    child(s.value.get());
  }

  void labeledStatement(const LabeledStatement& s) {
    open("LabeledStatement"); field("label", escapeQuoted(s.label));
    closeLine(s.range);
    child(s.statement.get());
  }

  void switchStatement(const SwitchStatement& s) {
    open("SwitchStatement"); closeLine(s.range);
    child(s.selector.get());
    switchCases(s.cases);
  }

  void tryStmt(const Try& s) {
    open("Try"); closeLine(s.range);
    for (const TryResource& r : s.resources) {
      ++depth;
      open("Resource"); boolField("declared", r.isDeclaration);
      closeLine(r.range);
      if (r.isDeclaration) child(r.decl.get());
      else child(r.access.get());
      --depth;
    }
    child(s.block.get());
    for (const CatchClause& c : s.catches) {
      ++depth;
      open("Catch"); field("name", escapeQuoted(c.name));
      modsField(c.mods);
      closeLine(c.range);
      annotations(c.annotations);
      child(c.type.get());
      child(c.body.get());
      --depth;
    }
    child(s.finallyBlock.get());
  }

  void synchronized(const Synchronized& s) {
    open("Synchronized"); closeLine(s.range);
    child(s.lock.get());
    child(s.body.get());
  }

  void assertStatement(const AssertStatement& s) {
    open("AssertStatement"); closeLine(s.range);
    child(s.condition.get());
    child(s.message.get());
  }

  void localTypeDeclaration(const LocalTypeDeclaration& s) {
    open("LocalTypeDeclaration"); closeLine(s.range);
    child(s.decl.get());
  }

  // ---------------------------------------------------------- declarations -

  void compilationUnit(const CompilationUnit& u) {
    open("CompilationUnit"); closeLine(u.range);
    child(u.package.get());
    for (const auto& imp : u.imports) child(imp.get());
    for (const auto& t : u.types) child(t.get());
  }

  void packageDeclaration(const PackageDeclaration& p) {
    open("PackageDeclaration"); field("name", escapeQuoted(dotted(p.name)));
    closeLine(p.range);
    annotations(p.annotations);
  }

  void importDeclaration(const ImportDeclaration& i) {
    open("ImportDeclaration"); field("name", escapeQuoted(dotted(i.name)));
    if (i.isStatic) boolField("static", true);
    if (i.isOnDemand) boolField("onDemand", true);
    closeLine(i.range);
  }

  void classDeclaration(const ClassDeclaration& c) {
    open("ClassDeclaration"); field("name", escapeQuoted(c.name));
    modsField(c.mods);
    closeLine(c.range);
    annotations(c.annotations);
    typeParams(c.typeParameters);
    child(c.extendsType.get());
    for (const auto& t : c.implements) child(t.get());
    for (const auto& t : c.permits) child(t.get());
    for (const auto& mem : c.members) child(mem.get());
  }

  void interfaceDeclaration(const InterfaceDeclaration& i) {
    open("InterfaceDeclaration"); field("name", escapeQuoted(i.name));
    modsField(i.mods);
    closeLine(i.range);
    annotations(i.annotations);
    typeParams(i.typeParameters);
    for (const auto& t : i.extends) child(t.get());
    for (const auto& t : i.permits) child(t.get());
    for (const auto& mem : i.members) child(mem.get());
  }

  void enumDeclaration(const EnumDeclaration& e) {
    open("EnumDeclaration"); field("name", escapeQuoted(e.name));
    modsField(e.mods);
    closeLine(e.range);
    annotations(e.annotations);
    for (const auto& t : e.implements) child(t.get());
    for (const EnumConstant& k : e.constants) {
      ++depth;
      open("EnumConstant"); field("name", escapeQuoted(k.name));
      closeLine(k.range);
      annotations(k.annotations);
      for (const auto& arg : k.arguments) child(arg.get());
      child(k.body.get());
      --depth;
    }
    for (const auto& mem : e.members) child(mem.get());
  }

  void recordDeclaration(const RecordDeclaration& r) {
    open("RecordDeclaration"); field("name", escapeQuoted(r.name));
    modsField(r.mods);
    closeLine(r.range);
    annotations(r.annotations);
    typeParams(r.typeParameters);
    for (const RecordComponent& rc : r.components) {
      ++depth;
      open("RecordComponent"); field("name", escapeQuoted(rc.name));
      closeLine(rc.range);
      annotations(rc.annotations);
      child(rc.type.get());
      --depth;
    }
    for (const auto& t : r.implements) child(t.get());
    for (const auto& mem : r.members) child(mem.get());
  }

  void annotationTypeDeclaration(const AnnotationTypeDeclaration& a) {
    open("AnnotationTypeDeclaration"); field("name", escapeQuoted(a.name));
    modsField(a.mods);
    closeLine(a.range);
    annotations(a.annotations);
    for (const auto& mem : a.members) child(mem.get());
  }

  void fieldDeclaration(const FieldDeclaration& f) {
    open("FieldDeclaration");
    modsField(f.mods);
    closeLine(f.range);
    annotations(f.annotations);
    child(f.type.get());
    declarators(f.declarators);
  }

  void methodDeclaration(const MethodDeclaration& m) {
    open("MethodDeclaration"); field("name", escapeQuoted(m.name));
    modsField(m.mods);
    closeLine(m.range);
    annotations(m.annotations);
    typeParams(m.typeParameters);
    child(m.returnType.get());
    formalParameters(m.parameters);
    for (const auto& t : m.throws) child(t.get());
    child(m.body.get());
    child(m.defaultValue.get());
  }

  void constructorDeclaration(const ConstructorDeclaration& c) {
    open("ConstructorDeclaration"); field("name", escapeQuoted(c.name));
    modsField(c.mods);
    if (c.isCompact) boolField("compact", true);
    closeLine(c.range);
    annotations(c.annotations);
    typeParams(c.typeParameters);
    formalParameters(c.parameters);
    for (const auto& t : c.throws) child(t.get());
    child(c.body.get());
  }

  void initializerBlock(const InitializerBlock& i) {
    open("InitializerBlock");
    if (i.isStatic) boolField("static", true);
    closeLine(i.range);
    child(i.body.get());
  }
};

}  // namespace

// -------------------------------------------------------------- public API -

std::string AstDumper::dump(const ast::CompilationUnit& unit) {
  std::string out;
  dump(unit, out);
  return out;
}

void AstDumper::dump(const ast::CompilationUnit& unit, std::string& out) {
  Dumper dumper(sm_);
  dumper.compilationUnit(unit);
  out.append(dumper.out);  // append: never destroys caller data
}

}  // namespace b2::frontend
