// B-2 RBC builder tests.
//
// RbcBuilder is the only sanctioned way to produce methods (no hand-assembled
// Ins records outside the verifier tests); these tests cover the emission
// factories, label resolution at finish(), the documented finish() error
// channels, constant-pool interning, switch payload construction and handler
// registration, and that well-formed builder output passes b2::rbc::verify().
//
// emitSwitch emits the CANONICAL SwitchTable layouts shared with the
// verifier and the text format (docs/rbc_spec.md): tableswitch
// [low, high, default, targets...] and lookupswitch
// [N, default, match, target, ...]; the default is the fall-through pc+1.
// The switch tests below pin those layouts, the sorted-match guarantee,
// and the finish() misuse error channels.

#include <string>
#include <vector>

#include "TestHarness.h"
#include "b2/rbc/Opcode.h"
#include "b2/rbc/Rbc.h"
#include "b2/rbc/RbcBuilder.h"
#include "b2/rbc/Verifier.h"

using b2::rbc::Ins;
using b2::rbc::Method;
namespace method_flags = b2::rbc::method_flags;
using b2::rbc::Op;
using b2::rbc::RbcBuilder;
using b2::rbc::verify;

namespace {

// static int add(int a, int b) { return a + b; }
// pc0: iload r0 l0 / pc1: iload r1 l1 / pc2: iadd r2 r0 r1 / pc3: ireturn r2
RbcBuilder makeStaticAdd() {
  RbcBuilder b("add", "(II)I", method_flags::Static);
  b.setLocals(2);
  const std::uint32_t r0 = b.newReg();
  const std::uint32_t r1 = b.newReg();
  const std::uint32_t r2 = b.newReg();
  b.emitRegSlot(Op::Iload, static_cast<std::uint16_t>(r0), 0);
  b.emitRegSlot(Op::Iload, static_cast<std::uint16_t>(r1), 1);
  b.emitRegRegReg(Op::Iadd, static_cast<std::uint16_t>(r2),
                 static_cast<std::uint16_t>(r0), static_cast<std::uint16_t>(r1));
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r2));
  return b;
}

B2_TEST(rbc_builder_static_add) {
  RbcBuilder b = makeStaticAdd();
  Method m;
  const auto fr = b.finish(m);
  CHECK_MSG(fr.ok, "finish failed: " + fr.error);
  CHECK(m.name == "add");
  CHECK(m.descriptor == "(II)I");
  CHECK(m.isStatic());
  CHECK(m.numRegs == 3);
  CHECK(m.numLocals == 2);
  CHECK(m.code.size() == 4);
  if (m.code.size() == 4) {
    CHECK(m.code[0].opcode() == Op::Iload);
    CHECK(m.code[0].dst == 0);
    CHECK(m.code[0].imm == 0);
    CHECK(m.code[1].opcode() == Op::Iload);
    CHECK(m.code[1].dst == 1);
    CHECK(m.code[1].imm == 1);
    CHECK(m.code[2].opcode() == Op::Iadd);
    CHECK(m.code[2].dst == 2);
    CHECK(m.code[2].a == 0);
    CHECK(m.code[2].b == 1);
    CHECK(m.code[3].opcode() == Op::Ireturn);
    CHECK(m.code[3].a == 2);
  }
  const auto vr = verify(m);
  CHECK_MSG(vr.ok, vr.diags.empty() ? "verify failed"
                                    : "verify failed: " + vr.diags.front().message);
}

B2_TEST(rbc_builder_forward_backward_branches) {
  // int loop(int n) { int i = 0, s = 0; while (i < n) { s += i; i++; } return s; }
  // pc0 iload / pc1 iconst / pc2 poll / pc3 if_icmpge -> Lend(6, forward) /
  // pc4 iinc / pc5 goto -> Lloop(2, backward) / pc6 ireturn
  RbcBuilder b("loop", "(I)I", method_flags::Static);
  b.setLocals(1);
  const std::uint32_t r0 = b.newReg(); // n
  const std::uint32_t r1 = b.newReg(); // i
  const RbcBuilder::Label loop = b.newLabel();
  const RbcBuilder::Label end = b.newLabel();
  b.emitRegSlot(Op::Iload, static_cast<std::uint16_t>(r0), 0); // pc0
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 0); // pc1
  b.bind(loop);                                                // pc2
  b.emit(Op::SafepointPoll);                                   // pc2
  b.emitRegRegBranch(Op::IfIcmpge, static_cast<std::uint16_t>(r1),
                     static_cast<std::uint16_t>(r0), end);      // pc3
  b.emitRegImm(Op::Iinc, static_cast<std::uint16_t>(r1), 1);    // pc4
  b.emitBranch(Op::Goto, loop);                                // pc5
  b.bind(end);                                                 // pc6
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));       // pc6
  Method m;
  const auto fr = b.finish(m);
  CHECK_MSG(fr.ok, "finish failed: " + fr.error);
  CHECK(m.code.size() == 7);
  if (m.code.size() == 7) {
    CHECK(m.code[2].opcode() == Op::SafepointPoll);
    CHECK(m.code[3].opcode() == Op::IfIcmpge);
    CHECK(m.code[3].imm == 6); // forward branch resolves to Lend's index
    CHECK(m.code[5].opcode() == Op::Goto);
    CHECK(m.code[5].imm == 2); // backward branch resolves to Lloop's index
    CHECK(m.code[6].opcode() == Op::Ireturn);
  }
  CHECK_MSG(verify(m).ok, "loop method must verify");
}

