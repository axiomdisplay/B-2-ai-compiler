#pragma once
// B-2 Frontend - AST -> RBC lowering internals (implementation detail).
//
// WHY THIS FILE EXISTS:
// The public contract is Lower.h (one function). The machinery - the
// lowering static-type system, the class model, the slot planner, and the
// per-method lowerer - is shared by the Lower*.cpp translation units and
// deliberately NOT in include/: no other subsystem may depend on it.
//
// ARCHITECTURE (three passes per method, two per class):
//   1. ClassModel pass: fields/methods/ctors tables from declarations.
//   2. SlotPlanner pass (per method): pre-order walk assigning one local
//      slot per declared variable (and one hidden Int slot per enhanced-
//      for), computing every slot's JType, and inferring `var` types. The
//      planner is the AUTHORITY: the emission pass looks slots up by AST
//      node, so both passes cannot disagree.
//   3. MethodLowerer pass (per method): prologue default-init for every
//      non-parameter slot (fixed family from pc 0 - protected-range
//      stability by construction), then body emission.
//
// The JType system is a static-typing overlay for opcode selection ONLY
// (which *add/*load family, which conversions). It is NOT the future
// binding stage (Rule 80: static types are not runtime proof); its checks
// are exactly deep enough to emit verifiable RBC and refuse what v0 cannot
// compile honestly.
//
// SCOPE WALK PARITY: planMethod() and MethodLowerer::lowerStmt() must
// traverse statement structure IDENTICALLY (same push/pop of scopes, same
// slot lookups). The planner assigns slots by AST node identity; the
// emitter reads them back by the same identity. Changing one walk without
// the other is a bug.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "b2/frontend/Diagnostics.h"
#include "b2/frontend/Lower.h"
#include "b2/frontend/SourceManager.h"
#include "b2/frontend/ast/Ast.h"
#include "b2/rbc/Opcode.h"
#include "b2/rbc/Rbc.h"
#include "b2/rbc/RbcBuilder.h"

namespace b2::frontend::lower {

// ---------------------------------------------------------------- JType ----

enum class JK : std::uint8_t {
  Err = 0, Void, Bool, Byte, Short, Char, Int, Long, Float, Double,
  Null, Ref,
};

// A lowering static type. For Ref, `name` is an internal class name
// ("java/lang/String", "pkg/Foo") or a full array descriptor ("[I",
// "[Ljava/lang/String;"). Everything else ignores `name`.
struct JType {
  JK kind = JK::Err;
  std::string name;

  [[nodiscard]] static JType prim(JK k) { return JType{k, {}}; }
  [[nodiscard]] static JType ref(std::string internalName) {
    return JType{JK::Ref, std::move(internalName)};
  }
  [[nodiscard]] static JType err() { return JType{}; }

  [[nodiscard]] bool isErr() const { return kind == JK::Err; }
  [[nodiscard]] bool isNumeric() const {
    return kind == JK::Bool || kind == JK::Byte || kind == JK::Short ||
           kind == JK::Char || kind == JK::Int || kind == JK::Long ||
           kind == JK::Float || kind == JK::Double;
  }
  // Bool/Byte/Short/Char/Int all live in Int registers/slots.
  [[nodiscard]] bool isIntFamily() const {
    return kind == JK::Bool || kind == JK::Byte || kind == JK::Short ||
           kind == JK::Char || kind == JK::Int;
  }
  [[nodiscard]] bool isRefLike() const {
    return kind == JK::Ref || kind == JK::Null;
  }
  [[nodiscard]] bool isArray() const {
    return kind == JK::Ref && !name.empty() && name[0] == '[';
  }
  [[nodiscard]] bool isString() const {
    return kind == JK::Ref && name == "java/lang/String";
  }
  [[nodiscard]] bool isVoid() const { return kind == JK::Void; }

