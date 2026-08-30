// B-2 Frontend - AST -> RBC lowering: types, class model, slot planning,
// method and unit drivers.
//
// This TU owns the lowering's "front matter": the JType static-typing
// overlay (JLS numeric promotions, descriptors, conversions), the class
// model built from one top-level ClassDeclaration, the slot planner whose
// pre-order walk is the single authority for local-slot identity, the
// MethodLowerer prologue/finish plumbing, and the lowerUnit entry point
// that sequences <clinit>, constructors, and methods deterministically.

#include "LowerImpl.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace b2::frontend::lower {
namespace {

using M = ast::Mod;  // modifier bit names
using ast::ModPublic;
using ast::ModPrivate;
using ast::ModProtected;
using ast::ModStatic;
using ast::ModFinal;
using ast::ModSynchronized;
using ast::ModAbstract;
using ast::ModNative;

// --- java.lang resolution table (v0 heuristic, documented in the contract) -
// A single-segment class name that is not the program class resolves into
// java.lang when on this list; otherwise it stays in the program's own
// package. Honest v0 stand-in for a classpath until the binding stage.
bool isJavaLangClass(std::string_view simple) {
  static constexpr std::string_view kNames[] = {
      "Object", "String", "Class", "Integer", "Long", "Float", "Double",
      "Boolean", "Character", "Byte", "Short", "Number", "Math", "System",
      "StringBuilder", "StringBuffer", "CharSequence", "Comparable",
      "Iterable", "Exception", "RuntimeException", "Throwable", "Error",
      "IllegalArgumentException", "IllegalStateException",
      "NullPointerException", "ArithmeticException",
      "IndexOutOfBoundsException", "ArrayIndexOutOfBoundsException",
      "StringIndexOutOfBoundsException", "ClassCastException",
      "NumberFormatException", "UnsupportedOperationException",
      "CloneNotSupportedException", "InterruptedException",
      "AssertionError", "AutoCloseable", "Void", "Enum", "Record",
      "Runnable", "Thread", "Appendable", "Readable", "Process", "Runtime",
      "ThreadLocal", "Package", "ClassLoader", "SecurityManager",
      "StrictMath"};
  for (std::string_view n : kNames) {
    if (n == simple) {
      return true;
    }
  }
  return false;
}

std::string packagePrefixOf(std::string_view internal) {
  const auto slash = internal.rfind('/');
  return slash == std::string_view::npos
             ? std::string{}
             : std::string(internal.substr(0, slash + 1));
}

[[nodiscard]] std::string simpleOf(std::string_view internal) {
  const auto slash = internal.rfind('/');
  return std::string(internal.substr(slash + 1));
}

// AST modifiers -> rbc::method_flags.
std::uint16_t methodFlagsOf(std::uint32_t mods) {
  std::uint16_t f = 0;
  if (mods & ModPublic) f |= rbc::method_flags::Public;
  if (mods & ModPrivate) f |= rbc::method_flags::Private;
  if (mods & ModProtected) f |= rbc::method_flags::Protected;
  if (mods & ModStatic) f |= rbc::method_flags::Static;
  if (mods & ModFinal) f |= rbc::method_flags::Final;
  if (mods & ModSynchronized) f |= rbc::method_flags::Synchronized;
  return f;
}

// Initializer sequence for <clinit> (static) or ctor prepending
// (instance): FieldDeclarations with initializers and InitializerBlocks,
// declaration order (JLS 12.4.2 / 12.5).
void collectInits(const ast::ClassDeclaration& cd, bool wantStatic,
                  std::vector<const ast::Decl*>& out) {
  for (const ast::Ptr<ast::Decl>& m : cd.members) {
    if (m->kind == ast::NodeKind::FieldDeclaration) {
      const auto& fd = static_cast<const ast::FieldDeclaration&>(*m);
      const bool st = (fd.mods & ModStatic) != 0;
      const bool anyInit = std::any_of(
          fd.declarators.begin(), fd.declarators.end(),
          [](const ast::VariableDeclarator& d) { return d.initializer != nullptr; });
      if (st == wantStatic && anyInit) {
        out.push_back(m.get());
      }
    } else if (m->kind == ast::NodeKind::InitializerBlock) {
      const auto& ib = static_cast<const ast::InitializerBlock&>(*m);
      if (ib.isStatic == wantStatic) {
        out.push_back(m.get());
      }
    }
  }
}

[[nodiscard]] std::string paramsDescriptor(const std::vector<JType>& ps) {
  std::string d = "(";
  for (const JType& p : ps) {
    d += p.descriptor();
  }
  return d + ")";
}

// --------------------------------------------------------------------------

// Planner/emitter-shared scope model: a stack of (name, slot, JType).
class ScopeStack {
 public:
  struct Entry {
    std::string name;
    std::uint32_t slot = 0;
    JType type;
  };

  void push() { s_.emplace_back(); }
  void pop() { s_.pop_back(); }
  void bind(std::string name, std::uint32_t slot, JType type) {
    s_.back().emplace_back(Entry{std::move(name), slot, std::move(type)});
  }
  [[nodiscard]] const Entry* find(std::string_view n) const {
    for (auto layer = s_.rbegin(); layer != s_.rend(); ++layer) {
      for (auto e = layer->rbegin(); e != layer->rend(); ++e) {
        if (e->name == n) {
          return &*e;
        }
      }
    }
    return nullptr;
  }
  [[nodiscard]] std::vector<std::pair<std::string, JType>> flatten() const {
    std::vector<std::pair<std::string, JType>> out;
    for (auto layer = s_.rbegin(); layer != s_.rend(); ++layer) {
      for (auto e = layer->rbegin(); e != layer->rend(); ++e) {
        out.emplace_back(e->name, e->type);
      }
    }
    return out;
  }

 private:
  std::vector<std::vector<Entry>> s_;
};

}  // namespace

// ---------------------------------------------------------------- JType ----

std::string JType::display() const {
  switch (kind) {
  case JK::Bool: return "boolean";
  case JK::Byte: return "byte";
  case JK::Short: return "short";
  case JK::Char: return "char";
  case JK::Int: return "int";
  case JK::Long: return "long";
  case JK::Float: return "float";
  case JK::Double: return "double";
  case JK::Void: return "void";
  case JK::Null: return "null";
  case JK::Ref:
    if (!name.empty() && name[0] == '[') {
      return name;  // array descriptor is displayable
    }
    {  // dots for readability
      std::string d = name;
      std::replace(d.begin(), d.end(), '/', '.');
      return d;
    }
  case JK::Err: break;
  }
  return "<error>";
}

std::string JType::descriptor() const {
  switch (kind) {
  case JK::Bool: return "Z";
  case JK::Byte: return "B";
  case JK::Short: return "S";
  case JK::Char: return "C";
  case JK::Int: return "I";
  case JK::Long: return "J";
  case JK::Float: return "F";
  case JK::Double: return "D";
  case JK::Void: return "V";
  case JK::Ref:
    if (!name.empty() && name[0] == '[') {
      return name;  // already an array descriptor
    }
    return "L" + name + ";";
  case JK::Null: case JK::Err: break;
  }
  return "?";
}