B2_TEST(rbc_builder_unbound_label_fails) {
  RbcBuilder b("f", "()V", method_flags::Static);
  const RbcBuilder::Label dangling = b.newLabel();
  b.emitBranch(Op::Goto, dangling); // pc0, never bound
  b.emit(Op::Return);               // pc1
  Method m;
  const auto fr = b.finish(m);
  CHECK(!fr.ok);
  CHECK_MSG(fr.error.find("not bound") != std::string::npos,
            "expected 'not bound' diagnostic, got: " + fr.error);
}

B2_TEST(rbc_builder_branch_past_end_fails) {
  RbcBuilder b("f", "()V", method_flags::Static);
  const RbcBuilder::Label pastEnd = b.newLabel();
  b.emitBranch(Op::Goto, pastEnd); // pc0
  b.emit(Op::Return);              // pc1
  b.bind(pastEnd);                 // binds at code size 2 == past the end
  Method m;
  const auto fr = b.finish(m);
  CHECK(!fr.ok);
  CHECK_MSG(fr.error.find("out of range") != std::string::npos,
            "expected 'out of range' diagnostic, got: " + fr.error);
}

B2_TEST(rbc_builder_register_out_of_range_fails) {
  RbcBuilder b("f", "()V", method_flags::Static);
  b.setRegs(1);
  b.emit(Op::Return);                        // pc0
  b.emitRegImm(Op::Iconst, 1, 7);            // pc1: r1 >= numRegs 1
  Method m;
  const auto fr = b.finish(m);
  CHECK(!fr.ok);
  CHECK_MSG(fr.error.find("register r1") != std::string::npos,
            "error must name the offending register, got: " + fr.error);
  CHECK_MSG(fr.error.find("out of range") != std::string::npos,
            "error must say 'out of range', got: " + fr.error);
  CHECK_MSG(fr.error.find("pc 1") != std::string::npos,
            "error must name the pc, got: " + fr.error);
}

B2_TEST(rbc_builder_slot_out_of_range_fails) {
  RbcBuilder b("f", "()V", method_flags::Static);
  b.setRegs(1);
  b.setLocals(0);
  b.emitRegSlot(Op::Iload, 0, 1); // pc0: slot 1 >= numLocals 0
  b.emit(Op::Return);             // pc1
  Method m;
  const auto fr = b.finish(m);
  CHECK(!fr.ok);
  CHECK_MSG(fr.error.find("local slot 1") != std::string::npos,
            "error must name the offending slot, got: " + fr.error);
  CHECK_MSG(fr.error.find("out of range") != std::string::npos,
            "error must say 'out of range', got: " + fr.error);
}

B2_TEST(rbc_builder_newreg_sequence) {
  RbcBuilder b("f", "()I", method_flags::Static);
  CHECK(b.newReg() == 0);
  CHECK(b.newReg() == 1);
  CHECK(b.newReg() == 2);
  b.emitRegImm(Op::Iconst, 2, 5);
  b.emitReg(Op::Ireturn, 2);
  Method m;
  const auto fr = b.finish(m);
  CHECK_MSG(fr.ok, "finish failed: " + fr.error);
  CHECK(m.numRegs == 3); // newReg grew the frame; bound check passes
  CHECK_MSG(verify(m).ok, "newReg-allocated method must verify");
}

