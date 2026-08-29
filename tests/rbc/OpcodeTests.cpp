// B-2 RBC opcode-metadata tests.
//
// The OpInfo table in compiler/rbc/src/Opcode.cpp is the single source of
// truth shared by the verifier, the text printer/parser, the quickener and
// stencil selection; these tests pin its shape (count, unique mnemonics) and
// the load-bearing entries called out by docs/rbc_spec.md §3 and §10.1
// (spec pins P1-P5, effect assignments, descriptor-derived Bottom results).

#include <algorithm>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "b2/rbc/Opcode.h"
#include "b2/rbc/Type.h"

using b2::rbc::Eff;
using b2::rbc::hasEff;
using b2::rbc::info;
using b2::rbc::Op;
using b2::rbc::opCount;
using b2::rbc::OpInfo;
using b2::rbc::opName;
using b2::rbc::RType;
using b2::rbc::Sig;

namespace {

B2_TEST(rbc_opcode_count_is_150) {
  // Frozen by docs/rbc_spec.md §3 and static_asserted in Opcode.cpp; the test
  // re-checks it at runtime against the real library so a stale build cannot
  // silently pass.
  CHECK(opCount() == 150);
  CHECK(opCount() == static_cast<std::uint16_t>(Op::_Count));
}

B2_TEST(rbc_opcode_names_nonempty_and_unique) {
  std::vector<std::string> names;
  names.reserve(opCount());
  for (std::uint16_t i = 0; i < opCount(); ++i) {
    const OpInfo& oi = info(static_cast<Op>(i));
    CHECK_MSG(oi.name != nullptr && oi.name[0] != '\0',
              "empty mnemonic at opcode " + std::to_string(i));
    names.emplace_back(oi.name);
  }
  std::sort(names.begin(), names.end());
  const auto dup = std::adjacent_find(names.begin(), names.end());
  CHECK_MSG(dup == names.end(),
            "duplicate mnemonic '" + *dup + "' in the opcode table");
}

B2_TEST(rbc_opcode_table_endpoints_and_fallback) {
  // Enum order pins: table starts at Nop, ends at GuardClass (Opcode.cpp
  // static_asserts the same; kept here as a runtime guard).
  CHECK(std::string(opName(Op::Nop)) == "nop");
  CHECK(std::string(opName(Op::GuardClass)) == "guard_class");
  // Out-of-range lookups must never be UB: the documented "bad<op>" fallback.
  CHECK(std::string(opName(static_cast<Op>(9999))) == "bad<op>");
  CHECK(info(static_cast<Op>(9999)).sig == Sig::None);
}

B2_TEST(rbc_opcode_info_iadd) {
  const OpInfo& oi = info(Op::Iadd);
  CHECK(std::string(oi.name) == "iadd");
  CHECK(oi.sig == Sig::RegRegReg);
  CHECK(oi.result == RType::Int);
  CHECK(oi.operandA == RType::Int);
  CHECK(oi.operandB == RType::Int);
  CHECK(oi.effects == Eff::None);
}

B2_TEST(rbc_opcode_info_loads_and_stores) {
  const OpInfo& al = info(Op::Aload);
  CHECK(al.sig == Sig::RegSlot);
  CHECK(al.result == RType::Ref);
  const OpInfo& as = info(Op::Astore);
  CHECK(as.sig == Sig::SlotReg);
  CHECK(as.result == RType::Bottom); // stores write nothing
  CHECK(as.operandA == RType::Ref);
  // Integer load/store for contrast.
  CHECK(info(Op::Iload).sig == Sig::RegSlot);
  CHECK(info(Op::Iload).result == RType::Int);
  CHECK(info(Op::Istore).sig == Sig::SlotReg);
  CHECK(info(Op::Istore).operandA == RType::Int);
}

B2_TEST(rbc_opcode_info_goto_branch) {
  const OpInfo& oi = info(Op::Goto);
  CHECK(oi.sig == Sig::Branch);
  CHECK(hasEff(oi.effects, Eff::CanBranch));
  CHECK(!hasEff(oi.effects, Eff::CanCall));
  CHECK(!hasEff(oi.effects, Eff::IsSafepoint)); // polls are explicit ops
  const OpInfo& ife = info(Op::Ifeq);
  CHECK(ife.sig == Sig::RegBranch);
  CHECK(hasEff(ife.effects, Eff::CanBranch));
  CHECK(ife.operandA == RType::Int);
}

B2_TEST(rbc_opcode_info_invokevirtual_call) {
  const OpInfo& oi = info(Op::Invokevirtual);
  CHECK(oi.sig == Sig::Call);
  CHECK(hasEff(oi.effects, Eff::CanCall));
  CHECK(hasEff(oi.effects, Eff::CanTrap));
  CHECK(hasEff(oi.effects, Eff::IsSafepoint));
  // Result and argument types are descriptor-derived (verifier reads the cp).
  CHECK(oi.result == RType::Bottom);
  CHECK(oi.operandA == RType::Bottom);
  CHECK(oi.operandB == RType::Bottom);
  // All five invoke families share the Call signature.
  CHECK(info(Op::Invokespecial).sig == Sig::Call);
  CHECK(info(Op::Invokestatic).sig == Sig::Call);
  CHECK(info(Op::Invokeinterface).sig == Sig::Call);
  CHECK(info(Op::Invokedynamic).sig == Sig::Call);
}

B2_TEST(rbc_opcode_info_new_allocates) {
  const OpInfo& oi = info(Op::New);
  CHECK(oi.sig == Sig::RegCp);
  CHECK(oi.result == RType::Ref);
  CHECK(hasEff(oi.effects, Eff::CanAllocate));
  CHECK(hasEff(oi.effects, Eff::CanTrap));
}

B2_TEST(rbc_opcode_info_division_traps) {
  CHECK(hasEff(info(Op::Idiv).effects, Eff::CanTrap));
  CHECK(hasEff(info(Op::Irem).effects, Eff::CanTrap));
  CHECK(hasEff(info(Op::Ldiv).effects, Eff::CanTrap));
  CHECK(hasEff(info(Op::Lrem).effects, Eff::CanTrap));
  // IEEE 754 division never traps.
  CHECK(!hasEff(info(Op::Fadd).effects, Eff::CanTrap));
  CHECK(!hasEff(info(Op::Fdiv).effects, Eff::CanTrap));
  CHECK(!hasEff(info(Op::Ddiv).effects, Eff::CanTrap));
  // Plain integer arithmetic wraps silently.
  CHECK(!hasEff(info(Op::Iadd).effects, Eff::CanTrap));
}

B2_TEST(rbc_opcode_info_quickened_flags) {
  CHECK(hasEff(info(Op::GetfieldQuick).effects, Eff::Quickened));
  CHECK(hasEff(info(Op::PutfieldQuick).effects, Eff::Quickened));
  CHECK(hasEff(info(Op::InvokevirtualQuick).effects, Eff::Quickened));
  CHECK(hasEff(info(Op::InvokespecialQuick).effects, Eff::Quickened));
  CHECK(hasEff(info(Op::InvokestaticQuick).effects, Eff::Quickened));
  CHECK(hasEff(info(Op::InvokeinterfaceQuick).effects, Eff::Quickened));
  // Un-quickened forms must never carry the flag (spec §6.2: the flag exists
  // exactly for the quickened variants).
  CHECK(!hasEff(info(Op::Getfield).effects, Eff::Quickened));
  CHECK(!hasEff(info(Op::Invokevirtual).effects, Eff::Quickened));
  // Quickened calls keep the Call operand layout but via CallQuick.
  CHECK(info(Op::InvokevirtualQuick).sig == Sig::CallQuick);
  CHECK(info(Op::GetfieldQuick).sig == Sig::RegRegCp);
}

B2_TEST(rbc_opcode_info_checkcast_and_safepoint) {
  const OpInfo& cc = info(Op::Checkcast);
  CHECK(cc.sig == Sig::RegRegCp); // P4: the cp operand is required
  CHECK(cc.result == RType::Ref);
  CHECK(cc.operandA == RType::Ref);
  CHECK(hasEff(cc.effects, Eff::CanTrap));
  const OpInfo& sp = info(Op::SafepointPoll);
  CHECK(sp.sig == Sig::None);
  CHECK(hasEff(sp.effects, Eff::IsSafepoint));
  CHECK(!hasEff(sp.effects, Eff::CanBranch));
}

B2_TEST(rbc_opcode_info_switches) {
  const OpInfo& ts = info(Op::Tableswitch);
  CHECK(ts.sig == Sig::RegCpBranch);
  CHECK(hasEff(ts.effects, Eff::CanBranch));
  CHECK(ts.operandA == RType::Int); // selector
  const OpInfo& ls = info(Op::Lookupswitch);
  CHECK(ls.sig == Sig::RegCpBranch);
  CHECK(hasEff(ls.effects, Eff::CanBranch));
  CHECK(ls.operandA == RType::Int);
}

B2_TEST(rbc_opcode_info_spec_pins) {
  // Spec pins P1-P5 (docs/rbc_spec.md §10.1), mirrored by static_asserts in
  // Opcode.cpp; re-checked here at runtime.
  CHECK(info(Op::Putstatic).sig == Sig::RegCp);       // P1: dst = value
  CHECK(info(Op::AconstNull).sig == Sig::RegImm);     // P2: imm unused
  CHECK(info(Op::AconstNull).result == RType::Null);
  CHECK(info(Op::Iastore).sig == Sig::RegRegReg);     // P3: dst = value
  CHECK(info(Op::Multianewarray).sig == Sig::RegRegRegCp); // P5
  // Long shift counts are Int, not Long.
  CHECK(info(Op::Lshl).operandB == RType::Int);
  CHECK(info(Op::Lshr).operandB == RType::Int);
  CHECK(info(Op::Lushr).operandB == RType::Int);
}

B2_TEST(rbc_type_lattice_join) {
  using b2::rbc::join;
  CHECK(join(RType::Null, RType::Ref) == RType::Ref);
  CHECK(join(RType::Ref, RType::Null) == RType::Ref);
  CHECK(join(RType::Int, RType::Ref) == RType::Bottom); // incompatible
  CHECK(join(RType::Int, RType::Long) == RType::Bottom);
  CHECK(join(RType::Int, RType::Int) == RType::Int);
  CHECK(join(RType::Bottom, RType::Int) == RType::Bottom);
  CHECK(join(RType::Null, RType::Null) == RType::Null);
}

B2_TEST(rbc_type_lattice_assignable) {
  using b2::rbc::isAssignable;
  CHECK(isAssignable(RType::Null, RType::Ref)); // Null <: Ref
  CHECK(!isAssignable(RType::Ref, RType::Null));
  CHECK(isAssignable(RType::Ref, RType::Ref));
  CHECK(!isAssignable(RType::Int, RType::Long));
  CHECK(!isAssignable(RType::Long, RType::Int));
  CHECK(!isAssignable(RType::Bottom, RType::Int)); // uninit never usable
}

B2_TEST(rbc_type_lattice_numeric_and_names) {
  using b2::rbc::isCategory2;
  using b2::rbc::isNumeric;
  using b2::rbc::typeName;
  CHECK(isNumeric(RType::Int));
  CHECK(isNumeric(RType::Long));
  CHECK(isNumeric(RType::Float));
  CHECK(isNumeric(RType::Double));
  CHECK(!isNumeric(RType::Ref));
  CHECK(!isNumeric(RType::Null));
  CHECK(!isNumeric(RType::Bottom));
  CHECK(isCategory2(RType::Long));
  CHECK(isCategory2(RType::Double));
  CHECK(!isCategory2(RType::Int));
  CHECK(!isCategory2(RType::Float));
  CHECK(std::string(typeName(RType::Bottom)) == "bottom");
  CHECK(std::string(typeName(RType::Int)) == "int");
  CHECK(std::string(typeName(RType::Long)) == "long");
  CHECK(std::string(typeName(RType::Float)) == "float");
  CHECK(std::string(typeName(RType::Double)) == "double");
  CHECK(std::string(typeName(RType::Null)) == "null");
  CHECK(std::string(typeName(RType::Ref)) == "ref");
}

}  // namespace
