// B-2 Frontend - AST -> RBC lowering: expressions and calls.
//
// This TU owns everything that produces a value: literals, name resolution
// (locals -> own fields -> class-qualified statics), JLS numeric-promotion
// arithmetic, NaN-correct float/double comparisons, short-circuit booleans,
// conditional expressions, casts, compound assignment with implicit
// narrowing, ++/--, array creation/access/initializers, instance creation,
// and the call-lowering machinery (own static/instance methods, ctor
// delegation, println/print on unknown receivers).
//
// EVALUATION ORDER IS JAVA'S (JLS 15.7): receiver first, then arguments
// left to right; assignment-target subexpressions evaluate exactly once.

#include "LowerImpl.h"

#include <bit>
#include <string>
#include <utility>
#include <vector>

namespace b2::frontend::lower {
namespace {

using rbc::Op;

// Descriptor char for an argument whose static type feeds an
// unknown-receiver call descriptor. javac would resolve the println(Object)
// overload for every non-String reference (arrays and user classes alike),
// so v0 does the same; byte/short widen to I (no (B)/(S) overloads exist).
[[nodiscard]] std::string argDescriptorOf(const JType& t) {
  switch (t.kind) {
  case JK::Byte: case JK::Short:
    return "I";  // invoke-time widening (println has no (B)/(S) overloads)
  case JK::Bool: return "Z";
  case JK::Char: return "C";
  case JK::Int: return "I";
  case JK::Long: return "J";
  case JK::Float: return "F";
  case JK::Double: return "D";
  case JK::Null: case JK::Ref:
    return t.isString() ? "Ljava/lang/String;" : "Ljava/lang/Object;";
  default: return "?";
  }
}

// A one-segment ClassType for class-name internalization (type args none).
[[nodiscard]] ast::ClassType oneSegmentClass(std::string_view name,
                                            SourceRange r) {
  ast::ClassType ct(r, {});
  ast::ClassType::Segment seg;
  seg.name = std::string(name);
  seg.range = r;
  ct.segments.push_back(std::move(seg));
  return ct;
}

// Atype code for a primitive element type.
[[nodiscard]] std::uint32_t atypeOf(const JType& t) {
  switch (t.kind) {
  case JK::Bool: return static_cast<std::uint32_t>(rbc::Atype::Boolean);
  case JK::Byte: return static_cast<std::uint32_t>(rbc::Atype::Byte);
  case JK::Short: return static_cast<std::uint32_t>(rbc::Atype::Short);
  case JK::Char: return static_cast<std::uint32_t>(rbc::Atype::Char);
  case JK::Int: return static_cast<std::uint32_t>(rbc::Atype::Int);
  case JK::Long: return static_cast<std::uint32_t>(rbc::Atype::Long);
  case JK::Float: return static_cast<std::uint32_t>(rbc::Atype::Float);
  case JK::Double: return static_cast<std::uint32_t>(rbc::Atype::Double);
  default: return 0;
  }
}

// Binary arithmetic op in `family` (shifts keep an Int right operand).
[[nodiscard]] Op arithOpOf(ast::BinaryOp op, JK family) {
  const bool isLong = family == JK::Long;
  const bool isFloat = family == JK::Float;
  const bool isDouble = family == JK::Double;
  using B = ast::BinaryOp;
  switch (op) {
  case B::Mul: return isLong ? Op::Lmul : isFloat ? Op::Fmul : isDouble ? Op::Dmul : Op::Imul;
  case B::Div: return isLong ? Op::Ldiv : isFloat ? Op::Fdiv : isDouble ? Op::Ddiv : Op::Idiv;
  case B::Mod: return isLong ? Op::Lrem : isFloat ? Op::Frem : isDouble ? Op::Drem : Op::Irem;
  case B::Add: return isLong ? Op::Ladd : isFloat ? Op::Fadd : isDouble ? Op::Dadd : Op::Iadd;
  case B::Sub: return isLong ? Op::Lsub : isFloat ? Op::Fsub : isDouble ? Op::Dsub : Op::Isub;
  case B::Shl: return isLong ? Op::Lshl : Op::Ishl;
  case B::Shr: return isLong ? Op::Lshr : Op::Ishr;
  case B::UShr: return isLong ? Op::Lushr : Op::Iushr;
  case B::And: return isLong ? Op::Land : Op::Iand;
  case B::Or: return isLong ? Op::Lor : Op::Ior;
  case B::Xor: return isLong ? Op::Lxor : Op::Ixor;
  default: return Op::Nop;
  }
}

bool isShift(ast::BinaryOp op) {
  return op == ast::BinaryOp::Shl || op == ast::BinaryOp::Shr ||
         op == ast::BinaryOp::UShr;
}

bool isBitwise(ast::BinaryOp op) {
  using B = ast::BinaryOp;
  return op == B::And || op == B::Or || op == B::Xor || isShift(op);
}

bool isCompare(ast::BinaryOp op) {
  using B = ast::BinaryOp;
  return op == B::Lt || op == B::Gt || op == B::Le || op == B::Ge ||
         op == B::Eq || op == B::Ne;
}

// An assignable target (JLS 15.26 LHS), subexpressions already evaluated.
struct TargetRef {
  enum class Kind { Local, InstanceField, StaticField, ArrayElem };
  Kind kind = Kind::Local;
  JType type;                     // target's own type
  std::uint32_t slot = 0;         // Local
  std::uint16_t recv = 0;         // InstanceField receiver register
  const FieldEntry* field = nullptr;  // InstanceField / StaticField
  std::uint16_t arr = 0;          // ArrayElem: array register
  std::uint16_t idx = 0;          // ArrayElem: index register
};

}  // namespace

// Array element load/store ops by element family (shared with LowerStmt).
Op aloadOpOf(const JType& elem) {
  switch (elem.kind) {
  case JK::Long: return Op::Laload;
  case JK::Float: return Op::Faload;
  case JK::Double: return Op::Daload;
  case JK::Ref: case JK::Null: return Op::Aaload;
  case JK::Byte: case JK::Bool: return Op::Baload;
  case JK::Char: return Op::Caload;
  case JK::Short: return Op::Saload;
  default: return Op::Iaload;
  }
}

Op astoreOpOf(const JType& elem) {
  switch (elem.kind) {
  case JK::Long: return Op::Lastore;
  case JK::Float: return Op::Fastore;
  case JK::Double: return Op::Dastore;
  case JK::Ref: case JK::Null: return Op::Aastore;
  case JK::Byte: case JK::Bool: return Op::Bastore;
  case JK::Char: return Op::Castore;
  case JK::Short: return Op::Sastore;
  default: return Op::Iastore;
  }
}

// ---------------------------------------------------------------- target ----

namespace {

// Lowers an assignment/incdec target: evaluates receiver/array/index ONCE.
TargetRef lowerTarget(MethodLowerer& ml, const ast::Expr& target) {
  using K = ast::NodeKind;
  TargetRef t;
  if (target.kind == K::Name) {
    const auto& n = static_cast<const ast::Name&>(target);
    if (const auto* local = ml.lookupLocal(n.identifier)) {
      t.kind = TargetRef::Kind::Local;
      t.slot = local->slot;
      t.type = local->type;
      return t;
    }
    const FieldEntry* fe = ml.cls().findField(n.identifier);
    if (fe != nullptr) {
      if (fe->isStatic) {
        t.kind = TargetRef::Kind::StaticField;
        t.field = fe;
        t.type = fe->type;
        return t;
      }
      if (ml.isStatic()) {
        ml.error(n.range,
                 "instance field '" + n.identifier +
                     "' cannot be referenced from a static context (expected: "
                     "a static field or local; hint: make the method "
                     "non-static)");
        t.type = JType::err();
        return t;
      }
      t.kind = TargetRef::Kind::InstanceField;
      t.field = fe;
      t.type = fe->type;
      t.recv = ml.loadThis(n.range);
      return t;
    }
    ml.error(n.range,
             "cannot resolve '" + n.identifier +
                 "' as a variable or field (expected: a local variable, "
                 "parameter, or field of " + ml.cls().internalName + ")");
    t.type = JType::err();
    return t;
  }

  if (target.kind == K::FieldAccess) {
    const auto& fa = static_cast<const ast::FieldAccess&>(target);
    // Class-qualified static field: Name that resolves to no local/field.
    if (fa.target != nullptr && fa.target->kind == K::Name) {
      const auto& qn = static_cast<const ast::Name&>(*fa.target);
      if (ml.lookupLocal(qn.identifier) == nullptr &&
          ml.cls().findField(qn.identifier) == nullptr) {
        const std::string owner = internalizeClassType(
            oneSegmentClass(qn.identifier, fa.target->range),
            ml.cls().internalName);
        if (owner == ml.cls().internalName) {
          const FieldEntry* fe = ml.cls().findField(fa.name);
          if (fe != nullptr && fe->isStatic) {
            t.kind = TargetRef::Kind::StaticField;
            t.field = fe;
            t.type = fe->type;
            return t;
          }
        }
        if (owner == "java/lang/System" && (fa.name == "out" || fa.name == "err")) {
          ml.error(fa.nameRange,
                   "System." + fa.name +
                       " is not assignable (expected: a writable target)");
          t.type = JType::err();
          return t;
        }
        ml.error(fa.nameRange,
                 "static field '" + fa.name + "' on class '" + owner +
                     "' is not known to v0 lowering (expected: a static field "
                     "of " + ml.cls().internalName +
                     "; cross-class fields arrive with the class model)");
        t.type = JType::err();
        return t;
      }
    }
    // Instance field on an evaluated target expression.
    Val recv = ml.lowerExpr(*fa.target);
    if (!recv.ok()) {
      t.type = JType::err();
      return t;
    }
    if (recv.type.kind == JK::Ref && !recv.type.isArray() &&
        recv.type.name == ml.cls().internalName) {
      const FieldEntry* fe = ml.cls().findField(fa.name);
      if (fe == nullptr) {
        ml.error(fa.nameRange,
                 "class " + ml.cls().internalName + " has no field '" +
                     fa.name + "' (expected: a declared field name)");
        t.type = JType::err();
        return t;
      }
      if (fe->isStatic) {
        // Static via instance reference: target evaluates, value discarded.
        t.kind = TargetRef::Kind::StaticField;
        t.field = fe;
        t.type = fe->type;
        return t;
      }
      t.kind = TargetRef::Kind::InstanceField;
      t.field = fe;
      t.type = fe->type;
      t.recv = recv.reg;
      return t;
    }
    if (recv.type.isArray() && fa.name == "length") {
      ml.error(fa.nameRange,
               "array length is not assignable (expected: a writable target)");
      t.type = JType::err();
      return t;
    }
    ml.error(fa.nameRange,
             "field access on values of type " + recv.type.display() +
                 " is not supported by v0 lowering (expected: a receiver of "
                 "type " + ml.cls().internalName + ")");
    t.type = JType::err();
    return t;
  }

  if (target.kind == K::ArrayAccess) {
    const auto& aa = static_cast<const ast::ArrayAccess&>(target);
    Val arr = ml.lowerExpr(*aa.array);
    if (!arr.ok()) {
      t.type = JType::err();
      return t;
    }
    if (!arr.type.isArray()) {
      ml.error(aa.array->range,
               "array required, found " + arr.type.display() +
                   " (expected: an array-typed expression before '[')");
      t.type = JType::err();
      return t;
    }
    Val idx = ml.lowerExpr(*aa.index);
    if (!idx.ok()) {
      t.type = JType::err();
      return t;
    }
    if (!idx.type.isIntFamily() || idx.type.kind == JK::Bool) {
      ml.error(aa.index->range,
               "array index must be int, found " + idx.type.display() +
                   " (expected: an int-valued expression)");
      t.type = JType::err();
      return t;
    }
    t.kind = TargetRef::Kind::ArrayElem;
    t.type = arr.type.element();
    t.arr = arr.reg;
    t.idx = idx.reg;
    return t;
  }

  ml.error(target.range,
           "invalid assignment target (expected: a variable, field, or array "
           "element; hint: this expression form cannot be assigned to)");
  t.type = JType::err();
  return t;
}

std::uint16_t emitLoadTarget(MethodLowerer& ml, const TargetRef& t) {
  switch (t.kind) {
  case TargetRef::Kind::Local:
    return ml.emitLoad(t.slot, t.type);
  case TargetRef::Kind::InstanceField: {
    const std::uint32_t cp = ml.cpField(ml.cls().internalName, t.field->name,
                                        t.field->type);
    const std::uint16_t r = ml.newReg();
    ml.b().emitRegRegCp(Op::Getfield, r, t.recv, cp);
    return r;
  }
  case TargetRef::Kind::StaticField: {
    const std::uint32_t cp = ml.cpField(ml.cls().internalName, t.field->name,
                                        t.field->type);
    const std::uint16_t r = ml.newReg();
    ml.b().emitRegCp(Op::Getstatic, r, cp);
    return r;
  }
  case TargetRef::Kind::ArrayElem: {
    const std::uint16_t r = ml.newReg();
    ml.b().emitRegRegReg(aloadOpOf(t.type), r, t.arr, t.idx);
    return r;
  }
  }
  return 0;
}

void emitStoreToTarget(MethodLowerer& ml, const TargetRef& t,
                       std::uint16_t value) {
  switch (t.kind) {
  case TargetRef::Kind::Local:
    ml.emitStore(value, t.slot, t.type);
    return;
  case TargetRef::Kind::InstanceField: {
    const std::uint32_t cp = ml.cpField(ml.cls().internalName, t.field->name,
                                        t.field->type);
    ml.b().emitRegRegRegCp(Op::Putfield, 0, t.recv, value, cp);
    return;
  }
  case TargetRef::Kind::StaticField: {
    const std::uint32_t cp = ml.cpField(ml.cls().internalName, t.field->name,
                                        t.field->type);
    ml.b().emitRegCp(Op::Putstatic, value, cp);
    return;
  }
  case TargetRef::Kind::ArrayElem:
    ml.b().emitRegRegReg(astoreOpOf(t.type), value, t.arr, t.idx);
    return;
  }
}

// x++ / ++x / x-- / --x
Val lowerIncDec(MethodLowerer& ml, const ast::Expr& operand, bool inc,
                bool prefix, SourceRange at) {
  const TargetRef t = lowerTarget(ml, operand);
  if (t.type.isErr()) {
    return Val{};
  }
  if (!t.type.isNumeric() || t.type.kind == JK::Bool) {
    ml.error(at,
             "bad operand type " + t.type.display() +
                 " for " + (inc ? "increment" : "decrement") +
                 " (expected: a numeric variable, field, or array element)");
    return Val{};
  }
  const std::uint16_t cur = emitLoadTarget(ml, t);
  std::uint16_t oldReg = 0;
  if (!prefix) {
    oldReg = ml.newReg();
    ml.emitMove(oldReg, cur, t.type);
  }
  std::uint16_t updated = 0;
  if (t.type.isIntFamily()) {
    const std::uint16_t raw = ml.newReg();
    ml.b().emitRegImm(Op::Iinc, cur, inc ? 1u : 0xFFFFFFFFu);
    ml.emitMove(raw, cur, JType::prim(JK::Int));
    // Narrow back for byte/short/char targets (byte 127++ must wrap).
    updated = ml.emitConvert(raw, JType::prim(JK::Int), t.type, at);
  } else {
    const JType one = t.type;  // same family
    const std::uint16_t oneReg = ml.newReg();
    if (t.type.kind == JK::Long) {
      ml.b().emitRegCp(Op::Lconst, oneReg, ml.b().constLong(1));
    } else if (t.type.kind == JK::Float) {
      ml.b().emitRegImm(Op::Fconst, oneReg, std::bit_cast<std::uint32_t>(1.0F));
    } else {
      ml.b().emitRegCp(Op::Dconst, oneReg, ml.b().constDouble(1.0));
    }
    const ast::BinaryOp op = inc ? ast::BinaryOp::Add : ast::BinaryOp::Sub;
    updated = ml.newReg();
    ml.b().emitRegRegReg(arithOpOf(op, t.type.kind), updated, cur, oneReg);
    (void)one;
  }
  // ++/-- store the SAME type back (no implicit narrowing beyond identity
  // for int-family; the arithmetic already happened in the target family).
  emitStoreToTarget(ml, t, updated);
  return prefix ? Val{updated, t.type} : Val{oldReg, t.type};
}

}  // namespace

// -------------------------------------------------------------- inference ----

JType inferVarType(const ast::Expr& e,
                   const std::vector<std::pair<std::string, JType>>& locals,
                   const ClassModel& cls, DiagnosticEngine& diags,
                   const SourceManager& sm) {
  using K = ast::NodeKind;
  const auto localType = [&](std::string_view n) -> JType {
    for (const auto& [name, ty] : locals) {
      if (name == n) {
        return ty;
      }
    }
    return JType::err();
  };
  const auto fieldOfType = [&](const FieldEntry* fe) -> JType {
    return fe == nullptr ? JType::err() : fe->type;
  };

  switch (e.kind) {
  case K::Literal: {
    const auto& l = static_cast<const ast::Literal&>(e);
    switch (l.lit) {
    case ast::LitKind::Int: return JType::prim(JK::Int);
    case ast::LitKind::Long: return JType::prim(JK::Long);
    case ast::LitKind::Float: return JType::prim(JK::Float);
    case ast::LitKind::Double: return JType::prim(JK::Double);
    case ast::LitKind::Char: return JType::prim(JK::Char);
    case ast::LitKind::Boolean: return JType::prim(JK::Bool);
    case ast::LitKind::String: return JType::ref("java/lang/String");
    case ast::LitKind::Null:
      diags.error(e.range.offset,
                  "'var' cannot be inferred from the null literal (expected: "
                  "an initialized expression with a type; hint: write the "
                  "explicit type)");
      return JType::err();
    }
    return JType::err();
  }
  case K::ParenExpression:
    return inferVarType(*static_cast<const ast::ParenExpression&>(e).inner,
                        locals, cls, diags, sm);
  case K::Name: {
    const auto& n = static_cast<const ast::Name&>(e);
    JType t = localType(n.identifier);
    if (!t.isErr()) return t;
    return fieldOfType(cls.findField(n.identifier));
  }
  case K::ThisExpression:
    return JType::ref(cls.internalName);
  case K::Binary: {
    const auto& x = static_cast<const ast::Binary&>(e);
    const JType a = inferVarType(*x.lhs, locals, cls, diags, sm);
    const JType b = inferVarType(*x.rhs, locals, cls, diags, sm);
    if (a.isErr() || b.isErr()) return JType::err();
    if (a.isString() || b.isString()) {
      diags.error(e.range.offset,
                  "string concatenation is not supported by v0 lowering "
                  "(expected: non-String operands; String builtins arrive "
                  "with the runtime builtins RFC)");
      return JType::err();
    }
    if (isCompare(x.op) || x.op == ast::BinaryOp::CAnd ||
        x.op == ast::BinaryOp::COr) {
      return JType::prim(JK::Bool);
    }
    if (!a.isNumeric() || !b.isNumeric() || a.kind == JK::Bool ||
        b.kind == JK::Bool) {
      return JType::err();
    }
    return promote(a, b);
  }
  case K::PrefixUnary: {
    const auto& u = static_cast<const ast::PrefixUnary&>(e);
    const JType t = inferVarType(*u.operand, locals, cls, diags, sm);
    if (t.isErr()) return t;
    if (u.op == ast::UnaryOp::LogicalNot) {
      return JType::prim(JK::Bool);
    }
    if (!t.isNumeric() || t.kind == JK::Bool) return JType::err();
    return t.isIntFamily() ? JType::prim(JK::Int) : t;  // unary promotion
  }
  case K::PostfixUnary:
    return inferVarType(*static_cast<const ast::PostfixUnary&>(e).operand,
                        locals, cls, diags, sm);
  case K::Assignment: {
    const auto& a = static_cast<const ast::Assignment&>(e);
    // Target type without emission: locals and fields only.
    const auto& tgt = *a.target;
    if (tgt.kind == K::Name) {
      const auto& n = static_cast<const ast::Name&>(tgt);
      JType t = localType(n.identifier);
      if (!t.isErr()) return t;
      return fieldOfType(cls.findField(n.identifier));
    }
    return JType::err();
  }
  case K::Conditional: {
    const auto& c = static_cast<const ast::Conditional&>(e);
    const JType a = inferVarType(*c.thenExpr, locals, cls, diags, sm);
    const JType b = inferVarType(*c.elseExpr, locals, cls, diags, sm);
    if (a.isErr() || b.isErr()) return JType::err();
    if (a.isNumeric() && b.isNumeric()) return promote(a, b);
    if (a.isRefLike() && b.isRefLike()) {
      if (a.kind == JK::Null) return b;
      if (b.kind == JK::Null) return a;
      if (a.name == b.name) return a;
      return JType::ref("java/lang/Object");
    }
    if (a.kind == b.kind) return a;
    return JType::err();
  }
  case K::Cast: {
    const auto& c = static_cast<const ast::Cast&>(e);
    return jtypeOf(*c.type, cls.internalName, diags, sm);
  }
  case K::InstanceOf:
    return JType::prim(JK::Bool);
  case K::ClassLiteral:
    return JType::ref("java/lang/Class");
  case K::MethodInvocation: {
    const auto& m = static_cast<const ast::MethodInvocation&>(e);
    if (m.target == nullptr) {
      const MethodEntry* me = cls.findUnique(m.name, m.arguments.size());
      if (me != nullptr) return me->result;
      diags.error(e.range.offset,
                  "cannot infer 'var': no unique method '" + m.name + "' with " +
                      std::to_string(m.arguments.size()) +
                      " argument(s) in " + cls.internalName);
      return JType::err();
    }
    diags.error(e.range.offset,
                "cannot infer 'var' from a qualified call in v0 (expected: a "
                "call to a method of " + cls.internalName + ")");
    return JType::err();
  }
  case K::ClassInstanceCreation: {
    const auto& c = static_cast<const ast::ClassInstanceCreation&>(e);
    if (c.type == nullptr || c.type->kind != K::ClassType) {
      return JType::err();
    }
    return JType::ref(internalizeClassType(
        static_cast<const ast::ClassType&>(*c.type), cls.internalName));
  }
  case K::ArrayCreation: {
    const auto& a = static_cast<const ast::ArrayCreation&>(e);
    if (a.elementType == nullptr) return JType::err();
    const JType elem = jtypeOf(*a.elementType, cls.internalName, diags, sm);
    if (elem.isErr()) return elem;
    return arrayOf(elem);
  }
  case K::ArrayAccess: {
    const auto& a = static_cast<const ast::ArrayAccess&>(e);
    const JType arr = inferVarType(*a.array, locals, cls, diags, sm);
    return arr.isArray() ? arr.element() : JType::err();
  }
  case K::FieldAccess: {
    const auto& f = static_cast<const ast::FieldAccess&>(e);
    if (f.target == nullptr || f.target->kind != K::Name) {
      return JType::err();
    }
    const auto& n = static_cast<const ast::Name&>(*f.target);
    if (!localType(n.identifier).isErr()) {
      const JType t = inferVarType(*f.target, locals, cls, diags, sm);
      if (t.isArray() && f.name == "length") return JType::prim(JK::Int);
      if (t.kind == JK::Ref && !t.isArray() && t.name == cls.internalName) {
        return fieldOfType(cls.findField(f.name));
      }
      return JType::err();
    }
    // class-qualified static field
    const std::string owner = internalizeClassType(
        oneSegmentClass(n.identifier, f.target->range), cls.internalName);
    if (owner == cls.internalName) {
      const FieldEntry* fe = cls.findField(f.name);
      if (fe != nullptr && fe->isStatic) return fe->type;
    }
    if (owner == "java/lang/System" && (f.name == "out" || f.name == "err")) {
      return JType::ref("java/io/PrintStream");
    }
    return JType::err();
  }
  case K::ArrayInitializer:
    diags.error(e.range.offset,
                "'var' cannot be inferred from an array initializer "
                "(expected: 'new T[] {...}' or an explicit array type)");
    return JType::err();
  default:
    diags.error(e.range.offset,
                "cannot infer the type of this expression for 'var' (expected: "
                "an expression whose type v0 can compute; hint: write the "
                "explicit type)");
    return JType::err();
  }
}

// ------------------------------------------------------------ expressions ----

Val MethodLowerer::lowerExpr(const ast::Expr& e) {
  using K = ast::NodeKind;
  switch (e.kind) {
  case K::Literal: {
    const auto& l = static_cast<const ast::Literal&>(e);
    switch (l.lit) {
    case ast::LitKind::Int: {
      const std::uint16_t r = newReg();
      b().emitRegImm(Op::Iconst, r,
                     static_cast<std::uint32_t>(
                         static_cast<std::int32_t>(l.intValue)));
      return Val{r, JType::prim(JK::Int)};
    }
    case ast::LitKind::Long: {
      const std::uint16_t r = newReg();
      b().emitRegCp(Op::Lconst, r,
                    b().constLong(static_cast<std::int64_t>(l.intValue)));
      return Val{r, JType::prim(JK::Long)};
    }
    case ast::LitKind::Float: {
      const std::uint16_t r = newReg();
      b().emitRegImm(Op::Fconst, r,
                     std::bit_cast<std::uint32_t>(static_cast<float>(l.floatValue)));
      return Val{r, JType::prim(JK::Float)};
    }
    case ast::LitKind::Double: {
      const std::uint16_t r = newReg();
      b().emitRegCp(Op::Dconst, r, b().constDouble(l.floatValue));
      return Val{r, JType::prim(JK::Double)};
    }
    case ast::LitKind::Char: {
      const std::uint16_t r = newReg();
      b().emitRegImm(Op::Iconst, r,
                     static_cast<std::uint32_t>(l.intValue));
      return Val{r, JType::prim(JK::Char)};
    }
    case ast::LitKind::Boolean: {
      const std::uint16_t r = newReg();
      b().emitRegImm(Op::Iconst, r, l.intValue != 0 ? 1u : 0u);
      return Val{r, JType::prim(JK::Bool)};
    }
    case ast::LitKind::String: {
      const std::uint16_t r = newReg();
      b().emitRegCp(Op::Ldc, r, b().constString(l.stringValue));
      return Val{r, JType::ref("java/lang/String")};
    }
    case ast::LitKind::Null: {
      const std::uint16_t r = newReg();
      b().emitRegImm(Op::AconstNull, r, 0);
      return Val{r, JType::prim(JK::Null)};
    }
    }
    return Val{};
  }
  case K::ParenExpression:
    return lowerExpr(*static_cast<const ast::ParenExpression&>(e).inner);

  case K::Name: {
    const auto& n = static_cast<const ast::Name&>(e);
    if (const ScopeEntry* local = lookupLocal(n.identifier)) {
      const std::uint16_t r = emitLoad(local->slot, local->type);
      return Val{r, local->type};
    }
    const FieldEntry* fe = cls().findField(n.identifier);
    if (fe != nullptr) {
      if (fe->isStatic) {
        const std::uint32_t cp = cpField(cls().internalName, fe->name, fe->type);
        const std::uint16_t r = newReg();
        b().emitRegCp(Op::Getstatic, r, cp);
        return Val{r, fe->type};
      }
      if (isStatic()) {
        error(n.range,
              "instance field '" + n.identifier +
                  "' cannot be referenced from a static context (expected: a "
                  "static field or local; hint: make the enclosing method "
                  "non-static)");
        return Val{};
      }
      const std::uint32_t cp = cpField(cls().internalName, fe->name, fe->type);
      const std::uint16_t recv = loadThis(n.range);
      const std::uint16_t r = newReg();
      b().emitRegRegCp(Op::Getfield, r, recv, cp);
      return Val{r, fe->type};
    }
    error(n.range,
          "cannot resolve '" + n.identifier +
              "' (expected: a local variable, parameter, or field of " +
              cls().internalName + "; it is not a value in this context)");
    return Val{};
  }

  case K::ThisExpression: {
    const auto& t = static_cast<const ast::ThisExpression&>(e);
    if (!t.qualifier.empty()) {
      error(e.range,
            "qualified 'this' requires nested classes, which v0 lowering "
            "does not support (expected: plain 'this')");
      return Val{};
    }
    const std::uint16_t r = loadThis(e.range);
    return r != 0 || !isStatic() ? Val{r, JType::ref(cls().internalName)}
                                 : Val{};
  }

  case K::SuperAccess:
    error(e.range,
          "super member access is not supported by v0 lowering: the program "
          "class has no superclass in the v0 world (expected: an unqualified "
          "or this-qualified member)");
    return Val{};

  case K::ClassLiteral: {
    const auto& c = static_cast<const ast::ClassLiteral&>(e);
    const JType t = jtypeOf(*c.type, cls().internalName, diags(), sm());
    if (t.isErr()) {
      return Val{};
    }
    const std::uint16_t r = newReg();
    b().emitRegCp(Op::Ldc, r, cpClassOf(t));
    return Val{r, JType::ref("java/lang/Class")};
  }

  case K::Binary: {
    const auto& x = static_cast<const ast::Binary&>(e);
    using B = ast::BinaryOp;

    // Short-circuit booleans (JLS 15.23/15.24): the right operand evaluates
    // ONLY on the path that needs it - interleaving evaluation with the
    // branches is what makes the side effects (and their absence) correct.
    if (x.op == B::CAnd || x.op == B::COr) {
      const Val a = lowerBoolExpr(*x.lhs);
      if (!a.ok()) return Val{};
      const bool isAnd = x.op == B::CAnd;
      const auto Lshort = b().newLabel();  // and: false | or: true
      const auto Lend = b().newLabel();
      const std::uint16_t r = newReg();
      b().emitRegBranch(isAnd ? Op::Ifeq : Op::Ifne, a.reg, Lshort);
      const Val bb = lowerBoolExpr(*x.rhs);
      if (!bb.ok()) return Val{};
      b().emitRegBranch(isAnd ? Op::Ifeq : Op::Ifne, bb.reg, Lshort);
      b().emitRegImm(Op::Iconst, r, isAnd ? 1u : 0u);
      b().emitBranch(Op::Goto, Lend);
      b().bind(Lshort);
      b().emitRegImm(Op::Iconst, r, isAnd ? 0u : 1u);
      b().bind(Lend);
      return Val{r, JType::prim(JK::Bool)};
    }

    const Val a = lowerExpr(*x.lhs);
    if (!a.ok()) return Val{};
    const Val bb = lowerExpr(*x.rhs);
    if (!bb.ok()) return Val{};

    // String concatenation is a '+' with a String operand; comparisons and
    // equality on String references are reference operations and legal.
    if (x.op == ast::BinaryOp::Add &&
        (a.type.isString() || bb.type.isString())) {
      error(e.range,
            "string concatenation ('+') is not supported by v0 lowering "
            "(expected: numeric operands; String builtins arrive with the "
            "runtime builtins RFC)");
      return Val{};
    }

    if (isCompare(x.op)) {
      // Reference / null comparisons.
      if (a.type.isRefLike() && bb.type.isRefLike()) {
        if (x.op != B::Eq && x.op != B::Ne) {
          error(e.range,
                "reference operands only support == and != (expected: "
                "reference equality, not ordering)");
          return Val{};
        }
        const auto Ltrue = b().newLabel();
        const auto Lend = b().newLabel();
        const std::uint16_t r = newReg();
        if (a.type.kind == JK::Null && bb.type.kind != JK::Null) {
          b().emitRegBranch(x.op == B::Eq ? Op::Ifnull : Op::Ifnonnull,
                            bb.reg, Ltrue);
        } else if (bb.type.kind == JK::Null) {
          b().emitRegBranch(x.op == B::Eq ? Op::Ifnull : Op::Ifnonnull,
                            a.reg, Ltrue);
        } else {
          b().emitRegRegBranch(x.op == B::Eq ? Op::IfAcmpeq : Op::IfAcmpne,
                               a.reg, bb.reg, Ltrue);
        }
        b().emitRegImm(Op::Iconst, r, 0);
        b().emitBranch(Op::Goto, Lend);
        b().bind(Ltrue);
        b().emitRegImm(Op::Iconst, r, 1);
        b().bind(Lend);
        return Val{r, JType::prim(JK::Bool)};
      }

      if (!a.type.isNumeric() || !bb.type.isNumeric() ||
          a.type.kind == JK::Bool || bb.type.kind == JK::Bool) {
        error(e.range,
              "bad operand types " + a.type.display() + " and " +
                  bb.type.display() + " for " +
                  std::string(x.op == B::Eq ? "==" : x.op == B::Ne ? "!=" :
                              x.op == B::Lt ? "<" : x.op == B::Gt ? ">" :
                              x.op == B::Le ? "<=" : ">=") +
                  " (expected: numeric or reference operands)");
        return Val{};
      }

      const JType p = promote(a.type, bb.type);
      const std::uint16_t ar = emitConvert(a.reg, a.type, p, x.range);
      const std::uint16_t br = emitConvert(bb.reg, bb.type, p, x.range);
      const std::uint16_t c = newReg();
      // NaN discipline (JLS 15.20.1): comparisons use the l/g form that
      // makes NaN compare the way Java requires for THIS operator.
      Op cmpOp = Op::Icmp;
      bool useG = false;
      if (p.kind == JK::Long) {
        cmpOp = Op::Lcmp;
      } else if (p.kind == JK::Float) {
        // <: g, >: l, <=: g, >=: l, ==/!=: l
        useG = (x.op == B::Lt || x.op == B::Le);
        cmpOp = useG ? Op::Fcmpg : Op::Fcmpl;
      } else if (p.kind == JK::Double) {
        useG = (x.op == B::Lt || x.op == B::Le);
        cmpOp = useG ? Op::Dcmpg : Op::Dcmpl;
      }
      b().emitRegRegReg(cmpOp, c, ar, br);
      Op brOp = Op::Ifeq;
      switch (x.op) {
      case B::Lt: brOp = Op::Iflt; break;
      case B::Gt: brOp = Op::Ifgt; break;
      case B::Le: brOp = Op::Ifle; break;
      case B::Ge: brOp = Op::Ifge; break;
      case B::Eq: brOp = Op::Ifeq; break;
      case B::Ne: brOp = Op::Ifne; break;
      default: break;
      }
      const auto Ltrue = b().newLabel();
      const auto Lend = b().newLabel();
      const std::uint16_t r = newReg();
      b().emitRegBranch(brOp, c, Ltrue);
      b().emitRegImm(Op::Iconst, r, 0);
      b().emitBranch(Op::Goto, Lend);
      b().bind(Ltrue);
      b().emitRegImm(Op::Iconst, r, 1);
      b().bind(Lend);
      return Val{r, JType::prim(JK::Bool)};
    }

    // Arithmetic / bitwise / shifts.
    if (!a.type.isNumeric() || !bb.type.isNumeric() ||
        a.type.kind == JK::Bool || bb.type.kind == JK::Bool) {
      error(e.range,
            "bad operand types " + a.type.display() + " and " +
                bb.type.display() + " for binary operator (expected: numeric "
                "operands; boxing/unboxing arrives with the binding stage)");
      return Val{};
    }
    if (isBitwise(x.op)) {
      if (a.type.kind == JK::Float || a.type.kind == JK::Double ||
          bb.type.kind == JK::Float || bb.type.kind == JK::Double) {
        error(e.range,
              "bitwise/shift operators are not defined for floating-point "
              "operands (expected: int or long operands)");
        return Val{};
      }
    }
    if (isShift(x.op)) {
      // Shift: lhs family decides; rhs stays Int (JLS 5.6.2).
      const JType lt = a.type.isIntFamily() ? JType::prim(JK::Int) : a.type;
      const std::uint16_t ar = emitConvert(a.reg, a.type, lt, x.range);
      if (!bb.type.isIntFamily() || bb.type.kind == JK::Bool) {
        error(x.rhs->range,
              "shift count must be int, found " + bb.type.display() +
                  " (expected: an int-valued expression)");
        return Val{};
      }
      const std::uint16_t r = newReg();
      b().emitRegRegReg(arithOpOf(x.op, lt.kind), r, ar, bb.reg);
      return Val{r, lt};
    }
    const JType p = promote(a.type, bb.type);
    const std::uint16_t ar = emitConvert(a.reg, a.type, p, x.range);
    const std::uint16_t br = emitConvert(bb.reg, bb.type, p, x.range);
    const std::uint16_t r = newReg();
    b().emitRegRegReg(arithOpOf(x.op, p.kind), r, ar, br);
    return Val{r, p};
  }

  case K::PrefixUnary: {
    const auto& u = static_cast<const ast::PrefixUnary&>(e);
    if (u.op == ast::UnaryOp::PreInc || u.op == ast::UnaryOp::PreDec) {
      return lowerIncDec(*this, *u.operand, u.op == ast::UnaryOp::PreInc, true,
                         e.range);
    }
    const Val v = lowerExpr(*u.operand);
    if (!v.ok()) return Val{};
    switch (u.op) {
    case ast::UnaryOp::Plus: {
      if (!v.type.isNumeric() || v.type.kind == JK::Bool) {
        error(e.range,
              "bad operand type " + v.type.display() +
                  " for unary '+' (expected: a numeric operand)");
        return Val{};
      }
      const JType t = v.type.isIntFamily() ? JType::prim(JK::Int) : v.type;
      return Val{v.reg, t};  // unary promotion, identity emission
    }
    case ast::UnaryOp::Minus: {
      if (!v.type.isNumeric() || v.type.kind == JK::Bool) {
        error(e.range,
              "bad operand type " + v.type.display() +
                  " for unary '-' (expected: a numeric operand)");
        return Val{};
      }
      // Fold negated float/double literals to constants.
      if (u.operand->kind == K::Literal) {
        const auto& l = static_cast<const ast::Literal&>(*u.operand);
        if (l.lit == ast::LitKind::Float) {
          const std::uint16_t r = newReg();
          b().emitRegImm(Op::Fconst, r,
                         std::bit_cast<std::uint32_t>(-static_cast<float>(l.floatValue)));
          return Val{r, JType::prim(JK::Float)};
        }
        if (l.lit == ast::LitKind::Double) {
          const std::uint16_t r = newReg();
          b().emitRegCp(Op::Dconst, r, b().constDouble(-l.floatValue));
          return Val{r, JType::prim(JK::Double)};
        }
      }
      const JType t = v.type.isIntFamily() ? JType::prim(JK::Int) : v.type;
      const std::uint16_t ar = emitConvert(v.reg, v.type, t, e.range);
      const std::uint16_t r = newReg();
      b().emitRegReg(t.kind == JK::Long ? Op::Lneg
                                        : t.kind == JK::Float ? Op::Fneg
                                        : t.kind == JK::Double ? Op::Dneg
                                                               : Op::Ineg,
                     r, ar);
      return Val{r, t};
    }
    case ast::UnaryOp::BitNot: {
      if (!v.type.isIntFamily() && v.type.kind != JK::Long) {
        error(e.range,
              "bad operand type " + v.type.display() +
                  " for '~' (expected: int or long)");
        return Val{};
      }
      const JType t = v.type.isIntFamily() ? JType::prim(JK::Int) : v.type;
      const std::uint16_t ar = emitConvert(v.reg, v.type, t, e.range);
      // ~x == x ^ -1  (RBC has no not op; xor with all-ones)
      const std::uint16_t m = newReg();
      if (t.kind == JK::Long) {
        b().emitRegCp(Op::Lconst, m, b().constLong(-1));
      } else {
        b().emitRegImm(Op::Iconst, m, 0xFFFFFFFFu);
      }
      const std::uint16_t r = newReg();
      b().emitRegRegReg(t.kind == JK::Long ? Op::Lxor : Op::Ixor, r, ar, m);
      return Val{r, t};
    }
    case ast::UnaryOp::LogicalNot: {
      if (v.type.kind != JK::Bool) {
        error(e.range,
              "bad operand type " + v.type.display() +
                  " for '!' (expected: boolean)");
        return Val{};
      }
      const std::uint16_t one = newReg();
      b().emitRegImm(Op::Iconst, one, 1);
      const std::uint16_t r = newReg();
      b().emitRegRegReg(Op::Ixor, r, v.reg, one);
      return Val{r, JType::prim(JK::Bool)};
    }
    default: break;
    }
    return Val{};
  }

  case K::PostfixUnary: {
    const auto& p = static_cast<const ast::PostfixUnary&>(e);
    return lowerIncDec(*this, *p.operand, p.op == ast::PostfixOp::Inc, false,
                       e.range);
  }

  case K::Assignment: {
    const auto& a = static_cast<const ast::Assignment&>(e);
    using A = ast::AssignOp;
    const TargetRef t = lowerTarget(*this, *a.target);
    if (t.type.isErr()) {
      return Val{};
    }
    if (a.op == A::Assign) {
      const Val v =
          lowerExprTo(*a.value, t.type, "assignment", a.value->range);
      if (!v.ok()) return Val{};
      emitStoreToTarget(*this, t, v.reg);
      return v;
    }
    // Compound assignment: E1 op= E2 == E1 = (T)((E1) op (E2)).
    if (t.type.isString()) {
      error(e.range,
            "String compound assignment is not supported by v0 lowering "
            "(expected: numeric operands; String builtins arrive with the "
            "runtime builtins RFC)");
      return Val{};
    }
    if (!t.type.isNumeric() || t.type.kind == JK::Bool) {
      error(e.range,
            "bad operand type " + t.type.display() +
                " for compound assignment (expected: a numeric variable)");
      return Val{};
    }
    const bool bitwise = a.op == A::AndA || a.op == A::OrA || a.op == A::XorA ||
                         a.op == A::ShlA || a.op == A::ShrA || a.op == A::UShrA;
    if (bitwise &&
        (t.type.kind == JK::Float || t.type.kind == JK::Double)) {
      error(e.range,
            "bitwise/shift compound assignment is not defined for "
            "floating-point targets (expected: int or long)");
      return Val{};
    }
    const std::uint16_t cur = emitLoadTarget(*this, t);
    const Val rhs = lowerExpr(*a.value);
    if (!rhs.ok()) return Val{};
    if (!rhs.type.isNumeric() || rhs.type.kind == JK::Bool) {
      error(a.value->range,
            "bad operand type " + rhs.type.display() +
                " for compound assignment (expected: a numeric value)");
      return Val{};
    }
    const ast::BinaryOp bin = [&]() {
      switch (a.op) {
      case A::AddA: return ast::BinaryOp::Add;
      case A::SubA: return ast::BinaryOp::Sub;
      case A::MulA: return ast::BinaryOp::Mul;
      case A::DivA: return ast::BinaryOp::Div;
      case A::ModA: return ast::BinaryOp::Mod;
      case A::AndA: return ast::BinaryOp::And;
      case A::OrA: return ast::BinaryOp::Or;
      case A::XorA: return ast::BinaryOp::Xor;
      case A::ShlA: return ast::BinaryOp::Shl;
      case A::ShrA: return ast::BinaryOp::Shr;
      case A::UShrA: return ast::BinaryOp::UShr;
      default: return ast::BinaryOp::Add;
      }
    }();
    JType p = promote(t.type, rhs.type);
    if (isShift(bin)) {
      p = t.type.isIntFamily() ? JType::prim(JK::Int) : t.type;
    }
    const std::uint16_t ar = emitConvert(cur, t.type, p, e.range);
    std::uint16_t br = rhs.reg;
    if (!isShift(bin) || !rhs.type.isIntFamily()) {
      br = emitConvert(rhs.reg, rhs.type, p, a.value->range);
    } else if (rhs.type.kind == JK::Bool) {
      error(a.value->range,
            "boolean shift count is invalid (expected: an int-valued "
             "expression)");
      return Val{};
    }
    const std::uint16_t computed = newReg();
    b().emitRegRegReg(arithOpOf(bin, p.kind), computed, ar, br);
    // Implicit narrowing back to the target type (JLS 15.26.2).
    const std::uint16_t stored =
        emitConvert(computed, p, t.type, e.range);
    emitStoreToTarget(*this, t, stored);
    return Val{stored, t.type};
  }

  case K::Conditional: {
    const auto& c = static_cast<const ast::Conditional&>(e);
    const Val cond = lowerBoolExpr(*c.condition);
    if (!cond.ok()) return Val{};
    const Val tv = lowerExpr(*c.thenExpr);
    if (!tv.ok()) return Val{};
    const Val ev = lowerExpr(*c.elseExpr);
    if (!ev.ok()) return Val{};
    JType rt;
    if (tv.type.isNumeric() && ev.type.isNumeric()) {
      rt = promote(tv.type, ev.type);
    } else if (tv.type.isRefLike() && ev.type.isRefLike()) {
      if (tv.type.kind == JK::Null) rt = ev.type;
      else if (ev.type.kind == JK::Null) rt = tv.type;
      else if (tv.type.name == ev.type.name) rt = tv.type;
      else rt = JType::ref("java/lang/Object");
    } else if (tv.type.kind == ev.type.kind) {
      rt = tv.type;
    } else {
      error(e.range,
            "incompatible branch types " + tv.type.display() + " and " +
                ev.type.display() + " (expected: branches with a common "
                "promotion)");
      return Val{};
    }
    const auto Lelse = b().newLabel();
    const auto Lend = b().newLabel();
    b().emitRegBranch(Op::Ifeq, cond.reg, Lelse);
    const std::uint16_t tr = emitConvert(tv.reg, tv.type, rt, c.thenExpr->range);
    b().emitBranch(Op::Goto, Lend);
    b().bind(Lelse);
    const std::uint16_t er = emitConvert(ev.reg, ev.type, rt, c.elseExpr->range);
    emitMove(tr, er, rt);
    b().bind(Lend);
    return Val{tr, rt};
  }

  case K::Cast: {
    const auto& c = static_cast<const ast::Cast&>(e);
    const JType to = jtypeOf(*c.type, cls().internalName, diags(), sm());
    if (to.isErr()) return Val{};
    const Val v = lowerExpr(*c.expression);
    if (!v.ok()) return Val{};
    if (to.isRefLike() && v.type.isRefLike()) {
      const std::uint32_t cp = cpClassOf(to);
      const std::uint16_t r = newReg();
      b().emitRegRegCp(Op::Checkcast, r, v.reg, cp);
      return Val{r, to};
    }
    if (to.isNumeric() && v.type.isNumeric() &&
        to.kind != JK::Bool && v.type.kind != JK::Bool) {
      const std::uint16_t r = emitConvert(v.reg, v.type, to, e.range);
      return Val{r, to};
    }
    if (to.kind == JK::Bool && v.type.kind == JK::Bool) {
      return v;
    }
    error(e.range,
          "cannot cast " + v.type.display() + " to " + to.display() +
              " (expected: numeric-to-numeric or reference-to-reference)");
    return Val{};
  }

  case K::InstanceOf: {
    const auto& io = static_cast<const ast::InstanceOf&>(e);
    if (io.pattern != nullptr) {
      error(e.range,
            "pattern matching in instanceof is not supported by v0 lowering "
            "(expected: a plain type test; patterns arrive with the binding "
            "stage)");
      return Val{};
    }
    const Val v = lowerExpr(*io.expression);
    if (!v.ok()) return Val{};
    if (!v.type.isRefLike()) {
      error(e.range,
            "instanceof requires a reference operand, found " +
                v.type.display() + " (expected: a reference-typed "
                "expression)");
      return Val{};
    }
    const JType to = jtypeOf(*io.type, cls().internalName, diags(), sm());
    if (to.isErr()) return Val{};
    const std::uint32_t cp = cpClassOf(to);
    const std::uint16_t r = newReg();
    b().emitRegRegCp(Op::Instanceof, r, v.reg, cp);
    return Val{r, JType::prim(JK::Bool)};
  }

  case K::ArrayAccess: {
    const auto& aa = static_cast<const ast::ArrayAccess&>(e);
    const Val arr = lowerExpr(*aa.array);
    if (!arr.ok()) return Val{};
    if (!arr.type.isArray()) {
      error(aa.array->range,
            "array required, found " + arr.type.display() +
                " (expected: an array-typed expression before '[')");
      return Val{};
    }
    const Val idx = lowerExpr(*aa.index);
    if (!idx.ok()) return Val{};
    if (!idx.type.isIntFamily() || idx.type.kind == JK::Bool) {
      error(aa.index->range,
            "array index must be int, found " + idx.type.display() +
                " (expected: an int-valued expression)");
      return Val{};
    }
    const JType elem = arr.type.element();
    const std::uint16_t r = newReg();
    b().emitRegRegReg(aloadOpOf(elem), r, arr.reg, idx.reg);
    return Val{r, elem};
  }

  case K::ArrayCreation: {
    const auto& ac = static_cast<const ast::ArrayCreation&>(e);
    if (ac.dimensions.size() > 1) {
      error(e.range,
            "multi-dimensional array creation is not supported by v0 "
            "lowering (expected: a one-dimensional 'new T[n]' or 'new "
            "T[]{...}' — multi-dim arrives with the array lowering RFC)");
      return Val{};
    }
    const JType elem = jtypeOf(*ac.elementType, cls().internalName, diags(),
                               sm());
    if (elem.isErr()) return Val{};
    const JType arr = arrayOf(elem);
    if (ac.initializer != nullptr) {
      if (!ac.dimensions.empty() && ac.dimensions[0].size != nullptr) {
        error(e.range,
              "array creation cannot mix a dimension and an initializer "
              "(expected: 'new T[n]' or 'new T[]{...}')");
        return Val{};
      }
      return lowerArrayInitializer(*ac.initializer, elem, e.range);
    }
    if (ac.dimensions.empty() || ac.dimensions[0].size == nullptr) {
      error(e.range,
            "array creation requires a length or an initializer (expected: "
            "'new T[n]' or 'new T[]{...}')");
      return Val{};
    }
    const Val len = lowerExpr(*ac.dimensions[0].size);
    if (!len.ok()) return Val{};
    if (!len.type.isIntFamily() || len.type.kind == JK::Bool) {
      error(ac.dimensions[0].size->range,
            "array length must be int, found " + len.type.display() +
                " (expected: an int-valued expression)");
      return Val{};
    }
    const std::uint16_t r = newReg();
    if (elem.isRefLike()) {
      b().emitRegRegCp(Op::AnewArray, r, len.reg, cpClassOf(elem));
    } else {
      b().emitRegRegImm(Op::NewArray, r, len.reg, atypeOf(elem));
    }
    return Val{r, arr};
  }

  case K::ArrayInitializer:
    error(e.range,
          "an array initializer needs a declared type or 'new T[]{...}' "
          "(expected: 'T[] x = {...}' or 'new T[]{...}')");
    return Val{};

  case K::ClassInstanceCreation:
    return lowerNew(static_cast<const ast::ClassInstanceCreation&>(e));

  case K::MethodInvocation:
    return lowerInvoke(static_cast<const ast::MethodInvocation&>(e));

  case K::ConstructorInvocation:
    error(e.range,
          "constructor invocation 'this(...)'/'super(...)' is only allowed "
          "as the first statement of a constructor (expected: a normal "
          "expression here)");
    return Val{};

  case K::Lambda:
    error(e.range,
          "lambdas are not supported by v0 lowering: they need "
          "invokedynamic linkage (expected: a class instance or method "
          "call; lambdas arrive with the invokedynamic RFC)");
    return Val{};

  case K::MethodReference:
    error(e.range,
          "method references are not supported by v0 lowering: they need "
          "invokedynamic linkage (expected: a class instance or method "
          "call; method references arrive with the invokedynamic RFC)");
    return Val{};

  case K::SwitchExpression:
    error(e.range,
          "switch expressions are not supported by v0 lowering (expected: a "
          "switch statement or conditional expression; switch expressions "
          "arrive with the binding stage)");
    return Val{};

  case K::FieldAccess: {
    const auto& fa = static_cast<const ast::FieldAccess&>(e);
    return lowerFieldAccess(fa);
  }

  default:
    error(e.range,
          "expression form is not supported by v0 lowering (expected: a "
          "supported expression; this construct arrives with a later "
          "lowering version)");
    return Val{};
  }
}

Val MethodLowerer::lowerExprTo(const ast::Expr& e, const JType& target,
                               std::string_view context, SourceRange at) {
  const IntFold f = foldInt(e);
  Val v = lowerExpr(e);
  if (!v.ok()) {
    return v;
  }
  if (target.isErr() || target.isVoid()) {
    return v;
  }
  if (v.type.isVoid()) {
    error(at, std::string(context) +
                  ": a void result cannot be used as a value (expected: a "
                  "non-void expression)");
    return Val{};
  }
  const std::optional<std::int64_t> fold =
      f.ok ? std::optional<std::int64_t>(f.v) : std::nullopt;
  const std::uint16_t r = emitAssignConvert(v.reg, v.type, target, fold, at);
  return Val{r, target};
}

Val MethodLowerer::lowerBoolExpr(const ast::Expr& e) {
  Val v = lowerExpr(e);
  if (!v.ok()) {
    return v;
  }
  if (v.type.kind != JK::Bool) {
    error(e.range,
          "condition has type " + v.type.display() +
              ", expected boolean (expected: a boolean expression)");
    return Val{};
  }
  return v;
}

// ---------------------------------------------------------- sub-lowerings ----

Val MethodLowerer::lowerFieldAccess(const ast::FieldAccess& fa) {
  // Class-qualified static field.
  if (fa.target != nullptr && fa.target->kind == ast::NodeKind::Name) {
    const auto& n = static_cast<const ast::Name&>(*fa.target);
    if (lookupLocal(n.identifier) == nullptr &&
        cls().findField(n.identifier) == nullptr) {
      const std::string owner = internalizeClassType(
          oneSegmentClass(n.identifier, fa.target->range), cls().internalName);
      if (owner == cls().internalName) {
        const FieldEntry* fe = cls().findField(fa.name);
        if (fe != nullptr && fe->isStatic) {
          const std::uint32_t cp = cpField(cls().internalName, fe->name, fe->type);
          const std::uint16_t r = newReg();
          b().emitRegCp(Op::Getstatic, r, cp);
          return Val{r, fe->type};
        }
      }
      if (owner == "java/lang/System" && (fa.name == "out" || fa.name == "err")) {
        const std::uint32_t cp = cpField("java/lang/System", fa.name,
                                         JType::ref("java/io/PrintStream"));
        const std::uint16_t r = newReg();
        b().emitRegCp(Op::Getstatic, r, cp);
        return Val{r, JType::ref("java/io/PrintStream")};
      }
      error(fa.nameRange,
            "field '" + fa.name + "' on class '" + owner +
                "' is not known to v0 lowering (expected: a field of " +
                cls().internalName +
                " or System.out/System.err; the classpath arrives with the "
                "loader)");
      return Val{};
    }
  }

  const Val recv = lowerExpr(*fa.target);
  if (!recv.ok()) {
    return Val{};
  }
  if (recv.type.isArray() && fa.name == "length") {
    const std::uint16_t r = newReg();
    b().emitRegReg(Op::Arraylength, r, recv.reg);
    return Val{r, JType::prim(JK::Int)};
  }
  if (recv.type.kind == JK::Ref && !recv.type.isArray() &&
      recv.type.name == cls().internalName) {
    const FieldEntry* fe = cls().findField(fa.name);
    if (fe == nullptr) {
      error(fa.nameRange,
            "class " + cls().internalName + " has no field '" + fa.name +
                "' (expected: a declared field name)");
      return Val{};
    }
    if (fe->isStatic) {
      const std::uint32_t cp = cpField(cls().internalName, fe->name, fe->type);
      const std::uint16_t r = newReg();
      b().emitRegCp(Op::Getstatic, r, cp);
      return Val{r, fe->type};
    }
    const std::uint32_t cp = cpField(cls().internalName, fe->name, fe->type);
    const std::uint16_t r = newReg();
    b().emitRegRegCp(Op::Getfield, r, recv.reg, cp);
    return Val{r, fe->type};
  }
  if (recv.type.isString()) {
    error(fa.nameRange,
          "field access on String is not supported by v0 lowering "
          "(expected: a receiver of type " + cls().internalName +
          "; String members arrive with the runtime builtins RFC)");
    return Val{};
  }
  error(fa.nameRange,
        "field access on values of type " + recv.type.display() +
            " is not supported by v0 lowering (expected: a receiver of type " +
            cls().internalName + " or an array '.length')");
  return Val{};
}

// Emits the call: receiver (already in `recvReg` when present) and argument
// values (already evaluated, left to right, in `argVals`), moved into one
// consecutive block, then the invoke.
Val MethodLowerer::emitCallSeq(Op op, std::optional<std::uint16_t> recvReg,
                               const std::vector<const ast::Expr*>& args,
                               const std::vector<JType>* declaredParams,
                               const std::string& clsInternal,
                               const std::string& name, SourceRange at,
                               const JType& declaredResult) {
  std::vector<Val> vals;
  vals.reserve(args.size());
  for (const ast::Expr* a : args) {
    Val v = lowerExpr(*a);
    if (!v.ok()) {
      return Val{};
    }
    vals.push_back(v);
  }

  // Parameter types: declared (own methods) or derived (unknown receivers).
  std::vector<JType> paramTypes;
  if (declaredParams != nullptr) {
    paramTypes = *declaredParams;
  } else {
    paramTypes.reserve(vals.size());
    for (const Val& v : vals) {
      if (v.type.isVoid()) {
        error(at, "argument has no value (expected: a non-void expression)");
        return Val{};
      }
      paramTypes.push_back(v.type);
    }
  }

  // Consecutive argument block: [receiver?] args...
  const std::size_t total = vals.size() + (recvReg.has_value() ? 1 : 0);
  std::vector<std::uint16_t> block;
  block.reserve(total);
  for (std::size_t i = 0; i < total; ++i) {
    block.push_back(newReg());
  }
  std::size_t bi = 0;
  if (recvReg.has_value()) {
    emitMove(block[bi++], *recvReg, JType::ref("java/lang/Object"));
  }
  for (std::size_t i = 0; i < vals.size(); ++i) {
    if (i < paramTypes.size()) {
      const IntFold f = foldInt(*args[i]);
      const std::optional<std::int64_t> fold =
          f.ok ? std::optional<std::int64_t>(f.v) : std::nullopt;
      const std::uint16_t cv = emitAssignConvert(
          vals[i].reg, vals[i].type, paramTypes[i], fold, args[i]->range);
      emitMove(block[bi++], cv, paramTypes[i]);
    } else {
      emitMove(block[bi++], vals[i].reg, vals[i].type);
    }
  }

  std::string desc = "(";
  for (const JType& p : paramTypes) {
    desc += (declaredParams != nullptr) ? p.descriptor() : argDescriptorOf(p);
  }
  desc += ")";
  desc += (declaredParams != nullptr) ? declaredResult.descriptor() : "V";

  const std::uint32_t cp = cpMethod(clsInternal, name, desc);
  const std::uint16_t dst = newReg();
  // Zero-arg static calls have no argument block; argBase is then unused
  // (argCount = 0) and dst is a safe placeholder.
  const std::uint16_t argBase = block.empty() ? dst : block.front();
  b().emitCall(op, dst, argBase, static_cast<std::uint16_t>(total), cp);
  (void)at;
  return Val{dst, declaredParams != nullptr ? declaredResult
                                            : JType::prim(JK::Void)};
}

Val MethodLowerer::lowerInvoke(const ast::MethodInvocation& mi) {
  // Unqualified call: own static, then own instance (on this).
  if (mi.target == nullptr) {
    const MethodEntry* me =
        cls().findUnique(mi.name, mi.arguments.size());
    if (me == nullptr) {
      // Better message for the varargs case.
      for (const MethodEntry& m : cls().methods) {
        if (m.name == mi.name &&
            m.params.size() != mi.arguments.size()) {
          error(mi.nameRange,
                "call to '" + mi.name + "' does not match any declared "
                "method of " + cls().internalName + " with " +
                    std::to_string(mi.arguments.size()) +
                    " argument(s) (expected: the exact arity; varargs call "
                    "sites are not supported by v0 — pass an explicit "
                    "array)");
          return Val{};
        }
      }
      error(mi.nameRange,
            "cannot resolve method '" + mi.name +
                "' with " + std::to_string(mi.arguments.size()) +
                " argument(s) (expected: a declared method of " +
                cls().internalName + ")");
      return Val{};
    }
    std::vector<const ast::Expr*> args;
    for (const ast::Ptr<ast::Expr>& a : mi.arguments) {
      args.push_back(a.get());
    }
    if (me->isStatic) {
      return emitCallSeq(Op::Invokestatic, std::nullopt, args, &me->params,
                         cls().internalName, me->name, mi.range, me->result);
    }
    if (isStatic()) {
      error(mi.nameRange,
            "instance method '" + mi.name +
                "' cannot be invoked from a static context (expected: a "
                "static method or an instance receiver)");
      return Val{};
    }
    const std::uint16_t thiz = loadThis(mi.range);
    return emitCallSeq(Op::Invokevirtual, thiz, args, &me->params,
                       cls().internalName, me->name, mi.range, me->result);
  }

  // Class-qualified static call.
  if (mi.target->kind == ast::NodeKind::Name) {
    const auto& n = static_cast<const ast::Name&>(*mi.target);
    if (lookupLocal(n.identifier) == nullptr &&
        cls().findField(n.identifier) == nullptr) {
      const std::string owner = internalizeClassType(
          oneSegmentClass(n.identifier, mi.target->range), cls().internalName);
      if (owner == cls().internalName) {
        const MethodEntry* me =
            cls().findUnique(mi.name, mi.arguments.size());
        if (me != nullptr && me->isStatic) {
          std::vector<const ast::Expr*> args;
          for (const ast::Ptr<ast::Expr>& a : mi.arguments) {
            args.push_back(a.get());
          }
          return emitCallSeq(Op::Invokestatic, std::nullopt, args, &me->params,
                             cls().internalName, me->name, mi.range,
                             me->result);
        }
      }
      error(mi.nameRange,
            "static call into '" + owner +
                "' is not supported by v0 lowering: v0 resolves methods of "
                "the program class only (expected: a method of " +
                cls().internalName +
                "; cross-class calls arrive with the loader)");
      return Val{};
    }
  }

  // Qualified call on an evaluated receiver.
  const Val recv = lowerExpr(*mi.target);
  if (!recv.ok()) {
    return Val{};
  }
  std::vector<const ast::Expr*> args;
  for (const ast::Ptr<ast::Expr>& a : mi.arguments) {
    args.push_back(a.get());
  }

  if (recv.type.kind == JK::Ref && !recv.type.isArray() &&
      recv.type.name == cls().internalName) {
    const MethodEntry* me =
        cls().findUnique(mi.name, mi.arguments.size());
    if (me == nullptr) {
      error(mi.nameRange,
            "cannot resolve method '" + mi.name + "' with " +
                std::to_string(mi.arguments.size()) + " argument(s) in " +
                cls().internalName + " (expected: a declared method)");
      return Val{};
    }
    if (me->isStatic) {
      return emitCallSeq(Op::Invokestatic, std::nullopt, args, &me->params,
                         cls().internalName, me->name, mi.range, me->result);
    }
    return emitCallSeq(Op::Invokevirtual, recv.reg, args, &me->params,
                       cls().internalName, me->name, mi.range, me->result);
  }

  if (recv.type.isArray()) {
    error(mi.nameRange,
          "method calls on arrays are not supported by v0 lowering "
          "(expected: element access or .length)");
    return Val{};
  }
  if (recv.type.isString()) {
    error(mi.nameRange,
          "method calls on String are not supported by v0 lowering: the v0 "
          "runtime builtins are println/print only (expected: a receiver of "
          "type " + cls().internalName +
          "; String methods arrive with the runtime builtins RFC)");
    return Val{};
  }
  if (!recv.type.isRefLike()) {
    error(mi.nameRange,
          "cannot invoke method '" + mi.name + "' on " +
              recv.type.display() + " (expected: a reference receiver)");
    return Val{};
  }

  // Unknown reference receiver: dynamic resolution (JVM contract). v0
  // supports println/print on System.out/err this way; the result is void.
  if (recv.type.kind == JK::Null) {
    error(mi.target->range,
          "cannot invoke a method on the null literal (expected: a "
          "non-null receiver)");
    return Val{};
  }
  return emitCallSeq(Op::Invokevirtual, recv.reg, args, nullptr,
                     "java/lang/Object", mi.name, mi.range,
                     JType::prim(JK::Void));
}

Val MethodLowerer::lowerNew(const ast::ClassInstanceCreation& nc) {
  if (nc.anonymousBody != nullptr) {
    error(nc.range,
          "anonymous classes are not supported by v0 lowering (expected: a "
          "named class; nested/anonymous classes arrive with the class "
          "model)");
    return Val{};
  }
  if (nc.type == nullptr || nc.type->kind != ast::NodeKind::ClassType) {
    error(nc.range,
          "v0 lowering constructs class types only (expected: 'new "
          "ClassName(...)')");
    return Val{};
  }
  const std::string owner = internalizeClassType(
      static_cast<const ast::ClassType&>(*nc.type), cls().internalName);
  if (owner != cls().internalName) {
    error(nc.range,
          "construction of '" + owner +
              "' is not supported by v0 lowering: v0 constructs the program "
              "class only (expected: new " + cls().simpleName +
              "(...); library construction arrives with the loader)");
    return Val{};
  }
  const MethodEntry* ctor = cls().findUnique("<init>", nc.arguments.size());
  if (ctor == nullptr) {
    error(nc.range,
          "no unique constructor of " + cls().internalName + " with " +
              std::to_string(nc.arguments.size()) +
              " argument(s) (expected: a declared constructor; varargs "
              "call sites are not supported by v0)");
    return Val{};
  }
  const std::uint32_t cpC = cpClass(cls().internalName);
  const std::uint16_t obj = newReg();
  b().emitRegCp(Op::New, obj, cpC);
  std::vector<const ast::Expr*> args;
  for (const ast::Ptr<ast::Expr>& a : nc.arguments) {
    args.push_back(a.get());
  }
  (void)emitCallSeq(Op::Invokespecial, obj, args, &ctor->params,
                    cls().internalName, "<init>", nc.range,
                    JType::prim(JK::Void));
  if (poisoned()) {
    return Val{};
  }
  return Val{obj, JType::ref(cls().internalName)};
}

void MethodLowerer::lowerCtorDelegation(const ast::ConstructorInvocation& ci) {
  const MethodEntry* ctor = cls().findUnique("<init>", ci.arguments.size());
  if (ctor == nullptr) {
    error(ci.range,
          "no unique constructor of " + cls().internalName + " with " +
              std::to_string(ci.arguments.size()) +
              " argument(s) to delegate to (expected: a declared "
              "constructor)");
    return;
  }
  const std::uint16_t thiz = loadThis(ci.range);
  std::vector<const ast::Expr*> args;
  for (const ast::Ptr<ast::Expr>& a : ci.arguments) {
    args.push_back(a.get());
  }
  (void)emitCallSeq(Op::Invokespecial, thiz, args, &ctor->params,
                    cls().internalName, "<init>", ci.range,
                    JType::prim(JK::Void));
}

Val MethodLowerer::lowerArrayInitializer(const ast::ArrayInitializer& ai,
                                         const JType& elem, SourceRange at) {
  const JType arr = arrayOf(elem);
  const std::uint16_t len = newReg();
  b().emitRegImm(Op::Iconst, len,
                 static_cast<std::uint32_t>(ai.values.size()));
  const std::uint16_t r = newReg();
  if (elem.isRefLike()) {
    b().emitRegRegCp(Op::AnewArray, r, len, cpClassOf(elem));
  } else {
    b().emitRegRegImm(Op::NewArray, r, len, atypeOf(elem));
  }
  const std::uint16_t idx = newReg();
  for (std::size_t i = 0; i < ai.values.size(); ++i) {
    const ast::Expr& v = *ai.values[i];
    const IntFold f = foldInt(v);
    Val ev = lowerExpr(v);
    if (!ev.ok()) {
      return Val{};
    }
    if (ev.type.isVoid()) {
      error(v.range, "array initializer element has no value (expected: a "
                     "non-void expression)");
      return Val{};
    }
    b().emitRegImm(Op::Iconst, idx, static_cast<std::uint32_t>(i));
    const std::optional<std::int64_t> fold =
        f.ok ? std::optional<std::int64_t>(f.v) : std::nullopt;
    const std::uint16_t cv =
        emitAssignConvert(ev.reg, ev.type, elem, fold, v.range);
    b().emitRegRegReg(astoreOpOf(elem), cv, r, idx);
  }
  (void)at;
  return Val{r, arr};
}

}  // namespace b2::frontend::lower