  // The RBC verification type of values of this JType.
  [[nodiscard]] rbc::RType rtype() const noexcept {
    switch (kind) {
    case JK::Long: return rbc::RType::Long;
    case JK::Float: return rbc::RType::Float;
    case JK::Double: return rbc::RType::Double;
    case JK::Null: return rbc::RType::Null;
    case JK::Ref: return rbc::RType::Ref;
    case JK::Bool: case JK::Byte: case JK::Short: case JK::Char: case JK::Int:
      return rbc::RType::Int;
    case JK::Err: case JK::Void: break;
    }
    return rbc::RType::Bottom;
  }

  // Human-ish name for diagnostics ("int", "String", "int[]").
  [[nodiscard]] std::string display() const;

  // JVM descriptor ("I", "J", "Lpkg/Foo;", "[I", "V", ...).
  [[nodiscard]] std::string descriptor() const;

  // Element type of an array type (Err when not an array).
  [[nodiscard]] JType element() const;
};

// arrayOf(t) = array-of-t JType.
[[nodiscard]] JType arrayOf(const JType& elem);

// JLS 5.6.2 binary numeric promotion of two operand types.
[[nodiscard]] JType promote(const JType& a, const JType& b);

// Can a value of `from` reach `to` by assignment conversion (identity or
// widening)? Narrowing returns false - callers decide between
// constant-fold+range-check and a diagnostic.
[[nodiscard]] bool widensTo(const JType& from, const JType& to);

// Internal class name for a ClassType AST node (dots -> slashes, java.lang
// table, own-package default; type arguments erased). `ownInternal` is the
// program class's internal name for self references ("" disables).
[[nodiscard]] std::string internalizeClassType(const ast::ClassType& ct,
                                               std::string_view ownInternal);

// JType of an AST type node (Err + diagnostic on unsupported shapes).
[[nodiscard]] JType jtypeOf(const ast::Type& t, std::string_view ownInternal,
                            DiagnosticEngine& diags, const SourceManager& sm);

// Range bounds for narrowing range checks (0/0 for non-narrowable).
[[nodiscard]] std::int64_t loOf(const JType& t) noexcept;
[[nodiscard]] std::int64_t hiOf(const JType& t) noexcept;

// ------------------------------------------------------------- constants ----

// Fold an int-family constant expression (literals, unary -, +, ~, !, and
// binary + - * / % << >> >>> & | ^ over folded operands). out is the value
// in int64 range; `isBool` marks boolean results (still Int-valued).
struct IntFold {
  bool ok = false;
  std::int64_t v = 0;
  bool isBool = false;
};
[[nodiscard]] IntFold foldInt(const ast::Expr& e);

// ----------------------------------------------------------- ClassModel ----

struct FieldEntry {
  std::string name;
  JType type;
  bool isStatic = false;
  bool hasInit = false;
  SourceRange range{};
};

struct MethodEntry {
  std::string name;         // "<init>", "<clinit>", or simple name
  std::string descriptor;
  std::vector<JType> params;  // excludes the receiver
  JType result;               // Void for ctors/clinit
  bool isStatic = false;
  bool isCtor = false;
  bool isClinit = false;
  bool hasBody = false;
  const ast::MethodDeclaration* mdecl = nullptr;
  const ast::ConstructorDeclaration* cdecl = nullptr;
};

// One top-level class's declaration-level view: what the name-resolution
// and call-lowering queries run against.
struct ClassModel {
  std::string internalName;  // "pkg/Foo"
  std::string simpleName;    // "Foo"
  std::vector<FieldEntry> fields;
  std::vector<MethodEntry> methods;  // declaration order (ctors + methods)