B2_TEST(rbc_builder_const_interning) {
  RbcBuilder b("f", "()V", method_flags::Static);
  b.emit(Op::Return);
  CHECK(b.constInt(5) == b.constInt(5));     // same value -> same index
  CHECK(b.constInt(5) != b.constInt(6));     // different -> different index
  CHECK(b.constString("hi") == b.constString("hi"));
  CHECK(b.constString("hi") != b.constString("bye"));
  CHECK(b.constLong(12345678901LL) == b.constLong(12345678901LL));
  CHECK(b.constFloat(1.5F) == b.constFloat(1.5F));
  CHECK(b.constDouble(-0.25) == b.constDouble(-0.25));
  CHECK(b.constClass("a/B") == b.constClass("a/B"));
  CHECK(b.constFieldRef("C", "x", "I") == b.constFieldRef("C", "x", "I"));
  CHECK(b.constFieldRef("C", "x", "I") != b.constFieldRef("C", "y", "I"));
  Method m;
  const auto fr = b.finish(m);
  CHECK_MSG(fr.ok, "finish failed: " + fr.error);
  // Distinct constants: int5, int6, "hi", "bye", long, float, double, class,
  // field x, field y -> 10 entries in first-use order.
  CHECK(m.cp.size() == 10);
}

B2_TEST(rbc_builder_const_payloads) {
  RbcBuilder b("f", "()V", method_flags::Static);
  b.emit(Op::Return);
  const std::uint32_t ci = b.constInt(-42);
  const std::uint32_t cl = b.constLong(12345678901LL);
  const std::uint32_t cf = b.constFloat(1.5F);
  const std::uint32_t cd = b.constDouble(-0.25);
  const std::uint32_t cs = b.constString("payload");
  const std::uint32_t cc = b.constClass("java/lang/String");
  const std::uint32_t cfr = b.constFieldRef("Owner", "size", "I");
  const std::uint32_t cmr = b.constMethodRef("Owner", "run", "(I)V");
  Method m;
  const auto fr = b.finish(m);
  CHECK_MSG(fr.ok, "finish failed: " + fr.error);
  CHECK(m.cp.size() == 8);
  CHECK(ci == 0 && cl == 1 && cf == 2 && cd == 3);
  CHECK(cs == 4 && cc == 5 && cfr == 6 && cmr == 7);
  CHECK(m.cp[ci].kind == b2::rbc::Const::Kind::Int32);
  CHECK(m.cp[ci].i32 == -42);
  CHECK(m.cp[cl].kind == b2::rbc::Const::Kind::Int64);
  CHECK(m.cp[cl].i64 == 12345678901LL);
  CHECK(m.cp[cf].kind == b2::rbc::Const::Kind::Float);
  CHECK(m.cp[cf].f32 == 1.5F);
  CHECK(m.cp[cd].kind == b2::rbc::Const::Kind::Double);
  CHECK(m.cp[cd].f64 == -0.25);
  CHECK(m.cp[cs].kind == b2::rbc::Const::Kind::String);
  CHECK(m.cp[cs].str == "payload");
  CHECK(m.cp[cc].kind == b2::rbc::Const::Kind::Class);
  CHECK(m.cp[cc].str == "java/lang/String");
  CHECK(m.cp[cfr].kind == b2::rbc::Const::Kind::FieldRef);
  CHECK(m.cp[cfr].str == "Owner" && m.cp[cfr].str2 == "size" &&
        m.cp[cfr].str3 == "I");
  CHECK(m.cp[cmr].kind == b2::rbc::Const::Kind::MethodRef);
  CHECK(m.cp[cmr].str == "Owner" && m.cp[cmr].str2 == "run" &&
        m.cp[cmr].str3 == "(I)V");
}