JType JType::element() const {
  if (!isArray()) {
    return err();
  }
  const std::string_view d = name;
  switch (d[1]) {
  case 'Z': return prim(JK::Bool);
  case 'B': return prim(JK::Byte);
  case 'S': return prim(JK::Short);
  case 'C': return prim(JK::Char);
  case 'I': return prim(JK::Int);
  case 'J': return prim(JK::Long);
  case 'F': return prim(JK::Float);
  case 'D': return prim(JK::Double);
  case 'L': {
    const auto semi = d.find(';', 2);
    if (semi == std::string_view::npos) {
      return err();
    }
    return ref(std::string(d.substr(2, semi - 2)));
  }
  case '[':
    return ref(std::string(d.substr(1)));  // nested array
  default: return err();
  }
}

JType arrayOf(const JType& elem) {
  return JType::ref("[" + elem.descriptor());
}

JType promote(const JType& a, const JType& b) {
  if (a.kind == JK::Double || b.kind == JK::Double) return JType::prim(JK::Double);
  if (a.kind == JK::Float || b.kind == JK::Float) return JType::prim(JK::Float);
  if (a.kind == JK::Long || b.kind == JK::Long) return JType::prim(JK::Long);
  return JType::prim(JK::Int);
}

bool widensTo(const JType& from, const JType& to) {
  if (from.kind == to.kind) {
    return true;
  }
  if (from.kind == JK::Null && to.kind == JK::Ref) {
    return true;
  }
  using J = JK;
  switch (from.kind) {
  case J::Byte:
    return to.kind == J::Short || to.kind == J::Int || to.kind == J::Long ||
           to.kind == J::Float || to.kind == J::Double;
  case J::Short: case J::Char:
    return to.kind == J::Int || to.kind == J::Long ||
           to.kind == J::Float || to.kind == J::Double;
  case J::Int:
    return to.kind == J::Long || to.kind == J::Float || to.kind == J::Double;
  case J::Long:
    return to.kind == J::Float || to.kind == J::Double;
  case J::Float:
    return to.kind == J::Double;
  case J::Ref:
    return to.kind == J::Ref;  // unchecked reference assign (no hierarchy yet)
  default:
    return false;
  }
}

std::string internalizeClassType(const ast::ClassType& ct,
                                 std::string_view ownInternal) {
  std::string dotted;
  for (std::size_t i = 0; i < ct.segments.size(); ++i) {
    if (i != 0) dotted += '.';
    dotted += ct.segments[i].name;
  }
  if (dotted == ownInternal) {
    return std::string(ownInternal);
  }
  const std::string& simple = ct.segments.back().name;
  if (ct.segments.size() == 1) {
    if (!ownInternal.empty() && simple == simpleOf(ownInternal)) {
      return std::string(ownInternal);
    }
    if (isJavaLangClass(simple)) {
      return "java/lang/" + simple;
    }
    return packagePrefixOf(ownInternal) + simple;
  }
  std::string internal;
  for (char c : dotted) {
    internal += (c == '.') ? '/' : c;
  }
  return internal;
}

JType jtypeOf(const ast::Type& t, std::string_view ownInternal,
              DiagnosticEngine& diags, const SourceManager& sm) {
  switch (t.kind) {
  case ast::NodeKind::PrimitiveType: {
    const auto& p = static_cast<const ast::PrimitiveType&>(t);
    switch (p.primitive) {
    case ast::PrimitiveKind::Boolean: return JType::prim(JK::Bool);
    case ast::PrimitiveKind::Byte: return JType::prim(JK::Byte);
    case ast::PrimitiveKind::Short: return JType::prim(JK::Short);
    case ast::PrimitiveKind::Char: return JType::prim(JK::Char);
    case ast::PrimitiveKind::Int: return JType::prim(JK::Int);
    case ast::PrimitiveKind::Long: return JType::prim(JK::Long);
    case ast::PrimitiveKind::Float: return JType::prim(JK::Float);
    case ast::PrimitiveKind::Double: return JType::prim(JK::Double);
    case ast::PrimitiveKind::Void: return JType::prim(JK::Void);
    }
    return JType::err();
  }
  case ast::NodeKind::ClassType: {
    const auto& c = static_cast<const ast::ClassType&>(t);
    return JType::ref(internalizeClassType(c, ownInternal));
  }
  case ast::NodeKind::ArrayType: {
    const auto& a = static_cast<const ast::ArrayType&>(t);
    const JType elem = jtypeOf(*a.elementType, ownInternal, diags, sm);
    if (elem.isErr()) {
      return elem;
    }
    return arrayOf(elem);
  }
  case ast::NodeKind::IntersectionType:
  case ast::NodeKind::WildcardType:
  case ast::NodeKind::UnionType: {
    const std::string_view src = sm.source().substr(
        t.range.offset, t.range.length);
    diags.error(t.range.offset,
                "type is not supported by v0 lowering: " + std::string(src) +
                    " (expected: a primitive, class, or array type; "
                    "intersection/wildcard/union types arrive with the "
                    "binding stage)");
    return JType::err();
  }
  default:
    break;
  }
  return JType::err();
}

std::int64_t loOf(const JType& t) noexcept {
  switch (t.kind) {
  case JK::Byte: return -128;
  case JK::Short: return -32768;
  case JK::Char: return 0;
  case JK::Int: return -2147483648LL;
  default: return 0;
  }
}

std::int64_t hiOf(const JType& t) noexcept {
  switch (t.kind) {
  case JK::Byte: return 127;
  case JK::Short: return 32767;
  case JK::Char: return 65535;
  case JK::Int: return 2147483647LL;
  case JK::Bool: return 1;
  default: return 0;
  }
}

// ------------------------------------------------------------- constants ----