  [[nodiscard]] const FieldEntry* findField(std::string_view n) const;
  [[nodiscard]] const MethodEntry* findUnique(std::string_view n,
                                              std::size_t argc) const;
};

// Builds the model from one ClassDeclaration (diagnosing members v0 cannot
// ever compile: abstract/native methods, malformed types). Never throws.
void buildClassModel(const ast::ClassDeclaration& cd,
                     const std::string& internalName,
                     DiagnosticEngine& diags, const SourceManager& sm,
                     ClassModel& out);

// ------------------------------------------------------------ SlotPlanner ----

// A planned local slot.
struct SlotInfo {
  std::uint32_t slot = 0;
  JType type;
};

// The plan for one method body: slot assignments (the emission pass reads
// them by AST node identity) + the full slot list for the prologue.
struct MethodPlan {
  // Slots [0, numParams) are parameters (after the receiver slot for
  // instance methods); [numParams, ...) are declared/hidden slots that the
  // prologue default-initializes.
  std::uint32_t numParams = 0;
  std::uint32_t numLocals = 0;
  std::vector<SlotInfo> slots;  // all slots, slot-index order
  // VariableDeclarator* -> slot
  std::unordered_map<const void*, std::uint32_t> varSlots;
  std::unordered_map<const ast::EnhancedFor*, std::uint32_t> forIndexSlots;
  std::unordered_map<const ast::EnhancedFor*, std::uint32_t> forVarSlots;
  // Catch variables and finally-rethrow temporaries are LOCALS (not
  // registers): their slots are pre-typed Ref in the prologue via ldc, so
  // handler-entry astores never change a slot's type inside any enclosing
  // protected range (rbc_spec SS5.3 stability by construction) and the
  // value survives nested handler entries.
  std::unordered_map<const ast::CatchClause*, std::uint32_t> catchVarSlots;
  std::unordered_map<const ast::Try*, std::uint32_t> rethrowSlots;
  bool ok = false;
};

// Plans one method body. `isStatic` controls whether l0 is the receiver.
// `visibleLocals` out-param accumulates nothing (inference is local).
void planMethod(const std::vector<ast::Ptr<ast::Stmt>>& body, bool isStatic,
                const std::vector<JType>& paramTypes,
                const ClassModel& cls, DiagnosticEngine& diags,
                const SourceManager& sm, MethodPlan& out);

// Infers the type of a `var` initializer (v0 subset; diagnose + Err when
// the initializer's type is not statically known). `visibleLocals` is the
// flattened innermost-first list of in-scope locals.
[[nodiscard]] JType inferVarType(
    const ast::Expr& e,
    const std::vector<std::pair<std::string, JType>>& visibleLocals,
    const ClassModel& cls, DiagnosticEngine& diags, const SourceManager& sm);

// ------------------------------------------------------------- helpers -----

[[nodiscard]] std::string quote(std::string_view s);  // 'foo' for messages

// Implementation entry point (Lower.h's b2::frontend::lowerUnit delegates
// here).
[[nodiscard]] LoweredUnit lowerUnitImpl(const ast::CompilationUnit& unit,
                                        const SourceManager& sm,
                                        DiagnosticEngine& diags);

// Array element load/store ops by element family (shared by the expression
// and statement lowering TUs).
[[nodiscard]] rbc::Op aloadOpOf(const JType& elem);
[[nodiscard]] rbc::Op astoreOpOf(const JType& elem);

// ---------------------------------------------------------- MethodLowerer ----

// One value produced by expression lowering: a register + its static type.
struct Val {
  std::uint16_t reg = 0;
  JType type;
  [[nodiscard]] bool ok() const { return !type.isErr(); }
};

struct LoopCtx {
  rbc::RbcBuilder::Label breakT{};
  rbc::RbcBuilder::Label contT{};
  bool hasCont = false;  // false for switch break-targets (no continue)
  std::string label;     // "" = unlabeled (matches any unlabeled break)
};

// Per-method emission state. Statements and expressions are lowered through
// this context (LowerStmt.cpp / LowerExpr.cpp); Lower.cpp owns construction,
// the prologue, and finish().
class MethodLowerer {
 public:
  struct ParamBind {
    std::string name;
    JType type;
  };

