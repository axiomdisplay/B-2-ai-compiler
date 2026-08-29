// B-2 RBC verifier tests.
//
// verify() is the hard gate before any tier executes RBC (Law: verifier
// before quickener before execution), so these tests hand-assemble Method
// structs directly - the verifier's real input contract - and check both
// acceptance of well-formed methods and the exact stable diagnostic
// substrings of Verifier.cpp's rejection paths (structural phase 1, type
// phase 2, protected-range stability). Hand-assembly is deliberate here:
// the builder cannot (and must not) produce malformed methods.

#include <string>
#include <vector>

#include "TestHarness.h"
#include "b2/rbc/Rbc.h"
#include "b2/rbc/Verifier.h"

using b2::rbc::Const;
using b2::rbc::ExceptionHandler;
using b2::rbc::Ins;
using b2::rbc::Method;
namespace method_flags = b2::rbc::method_flags;
using b2::rbc::Op;
using b2::rbc::verify;

namespace {

Method makeMethod(const char* name, const char* desc, std::uint16_t flags,
                  std::uint32_t regs, std::uint32_t locals) {
  Method m;
  m.name = name;
  m.descriptor = desc;
  m.flags = flags;
  m.numRegs = regs;
  m.numLocals = locals;
  return m;
}

Const classConst(const char* internalName) {
  Const c;
  c.kind = Const::Kind::Class;
  c.str = internalName;
  return c;
}

Const fieldRefConst(const char* cls, const char* name, const char* desc) {
  Const c;
  c.kind = Const::Kind::FieldRef;
  c.str = cls;
  c.str2 = name;
  c.str3 = desc;
  return c;
}

Const methodRefConst(const char* cls, const char* name, const char* desc) {
  Const c;
  c.kind = Const::Kind::MethodRef;
  c.str = cls;
  c.str2 = name;
  c.str3 = desc;
  return c;
}

// Asserts the method is rejected and that `substr` occurs in the FIRST
// diagnostic (the verifier's emission order is deterministic: phase 0/1 in pc
// order, then phase 2 fall-off/descriptor gate, then per-pc type errors, then
// protected-range stability).
void checkReject(const Method& m, const char* substr) {
  const auto r = verify(m);
  CHECK_MSG(!r.ok, "method must be rejected");
  if (r.ok) {
    return;
  }
  CHECK_MSG(!r.diags.empty(), "rejected without a diagnostic");
  if (!r.diags.empty()) {
    const std::string& first = r.diags.front().message;
    CHECK_MSG(first.find(substr) != std::string::npos,
              std::string("expected '") + substr + "' in first diagnostic '" +
                  first + "'");
  }
}

// --- positives --------------------------------------------------------------

B2_TEST(rbc_verify_static_add) {
  Method m = makeMethod("add", "(II)I", method_flags::Static, 3, 2);
  m.code.push_back(Ins(Op::Iload, 0, 0, 0, 0));   // pc0
  m.code.push_back(Ins(Op::Iload, 1, 0, 0, 1));   // pc1
  m.code.push_back(Ins(Op::Iadd, 2, 0, 1, 0));    // pc2
  m.code.push_back(Ins(Op::Ireturn, 0, 2, 0, 0)); // pc3
  const auto r = verify(m);
  CHECK_MSG(r.ok, r.diags.empty() ? "verify failed"
                                  : "verify failed: " + r.diags.front().message);
}

B2_TEST(rbc_verify_long_lmul) {
  Method m = makeMethod("lmul", "(JJ)J", method_flags::Static, 3, 2);
  m.code.push_back(Ins(Op::Lload, 0, 0, 0, 0));
  m.code.push_back(Ins(Op::Lload, 1, 0, 0, 1));
  m.code.push_back(Ins(Op::Lmul, 2, 0, 1, 0));
  m.code.push_back(Ins(Op::Lreturn, 0, 2, 0, 0));
  CHECK_MSG(verify(m).ok, "long multiplication method must verify");
}

B2_TEST(rbc_verify_instance_receiver) {
  // Instance method: l0 is the receiver, typed Ref from the entry state.
  Method m = makeMethod("self", "()Ljava/lang/Object;", 0, 1, 1);
  m.code.push_back(Ins(Op::Aload, 0, 0, 0, 0));
  m.code.push_back(Ins(Op::Areturn, 0, 0, 0, 0));
  CHECK_MSG(verify(m).ok, "instance method with receiver must verify");
}

B2_TEST(rbc_verify_void_return) {
  Method m = makeMethod("noop", "()V", method_flags::Static, 0, 0);
  m.code.push_back(Ins(Op::Return, 0, 0, 0, 0));
  CHECK_MSG(verify(m).ok, "void method with bare return must verify");
}

B2_TEST(rbc_verify_monitor_on_ref_local) {
  Method m = makeMethod("sync", "(Ljava/lang/Object;)V", method_flags::Static,
                        1, 1);
  m.code.push_back(Ins(Op::Aload, 0, 0, 0, 0));    // pc0
  m.code.push_back(Ins(Op::Monitorenter, 0, 0, 0, 0)); // pc1
  m.code.push_back(Ins(Op::Monitorexit, 0, 0, 0, 0));  // pc2
  m.code.push_back(Ins(Op::Return, 0, 0, 0, 0));   // pc3
  CHECK_MSG(verify(m).ok, "monitor pair on a Ref local must verify");
}

B2_TEST(rbc_verify_getfield_putfield) {
  // int get(Object o) { return o.value; }  (field corpus/Widget.value : I)
  Method g = makeMethod("get", "(Ljava/lang/Object;)I", method_flags::Static,
                        2, 1);
  g.cp.push_back(fieldRefConst("corpus/Widget", "value", "I"));
  g.code.push_back(Ins(Op::Aload, 0, 0, 0, 0));   // pc0
  g.code.push_back(Ins(Op::Getfield, 1, 0, 0, 0)); // pc1: r1 = r0.value
  g.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));  // pc2
  CHECK_MSG(verify(g).ok, "getfield with FieldRef cp must verify");

  // void put(Object o, int v) { o.value = v; }
  Method p = makeMethod("put", "(Ljava/lang/Object;I)V", method_flags::Static,
                        2, 2);
  p.cp.push_back(fieldRefConst("corpus/Widget", "value", "I"));
  p.code.push_back(Ins(Op::Aload, 0, 0, 0, 0));    // pc0
  p.code.push_back(Ins(Op::Iload, 1, 0, 0, 1));    // pc1
  p.code.push_back(Ins(Op::Putfield, 0, 0, 1, 0)); // pc2: r0.value = r1
  p.code.push_back(Ins(Op::Return, 0, 0, 0, 0));   // pc3
  CHECK_MSG(verify(p).ok, "putfield with FieldRef cp must verify");
}