B2_TEST(rbc_builder_switch_dense_canonical_table) {
  // emitSwitch(Tableswitch, matches, targets) must produce the canonical
  // table layout [low, high, default, target(low..high)...] with the
  // default as the fall-through pc+1 - and the method must VERIFY.
  RbcBuilder b("sw", "(I)I", method_flags::Static);
  b.setLocals(1);
  const std::uint32_t r0 = b.newReg();
  const std::uint32_t r1 = b.newReg();
  const RbcBuilder::Label l0 = b.newLabel();
  const RbcBuilder::Label l1 = b.newLabel();
  const RbcBuilder::Label l2 = b.newLabel();
  b.emitRegSlot(Op::Iload, static_cast<std::uint16_t>(r0), 0); // pc0
  b.emitSwitch(Op::Tableswitch, static_cast<std::uint16_t>(r0), {0, 1, 2},
               {l0, l1, l2});                                   // pc1
  b.bind(l0);                                                   // pc2
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 10); // pc2
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));       // pc3
  b.bind(l1);                                                   // pc4
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 20); // pc4
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));       // pc5
  b.bind(l2);                                                   // pc6
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 30); // pc6
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));       // pc7
  Method m;
  const auto fr = b.finish(m);
  CHECK_MSG(fr.ok, "finish failed: " + fr.error);
  CHECK(m.code.size() == 8);
  CHECK(m.code[1].opcode() == Op::Tableswitch);
  CHECK(m.code[1].a == r0);
  CHECK(m.code[1].imm == 0); // cp index of the (first) switch const
  // Canonical table layout: [low=0, high=2, default=pc2, t(0)=2, t(1)=4, t(2)=6]
  const std::vector<std::int32_t> want{0, 2, 2, 2, 4, 6};
  CHECK(m.cp[0].kind == b2::rbc::Const::Kind::SwitchTable);
  CHECK(m.cp[0].ints == want);
  const auto vr = verify(m);
  CHECK_MSG(vr.ok, vr.diags.empty() ? "verify failed"
                                    : "verify failed: " + vr.diags.front().message);
}

B2_TEST(rbc_builder_switch_sparse_lookup_verifies) {
  // emitSwitch(Lookupswitch, matches, targets) must produce the canonical
  // lookup layout [N, default, match, target, ...] - and the method must
  // VERIFY (the pre-canonicalization defect produced flat pairs here, which
  // the verifier rightly rejected; pinned fixed).
  RbcBuilder b("sw", "(I)I", method_flags::Static);
  b.setLocals(1);
  const std::uint32_t r0 = b.newReg();
  const std::uint32_t r1 = b.newReg();
  const RbcBuilder::Label t1 = b.newLabel();
  const RbcBuilder::Label t5 = b.newLabel();
  const RbcBuilder::Label t9 = b.newLabel();
  b.emitRegSlot(Op::Iload, static_cast<std::uint16_t>(r0), 0); // pc0
  b.emitSwitch(Op::Lookupswitch, static_cast<std::uint16_t>(r0), {1, 5, 9},
               {t1, t5, t9});                                   // pc1
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), -1); // pc2 default
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));       // pc3
  b.bind(t1);                                                   // pc4
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 1);  // pc4
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));       // pc5
  b.bind(t5);                                                   // pc6
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 5);  // pc6
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));       // pc7
  b.bind(t9);                                                   // pc8
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 9);  // pc8
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));       // pc9
  Method m;
  const auto fr = b.finish(m);
  CHECK_MSG(fr.ok, "finish failed: " + fr.error);
  // Canonical lookup layout: [N=3, default=2, 1->4, 5->6, 9->8].
  const std::vector<std::int32_t> want{3, 2, 1, 4, 5, 6, 9, 8};
  CHECK(m.cp[0].kind == b2::rbc::Const::Kind::SwitchTable);
  CHECK(m.cp[0].ints == want);
  const auto vr = verify(m);
  CHECK_MSG(vr.ok, vr.diags.empty() ? "verify failed"
                                    : "verify failed: " + vr.diags.front().message);
}

B2_TEST(rbc_builder_switch_unsorted_matches_sorted_canonically) {
  // emitSwitch sorts (match, target) pairs by match regardless of caller
  // order - the canonical lookup layout requires strictly increasing
  // matches. Input {9, 1, 5} must come out sorted 1, 5, 9 with the right
  // targets, and the method must verify.
  RbcBuilder b("sw", "(I)I", method_flags::Static);
  b.setLocals(1);
  const std::uint32_t r0 = b.newReg();
  const std::uint32_t r1 = b.newReg();
  const RbcBuilder::Label t1 = b.newLabel();
  const RbcBuilder::Label t5 = b.newLabel();
  const RbcBuilder::Label t9 = b.newLabel();
  b.emitRegSlot(Op::Iload, static_cast<std::uint16_t>(r0), 0);  // pc0
  b.emitSwitch(Op::Lookupswitch, static_cast<std::uint16_t>(r0), {9, 1, 5},
               {t9, t1, t5});                                    // pc1
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), -1); // pc2 default
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));       // pc3
  b.bind(t1);                                                    // pc4
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 1);   // pc4
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));        // pc5
  b.bind(t5);                                                    // pc6
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 5);   // pc6
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));        // pc7
  b.bind(t9);                                                    // pc8
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 9);   // pc8
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));        // pc9
  Method m;
  const auto fr = b.finish(m);
  CHECK_MSG(fr.ok, "finish failed: " + fr.error);
  // Sorted canonical lookup layout: [N=3, default=2, 1->4, 5->6, 9->8].
  const std::vector<std::int32_t> want{3, 2, 1, 4, 5, 6, 9, 8};
  CHECK(m.cp[0].ints == want);
  const auto vr = verify(m);
  CHECK_MSG(vr.ok, vr.diags.empty() ? "verify failed"
                                    : "verify failed: " + vr.diags.front().message);
}