IntFold foldInt(const ast::Expr& e) {
  using K = ast::NodeKind;
  if (e.kind == K::Literal) {
    const auto& l = static_cast<const ast::Literal&>(e);
    switch (l.lit) {
    case ast::LitKind::Int:
      return {true, static_cast<std::int64_t>(
                        static_cast<std::int32_t>(l.intValue)), false};
    case ast::LitKind::Char:
      return {true, static_cast<std::int64_t>(l.intValue), false};
    case ast::LitKind::Boolean:
      return {true, static_cast<std::int64_t>(l.intValue != 0), true};
    default:
      return {};
    }
  }
  if (e.kind == K::ParenExpression) {
    return foldInt(*static_cast<const ast::ParenExpression&>(e).inner);
  }
  if (e.kind == K::PrefixUnary) {
    const auto& u = static_cast<const ast::PrefixUnary&>(e);
    const IntFold v = foldInt(*u.operand);
    if (!v.ok) {
      return {};
    }
    std::int64_t r = 0;
    switch (u.op) {
    case ast::UnaryOp::Plus: r = v.v; break;
    case ast::UnaryOp::Minus: r = -v.v; break;
    case ast::UnaryOp::BitNot: r = ~static_cast<std::int32_t>(v.v); break;
    case ast::UnaryOp::LogicalNot:
      if (!v.isBool) return {};
      r = v.v == 0 ? 1 : 0;
      break;
    default: return {};
    }
    return {true, r, u.op == ast::UnaryOp::LogicalNot};
  }
  if (e.kind == K::Binary) {
    const auto& x = static_cast<const ast::Binary&>(e);
    using BO = ast::BinaryOp;
    const bool logical = x.op == BO::CAnd || x.op == BO::COr;
    const IntFold a = foldInt(*x.lhs);
    const IntFold b = foldInt(*x.rhs);
    if (!a.ok || !b.ok || (logical && (!a.isBool || !b.isBool)) ||
        (!logical && (a.isBool || b.isBool))) {
      return {};
    }
    const std::int32_t ai = static_cast<std::int32_t>(a.v);
    const std::int32_t bi = static_cast<std::int32_t>(b.v);
    std::int64_t r = 0;
    switch (x.op) {
    case BO::Add: r = a.v + b.v; break;
    case BO::Sub: r = a.v - b.v; break;
    case BO::Mul: r = a.v * b.v; break;
    case BO::Div:
      if (bi == 0) return {};
      r = ai / bi;  // int division truncates toward zero (JLS 15.17.2)
      break;
    case BO::Mod:
      if (bi == 0) return {};
      r = ai % bi;
      break;
    case BO::Shl: r = static_cast<std::int32_t>(ai << (bi & 31)); break;
    case BO::Shr: r = ai >> (bi & 31); break;
    case BO::UShr:
      r = static_cast<std::int32_t>(static_cast<std::uint32_t>(ai) >> (bi & 31));
      break;
    case BO::And: r = a.isBool ? (a.v & b.v) : (ai & bi); break;
    case BO::Or: r = a.isBool ? (a.v | b.v) : (ai | bi); break;
    case BO::Xor: r = a.isBool ? (a.v ^ b.v) : (ai ^ bi); break;
    case BO::CAnd: r = (a.v != 0 && b.v != 0) ? 1 : 0; break;
    case BO::COr: r = (a.v != 0 || b.v != 0) ? 1 : 0; break;
    default: return {};
    }
    return {true, r, logical};
  }
  return {};
}

// ----------------------------------------------------------- ClassModel ----

const FieldEntry* ClassModel::findField(std::string_view n) const {
  for (const FieldEntry& f : fields) {
    if (f.name == n) {
      return &f;
    }
  }
  return nullptr;
}

const MethodEntry* ClassModel::findUnique(std::string_view n,
                                          std::size_t argc) const {
  const MethodEntry* found = nullptr;
  std::size_t matches = 0;
  for (const MethodEntry& m : methods) {
    if (m.name == n && m.params.size() == argc) {
      found = &m;
      ++matches;
    }
  }
  return matches == 1 ? found : nullptr;
}

void buildClassModel(const ast::ClassDeclaration& cd,
                     const std::string& internalName,
                     DiagnosticEngine& diags, const SourceManager& sm,
                     ClassModel& out) {
  out = ClassModel{};
  out.internalName = internalName;
  out.simpleName = simpleOf(internalName);

  for (const ast::Ptr<ast::Decl>& m : cd.members) {
    switch (m->kind) {
    case ast::NodeKind::FieldDeclaration: {
      const auto& fd = static_cast<const ast::FieldDeclaration&>(*m);
      const JType base = jtypeOf(*fd.type, internalName, diags, sm);
      for (const ast::VariableDeclarator& d : fd.declarators) {
        JType t = base;
        for (std::size_t i = 0; i < d.extraDims.size() && !t.isErr(); ++i) {
          t = arrayOf(t);
        }
        if (t.isErr()) {
          continue;  // diagnostic already emitted
        }
        out.fields.push_back(FieldEntry{d.name, t, (fd.mods & ModStatic) != 0,
                                        d.initializer != nullptr, d.range});
      }
      break;
    }
    case ast::NodeKind::MethodDeclaration: {
      const auto& md = static_cast<const ast::MethodDeclaration&>(*m);
      if ((md.mods & (ModAbstract | ModNative)) != 0) {
        diags.error(md.nameRange.offset,
                    "method '" + md.name +
                        "' is abstract or native; v0 lowering requires a body "
                        "(expected: a concrete method; abstract/native "
                        "support arrives with the class model and loader)");
        continue;
      }
      MethodEntry me;
      me.name = md.name;
      me.isStatic = (md.mods & ModStatic) != 0;
      me.result = jtypeOf(*md.returnType, internalName, diags, sm);
      if (me.result.isErr()) {
        continue;
      }
      bool bad = false;
      for (const ast::FormalParameter& p : md.parameters) {
        if (p.isReceiver) {
          continue;
        }
        JType pt = jtypeOf(*p.type, internalName, diags, sm);
        if (pt.isErr()) {
          bad = true;
          break;
        }
        if (p.isVarArgs) {
          pt = arrayOf(pt);  // varargs params are arrays in the descriptor
        }
        me.params.push_back(std::move(pt));
      }
      if (bad) {
        continue;
      }
      me.descriptor = paramsDescriptor(me.params) + me.result.descriptor();
      me.hasBody = md.body != nullptr;
      me.mdecl = &md;
      out.methods.push_back(std::move(me));
      break;
    }
    case ast::NodeKind::ConstructorDeclaration: {
      const auto& ctor = static_cast<const ast::ConstructorDeclaration&>(*m);
      MethodEntry me;
      me.name = "<init>";
      me.isCtor = true;
      me.result = JType::prim(JK::Void);
      bool bad = false;
      for (const ast::FormalParameter& p : ctor.parameters) {
        if (p.isReceiver) {
          continue;
        }
        JType pt = jtypeOf(*p.type, internalName, diags, sm);
        if (pt.isErr()) {
          bad = true;
          break;
        }
        if (p.isVarArgs) {
          pt = arrayOf(pt);
        }
        me.params.push_back(std::move(pt));
      }
      if (bad) {
        continue;
      }
      me.descriptor = paramsDescriptor(me.params) + "V";
      me.hasBody = ctor.body != nullptr;
      me.cdecl = &ctor;
      out.methods.push_back(std::move(me));
      break;
    }
    case ast::NodeKind::InitializerBlock:
      break;  // collected by collectInits at emission time
    default: {
      // Nested type declarations (class/interface/enum/record/annotation).
      const char* what = "type";
      switch (m->kind) {
      case ast::NodeKind::ClassDeclaration: what = "class"; break;
      case ast::NodeKind::InterfaceDeclaration: what = "interface"; break;
      case ast::NodeKind::EnumDeclaration: what = "enum"; break;
      case ast::NodeKind::RecordDeclaration: what = "record"; break;
      case ast::NodeKind::AnnotationTypeDeclaration:
        what = "annotation type";
        break;
      default: break;
      }
      diags.error(m->range.offset,
                  std::string("nested ") + what +
                      " declarations are not supported by v0 lowering "
                      "(expected: members of the top-level class only; "
                      "nested types arrive with the class model)");
      break;
    }
  }
  }  // for each member

  // JLS 8.8.9: a class without declared constructors gets a default one.
  // The model carries it so `new C()` call sites resolve; emission
  // generates its body (instance field initializers) in lowerUnitImpl.
  bool hasCtor = false;
  for (const MethodEntry& m : out.methods) {
    if (m.isCtor) {
      hasCtor = true;
      break;
    }
  }
  if (!hasCtor) {
    MethodEntry def;
    def.name = "<init>";
    def.descriptor = "()V";
    def.isCtor = true;
    def.isStatic = false;
    def.result = JType::prim(JK::Void);
    def.hasBody = true;  // synthesized
    out.methods.push_back(std::move(def));
  }
}

