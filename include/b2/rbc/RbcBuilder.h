#pragma once
// B-2 RBC - method builder used by frontend lowering and tests.
//
// WHY THIS FILE EXISTS:
// Lowering (and tests) must not hand-assemble Ins records - operand layout
// knowledge would leak across the codebase. The builder owns that knowledge,
// gives every opcode a named, type-checked factory, resolves labels to
// instruction indices at finish(), and refuses to produce malformed methods
// where it can (register/slot bounds are checked at finish, not per-op, so
// callers may size methods after the fact).

#include <cstdint>
#include <string>
#include <vector>

#include "b2/rbc/Rbc.h"

namespace b2::rbc {

class RbcBuilder {
 public:
  // name/descriptor/flags fix the method header; numRegs/numLocals may be
  // raised later via setRegs()/setLocals() (builders rarely know sizes up front).
  explicit RbcBuilder(std::string name, std::string descriptor,
                      std::uint16_t flags = 0);

  // --- sizing -------------------------------------------------------------
  void setRegs(std::uint32_t n) noexcept;
  void setLocals(std::uint32_t n) noexcept;
  [[nodiscard]] std::uint32_t newReg() noexcept; // returns rN, bumps numRegs
  void reserveCode(std::size_t n) { code_.reserve(n); }

  // --- constant pool ------------------------------------------------------
  [[nodiscard]] std::uint32_t constInt(std::int32_t v);
  [[nodiscard]] std::uint32_t constLong(std::int64_t v);
  [[nodiscard]] std::uint32_t constFloat(float v);
  [[nodiscard]] std::uint32_t constDouble(double v);
  [[nodiscard]] std::uint32_t constString(std::string v);
  [[nodiscard]] std::uint32_t constClass(std::string internalName);
  [[nodiscard]] std::uint32_t constFieldRef(std::string cls, std::string name,
                                            std::string descriptor);
  [[nodiscard]] std::uint32_t constMethodRef(std::string cls, std::string name,
                                             std::string descriptor);
  [[nodiscard]] std::uint32_t constSwitch(std::vector<std::int32_t> payload);

  // --- emission (one factory per Sig family; operand order matches spec) ---
  std::uint32_t emit(Op op);                      // Sig::None
  std::uint32_t emitReg(Op op, std::uint16_t a);  // Sig::Reg
  std::uint32_t emitRegImm(Op op, std::uint16_t dst, std::uint32_t imm);
  std::uint32_t emitRegCp(Op op, std::uint16_t dst, std::uint32_t cpIdx);
  std::uint32_t emitRegSlot(Op op, std::uint16_t dst, std::uint32_t slot);
  std::uint32_t emitSlotReg(Op op, std::uint16_t a, std::uint32_t slot);
  std::uint32_t emitRegReg(Op op, std::uint16_t dst, std::uint16_t a);
  std::uint32_t emitRegRegReg(Op op, std::uint16_t dst, std::uint16_t a,
                              std::uint16_t b);
  std::uint32_t emitRegRegImm(Op op, std::uint16_t dst, std::uint16_t a,
                              std::uint32_t imm);
  std::uint32_t emitRegRegCp(Op op, std::uint16_t dst, std::uint16_t a,
                             std::uint32_t cpIdx);
  std::uint32_t emitRegRegRegCp(Op op, std::uint16_t dst, std::uint16_t a,
                                std::uint16_t b, std::uint32_t cpIdx);
  std::uint32_t emitCall(Op op, std::uint16_t dst, std::uint16_t argBase,
                         std::uint16_t argCount, std::uint32_t cpIdx);
  std::uint32_t emitGuard(Op op, std::uint16_t a, std::uint32_t deoptId);
  std::uint32_t emitTrap(std::uint32_t deoptId);

  // --- branches -----------------------------------------------------------
  // Labels are resolved at finish(). Forward and backward references both
  // allowed; redefinition of a bound label fails finish().
  struct Label {
    std::uint32_t id = 0xFFFFFFFFu; // opaque
    [[nodiscard]] bool isValid() const noexcept { return id != 0xFFFFFFFFu; }
  };

  [[nodiscard]] Label newLabel() noexcept;
  std::uint32_t emitBranch(Op op, Label target);            // goto
  std::uint32_t emitRegBranch(Op op, std::uint16_t a, Label target);
  std::uint32_t emitRegRegBranch(Op op, std::uint16_t a, std::uint16_t b,
                                 Label target);
  std::uint32_t emitSwitch(Op op, std::uint16_t a,
                           const std::vector<std::int32_t>& payload,
                           std::vector<Label> targets);
  void bind(Label l); // bind at the NEXT emitted instruction
  [[nodiscard]] std::uint32_t here() const noexcept; // next instruction index

  // --- exception handlers ---------------------------------------------------
  void addHandler(std::uint32_t start, std::uint32_t end, std::uint32_t handler,
                  std::int32_t catchTypeCp = -1);
  [[nodiscard]] Label handlerLabel(); // convenience: label to bind as handler

  // --- finish ----------------------------------------------------------------
  // Resolves labels, validates structural invariants the builder can see
  // (targets bound, register bounds if sized). Returns false + message on
  // builder-level misuse; full verification is the Verifier's job.
  struct FinishResult {
    bool ok = false;
    std::string error;
  };
  [[nodiscard]] FinishResult finish(Method& out);

  [[nodiscard]] const std::vector<Ins>& code() const noexcept { return code_; }
  [[nodiscard]] std::size_t size() const noexcept { return code_.size(); }

 private:
  // WHY slot: switch targets patch into the canonical SwitchTable payload
  // (see docs/rbc_spec.md: table [low,high,default,targets...], lookup
  // [N,default,match,target,...]) at payload-specific positions, while
  // branches patch Ins::imm. kBranchSlot selects the imm path.
  static constexpr std::uint32_t kBranchSlot = 0xFFFFFFFFu;

  struct Patch {
    std::uint32_t at;        // instruction index to patch
    std::uint32_t labelId;   // label to resolve
    std::uint32_t slot;      // SwitchTable ints slot, or kBranchSlot for imm
  };

  std::uint32_t internConst(Const c);
  std::uint32_t rawEmit(const Ins& i);

  std::string name_;
  std::string descriptor_;
  std::uint16_t flags_ = 0;
  std::uint32_t numRegs_ = 0;
  std::uint32_t numLocals_ = 0;
  std::vector<Ins> code_;
  std::vector<Const> cp_;
  std::vector<ExceptionHandler> handlers_;
  std::vector<Label> labels_;          // id == index
  std::vector<std::uint32_t> boundAt_; // per-label instruction index, or UNBOUND
  std::vector<Patch> patches_;
  std::string pendingError_;           // emit-time misuse (dup match, gaps), fails finish()
};

} // namespace b2::rbc