B2_TEST(rbc_verify_new_invokespecial_init) {
  // Object make() { return new corpus/Widget(); }
  Method m = makeMethod("make", "()Ljava/lang/Object;", method_flags::Static,
                        2, 1);
  m.cp.push_back(classConst("corpus/Widget"));
  m.cp.push_back(methodRefConst("corpus/Widget", "<init>", "()V"));
  m.code.push_back(Ins(Op::New, 0, 0, 0, 0));            // pc0: new r0 c0
  m.code.push_back(Ins(Op::Invokespecial, 0, 0, 1, 1));  // pc1: <init>(r0)
  m.code.push_back(Ins(Op::Astore, 0, 0, 0, 0));         // pc2
  m.code.push_back(Ins(Op::Aload, 1, 0, 0, 0));          // pc3
  m.code.push_back(Ins(Op::Areturn, 0, 1, 0, 0));        // pc4
  const auto r = verify(m);
  CHECK_MSG(r.ok, r.diags.empty() ? "verify failed"
                                  : "verify failed: " + r.diags.front().message);
}

B2_TEST(rbc_verify_tableswitch_targets_valid) {
  // Canonical table payload [low, high, default, targets...] (Verifier.cpp
  // normative layout): every target is a valid instruction index.
  Method m = makeMethod("pick", "(I)I", method_flags::Static, 2, 1);
  Const sw;
  sw.kind = Const::Kind::SwitchTable;
  sw.ints = {0, 2, 4, 6, 8, 10}; // low 0, high 2, default 4, cases -> 6/8/10
  m.cp.push_back(sw);
  m.code.push_back(Ins(Op::Iload, 1, 0, 0, 0));        // pc0
  m.code.push_back(Ins(Op::Tableswitch, 0, 1, 0, 0));  // pc1
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, -1));      // pc2 (dead)
  m.code.push_back(Ins(Op::Goto, 0, 0, 0, 4));         // pc3 -> default
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 0));       // pc4 default
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));      // pc5
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 100));     // pc6 case 0
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));      // pc7
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 200));     // pc8 case 1
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));      // pc9
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 300));     // pc10 case 2
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));      // pc11
  const auto r = verify(m);
  CHECK_MSG(r.ok, r.diags.empty() ? "verify failed"
                                  : "verify failed: " + r.diags.front().message);
}