// ------------------------------------------------------------ SlotPlanner ----

namespace {

// The planner walk. MUST mirror MethodLowerer::lowerStmt's traversal
// (LowerStmt.cpp) exactly: same scope pushes/pops, same slot lookups.
class Planner {
 public:
  Planner(const ClassModel& cls, DiagnosticEngine& diags,
          const SourceManager& sm, MethodPlan& plan)
      : cls_(cls), diags_(diags), sm_(sm), plan_(plan) {}

  void run(const std::vector<ast::Ptr<ast::Stmt>>& body) {
    plan_.ok = true;
    scopes_.push();  // entry scope (parameters bind into it)
    walkStmts(body);
    scopes_.pop();
  }

 private:
  std::uint32_t newSlot(const JType& t) {
    const std::uint32_t s = plan_.numLocals;
    ++plan_.numLocals;
    plan_.slots.push_back(SlotInfo{s, t});
    return s;
  }

  // Ref slot (catch variables, rethrow temps); the prologue's ldc init
  // makes every Ref-like slot's type stable from pc 0.
  std::uint32_t newSlotRef() {
    return newSlot(JType::ref("java/lang/Object"));
  }

  void planDecl(const ast::LocalVariableDeclaration& d) {
    JType base = JType::err();
    if (!d.isVar) {
      base = jtypeOf(*d.type, cls_.internalName, diags_, sm_);
    }
    for (const ast::VariableDeclarator& dec : d.declarators) {
      JType t = base;
      if (d.isVar) {
        if (dec.initializer == nullptr) {
          diags_.error(dec.range.offset,
                       "'var' requires an initializer (expected: 'var name = "
                       "value'; hint: write the explicit type instead)");
          plan_.ok = false;
          t = JType::prim(JK::Int);
        } else {
          t = inferVarType(*dec.initializer, scopes_.flatten(), cls_, diags_,
                           sm_);
          if (t.isErr()) {
            plan_.ok = false;
            t = JType::prim(JK::Int);
          }
        }
      }
      for (std::size_t i = 0; i < dec.extraDims.size() && !t.isErr(); ++i) {
        t = arrayOf(t);
      }
      const std::uint32_t slot = newSlot(t);
      plan_.varSlots[&dec] = slot;
      scopes_.bind(dec.name, slot, t);
    }
  }

  void walkStmts(const std::vector<ast::Ptr<ast::Stmt>>& stmts) {
    for (const ast::Ptr<ast::Stmt>& s : stmts) {
      walkStmt(*s);
    }
  }

  // A sub-statement in a position where Java allows a single statement:
  // declarations cannot legally appear here, but if the parser produced one
  // anyway we still plan it inside a fresh scope (both walks agree).
  void walkSub(const ast::Stmt& s) {
    if (s.kind == ast::NodeKind::Block) {
      walkStmt(s);
      return;
    }
    scopes_.push();
    walkStmt(s);
    scopes_.pop();
  }

  void walkStmt(const ast::Stmt& s) {
    using K = ast::NodeKind;
    switch (s.kind) {
    case K::Block:
      scopes_.push();
      walkStmts(static_cast<const ast::Block&>(s).statements);
      scopes_.pop();
      break;
    case K::LocalVariableDeclaration:
      planDecl(static_cast<const ast::LocalVariableDeclaration&>(s));
      break;
    case K::If: {
      const auto& i = static_cast<const ast::If&>(s);
      if (i.thenStmt) walkSub(*i.thenStmt);
      if (i.elseStmt) walkSub(*i.elseStmt);
      break;
    }
    case K::While: {
      const auto& w = static_cast<const ast::While&>(s);
      walkSub(*w.body);
      break;
    }
    case K::DoWhile: {
      const auto& w = static_cast<const ast::DoWhile&>(s);
      walkSub(*w.body);
      break;
    }
    case K::BasicFor: {
      const auto& f = static_cast<const ast::BasicFor&>(s);
      scopes_.push();
      for (const ast::Ptr<ast::Stmt>& init : f.init) {
        walkStmt(*init);  // declarations scoped to the loop
      }
      if (f.body) walkSub(*f.body);
      scopes_.pop();
      break;
    }
    case K::EnhancedFor: {
      const auto& f = static_cast<const ast::EnhancedFor&>(s);
      const std::uint32_t idx = newSlot(JType::prim(JK::Int));
      plan_.forIndexSlots[&f] = idx;
      scopes_.push();
      JType t = JType::prim(JK::Int);
      if (f.isVar) {
        if (f.iterable == nullptr) {
          plan_.ok = false;
        } else {
          t = inferVarType(*f.iterable, scopes_.flatten(), cls_, diags_, sm_);
          if (t.isArray()) {
            t = t.element();
          } else if (!t.isErr()) {
            // non-array iterables are refused at emission; plan Int
            t = JType::prim(JK::Int);
          } else {
            plan_.ok = false;
            t = JType::prim(JK::Int);
          }
        }
      } else if (f.type != nullptr) {
        t = jtypeOf(*f.type, cls_.internalName, diags_, sm_);
      }
      const std::uint32_t vs = newSlot(t);
      plan_.forVarSlots[&f] = vs;
      scopes_.bind(f.name, vs, t);
      if (f.body) walkSub(*f.body);
      scopes_.pop();
      break;
    }
    case K::SwitchStatement: {
      const auto& sw = static_cast<const ast::SwitchStatement&>(s);
      scopes_.push();  // one scope for the whole switch block (JLS 14.11)
      for (const ast::SwitchCase& c : sw.cases) {
        walkStmts(c.statements);
        if (c.ruleBody != nullptr && c.ruleBody->kind == K::Block) {
          walkStmt(static_cast<const ast::Block&>(*c.ruleBody));
        }
      }
      scopes_.pop();
      break;
    }
    case K::Try: {
      const auto& t = static_cast<const ast::Try&>(s);
      if (t.block) walkStmt(*t.block);
      for (const ast::CatchClause& c : t.catches) {
        scopes_.push();
        // Catch variable: a Ref slot pre-initialized via ldc (see
        // MethodPlan), bound in the clause scope (JLS 14.20).
        if (!c.name.empty()) {
          const std::uint32_t vs = newSlotRef();
          plan_.catchVarSlots[&c] = vs;
          JType ct = JType::ref("java/lang/Object");
          if (c.type != nullptr && c.type->kind == K::ClassType) {
            ct = JType::ref(internalizeClassType(
                static_cast<const ast::ClassType&>(*c.type),
                cls_.internalName));
          }
          scopes_.bind(c.name, vs, ct);
        }
        if (c.body) walkStmts(c.body->statements);
        scopes_.pop();
      }
      if (t.finallyBlock != nullptr) {
        const std::uint32_t rs = newSlotRef();
        plan_.rethrowSlots[&t] = rs;
        walkStmt(*t.finallyBlock);
      }
      break;
    }
    case K::Synchronized: {
      const auto& sy = static_cast<const ast::Synchronized&>(s);
      if (sy.body) walkStmt(*sy.body);
      break;
    }
    case K::LabeledStatement: {
      const auto& l = static_cast<const ast::LabeledStatement&>(s);
      walkSub(*l.statement);
      break;
    }
    default:
      break;  // no nested statements / declarations
    }
  }