B2_TEST(rbc_builder_switch_misuse_fails_finish) {
  // emitSwitch has no error channel, so misuse is recorded and fails
  // finish(): duplicate matches, and non-contiguous tableswitch matches
  // (the caller must use lookupswitch for sparse matches).
  {
    RbcBuilder b("sw", "(I)I", method_flags::Static);
    b.setLocals(1);
    const std::uint32_t r0 = b.newReg();
    const RbcBuilder::Label t = b.newLabel();
    b.emitSwitch(Op::Lookupswitch, static_cast<std::uint16_t>(r0), {7, 7},
                 {t, t});
    b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r0));
    b.bind(t);
    b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r0));
    Method m;
    const auto fr = b.finish(m);
    CHECK_MSG(!fr.ok, "duplicate switch match must fail finish()");
    if (!fr.ok) {
      CHECK_MSG(fr.error.find("duplicate match") != std::string::npos,
                "expected 'duplicate match', got: " + fr.error);
    }
  }
  {
    RbcBuilder b("sw", "(I)I", method_flags::Static);
    b.setLocals(1);
    const std::uint32_t r0 = b.newReg();
    const RbcBuilder::Label t0 = b.newLabel();
    const RbcBuilder::Label t5 = b.newLabel();
    b.emitSwitch(Op::Tableswitch, static_cast<std::uint16_t>(r0), {0, 5},
                 {t0, t5});
    b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r0));
    b.bind(t0);
    b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r0));
    b.bind(t5);
    b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r0));
    Method m;
    const auto fr = b.finish(m);
    CHECK_MSG(!fr.ok, "non-contiguous tableswitch matches must fail finish()");
    if (!fr.ok) {
      CHECK_MSG(fr.error.find("contiguous") != std::string::npos,
                "expected 'contiguous', got: " + fr.error);
    }
  }
}

B2_TEST(rbc_builder_handler_label_and_add_handler) {
  // static int guarded(int n) { try { return 100 / n; } catch (...) { return n; } }
  RbcBuilder b("guarded", "(I)I", method_flags::Static);
  b.setLocals(1);
  const std::uint32_t r0 = b.newReg();
  const std::uint32_t r1 = b.newReg();
  const RbcBuilder::Label h = b.handlerLabel();
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 100); // pc0
  b.emitRegSlot(Op::Iload, static_cast<std::uint16_t>(r0), 0);   // pc1
  b.emitRegRegReg(Op::Idiv, static_cast<std::uint16_t>(r1),
                 static_cast<std::uint16_t>(r1),
                 static_cast<std::uint16_t>(r0));                // pc2 (traps)
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));        // pc3
  b.bind(h);                                                     // pc4
  b.emitRegSlot(Op::Iload, static_cast<std::uint16_t>(r0), 0);   // pc4
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r0));        // pc5
  b.addHandler(0, 3, 4, -1); // [0,3): iconst, iload, idiv; handler pc4, all
  Method m;
  const auto fr = b.finish(m);
  CHECK_MSG(fr.ok, "finish failed: " + fr.error);
  CHECK(m.handlers.size() == 1);
  if (m.handlers.size() == 1) {
    CHECK(m.handlers[0].start == 0);
    CHECK(m.handlers[0].end == 3);
    CHECK(m.handlers[0].handler == 4);
    CHECK(m.handlers[0].catchType == -1); // catch-all
  }
  const auto vr = verify(m);
  CHECK_MSG(vr.ok, vr.diags.empty() ? "verify failed"
                                    : "verify failed: " + vr.diags.front().message);
}

B2_TEST(rbc_builder_handler_range_checked_at_finish) {
  // Out-of-code handler ranges are builder-level misuse: finish() must fail.
  RbcBuilder b("f", "()V", method_flags::Static);
  b.emit(Op::Return);
  b.addHandler(0, 9, 99, -1); // end 9 and entry 99 exceed code size 1
  Method m;
  const auto fr = b.finish(m);
  CHECK(!fr.ok);
  CHECK_MSG(fr.error.find("out of") != std::string::npos,
            "expected handler range diagnostic, got: " + fr.error);
}

}  // namespace