B2_TEST(rbc_verify_try_catch_stable_locals) {
  // int guarded(int n) { int one = 1; try { return one / n; } catch (...) {
  //   return n; } }
  // l1 is initialized BEFORE the protected range and stays Int inside it;
  // the handler only reads locals (handler-entry registers are all Bottom).
  Method m = makeMethod("guarded", "(I)I", method_flags::Static, 2, 2);
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 1));   // pc0: r1 = 1
  m.code.push_back(Ins(Op::Istore, 0, 1, 0, 1));   // pc1: l1 = Int (pre-try)
  m.code.push_back(Ins(Op::Iload, 0, 0, 0, 0));    // pc2: r0 = n   <- range
  m.code.push_back(Ins(Op::Idiv, 1, 1, 0, 0));     // pc3: r1 = r1/r0 <- ...
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));  // pc4           <- end
  m.code.push_back(Ins(Op::Iload, 0, 0, 0, 0));    // pc5: handler entry
  m.code.push_back(Ins(Op::Ireturn, 0, 0, 0, 0));  // pc6
  ExceptionHandler h;
  h.start = 2;
  h.end = 5;
  h.handler = 5;
  h.catchType = -1;
  m.handlers.push_back(h);
  const auto r = verify(m);
  CHECK_MSG(r.ok, r.diags.empty() ? "verify failed"
                                  : "verify failed: " + r.diags.front().message);
}

B2_TEST(rbc_verify_abstract_empty_body) {
  Method m = makeMethod("runIt", "()V", method_flags::Abstract, 0, 0);
  CHECK_MSG(verify(m).ok, "abstract method with empty body must verify");
}

// --- negatives: structural (phase 0/1) ---------------------------------------

B2_TEST(rbc_verify_reject_register_out_of_range) {
  Method m = makeMethod("f", "(I)I", method_flags::Static, 1, 1);
  m.code.push_back(Ins(Op::Iload, 0, 0, 0, 0)); // pc0: fine
  m.code.push_back(Ins(Op::Iadd, 2, 0, 0, 0));  // pc1: dst r2 >= numRegs 1
  m.code.push_back(Ins(Op::Ireturn, 0, 0, 0, 0));
  checkReject(m, "out of range");
}

B2_TEST(rbc_verify_reject_slot_out_of_range) {
  Method m = makeMethod("f", "(I)I", method_flags::Static, 1, 1);
  m.code.push_back(Ins(Op::Iload, 0, 0, 0, 3)); // pc0: slot 3 >= numLocals 1
  m.code.push_back(Ins(Op::Ireturn, 0, 0, 0, 0));
  checkReject(m, "out of range");
}

B2_TEST(rbc_verify_reject_cp_index_out_of_range) {
  Method m = makeMethod("f", "()J", method_flags::Static, 1, 0);
  m.code.push_back(Ins(Op::Lconst, 0, 0, 0, 7)); // pc0: c7 with empty pool
  m.code.push_back(Ins(Op::Lreturn, 0, 0, 0, 0));
  checkReject(m, "out of range");
}