  MethodLowerer(const ClassModel& cls, std::string methodName,
                std::string descriptor, std::uint16_t flags, bool isStatic,
                JType result, std::vector<ParamBind> params,
                const MethodPlan* plan, DiagnosticEngine& diags,
                const SourceManager& sm);

  // --- driver ---
  // Prologue (param binding + default-init of non-param slots), optional
  // ctor delegation + field initializers, body statements (skipping
  // `skipStmt` when non-null), default return, finish.
  [[nodiscard]] bool run(const std::vector<ast::Ptr<ast::Stmt>>& body,
                         rbc::Method& out,
                         const std::vector<const ast::Decl*>* fieldInits,
                         const ast::Stmt* skipStmt,
                         const ast::ConstructorInvocation* delegation);

  // Emits instance/static field initializers and initializer blocks into
  // the current stream (ctor and <clinit> bodies).
  void lowerFieldInitSequence(const std::vector<const ast::Decl*>& inits);

  // --- state ---
  [[nodiscard]] const ClassModel& cls() const { return cls_; }
  [[nodiscard]] const MethodPlan* plan() const { return plan_; }
  [[nodiscard]] bool isStatic() const { return isStatic_; }
  [[nodiscard]] bool poisoned() const { return poisoned_; }
  void error(SourceRange at, std::string msg);  // diagnostic + poison
  [[nodiscard]] DiagnosticEngine& diags() { return diags_; }
  [[nodiscard]] const SourceManager& sm() const { return sm_; }
  [[nodiscard]] rbc::RbcBuilder& b() { return b_; }
  [[nodiscard]] std::uint16_t newReg() {
    return static_cast<std::uint16_t>(b_.newReg());
  }

  // --- scopes ---
  struct ScopeEntry {
    std::string name;
    std::uint32_t slot = 0;
    JType type;
  };
  void pushScope() { scopes_.emplace_back(); }
  void popScope() { scopes_.pop_back(); }
  void bindLocal(std::string name, std::uint32_t slot, JType type);
  [[nodiscard]] const ScopeEntry* lookupLocal(std::string_view n) const;
  // Flattened innermost-first visible locals (for var inference parity).
  [[nodiscard]] std::vector<std::pair<std::string, JType>> visibleLocals() const;

  // --- loops / finally ---
  void pushLoop(rbc::RbcBuilder::Label brk, rbc::RbcBuilder::Label cont,
                std::string label, bool hasCont = true) {
    loops_.push_back(LoopCtx{brk, cont, hasCont, std::move(label)});
  }
  void popLoop() { loops_.pop_back(); }
  // break: innermost breakable (loop or switch), or the labeled match.
  [[nodiscard]] const LoopCtx* findBreakable(std::string_view label) const;
  // continue: innermost entry WITH a continue target, or the labeled match.
  [[nodiscard]] const LoopCtx* findLoop(std::string_view label) const;
  void pushFinally(const ast::Block* body) {
    finallies_.push_back(FinallyCtx{body});
  }
  void popFinally() { finallies_.pop_back(); }
  // Emits pending finally bodies innermost-to-outermost before an abrupt
  // transfer (return/break/continue/throw). Each body sees only outer
  // finallies (finite expansion).
  void emitFinallyChain();

  // --- constant pool ---
  [[nodiscard]] std::uint32_t cpClassOf(const JType& t);
  [[nodiscard]] std::uint32_t cpClass(std::string internal);
  [[nodiscard]] std::uint32_t cpField(std::string clsInternal, std::string name,
                                      JType type);
  [[nodiscard]] std::uint32_t cpMethod(std::string clsInternal, std::string name,
                                       std::string desc);

  // --- typed moves / loads / stores (family dispatch by JType) ---
  void emitMove(std::uint16_t dst, std::uint16_t a, const JType& t);
  [[nodiscard]] std::uint16_t emitLoad(std::uint32_t slot, const JType& t);
  void emitStore(std::uint16_t reg, std::uint32_t slot, const JType& t);