  const ClassModel& cls_;
  DiagnosticEngine& diags_;
  const SourceManager& sm_;
  MethodPlan& plan_;
  ScopeStack scopes_;
};

}  // namespace

void planMethod(const std::vector<ast::Ptr<ast::Stmt>>& body, bool isStatic,
                const std::vector<JType>& paramTypes,
                const ClassModel& cls, DiagnosticEngine& diags,
                const SourceManager& sm, MethodPlan& out) {
  out = MethodPlan{};
  std::uint32_t next = 0;
  if (!isStatic) {
    ++next;  // l0 = this
  }
  for (const JType& p : paramTypes) {
    out.slots.push_back(SlotInfo{next, p});
    ++next;
    ++out.numParams;
  }
  out.numLocals = next;
  Planner(cls, diags, sm, out).run(body);
}

// ------------------------------------------------------- MethodLowerer ----

MethodLowerer::MethodLowerer(const ClassModel& cls, std::string methodName,
                             std::string descriptor, std::uint16_t flags,
                             bool isStatic, JType result,
                             std::vector<ParamBind> params,
                             const MethodPlan* plan, DiagnosticEngine& diags,
                             const SourceManager& sm)
    : cls_(cls),
      name_(std::move(methodName)),
      descriptor_(std::move(descriptor)),
      flags_(flags),
      isStatic_(isStatic),
      result_(std::move(result)),
      params_(std::move(params)),
      plan_(plan),
      diags_(diags),
      sm_(sm),
      b_(name_, descriptor_, flags_) {
  if (plan_ != nullptr) {
    b_.setLocals(plan_->numLocals);
  }
}

void MethodLowerer::error(SourceRange at, std::string msg) {
  diags_.error(at.offset, std::move(msg));
  poisoned_ = true;
}

void MethodLowerer::bindLocal(std::string name, std::uint32_t slot, JType type) {
  scopes_.back().push_back(
      ScopeEntry{std::move(name), slot, std::move(type)});
}

const MethodLowerer::ScopeEntry* MethodLowerer::lookupLocal(
    std::string_view n) const {
  for (auto layer = scopes_.rbegin(); layer != scopes_.rend(); ++layer) {
    for (auto e = layer->rbegin(); e != layer->rend(); ++e) {
      if (e->name == n) {
        return &*e;
      }
    }
  }
  return nullptr;
}

std::vector<std::pair<std::string, JType>> MethodLowerer::visibleLocals() const {
  std::vector<std::pair<std::string, JType>> out;
  for (auto layer = scopes_.rbegin(); layer != scopes_.rend(); ++layer) {
    for (auto e = layer->rbegin(); e != layer->rend(); ++e) {
      out.emplace_back(e->name, e->type);
    }
  }
  return out;
}

const LoopCtx* MethodLowerer::findBreakable(std::string_view label) const {
  if (label.empty()) {
    return loops_.empty() ? nullptr : &loops_.back();
  }
  for (auto it = loops_.rbegin(); it != loops_.rend(); ++it) {
    if (it->label == label) {
      return &*it;
    }
  }
  return nullptr;
}

const LoopCtx* MethodLowerer::findLoop(std::string_view label) const {
  if (label.empty()) {
    for (auto it = loops_.rbegin(); it != loops_.rend(); ++it) {
      if (it->hasCont) {
        return &*it;
      }
    }
    return nullptr;
  }
  for (auto it = loops_.rbegin(); it != loops_.rend(); ++it) {
    if (it->hasCont && it->label == label) {
      return &*it;
    }
  }
  return nullptr;
}

void MethodLowerer::emitFinallyChain() {
  // Innermost finally runs first (LIFO); each body sees only outer
  // finallies so expansion is finite (a finally that transfers control
  // itself simply preempts the outer transfer - Java semantics).
  const std::size_t n = finallies_.size();
  for (std::size_t k = n; k-- > 0;) {
    const ast::Block* body = finallies_[k].body;
    std::vector<FinallyCtx> saved = std::move(finallies_);
    finallies_.assign(saved.begin(), saved.begin() + k);
    pushScope();
    for (const ast::Ptr<ast::Stmt>& st : body->statements) {
      lowerStmt(*st);
    }
    popScope();
    finallies_ = std::move(saved);
    if (poisoned_) {
      return;
    }
  }
}

std::uint32_t MethodLowerer::cpClassOf(const JType& t) {
  return cpClass(t.isArray() ? t.name : t.name);
}

std::uint32_t MethodLowerer::cpClass(std::string internal) {
  return b_.constClass(std::move(internal));
}

std::uint32_t MethodLowerer::cpField(std::string clsInternal, std::string name,
                                     JType type) {
  return b_.constFieldRef(std::move(clsInternal), std::move(name),
                          type.descriptor());
}

std::uint32_t MethodLowerer::cpMethod(std::string clsInternal, std::string name,
                                      std::string desc) {
  return b_.constMethodRef(std::move(clsInternal), std::move(name),
                           std::move(desc));
}

void MethodLowerer::emitMove(std::uint16_t dst, std::uint16_t a,
                             const JType& t) {
  using rbc::Op;
  if (t.kind == JK::Long) b_.emitRegReg(Op::Lmove, dst, a);
  else if (t.kind == JK::Float) b_.emitRegReg(Op::Fmove, dst, a);
  else if (t.kind == JK::Double) b_.emitRegReg(Op::Dmove, dst, a);
  else if (t.isRefLike()) b_.emitRegReg(Op::Amove, dst, a);
  else b_.emitRegReg(Op::Imove, dst, a);
}

