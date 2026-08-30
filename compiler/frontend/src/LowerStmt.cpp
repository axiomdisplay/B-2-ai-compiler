// B-2 Frontend - AST -> RBC lowering: statements.
//
// This TU owns control flow and declarations: blocks and scoping (mirroring
// the planner's walk exactly), local declarations (with `var` slots from
// the plan), if/while/do-while/basic-for/enhanced-for with safepoint polls
// on every backedge target, labeled break/continue, switch statements
// (colon form with fall-through + rule form), try/catch/finally with the
// catch-variable-as-local discipline, synchronized blocks, and abrupt
// transfers that run pending finally chains first (innermost-first).

#include "LowerImpl.h"

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace b2::frontend::lower {
namespace {

using rbc::Op;
using K = ast::NodeKind;

// Is this statement a loop or a switch (the only legal break targets)?
bool isBreakable(const ast::Stmt& s) {
  return s.kind == K::While || s.kind == K::DoWhile || s.kind == K::BasicFor ||
         s.kind == K::EnhancedFor || s.kind == K::SwitchStatement;
}

// Return opcode for a family.
[[nodiscard]] Op returnOpOf(const JType& t) {
  switch (t.kind) {
  case JK::Long: return Op::Lreturn;
  case JK::Float: return Op::Freturn;
  case JK::Double: return Op::Dreturn;
  case JK::Ref: case JK::Null: return Op::Areturn;
  default: return Op::Ireturn;
  }
}

}  // namespace

void MethodLowerer::lowerBlock(const ast::Block& blk) {
  pushScope();
  for (const ast::Ptr<ast::Stmt>& s : blk.statements) {
    lowerStmt(*s);
    if (poisoned()) {
      return;
    }
  }
  popScope();
}

void MethodLowerer::lowerSub(const ast::Stmt& s) {
  // Mirror of the planner's walkSub: blocks manage their own scope; any
  // other single statement runs in a fresh one.
  if (s.kind == K::Block) {
    lowerStmt(s);
    return;
  }
  pushScope();
  lowerStmt(s);
  popScope();
}

void MethodLowerer::lowerStmt(const ast::Stmt& s) {
  switch (s.kind) {
  case K::Block:
    lowerBlock(static_cast<const ast::Block&>(s));
    return;

  case K::EmptyStatement:
    return;

  case K::LocalVariableDeclaration: {
    const auto& d = static_cast<const ast::LocalVariableDeclaration&>(s);
    if (d.mods != ast::ModNone && d.mods != ast::ModFinal) {
      error(s.range,
            "local variable modifiers other than 'final' are not supported "
            "by v0 lowering (expected: at most 'final')");
      return;
    }
    for (const ast::VariableDeclarator& dec : d.declarators) {
      const auto it = plan_->varSlots.find(&dec);
      if (it == plan_->varSlots.end()) {
        error(dec.range,
              "internal lowering error: no slot planned for '" + dec.name +
                  "' (this is a bug in the lowering; please report it)");
        return;
      }
      const std::uint32_t slot = it->second;
      JType type;
      for (const SlotInfo& si : plan_->slots) {
        if (si.slot == slot) {
          type = si.type;
          break;
        }
      }
      if (type.isErr()) {
        error(dec.range,
              "internal lowering error: no type planned for '" + dec.name +
                  "' (this is a bug in the lowering; please report it)");
        return;
      }
      if (dec.initializer != nullptr) {
        if (dec.initializer->kind == K::ArrayInitializer) {
          if (!type.isArray()) {
            error(dec.initializer->range,
                  "array initializer requires an array-typed variable, found " +
                      type.display() + " (expected: 'T[] name = {...}')");
            return;
          }
          const Val v = lowerArrayInitializer(
              static_cast<const ast::ArrayInitializer&>(*dec.initializer),
              type.element(), dec.initializer->range);
          if (!v.ok()) {
            return;
          }
          emitStore(v.reg, slot, type);
        } else {
          const Val v = lowerExprTo(*dec.initializer, type,
                                    "initializer for '" + dec.name + "'",
                                    dec.initializer->range);
          if (!v.ok()) {
            return;
          }
          emitStore(v.reg, slot, type);
        }
      }
      bindLocal(dec.name, slot, type);
    }
    return;
  }

  case K::ExpressionStatement: {
    const auto& es = static_cast<const ast::ExpressionStatement&>(s);
    (void)lowerExpr(*es.expression);
    return;
  }

  case K::If: {
    const auto& i = static_cast<const ast::If&>(s);
    const Val cond = lowerBoolExpr(*i.condition);
    if (!cond.ok()) {
      return;
    }
    if (i.elseStmt == nullptr) {
      const auto Lend = b().newLabel();
      b().emitRegBranch(Op::Ifeq, cond.reg, Lend);
      lowerSub(*i.thenStmt);
      b().bind(Lend);
      return;
    }
    const auto Lelse = b().newLabel();
    const auto Lend = b().newLabel();
    b().emitRegBranch(Op::Ifeq, cond.reg, Lelse);
    lowerSub(*i.thenStmt);
    b().emitBranch(Op::Goto, Lend);
    b().bind(Lelse);
    lowerSub(*i.elseStmt);
    b().bind(Lend);
    return;
  }

  case K::While: {
    const auto& w = static_cast<const ast::While&>(s);
    lowerWhile(w, {});
    return;
  }

  case K::DoWhile: {
    const auto& w = static_cast<const ast::DoWhile&>(s);
    lowerDoWhile(w, {});
    return;
  }

  case K::BasicFor: {
    const auto& f = static_cast<const ast::BasicFor&>(s);
    lowerBasicFor(f, {});
    return;
  }

  case K::EnhancedFor: {
    const auto& f = static_cast<const ast::EnhancedFor&>(s);
    lowerEnhancedFor(f, {});
    return;
  }

  case K::Return: {
    const auto& r = static_cast<const ast::Return&>(s);
    if (result_.isVoid()) {
      if (r.expression != nullptr) {
        error(r.range,
              "cannot return a value from a void method (expected: 'return;' "
              "or a void method)");
        return;
      }
      emitFinallyChain();
      b().emit(Op::Return);
      return;
    }
    if (r.expression == nullptr) {
      error(r.range,
            "missing return value (expected: 'return <expression>;')");
      return;
    }
    const Val v = lowerExprTo(*r.expression, result_, "return value",
                              r.expression->range);
    if (!v.ok()) {
      return;
    }
    emitFinallyChain();
    b().emitReg(returnOpOf(v.type), v.reg);
    return;
  }

  case K::Throw: {
    const auto& t = static_cast<const ast::Throw&>(s);
    const Val v = lowerExpr(*t.expression);
    if (!v.ok()) {
      return;
    }
    if (!v.type.isRefLike()) {
      error(t.expression->range,
            "throw requires a reference operand, found " + v.type.display() +
                " (expected: a Throwable-typed expression)");
      return;
    }
    if (v.type.kind == JK::Null) {
      // Java: throw null; raises NPE at the throw site - athrow traps it.
    }
    // NO emitFinallyChain here: an athrow inside a pending finally's
    // protected region is caught by that region's catch-all handler, which
    // runs the finally and rethrows (javac's model). Inlining the chain
    // here would run the finally TWICE. Only non-exception transfers
    // (return/break/continue) inline pending finallies.
    b().emitReg(Op::Athrow, v.reg);
    return;
  }

  case K::BreakStatement: {
    const auto& bs = static_cast<const ast::BreakStatement&>(s);
    const LoopCtx* target =
        findBreakable(bs.hasLabel ? bs.label : std::string_view{});
    if (target == nullptr) {
      error(s.range,
            std::string("'break'") +
                (bs.hasLabel ? " '" + bs.label + "'" : "") +
                " is not inside a loop or switch (expected: an enclosing "
                "breakable statement)");
      return;
    }
    emitFinallyChain();
    b().emitBranch(Op::Goto, target->breakT);
    return;
  }

  case K::ContinueStatement: {
    const auto& cs = static_cast<const ast::ContinueStatement&>(s);
    const LoopCtx* target =
        findLoop(cs.hasLabel ? cs.label : std::string_view{});
    if (target == nullptr) {
      error(s.range,
            std::string("'continue'") +
                (cs.hasLabel ? " '" + cs.label + "'" : "") +
                " is not inside a loop (expected: an enclosing loop)");
      return;
    }
    emitFinallyChain();
    b().emitBranch(Op::Goto, target->contT);
    return;
  }

  case K::LabeledStatement: {
    const auto& l = static_cast<const ast::LabeledStatement&>(s);
    if (!isBreakable(*l.statement)) {
      error(s.range,
            "labels are supported only on loops and switch statements in v0 "
            "(expected: a labeled loop/switch; labeled blocks arrive with "
            "the binding stage)");
      return;
    }
    switch (l.statement->kind) {
    case K::While:
      lowerWhile(static_cast<const ast::While&>(*l.statement), l.label);
      return;
    case K::DoWhile:
      lowerDoWhile(static_cast<const ast::DoWhile&>(*l.statement), l.label);
      return;
    case K::BasicFor:
      lowerBasicFor(static_cast<const ast::BasicFor&>(*l.statement), l.label);
      return;
    case K::EnhancedFor:
      lowerEnhancedFor(static_cast<const ast::EnhancedFor&>(*l.statement),
                       l.label);
      return;
    case K::SwitchStatement:
      lowerSwitch(static_cast<const ast::SwitchStatement&>(*l.statement),
                  l.label);
      return;
    default:
      return;
    }
  }

  case K::SwitchStatement: {
    const auto& sw = static_cast<const ast::SwitchStatement&>(s);
    lowerSwitch(sw, {});
    return;
  }

  case K::Try: {
    const auto& t = static_cast<const ast::Try&>(s);
    lowerTry(t);
    return;
  }

  case K::Synchronized: {
    const auto& sy = static_cast<const ast::Synchronized&>(s);
    const Val lock = lowerExpr(*sy.lock);
    if (!lock.ok()) {
      return;
    }
    if (!lock.type.isRefLike() || lock.type.kind == JK::Null) {
      error(sy.lock->range,
            "synchronized requires a reference lock, found " +
                lock.type.display() + " (expected: a non-null reference)");
      return;
    }
    b().emitReg(Op::Monitorenter, lock.reg);
    lowerBlock(*sy.body);
    if (poisoned()) {
      return;
    }
    b().emitReg(Op::Monitorexit, lock.reg);
    return;
  }

  case K::Yield:
    error(s.range,
          "'yield' requires a switch expression, which v0 lowering does not "
          "support (expected: a switch statement; switch expressions arrive "
          "with the binding stage)");
    return;

  case K::AssertStatement:
    error(s.range,
          "'assert' is not supported by v0 lowering: AssertionError "
          "construction needs runtime support (expected: explicit checks; "
          "assert arrives with the runtime builtins RFC)");
    return;

  case K::LocalTypeDeclaration:
    error(s.range,
          "local class/interface/enum/record declarations are not supported "
          "by v0 lowering (expected: top-level classes only; local types "
          "arrive with the class model)");
    return;

  default:
    error(s.range,
          "statement form is not supported by v0 lowering (expected: a "
          "supported statement; this construct arrives with a later "
          "lowering version)");
    return;
  }
}

// --------------------------------------------------------------- loops ----

void MethodLowerer::lowerWhile(const ast::While& w, std::string label) {
  const auto Lhead = b().newLabel();
  const auto Lend = b().newLabel();
  b().bind(Lhead);
  b().emit(Op::SafepointPoll);  // backedge target polls (T0/T1 contract)
  const Val cond = lowerBoolExpr(*w.condition);
  if (!cond.ok()) {
    return;
  }
  b().emitRegBranch(Op::Ifeq, cond.reg, Lend);
  pushLoop(Lend, Lhead, std::move(label));
  lowerSub(*w.body);
  popLoop();
  if (poisoned()) {
    return;
  }
  b().emitBranch(Op::Goto, Lhead);
  b().bind(Lend);
}

void MethodLowerer::lowerDoWhile(const ast::DoWhile& w, std::string label) {
  const auto Lhead = b().newLabel();
  const auto Lcont = b().newLabel();
  const auto Lend = b().newLabel();
  b().bind(Lhead);
  b().emit(Op::SafepointPoll);
  pushLoop(Lend, Lcont, std::move(label));
  lowerSub(*w.body);
  popLoop();
  if (poisoned()) {
    return;
  }
  b().bind(Lcont);
  const Val cond = lowerBoolExpr(*w.condition);
  if (!cond.ok()) {
    return;
  }
  b().emitRegBranch(Op::Ifne, cond.reg, Lhead);
  b().bind(Lend);
}

void MethodLowerer::lowerBasicFor(const ast::BasicFor& f, std::string label) {
  pushScope();
  for (const ast::Ptr<ast::Stmt>& init : f.init) {
    lowerStmt(*init);
    if (poisoned()) {
      popScope();
      return;
    }
  }
  const auto Lcond = b().newLabel();
  const auto Lupd = b().newLabel();
  const auto Lend = b().newLabel();
  b().bind(Lcond);
  b().emit(Op::SafepointPoll);
  if (f.condition != nullptr) {
    const Val cond = lowerBoolExpr(*f.condition);
    if (!cond.ok()) {
      popScope();
      return;
    }
    b().emitRegBranch(Op::Ifeq, cond.reg, Lend);
  }
  pushLoop(Lend, Lupd, std::move(label));
  lowerSub(*f.body);
  popLoop();
  if (poisoned()) {
    popScope();
    return;
  }
  b().bind(Lupd);
  for (const ast::Ptr<ast::Expr>& u : f.update) {
    (void)lowerExpr(*u);
    if (poisoned()) {
      popScope();
      return;
    }
  }
  b().emitBranch(Op::Goto, Lcond);
  b().bind(Lend);
  popScope();
}

void MethodLowerer::lowerEnhancedFor(const ast::EnhancedFor& f,
                                     std::string label) {
  const Val iterable = lowerExpr(*f.iterable);
  if (!iterable.ok()) {
    return;
  }
  if (!iterable.type.isArray()) {
    error(f.iterable->range,
          "enhanced-for iterates arrays only in v0, found " +
              iterable.type.display() +
              " (expected: an array; Iterable support arrives with the "
              "class model and interfaces)");
    return;
  }
  const JType elem = iterable.type.element();
  const auto idxIt = plan_->forIndexSlots.find(&f);
  const auto varIt = plan_->forVarSlots.find(&f);
  if (idxIt == plan_->forIndexSlots.end() || varIt == plan_->forVarSlots.end()) {
    error(f.range,
          "internal lowering error: enhanced-for slots not planned (this is "
          "a bug in the lowering; please report it)");
    return;
  }
  const std::uint32_t idxSlot = idxIt->second;
  const std::uint32_t varSlot = varIt->second;
  JType varType = JType::prim(JK::Int);
  for (const SlotInfo& si : plan_->slots) {
    if (si.slot == varSlot) {
      varType = si.type;
      break;
    }
  }

  const std::uint16_t zero = newReg();
  b().emitRegImm(Op::Iconst, zero, 0);
  emitStore(zero, idxSlot, JType::prim(JK::Int));

  const auto Lhead = b().newLabel();
  const auto Lcont = b().newLabel();
  const auto Lend = b().newLabel();
  b().bind(Lhead);
  b().emit(Op::SafepointPoll);
  const std::uint16_t len = newReg();
  b().emitRegReg(Op::Arraylength, len, iterable.reg);
  const std::uint16_t idx = emitLoad(idxSlot, JType::prim(JK::Int));
  b().emitRegRegBranch(Op::IfIcmpge, idx, len, Lend);
  const std::uint16_t elemReg = newReg();
  b().emitRegRegReg(aloadOpOf(elem), elemReg, iterable.reg, idx);
  const std::uint16_t conv =
      emitConvert(elemReg, elem, varType, f.range);
  emitStore(conv, varSlot, varType);

  pushLoop(Lend, Lcont, std::move(label));
  pushScope();
  bindLocal(f.name, varSlot, varType);
  lowerSub(*f.body);
  popScope();
  popLoop();
  if (poisoned()) {
    return;
  }
  b().bind(Lcont);
  const std::uint16_t i2 = emitLoad(idxSlot, JType::prim(JK::Int));
  b().emitRegImm(Op::Iinc, i2, 1);
  emitStore(i2, idxSlot, JType::prim(JK::Int));
  b().emitBranch(Op::Goto, Lhead);
  b().bind(Lend);
}

// --------------------------------------------------------------- switch ----

void MethodLowerer::lowerSwitch(const ast::SwitchStatement& sw,
                                std::string label) {
  const Val sel = lowerExpr(*sw.selector);
  if (!sel.ok()) {
    return;
  }
  if (!sel.type.isIntFamily() || sel.type.kind == JK::Bool) {
    error(sw.selector->range,
          "switch selector must be char, byte, short, or int in v0, found " +
              sel.type.display() +
              " (expected: an int-family selector; String/enum switches "
              "arrive with the runtime class model)");
    return;
  }
  const std::uint16_t selReg = newReg();
  emitMove(selReg, sel.reg, JType::prim(JK::Int));

  // Collect matches (one per non-default label) and default position.
  struct CaseInfo {
    const ast::SwitchCase* c = nullptr;
    rbc::RbcBuilder::Label label{};
    bool isDefault = false;
  };
  std::vector<CaseInfo> cases;
  std::vector<std::int32_t> matches;
  std::vector<rbc::RbcBuilder::Label> targets;
  std::unordered_set<std::int64_t> seen;
  std::size_t defaultIdx = static_cast<std::size_t>(-1);

  for (const ast::SwitchCase& c : sw.cases) {
    if (c.guard != nullptr) {
      error(c.guard->range,
            "'when' guards in switch are not supported by v0 lowering "
            "(expected: plain case labels; guards arrive with the binding "
            "stage)");
      return;
    }
    CaseInfo ci;
    ci.c = &c;
    ci.label = b().newLabel();
    for (const ast::SwitchLabel& lab : c.labels) {
      if (lab.isDefault) {
        ci.isDefault = true;
        continue;
      }
      if (lab.isNull) {
        error(lab.range,
              "'case null' requires a reference selector, which v0 switch "
              "does not support (expected: int-family case labels)");
        return;
      }
      if (lab.pattern != nullptr) {
        error(lab.range,
              "pattern case labels are not supported by v0 lowering "
              "(expected: constant case labels; patterns arrive with the "
              "binding stage)");
        return;
      }
      if (lab.constant == nullptr) {
        error(lab.range,
              "case label must be a compile-time int constant in v0 "
              "(expected: a literal or constant expression)");
        return;
      }
      const IntFold f = foldInt(*lab.constant);
      if (!f.ok || f.isBool) {
        error(lab.constant->range,
              "case label must be a compile-time int constant, found a "
              "non-constant or non-int expression (expected: a constant "
              "like 1, 'a', or -2)");
        return;
      }
      if (f.v < -2147483648LL || f.v > 2147483647LL) {
        error(lab.constant->range,
              "case label is outside int range (expected: a value in "
              "[-2147483648, 2147483647])");
        return;
      }
      if (!seen.insert(f.v).second) {
        error(lab.constant->range,
              "duplicate case label " + std::to_string(f.v) +
                  " (expected: unique case constants)");
        return;
      }
      matches.push_back(static_cast<std::int32_t>(f.v));
      targets.push_back(ci.label);
    }
    if (ci.isDefault) {
      defaultIdx = cases.size();
    }
    cases.push_back(ci);
  }

  // The instruction right after the switch is the default entry (P7).
  const auto Ldefault = b().newLabel();
  b().emitSwitch(Op::Lookupswitch, selReg, matches, targets);
  const auto Lafter = b().newLabel();
  b().emitBranch(Op::Goto,
                 defaultIdx == static_cast<std::size_t>(-1) ? Lafter
                                                            : Ldefault);

  pushLoop(Lafter, {}, std::move(label), false);  // break target only
  pushScope();  // one scope for the whole switch block (JLS 14.11)
  for (std::size_t i = 0; i < cases.size(); ++i) {
    const CaseInfo& ci = cases[i];
    b().bind(ci.label);
    if (ci.isDefault) {
      b().bind(Ldefault);
    }
    const ast::SwitchCase& c = *ci.c;
    if (c.isRule) {
      // Rule form: no fall-through; the body is Expr | Block | Throw.
      if (c.ruleBody != nullptr) {
        if (c.ruleBody->kind == K::Block) {
          lowerBlock(static_cast<const ast::Block&>(*c.ruleBody));
        } else if (c.ruleBody->kind == K::Throw) {
          lowerStmt(static_cast<const ast::Throw&>(*c.ruleBody));
        } else {
          (void)lowerExpr(static_cast<const ast::Expr&>(*c.ruleBody));
        }
      }
      if (poisoned()) {
        popScope();
        popLoop();
        return;
      }
      b().emitBranch(Op::Goto, Lafter);
    } else {
      // Colon form: fall-through semantics.
      for (const ast::Ptr<ast::Stmt>& st : c.statements) {
        lowerStmt(*st);
        if (poisoned()) {
          popScope();
          popLoop();
          return;
        }
      }
    }
  }
  popScope();
  popLoop();
  b().bind(Lafter);
}

// ------------------------------------------------------------------ try ----

void MethodLowerer::lowerTry(const ast::Try& t) {
  if (!t.resources.empty()) {
    error(t.range,
          "try-with-resources is not supported by v0 lowering: AutoCloseable "
          "needs interfaces (expected: try/catch/finally; try-with-resources "
          "arrives with the class model)");
    return;
  }

  // The finally stays pending while the try AND catch bodies lower, so
  // every abrupt transfer inside them runs it first (innermost-first).
  if (t.finallyBlock != nullptr) {
    pushFinally(t.finallyBlock.get());
  }

  const auto Ltry = b().newLabel();
  const auto LtryEnd = b().newLabel();
  const auto Lafter = b().newLabel();
  b().bind(Ltry);
  const std::uint32_t startIdx = b().here();
  lowerBlock(*t.block);
  if (poisoned()) {
    return;
  }
  // An empty protected range is a verification error (SS5.2): keep the
  // range non-empty with a nop when the try body emitted nothing.
  if (b().here() == startIdx) {
    b().emit(Op::Nop);
  }
  b().bind(LtryEnd);
  const std::uint32_t endIdx = b().here();
  // Normal completion MUST skip the catch bodies: it jumps to the finally's
  // normal path (or past the catches when there is no finally).
  b().emitBranch(Op::Goto, Lafter);

  // Catch bodies. The handler entry index is the position bound BEFORE the
  // astore/body; one handler entry per union member, all to the same entry.
  struct PendingHandler {
    std::uint32_t handlerIdx = 0;
    std::int32_t catchCp = -1;
  };
  std::vector<PendingHandler> handlers;

  for (const ast::CatchClause& c : t.catches) {
    if (c.type == nullptr) {
      error(c.range, "catch clause requires a type (expected: 'catch (Type "
                     "name) { ... }')");
      return;
    }
    std::vector<std::string> members;
    if (c.type->kind == K::UnionType) {
      for (const ast::Ptr<ast::Type>& mt :
           static_cast<const ast::UnionType&>(*c.type).types) {
        if (mt->kind != K::ClassType) {
          error(mt->range,
                "multi-catch members must be class types (expected: e.g. "
                "'catch (A | B e)')");
          return;
        }
        members.push_back(internalizeClassType(
            static_cast<const ast::ClassType&>(*mt), cls().internalName));
      }
    } else if (c.type->kind == K::ClassType) {
      members.push_back(internalizeClassType(
          static_cast<const ast::ClassType&>(*c.type), cls().internalName));
    } else {
      error(c.type->range,
            "catch type must be a class type in v0 (expected: e.g. "
            "'catch (java.lang.Exception e)')");
      return;
    }

    const auto Lh = b().newLabel();
    b().bind(Lh);
    const std::uint32_t hIdx = b().here();
    std::uint32_t catchVarSlot = 0xFFFFFFFFu;
    if (!c.name.empty()) {
      const auto it = plan_->catchVarSlots.find(&c);
      if (it == plan_->catchVarSlots.end()) {
        error(c.range,
              "internal lowering error: catch variable slot not planned "
              "(this is a bug in the lowering; please report it)");
        return;
      }
      catchVarSlot = it->second;
      // Exception delivery: r0 -> the clause's pre-typed Ref slot.
      b().emitSlotReg(Op::Astore, 0, catchVarSlot);
    }
    pushScope();
    if (!c.name.empty()) {
      JType ct = JType::ref(members.front());
      bindLocal(c.name, catchVarSlot, ct);
    }
    lowerBlock(*c.body);
    popScope();
    if (poisoned()) {
      return;
    }
    for (const std::string& m : members) {
      handlers.push_back(
          PendingHandler{hIdx, static_cast<std::int32_t>(cpClass(m))});
    }
  }

  const std::uint32_t catchEnd = b().here();  // one past the catch bodies

  if (t.finallyBlock == nullptr) {
    for (const PendingHandler& h : handlers) {
      b().addHandler(startIdx, endIdx, h.handlerIdx, h.catchCp);
    }
    b().bind(Lafter);
    return;
  }

  // Normal completion path (and catch-body fall-through): the finally runs
  // exactly once, then control continues after the statement.
  popFinally();  // transfers inside the finally itself see only outer ones
  b().bind(Lafter);
  lowerBlock(*t.finallyBlock);
  if (poisoned()) {
    return;
  }
  const auto Ldone = b().newLabel();
  b().emitBranch(Op::Goto, Ldone);

  // Exceptional completion: catch-all handler runs the finally, rethrows.
  const auto Lre = b().newLabel();
  const auto it = plan_->rethrowSlots.find(&t);
  if (it == plan_->rethrowSlots.end()) {
    error(t.range,
          "internal lowering error: rethrow slot not planned (this is a bug "
          "in the lowering; please report it)");
    return;
  }
  b().bind(Lre);
  const std::uint32_t reIdx = b().here();
  b().emitSlotReg(Op::Astore, 0, it->second);
  lowerBlock(*t.finallyBlock);
  if (poisoned()) {
    return;
  }
  const std::uint16_t rex =
      emitLoad(it->second, JType::ref("java/lang/Object"));
  b().emitReg(Op::Athrow, rex);

  for (const PendingHandler& h : handlers) {
    b().addHandler(startIdx, endIdx, h.handlerIdx, h.catchCp);
  }
  // The catch-all covers the catch bodies too: an exception escaping a
  // catch clause must still run the finally (JLS 14.20.2).
  b().addHandler(startIdx, catchEnd, reIdx, -1);
  b().bind(Ldone);
}

}  // namespace b2::frontend::lower