  // Numeric conversion emission (identity when families already agree).
  [[nodiscard]] std::uint16_t emitConvert(std::uint16_t reg, const JType& from,
                                          const JType& to, SourceRange at);

  // Assignment conversion; `fold` carries the constant value when the
  // source expression folded (narrowing is constant-only, JLS 5.2).
  [[nodiscard]] std::uint16_t emitAssignConvert(
      std::uint16_t reg, const JType& from, const JType& to,
      std::optional<std::int64_t> fold, SourceRange at);

  // `this` receiver register (instance methods only).
  [[nodiscard]] std::uint16_t loadThis(SourceRange at);

  // Fall-off-the-end default return matching the declared result family.
  void emitDefaultReturn();

  // --- expressions (LowerExpr.cpp) ---
  [[nodiscard]] Val lowerExpr(const ast::Expr& e);
  // Lower `e` and apply assignment conversion to `target` (fold-aware).
  [[nodiscard]] Val lowerExprTo(const ast::Expr& e, const JType& target,
                                std::string_view context, SourceRange at);
  // Ctor delegation `this(...)` as an invokespecial <init> call.
  void lowerCtorDelegation(const ast::ConstructorInvocation& ci);

 private:
  [[nodiscard]] Val lowerFieldAccess(const ast::FieldAccess& fa);
  [[nodiscard]] Val lowerInvoke(const ast::MethodInvocation& mi);
  [[nodiscard]] Val lowerNew(const ast::ClassInstanceCreation& nc);
  [[nodiscard]] Val lowerArrayInitializer(const ast::ArrayInitializer& ai,
                                          const JType& elem, SourceRange at);
  // Emits a call: receiver (when present) + evaluated args moved into one
  // consecutive register block, then the invoke. declaredParams == nullptr
  // means unknown receiver: the descriptor comes from static arg types and
  // the result is void.
  [[nodiscard]] Val emitCallSeq(
      rbc::Op op, std::optional<std::uint16_t> recvReg,
      const std::vector<const ast::Expr*>& args,
      const std::vector<JType>* declaredParams, const std::string& clsInternal,
      const std::string& name, SourceRange at, const JType& declaredResult);

 public:
  // Boolean-valued expression as Int 0/1 (materialization is explicit).
  [[nodiscard]] Val lowerBoolExpr(const ast::Expr& e);

  // --- statements (LowerStmt.cpp) ---
  void lowerStmt(const ast::Stmt& s);
  void lowerBlock(const ast::Block& blk);  // pushes/pops the scope
  // Single-statement position (mirror of the planner's walkSub).
  void lowerSub(const ast::Stmt& s);

 private:
  void lowerWhile(const ast::While& w, std::string label);
  void lowerDoWhile(const ast::DoWhile& w, std::string label);
  void lowerBasicFor(const ast::BasicFor& f, std::string label);
  void lowerEnhancedFor(const ast::EnhancedFor& f, std::string label);
  void lowerSwitch(const ast::SwitchStatement& sw, std::string label);
  void lowerTry(const ast::Try& t);
  void emitPrologue();

  const ClassModel& cls_;
  std::string name_;
  std::string descriptor_;
  std::uint16_t flags_ = 0;
  bool isStatic_ = false;
  JType result_ = JType::prim(JK::Void);
  std::vector<ParamBind> params_;
  const MethodPlan* plan_ = nullptr;
  DiagnosticEngine& diags_;
  const SourceManager& sm_;
  rbc::RbcBuilder b_;
  bool poisoned_ = false;
  std::vector<std::vector<ScopeEntry>> scopes_;
  std::vector<LoopCtx> loops_;

  struct FinallyCtx {
    const ast::Block* body = nullptr;
  };
  std::vector<FinallyCtx> finallies_;
};

}  // namespace b2::frontend::lower