std::uint16_t MethodLowerer::emitLoad(std::uint32_t slot, const JType& t) {
  const std::uint16_t r = newReg();
  using rbc::Op;
  if (t.kind == JK::Long) b_.emitRegSlot(Op::Lload, r, slot);
  else if (t.kind == JK::Float) b_.emitRegSlot(Op::Fload, r, slot);
  else if (t.kind == JK::Double) b_.emitRegSlot(Op::Dload, r, slot);
  else if (t.isRefLike()) b_.emitRegSlot(Op::Aload, r, slot);
  else b_.emitRegSlot(Op::Iload, r, slot);
  return r;
}

void MethodLowerer::emitStore(std::uint16_t reg, std::uint32_t slot,
                              const JType& t) {
  using rbc::Op;
  if (t.kind == JK::Long) b_.emitSlotReg(Op::Lstore, reg, slot);
  else if (t.kind == JK::Float) b_.emitSlotReg(Op::Fstore, reg, slot);
  else if (t.kind == JK::Double) b_.emitSlotReg(Op::Dstore, reg, slot);
  else if (t.isRefLike()) b_.emitSlotReg(Op::Astore, reg, slot);
  else b_.emitSlotReg(Op::Istore, reg, slot);
}

std::uint16_t MethodLowerer::emitConvert(std::uint16_t reg, const JType& from,
                                         const JType& to, SourceRange at) {
  using rbc::Op;
  if (from.isErr() || to.isErr()) {
    return reg;
  }
  const auto conv = [&](Op op) {
    const std::uint16_t d = newReg();
    b_.emitRegReg(op, d, reg);
    return d;
  };
  const auto viaInt = [&](Op op) {
    const std::uint16_t i = conv(op);
    return emitConvert(i, JType::prim(JK::Int), to, at);
  };
  const bool fromInt = from.isIntFamily();
  switch (to.kind) {
  case JK::Long:
    if (from.kind == JK::Long) return reg;
    if (fromInt) return conv(Op::I2l);
    if (from.kind == JK::Float) return conv(Op::F2l);
    if (from.kind == JK::Double) return conv(Op::D2l);
    break;
  case JK::Float:
    if (from.kind == JK::Float) return reg;
    if (fromInt) return conv(Op::I2f);
    if (from.kind == JK::Long) return conv(Op::L2f);
    if (from.kind == JK::Double) return conv(Op::D2f);
    break;
  case JK::Double:
    if (from.kind == JK::Double) return reg;
    if (fromInt) return conv(Op::I2d);
    if (from.kind == JK::Long) return conv(Op::L2d);
    if (from.kind == JK::Float) return conv(Op::F2d);
    break;
  case JK::Int: case JK::Bool:
    if (fromInt) return reg;
    if (from.kind == JK::Long) return conv(Op::L2i);
    if (from.kind == JK::Float) return conv(Op::F2i);
    if (from.kind == JK::Double) return conv(Op::D2i);
    break;
  case JK::Byte:
    if (from.kind == JK::Byte) return reg;
    if (fromInt) return conv(Op::I2b);
    if (from.kind == JK::Long) return viaInt(Op::L2i);
    if (from.kind == JK::Float) return viaInt(Op::F2i);
    if (from.kind == JK::Double) return viaInt(Op::D2i);
    break;
  case JK::Short:
    if (from.kind == JK::Short || from.kind == JK::Byte) return reg;
    if (fromInt) return conv(Op::I2s);
    if (from.kind == JK::Long) return viaInt(Op::L2i);
    if (from.kind == JK::Float) return viaInt(Op::F2i);
    if (from.kind == JK::Double) return viaInt(Op::D2i);
    break;
  case JK::Char:
    if (from.kind == JK::Char) return reg;
    if (fromInt) return conv(Op::I2c);
    if (from.kind == JK::Long) return viaInt(Op::L2i);
    if (from.kind == JK::Float) return viaInt(Op::F2i);
    if (from.kind == JK::Double) return viaInt(Op::D2i);
    break;
  case JK::Ref: case JK::Null:
    return reg;  // reference conversions are explicit (Cast -> checkcast)
  case JK::Void: case JK::Err:
    break;
  }
  (void)at;
  return reg;
}

std::uint16_t MethodLowerer::emitAssignConvert(
    std::uint16_t reg, const JType& from, const JType& to,
    std::optional<std::int64_t> fold, SourceRange at) {
  if (widensTo(from, to)) {
    return emitConvert(reg, from, to, at);
  }
  // Boxing/unboxing first: a constant does NOT make an int-to-Integer
  // assignment legal (that is autoboxing, not constant narrowing).
  if (to.isRefLike() && from.isNumeric()) {
    error(at,
          "incompatible types: " + from.display() + " cannot be converted to " +
              to.display() +
              " (expected: a primitive type; boxing is not supported by v0 "
              "lowering and arrives with the binding stage)");
    return reg;
  }
  if (to.isNumeric() && from.isRefLike()) {
    error(at,
          "incompatible types: " + from.display() + " cannot be converted to " +
              to.display() +
              " (expected: a primitive value; unboxing is not supported by "
              "v0 lowering and arrives with the binding stage)");
    return reg;
  }
  // Narrowing: only constant expressions convert implicitly (JLS 5.2).
  if (fold.has_value()) {
    const std::int64_t v = *fold;
    bool inRange = false;
    switch (to.kind) {
    case JK::Byte: inRange = v >= -128 && v <= 127; break;
    case JK::Short: inRange = v >= -32768 && v <= 32767; break;
    case JK::Char: inRange = v >= 0 && v <= 65535; break;
    case JK::Int: inRange = v >= -2147483648LL && v <= 2147483647LL; break;
    case JK::Bool: inRange = v == 0 || v == 1; break;
    default: inRange = false; break;
    }
    if (!inRange) {
      error(at,
            "possible lossy conversion from " + from.display() + " to " +
                to.display() + " (expected: a constant in [" +
                std::to_string(loOf(to)) + ", " + std::to_string(hiOf(to)) +
                "] or an explicit cast)");
      return reg;
    }
    return emitConvert(reg, JType::prim(JK::Int), to, at);
  }
  error(at,
        "incompatible types: " + from.display() + " cannot be converted to " +
            to.display() +
            " without a cast (expected: an explicit cast or a matching type)");
  return reg;
}

std::uint16_t MethodLowerer::loadThis(SourceRange at) {
  if (isStatic_) {
    error(at,
          "'this' is not available in a static context (expected: an "
          "instance method; hint: make the method non-static or drop 'this')");
    return 0;
  }
  return emitLoad(0, JType::ref(cls_.internalName));
}