B2_TEST(rbc_verify_reject_cp_kind_mismatch) {
  // Lconst requires an Int64 entry; the pool holds Int32.
  Method m = makeMethod("f", "()J", method_flags::Static, 1, 0);
  Const c;
  c.kind = Const::Kind::Int32;
  c.i32 = 5;
  m.cp.push_back(c);
  m.code.push_back(Ins(Op::Lconst, 0, 0, 0, 0));
  m.code.push_back(Ins(Op::Lreturn, 0, 0, 0, 0));
  checkReject(m, "requires long");
}

B2_TEST(rbc_verify_reject_getfield_on_methodref) {
  Method m = makeMethod("f", "()I", method_flags::Static, 2, 0);
  m.cp.push_back(methodRefConst("C", "m", "()I"));
  m.code.push_back(Ins(Op::AconstNull, 0, 0, 0, 0));  // pc0: r0 = null
  m.code.push_back(Ins(Op::Getfield, 1, 0, 0, 0));    // pc1: on MethodRef
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));
  checkReject(m, "requires field");
}

B2_TEST(rbc_verify_reject_branch_target_out_of_range) {
  Method m = makeMethod("f", "()V", method_flags::Static, 0, 0);
  m.code.push_back(Ins(Op::Goto, 0, 0, 0, 5)); // target 5, code size 1
  checkReject(m, "out of range");
}

B2_TEST(rbc_verify_reject_invalid_opcode) {
  Method m = makeMethod("f", "()V", method_flags::Static, 0, 0);
  m.code.push_back(Ins(static_cast<Op>(9999), 0, 0, 0, 0));
  checkReject(m, "invalid opcode");
}

B2_TEST(rbc_verify_reject_malformed_switch_payload) {
  // Tableswitch payload with the wrong size for [low, high, default, ...].
  Method m = makeMethod("f", "(I)I", method_flags::Static, 1, 1);
  Const sw;
  sw.kind = Const::Kind::SwitchTable;
  sw.ints = {0, 2, 0}; // size 3: claims low 0..high 2 but has no targets
  m.cp.push_back(sw);
  m.code.push_back(Ins(Op::Iload, 0, 0, 0, 0));
  m.code.push_back(Ins(Op::Tableswitch, 0, 0, 0, 0));
  m.code.push_back(Ins(Op::Iconst, 0, 0, 0, 0));
  m.code.push_back(Ins(Op::Ireturn, 0, 0, 0, 0));
  checkReject(m, "malformed tableswitch payload");
}

B2_TEST(rbc_verify_reject_handler_range_empty) {
  Method m = makeMethod("f", "()V", method_flags::Static, 0, 0);
  m.code.push_back(Ins(Op::Return, 0, 0, 0, 0));
  ExceptionHandler h;
  h.start = 0;
  h.end = 0; // empty range
  h.handler = 0;
  h.catchType = -1;
  m.handlers.push_back(h);
  checkReject(m, "is empty");
}

// --- negatives: type checking (phase 2) --------------------------------------

B2_TEST(rbc_verify_reject_iadd_ref_operand) {
  Method m = makeMethod("f", "(Ljava/lang/Object;)I", method_flags::Static, 2,
                        1);
  m.code.push_back(Ins(Op::Aload, 0, 0, 0, 0));   // pc0: r0 = Ref
  m.code.push_back(Ins(Op::Iadd, 1, 0, 0, 0));    // pc1: iadd on a Ref
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));
  checkReject(m, "expects int");
}

B2_TEST(rbc_verify_reject_int_where_long_expected) {
  Method m = makeMethod("f", "(I)J", method_flags::Static, 1, 1);
  m.code.push_back(Ins(Op::Iload, 0, 0, 0, 0));
  m.code.push_back(Ins(Op::Lreturn, 0, 0, 0, 0)); // Int value, Long return
  checkReject(m, "expects long");
}

B2_TEST(rbc_verify_reject_uninitialized_register_use) {
  Method m = makeMethod("f", "()I", method_flags::Static, 1, 0);
  m.code.push_back(Ins(Op::Ireturn, 0, 0, 0, 0)); // r0 never assigned
  checkReject(m, "found bottom");
}