void MethodLowerer::emitDefaultReturn() {
  using rbc::Op;
  switch (result_.kind) {
  case JK::Void:
    b_.emit(Op::Return);
    break;
  case JK::Long: {
    const std::uint16_t r = newReg();
    b_.emitRegCp(Op::Lconst, r, b_.constLong(0));
    b_.emitReg(Op::Lreturn, r);
    break;
  }
  case JK::Float: {
    const std::uint16_t r = newReg();
    b_.emitRegImm(Op::Fconst, r, 0);
    b_.emitReg(Op::Freturn, r);
    break;
  }
  case JK::Double: {
    const std::uint16_t r = newReg();
    b_.emitRegCp(Op::Dconst, r, b_.constDouble(0.0));
    b_.emitReg(Op::Dreturn, r);
    break;
  }
  case JK::Ref: case JK::Null: {
    const std::uint16_t r = newReg();
    b_.emitRegImm(Op::AconstNull, r, 0);
    b_.emitReg(Op::Areturn, r);
    break;
  }
  default: {  // Int family (also the safe fallback)
    const std::uint16_t r = newReg();
    b_.emitRegImm(Op::Iconst, r, 0);
    b_.emitReg(Op::Ireturn, r);
    break;
  }
  }
}

void MethodLowerer::emitPrologue() {
  if (plan_ == nullptr) {
    return;
  }
  // Default-init every non-parameter slot so its verification type is
  // fixed from pc 0 (protected-range stability by construction, rbc_spec
  // SS5.3). Params keep their descriptor types.
  const std::uint32_t skip = plan_->numParams + (isStatic_ ? 0 : 1);
  using rbc::Op;
  for (std::size_t i = skip; i < plan_->slots.size(); ++i) {
    const SlotInfo& si = plan_->slots[i];
    if (si.type.kind == JK::Long) {
      const std::uint16_t r = newReg();
      b_.emitRegCp(Op::Lconst, r, b_.constLong(0));
      b_.emitSlotReg(Op::Lstore, r, si.slot);
    } else if (si.type.kind == JK::Float) {
      const std::uint16_t r = newReg();
      b_.emitRegImm(Op::Fconst, r, 0);
      b_.emitSlotReg(Op::Fstore, r, si.slot);
    } else if (si.type.kind == JK::Double) {
      const std::uint16_t r = newReg();
      b_.emitRegCp(Op::Dconst, r, b_.constDouble(0.0));
      b_.emitSlotReg(Op::Dstore, r, si.slot);
    } else if (si.type.isRefLike()) {
      // WHY ldc (not aconst_null): every Ref-like slot's verification type
      // must be Ref - not Null - from pc 0. A slot typed Null whose first
      // astore lands inside a protected range (a declaration inside try{},
      // a handler-entry store) changes its type mid-range, which SS5.3
      // forbids. A string constant is Ref-typed, interned once per method,
      // and never observable: every observable read follows a real store.
      const std::uint16_t r = newReg();
      b_.emitRegCp(Op::Ldc, r, b_.constString(""));
      b_.emitSlotReg(Op::Astore, r, si.slot);
    } else {
      const std::uint16_t r = newReg();
      b_.emitRegImm(Op::Iconst, r, 0);
      b_.emitSlotReg(Op::Istore, r, si.slot);
    }
  }
}

bool MethodLowerer::run(const std::vector<ast::Ptr<ast::Stmt>>& body,
                        rbc::Method& out,
                        const std::vector<const ast::Decl*>* fieldInits,
                        const ast::Stmt* skipStmt,
                        const ast::ConstructorInvocation* delegation) {
  emitPrologue();
  pushScope();
  std::uint32_t slot = isStatic_ ? 0 : 1;
  for (const ParamBind& p : params_) {
    bindLocal(p.name, slot, p.type);
    ++slot;
  }
  if (delegation != nullptr) {
    lowerCtorDelegation(*delegation);
  }
  if (fieldInits != nullptr) {
    lowerFieldInitSequence(*fieldInits);
  }
  for (const ast::Ptr<ast::Stmt>& s : body) {
    if (s.get() == skipStmt) {
      continue;
    }
    lowerStmt(*s);
    if (poisoned_) {
      return false;
    }
  }
  popScope();
  emitDefaultReturn();
  const auto fr = b_.finish(out);
  if (!fr.ok) {
    diags_.error(0, "internal lowering error building method '" + name_ +
                        "': " + fr.error +
                        " (this is a bug in the lowering; please report it)");
    return false;
  }
  return true;
}

void MethodLowerer::lowerFieldInitSequence(
    const std::vector<const ast::Decl*>& inits) {
  for (const ast::Decl* d : inits) {
    if (poisoned_) {
      return;
    }
    if (d->kind == ast::NodeKind::FieldDeclaration) {
      const auto& fd = static_cast<const ast::FieldDeclaration&>(*d);
      for (const ast::VariableDeclarator& dec : fd.declarators) {
        if (dec.initializer == nullptr) {
          continue;
        }
        const FieldEntry* fe = cls_.findField(dec.name);
        if (fe == nullptr) {
          continue;
        }
        Val v = lowerExprTo(*dec.initializer, fe->type,
                            "field initializer for '" + dec.name + "'",
                            dec.range);
        if (!v.ok()) {
          return;
        }
        const std::uint32_t cp =
            cpField(cls_.internalName, fe->name, fe->type);
        if (fe->isStatic) {
          b_.emitRegCp(rbc::Op::Putstatic, v.reg, cp);
        } else {
          const std::uint16_t thiz = loadThis(dec.range);
          b_.emitRegRegRegCp(rbc::Op::Putfield, 0, thiz, v.reg, cp);
        }
      }
    } else if (d->kind == ast::NodeKind::InitializerBlock) {
      const auto& ib = static_cast<const ast::InitializerBlock&>(*d);
      lowerBlock(*ib.body);
      if (poisoned_) {
        return;
      }
    }
  }
}

// ------------------------------------------------------------ lowerUnit ----

namespace {

std::vector<MethodLowerer::ParamBind> bindsOf(
    const std::vector<ast::FormalParameter>& ps,
    const std::vector<JType>& types) {
  std::vector<MethodLowerer::ParamBind> out;
  std::size_t ti = 0;
  for (const ast::FormalParameter& p : ps) {
    if (p.isReceiver) {
      continue;
    }
    if (ti < types.size()) {
      MethodLowerer::ParamBind pb;
      pb.name = p.name;
      pb.type = types[ti];
      // varargs params are arrays in the descriptor AND at call sites
      if (p.isVarArgs && pb.type.isArray()) {
        // keep the array type (call sites must pass arrays in v0)
      }
      out.push_back(std::move(pb));
    }
    ++ti;
  }
  return out;
}

}  // namespace

}  // namespace lower

namespace b2::frontend {

// Public contract entry point (Lower.h): one call, delegating to the
// lowering implementation namespace.
LoweredUnit lowerUnit(const ast::CompilationUnit& unit, const SourceManager& sm,
                      DiagnosticEngine& diags) {
  return lower::lowerUnitImpl(unit, sm, diags);
}

}  // namespace b2::frontend

namespace b2::frontend::lower {

LoweredUnit lowerUnitImpl(const ast::CompilationUnit& unit, const SourceManager& sm,
                          DiagnosticEngine& diags) {
  LoweredUnit result;

  // --- pick the one top-level class ---------------------------------------
  const ast::ClassDeclaration* clsDecl = nullptr;
  for (const ast::Ptr<ast::Decl>& t : unit.types) {
    if (t->kind == ast::NodeKind::ClassDeclaration) {
      if (clsDecl == nullptr) {
        clsDecl = static_cast<const ast::ClassDeclaration*>(t.get());
      } else {
        diags.warning(
            t->range.offset,
            "v0 lowering compiles exactly one top-level class per "
            "compilation unit; '" +
                static_cast<const ast::ClassDeclaration*>(t.get())->name +
                "' is skipped (expected: one class per file; hint: split the "
                "file and lower each class separately)");
      }
      continue;
    }
    const char* what = "type";
    switch (t->kind) {
    case ast::NodeKind::InterfaceDeclaration: what = "interface"; break;
    case ast::NodeKind::EnumDeclaration: what = "enum"; break;
    case ast::NodeKind::RecordDeclaration: what = "record"; break;
    case ast::NodeKind::AnnotationTypeDeclaration:
      what = "annotation type";
      break;
    default: break;
    }
    diags.error(t->range.offset,
                std::string("v0 lowering supports classes only; found a "
                            "top-level ") +
                    what +
                    " (expected: a class declaration; interfaces/enums/"
                    "records arrive with the class model and loader)");
    result.ok = false;
    return result;
  }
  if (clsDecl == nullptr) {
    diags.error(0,
                "no top-level class declaration to lower (expected: at least "
                "one 'class' in the compilation unit)");
    return result;
  }

  // Internal name: package + class, dots to slashes.
  std::string internal = clsDecl->name;
  if (unit.package != nullptr && !unit.package->name.empty()) {
    std::string pkg;
    for (const std::string& seg : unit.package->name) {
      pkg += seg;
      pkg += '/';
    }
    internal = pkg + clsDecl->name;
  }

  ClassModel cls;
  buildClassModel(*clsDecl, internal, diags, sm, cls);
  if (diags.hasErrors()) {
    result.ok = false;
    return result;
  }

  result.program.className = internal;

  // --- <clinit>: static initializers and static blocks, in order -----------
  std::vector<const ast::Decl*> staticInits;
  collectInits(*clsDecl, true, staticInits);
  if (!staticInits.empty()) {
    MethodPlan plan;
    planMethod({}, true, {}, cls, diags, sm, plan);
    MethodLowerer ml(cls, "<clinit>", "()V", rbc::method_flags::Static, true,
                     JType::prim(JK::Void), {}, &plan, diags, sm);
    rbc::Method m;
    if (!ml.run({}, m, &staticInits, nullptr, nullptr)) {
      result.ok = false;
      return result;
    }
    result.program.methods.push_back(std::move(m));
  }

  // --- methods and constructors, declaration order --------------------------
  std::vector<const ast::Decl*> instanceInits;
  collectInits(*clsDecl, false, instanceInits);
  bool anyCtor = false;

  for (const MethodEntry& me : cls.methods) {
    if (!me.hasBody) {
      continue;
    }
    if (me.isCtor) {
      anyCtor = true;
      if (me.cdecl == nullptr) {
        // Synthesized default constructor: instance field inits only.
        MethodPlan plan;
        planMethod({}, false, {}, cls, diags, sm, plan);
        MethodLowerer ml(cls, "<init>", "()V", rbc::method_flags::Public,
                         false, JType::prim(JK::Void), {}, &plan, diags, sm);
        rbc::Method m;
        if (!ml.run({}, m, &instanceInits, nullptr, nullptr)) {
          result.ok = false;
          return result;
        }
        result.program.methods.push_back(std::move(m));
        continue;
      }
      const ast::ConstructorDeclaration& cd = *me.cdecl;
      MethodPlan plan;
      planMethod(cd.body->statements, false, me.params, cls, diags, sm, plan);
      if (!plan.ok) {
        result.ok = false;
        return result;
      }
      MethodLowerer ml(cls, "<init>", me.descriptor,
                       methodFlagsOf(cd.mods & ~ModStatic), false,
                       JType::prim(JK::Void), bindsOf(cd.parameters, me.params),
                       &plan, diags, sm);
      // Ctor prologue: explicit this(...)/super() first statement, else the
      // instance initializer sequence, else straight into the body.
      const ast::Stmt* first =
          cd.body->statements.empty() ? nullptr : cd.body->statements.front().get();
      const ast::ConstructorInvocation* delegation = nullptr;
      const ast::Stmt* skip = nullptr;
      const std::vector<const ast::Decl*>* inits = &instanceInits;
      if (first != nullptr &&
          first->kind == ast::NodeKind::ExpressionStatement) {
        const auto& es = static_cast<const ast::ExpressionStatement&>(*first);
        if (es.expression->kind == ast::NodeKind::ConstructorInvocation) {
          const auto& ci =
              static_cast<const ast::ConstructorInvocation&>(*es.expression);
          skip = first;
          if (!ci.isSuper) {
            delegation = &ci;  // this(...): no field re-initialization
            inits = nullptr;
          } else {
            if (!ci.arguments.empty()) {
              diags.error(ci.range.offset,
                          "super(...) with arguments is not supported by v0 "
                          "lowering: the program class has no superclass in "
                          "the v0 world (expected: no explicit super call; "
                          "hint: remove it or use this(...))");
              result.ok = false;
              return result;
            }
            inits = &instanceInits;  // super() is a v0 no-op
          }
        }
      }
      rbc::Method m;
      if (!ml.run(cd.body->statements, m, inits, skip, delegation)) {
        result.ok = false;
        return result;
      }
      result.program.methods.push_back(std::move(m));
      continue;
    }

    const ast::MethodDeclaration& md = *me.mdecl;
    MethodPlan plan;
    planMethod(md.body->statements, me.isStatic, me.params, cls, diags, sm,
               plan);
    if (!plan.ok) {
      result.ok = false;
      return result;
    }
    MethodLowerer ml(cls, me.name, me.descriptor, methodFlagsOf(md.mods),
                     me.isStatic, me.result,
                     bindsOf(md.parameters, me.params), &plan, diags, sm);
    rbc::Method m;
    if (!ml.run(md.body->statements, m, nullptr, nullptr, nullptr)) {
      result.ok = false;
      return result;
    }
    result.program.methods.push_back(std::move(m));
  }

  (void)anyCtor;  // the synthesized default ctor is emitted in the loop
  result.ok = !diags.hasErrors();
  return result;
}

std::string quote(std::string_view s) {
  return "'" + std::string(s) + "'";
}

}  // namespace b2::frontend::lower