B2_TEST(rbc_verify_reject_local_type_instability) {
  // l1 is Int at the start of the protected range but becomes Null inside
  // it (astore at pc5): the handler could observe either type, which the
  // protected-range local-stability rule forbids.
  Method m = makeMethod("f", "(I)I", method_flags::Static, 2, 2);
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 1));    // pc0: r1 = 1
  m.code.push_back(Ins(Op::Istore, 0, 1, 0, 1));    // pc1: l1 = Int
  m.code.push_back(Ins(Op::Iload, 0, 0, 0, 0));     // pc2 <- range start
  m.code.push_back(Ins(Op::Idiv, 1, 1, 0, 0));      // pc3 (traps)
  m.code.push_back(Ins(Op::AconstNull, 0, 0, 0, 0)); // pc4: r0 = null
  m.code.push_back(Ins(Op::Astore, 0, 0, 0, 1));    // pc5: l1 = Null (!)
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));   // pc6 <- range end
  m.code.push_back(Ins(Op::Iload, 0, 0, 0, 0));     // pc7: handler entry
  m.code.push_back(Ins(Op::Ireturn, 0, 0, 0, 0));   // pc8
  ExceptionHandler h;
  h.start = 2;
  h.end = 7;
  h.handler = 7;
  h.catchType = -1;
  m.handlers.push_back(h);
  checkReject(m, "changes type within protected range");
}

B2_TEST(rbc_verify_reject_fall_off_end) {
  Method m = makeMethod("f", "()V", method_flags::Static, 1, 0);
  m.code.push_back(Ins(Op::Iconst, 0, 0, 0, 1)); // last insn falls through
  checkReject(m, "falls off the end");
}

B2_TEST(rbc_verify_reject_empty_body) {
  Method m = makeMethod("f", "()V", method_flags::Static, 0, 0);
  checkReject(m, "empty method body");
}

B2_TEST(rbc_verify_reject_abstract_with_body) {
  Method m = makeMethod("f", "()V", method_flags::Abstract, 0, 0);
  m.code.push_back(Ins(Op::Return, 0, 0, 0, 0));
  checkReject(m, "abstract");
}

B2_TEST(rbc_verify_reject_ireturn_in_void_method) {
  Method m = makeMethod("f", "()V", method_flags::Static, 1, 0);
  m.code.push_back(Ins(Op::Iconst, 0, 0, 0, 1));
  m.code.push_back(Ins(Op::Ireturn, 0, 0, 0, 0));
  checkReject(m, "does not match method return type");
}

B2_TEST(rbc_verify_reject_malformed_descriptor) {
  // "()X" is not a JVM descriptor; must be reported without crashing.
  Method m = makeMethod("f", "()X", method_flags::Static, 0, 0);
  m.code.push_back(Ins(Op::Return, 0, 0, 0, 0));
  checkReject(m, "malformed method descriptor");
}

B2_TEST(rbc_verify_reject_too_few_locals_for_params) {
  // static f(int) with zero local slots cannot even seed the entry state.
  Method m = makeMethod("f", "(I)I", method_flags::Static, 1, 0);
  m.code.push_back(Ins(Op::Iconst, 0, 0, 0, 1));
  m.code.push_back(Ins(Op::Ireturn, 0, 0, 0, 0));
  checkReject(m, "local slots");
}

// --- totality: hostile input must not crash ----------------------------------

B2_TEST(rbc_verify_wild_operands_no_crash) {
  Method m = makeMethod("g", "([Ljava/lang/String;)V", method_flags::Static, 3,
                        2);
  m.code.push_back(
      Ins(static_cast<Op>(0xFFFF), 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFFu));
  m.code.push_back(Ins(Op::Invokevirtual, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFFFFFFu));
  m.code.push_back(Ins(Op::Tableswitch, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFFu));
  m.code.push_back(Ins(Op::Iinc, 0xFFFF, 0, 0, 0xFFFFFFFFu));
  m.code.push_back(Ins(Op::Return, 0, 0, 0, 0));
  const auto r = verify(m);
  CHECK_MSG(!r.ok, "garbage must be rejected");
  CHECK(!r.diags.empty());
}

}  // namespace
