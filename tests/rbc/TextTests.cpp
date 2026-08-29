// B-2 RBC text-format tests.
//
// The text round-trip is the middle end's stability oracle (docs/
// cpp26_standards.md "Golden IR Tests"): printRbcText must be deterministic
// and parseRbcText(printRbcText(x)) must reproduce x byte-identically. These
// tests round-trip three non-trivial methods (arithmetic+branches+loop,
// try/catch, dense+sparse switches), parse a full embedded program covering
// every const kind, and pin the parser's negative diagnostics (offset > 0,
// stable message substrings) per RbcText.cpp.

#include <string>
#include <vector>

#include "TestHarness.h"
#include "b2/rbc/Opcode.h"
#include "b2/rbc/Rbc.h"
#include "b2/rbc/RbcBuilder.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

using b2::rbc::Const;
using b2::rbc::Ins;
using b2::rbc::Method;
namespace method_flags = b2::rbc::method_flags;
using b2::rbc::Op;
using b2::rbc::parseRbcText;
using b2::rbc::printRbcText;
using b2::rbc::Program;
using b2::rbc::RbcBuilder;
using b2::rbc::verify;

namespace {

std::vector<Op> opcodeSequence(const Method& m) {
  std::vector<Op> seq;
  seq.reserve(m.code.size());
  for (const Ins& i : m.code) {
    seq.push_back(i.opcode());
  }
  return seq;
}

// Builds the loop method used by the round-trip test.
// int sum(int n) { int i = 0, s = 0; while (i < n) { s += i; i++; } return s; }
RbcBuilder buildLoopSum() {
  RbcBuilder b("sum", "(I)I", method_flags::Static);
  b.setLocals(1);
  const std::uint32_t r0 = b.newReg(); // n
  const std::uint32_t r1 = b.newReg(); // i
  const std::uint32_t r2 = b.newReg(); // s
  const RbcBuilder::Label loop = b.newLabel();
  const RbcBuilder::Label done = b.newLabel();
  b.emitRegSlot(Op::Iload, static_cast<std::uint16_t>(r0), 0);   // pc0
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 0);   // pc1
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r2), 0);   // pc2
  b.bind(loop);                                                  // pc3
  b.emit(Op::SafepointPoll);                                     // pc3
  b.emitRegRegBranch(Op::IfIcmpge, static_cast<std::uint16_t>(r1),
                     static_cast<std::uint16_t>(r0), done);       // pc4
  b.emitRegRegReg(Op::Iadd, static_cast<std::uint16_t>(r2),
                  static_cast<std::uint16_t>(r2),
                  static_cast<std::uint16_t>(r1));                // pc5
  b.emitRegImm(Op::Iinc, static_cast<std::uint16_t>(r1), 1);     // pc6
  b.emitBranch(Op::Goto, loop);                                  // pc7
  b.bind(done);                                                  // pc8
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r2));        // pc8
  return b;
}

// Builds the try/catch method used by the round-trip test.
// int guarded(int n) { try { return 100 / n; } catch (...) { return n; } }
RbcBuilder buildTryCatch() {
  RbcBuilder b("guarded", "(I)I", method_flags::Static);
  b.setLocals(1);
  const std::uint32_t r0 = b.newReg();
  const std::uint32_t r1 = b.newReg();
  const RbcBuilder::Label h = b.handlerLabel();
  b.emitRegImm(Op::Iconst, static_cast<std::uint16_t>(r1), 100); // pc0
  b.emitRegSlot(Op::Iload, static_cast<std::uint16_t>(r0), 0);   // pc1
  b.emitRegRegReg(Op::Idiv, static_cast<std::uint16_t>(r1),
                  static_cast<std::uint16_t>(r1),
                  static_cast<std::uint16_t>(r0));                // pc2
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r1));         // pc3
  b.bind(h);                                                      // pc4
  b.emitRegSlot(Op::Iload, static_cast<std::uint16_t>(r0), 0);    // pc4
  b.emitReg(Op::Ireturn, static_cast<std::uint16_t>(r0));         // pc5
  b.addHandler(0, 3, 4, -1);
  return b;
}

// Hand-assembles a method with BOTH canonical switch layouts (dense
// tableswitch + sparse lookupswitch). Hand assembly is required because
// RbcBuilder::emitSwitch emits flat match/target pairs, not the canonical
// layouts the text format round-trips (see BuilderTests.cpp KNOWN BUG note).
Method buildSwitchMethod() {
  Method m;
  m.name = "pick";
  m.descriptor = "(I)I";
  m.flags = method_flags::Static;
  m.numRegs = 2;
  m.numLocals = 1;
  Const table;
  table.kind = Const::Kind::SwitchTable;
  table.ints = {0, 2, 2, 3, 5, 7}; // [low, high, default, t(low..high)]
  Const lookup;
  lookup.kind = Const::Kind::SwitchTable;
  lookup.ints = {3, 11, 10, 13, 20, 15, 30, 17}; // [N, def, match, target...]
  m.cp.push_back(table);
  m.cp.push_back(lookup);
  m.code.push_back(Ins(Op::Iload, 1, 0, 0, 0));       // pc0
  m.code.push_back(Ins(Op::Tableswitch, 0, 1, 0, 0));  // pc1
  m.code.push_back(Ins(Op::Goto, 0, 0, 0, 9));         // pc2 (default path)
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 100));     // pc3 case 0
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));      // pc4
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 200));     // pc5 case 1
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));      // pc6
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 300));     // pc7 case 2
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));      // pc8
  m.code.push_back(Ins(Op::Iload, 1, 0, 0, 0));        // pc9
  m.code.push_back(Ins(Op::Lookupswitch, 0, 1, 0, 1)); // pc10
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, -1));      // pc11 lookup default
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));      // pc12
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 10));      // pc13 match 10
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));      // pc14
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 20));      // pc15 match 20
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));      // pc16
  m.code.push_back(Ins(Op::Iconst, 1, 0, 0, 30));      // pc17 match 30
  m.code.push_back(Ins(Op::Ireturn, 0, 1, 0, 0));      // pc18
  return m;
}

// Asserts print -> parse -> print is byte-identical and that the parsed
// method preserves the opcode sequence and code size of the original.
void checkRoundTrip(const char* tag, const Method& m) {
  const std::string first = printRbcText(m);
  const auto parsed = parseRbcText(first);
  if (!parsed) {
    CHECK_MSG(false, std::string(tag) + ": reparse failed at offset " +
                         std::to_string(parsed.error().offset) + ": " +
                         parsed.error().message);
    return;
  }
  CHECK_MSG(parsed->methods.size() == 1,
            std::string(tag) + ": expected exactly one method");
  if (parsed->methods.size() != 1) {
    return;
  }
  const Method& back = parsed->methods.front();
  const std::string second = printRbcText(back);
  CHECK_MSG(second == first, std::string(tag) + ": round trip not identical");
  CHECK(back.code.size() == m.code.size());
  CHECK(opcodeSequence(back) == opcodeSequence(m));
  CHECK(back.cp.size() == m.cp.size());
  CHECK(back.numRegs == m.numRegs);
  CHECK(back.numLocals == m.numLocals);
  CHECK(back.handlers.size() == m.handlers.size());
  for (std::size_t i = 0; i < m.handlers.size() && i < back.handlers.size();
       ++i) {
    CHECK(back.handlers[i].start == m.handlers[i].start);
    CHECK(back.handlers[i].end == m.handlers[i].end);
    CHECK(back.handlers[i].handler == m.handlers[i].handler);
    CHECK(back.handlers[i].catchType == m.handlers[i].catchType);
  }
}

// Asserts the text fails to parse with offset > 0 and the given substring in
// the message.
void checkTextError(const char* tag, std::string_view text, const char* substr) {
  const auto r = parseRbcText(text);
  if (r) {
    CHECK_MSG(false, std::string(tag) + ": expected a parse error, got a program");
    return;
  }
  const b2::rbc::TextError& e = r.error();
  CHECK_MSG(e.offset > 0,
            std::string(tag) + ": error offset must be > 0, got " +
                std::to_string(e.offset));
  CHECK_MSG(e.message.find(substr) != std::string::npos,
            std::string(tag) + ": expected '" + substr + "' in '" + e.message +
                "'");
}

B2_TEST(rbc_text_roundtrip_loop) {
  RbcBuilder b = buildLoopSum();
  Method m;
  const auto fr = b.finish(m);
  CHECK_MSG(fr.ok, "finish failed: " + fr.error);
  CHECK(m.code.size() == 9);
  CHECK_MSG(verify(m).ok, "loop method must verify before round-tripping");
  checkRoundTrip("loop", m);
}

B2_TEST(rbc_text_roundtrip_try_catch) {
  RbcBuilder b = buildTryCatch();
  Method m;
  const auto fr = b.finish(m);
  CHECK_MSG(fr.ok, "finish failed: " + fr.error);
  CHECK(m.handlers.size() == 1);
  CHECK_MSG(verify(m).ok, "try/catch method must verify before round-tripping");
  checkRoundTrip("try-catch", m);
  // The .catch directive must survive with its exact range and catch type.
  const std::string text = printRbcText(m);
  CHECK(text.find(".catch all from L0 to L3 handler L4") != std::string::npos);
}

B2_TEST(rbc_text_roundtrip_switches) {
  const Method m = buildSwitchMethod();
  CHECK_MSG(verify(m).ok, "switch method must verify before round-tripping");
  checkRoundTrip("switches", m);
  // The printer renders the table const as a dense match list and the lookup
  // const as the sorted sparse list, each with its default entry.
  const std::string text = printRbcText(m);
  CHECK(text.find("switch { 0 : L3 1 : L5 2 : L7 default : L2 }") !=
        std::string::npos);
  CHECK(text.find("switch { 10 : L13 20 : L15 30 : L17 default : L11 }") !=
        std::string::npos);
  // Both switch instructions survive with their cp operands.
  const auto parsed = parseRbcText(text);
  if (parsed && parsed->methods.size() == 1) {
    const Method& back = parsed->methods.front();
    CHECK(back.code[1].opcode() == Op::Tableswitch && back.code[1].imm == 0);
    CHECK(back.code[10].opcode() == Op::Lookupswitch && back.code[10].imm == 1);
    CHECK(back.cp[0].ints == m.cp[0].ints);
    CHECK(back.cp[1].ints == m.cp[1].ints);
  }
}

B2_TEST(rbc_text_parse_embedded_program) {
  static constexpr std::string_view kText =
      "# top-level comment\n"
      ".class demo/Sample\n"
      "\n"
      ".method public static final add (II)I\n"
      ".regs 4\n"
      ".locals 2\n"
      ".const c0 = int 42\n"
      ".const c1 = long 12345678901\n"
      ".const c2 = float 1.5\n"
      ".const c3 = double -0.25\n"
      ".const c4 = string \"hello\\nworld\"\n"
      ".const c5 = class \"java/lang/String\"\n"
      ".const c6 = field Demo x I\n"
      ".const c7 = method Demo add (II)I\n"
      ".const c8 = imethod Runnable run ()V\n"
      ".const c9 = nametype value I\n"
      ".const c10 = methodtype (I)V\n"
      ".const c11 = methodhandle getField c6\n"
      ".const c12 = indy bootstrap (I)V\n"
      "iload r0 l0\n"
      "iload r1 l1\n"
      "iadd r2 r0 r1\n"
      "iconst r3 1\n"
      "iadd r2 r2 r3\n"
      "ireturn r2\n"
      ".end\n"
      "\n"
      "# abstract methods carry no body\n"
      ".method abstract runIt ()V\n"
      ".end\n";
  const auto r = parseRbcText(kText);
  if (!r) {
    CHECK_MSG(false, "parse failed at offset " +
                         std::to_string(r.error().offset) + ": " +
                         r.error().message);
    return;
  }
  CHECK(r->className == "demo/Sample");
  CHECK(r->methods.size() == 2);
  if (r->methods.size() != 2) {
    return;
  }
  const Method& m0 = r->methods[0];
  CHECK(m0.name == "add");
  CHECK(m0.descriptor == "(II)I");
  CHECK(m0.flags ==
        (method_flags::Public | method_flags::Static | method_flags::Final));
  CHECK(m0.numRegs == 4);
  CHECK(m0.numLocals == 2);
  CHECK(m0.code.size() == 6);
  CHECK(m0.cp.size() == 13);
  CHECK_MSG(verify(m0).ok, "embedded concrete method must verify");
  const Method& m1 = r->methods[1];
  CHECK(m1.name == "runIt");
  CHECK(m1.isAbstract());
  CHECK(m1.code.empty());
  CHECK_MSG(verify(m1).ok, "embedded abstract method must verify");
  // Exact const payloads.
  CHECK(m0.cp[0].kind == Const::Kind::Int32 && m0.cp[0].i32 == 42);
  CHECK(m0.cp[1].kind == Const::Kind::Int64 &&
        m0.cp[1].i64 == 12345678901LL);
  CHECK(m0.cp[2].kind == Const::Kind::Float && m0.cp[2].f32 == 1.5F);
  CHECK(m0.cp[3].kind == Const::Kind::Double && m0.cp[3].f64 == -0.25);
  CHECK(m0.cp[4].kind == Const::Kind::String && m0.cp[4].str == "hello\nworld");
  CHECK(m0.cp[5].kind == Const::Kind::Class &&
        m0.cp[5].str == "java/lang/String");
  CHECK(m0.cp[6].kind == Const::Kind::FieldRef);
  CHECK(m0.cp[6].str == "Demo" && m0.cp[6].str2 == "x" && m0.cp[6].str3 == "I");
  CHECK(m0.cp[7].kind == Const::Kind::MethodRef);
  CHECK(m0.cp[7].str == "Demo" && m0.cp[7].str2 == "add" &&
        m0.cp[7].str3 == "(II)I");
  CHECK(m0.cp[8].kind == Const::Kind::InterfaceMethodRef);
  CHECK(m0.cp[9].kind == Const::Kind::NameType);
  CHECK(m0.cp[9].str == "value" && m0.cp[9].str2 == "I");
  CHECK(m0.cp[10].kind == Const::Kind::MethodType &&
        m0.cp[10].str == "(I)V");
  CHECK(m0.cp[11].kind == Const::Kind::MethodHandle);
  CHECK(m0.cp[11].str == "getField" && m0.cp[11].str2 == "c6");
  CHECK(m0.cp[12].kind == Const::Kind::InvokeDynamic);
  CHECK(m0.cp[12].str == "bootstrap" && m0.cp[12].str2 == "(I)V");
}

B2_TEST(rbc_text_printed_form_reparses_identically) {
  const auto r = parseRbcText(
      ".class demo/Sample\n"
      "\n"
      ".method public static final add (II)I\n"
      ".regs 4\n"
      ".locals 2\n"
      ".const c0 = float 1.5\n"
      ".const c1 = double -0.25\n"
      "iload r0 l0\n"
      "iload r1 l1\n"
      "iadd r2 r0 r1\n"
      "ireturn r2\n"
      ".end\n");
  if (!r) {
    CHECK_MSG(false, "parse failed: " + r.error().message);
    return;
  }
  const std::string once = printRbcText(*r);
  const auto again = parseRbcText(once);
  CHECK_MSG(again.has_value(), "printed form must reparse");
  if (again) {
    CHECK(printRbcText(*again) == once);
  }
}

B2_TEST(rbc_text_print_twice_identical) {
  RbcBuilder b = buildLoopSum();
  Method m;
  CHECK(b.finish(m).ok);
  const std::string a = printRbcText(m);
  const std::string c = printRbcText(m);
  CHECK(a == c);
  Program p;
  p.className = "Round/Trip";
  p.methods.push_back(m);
  const std::string pa = printRbcText(p);
  const std::string pc = printRbcText(p);
  CHECK(pa == pc);
  // printRbcText(Method) is exactly the method's section of the program form.
  CHECK(pa == ".class Round/Trip\n\n" + a);
}

B2_TEST(rbc_text_flag_order_canonical) {
  // Flags supplied as a raw bitmask in non-canonical order still print in
  // the fixed kFlags order: public private protected static final
  // synchronized native abstract varargs.
  Method m;
  m.name = "flags";
  m.descriptor = "()V";
  m.flags = method_flags::Final | method_flags::Varargs | method_flags::Public |
            method_flags::Static;
  m.numRegs = 0;
  m.numLocals = 0;
  m.code.push_back(Ins(Op::Return, 0, 0, 0, 0));
  const std::string text = printRbcText(m);
  const std::string header = text.substr(0, text.find('\n'));
  CHECK(header == ".method public static final varargs flags ()V");
}

B2_TEST(rbc_text_error_unknown_mnemonic) {
  checkTextError("unknown-mnemonic",
                 ".method static f ()V\n.regs 0\nfrobnicate r0\nreturn\n.end\n",
                 "unknown mnemonic");
}

B2_TEST(rbc_text_error_missing_operands) {
  // iadd needs dst, a, b: "iadd r1" is missing two register operands.
  checkTextError("missing-operands",
                 ".method static f ()V\n.regs 2\niadd r1\nreturn\n.end\n",
                 "expected register");
}

B2_TEST(rbc_text_error_unbound_label) {
  checkTextError("unbound-label",
                 ".method static f ()V\n.regs 0\ngoto nowhere\nreturn\n.end\n",
                 "undefined label");
}

B2_TEST(rbc_text_error_duplicate_const) {
  checkTextError("duplicate-const",
                 ".method static f ()V\n.regs 0\n"
                 ".const c0 = int 1\n.const c0 = int 2\nreturn\n.end\n",
                 "redefinition of constant");
}

B2_TEST(rbc_text_error_unterminated_string) {
  checkTextError("unterminated-string",
                 ".method static f ()V\n.regs 0\n"
                 ".const c0 = string \"abc\nreturn\n.end\n",
                 "unterminated string");
}

B2_TEST(rbc_text_error_missing_end) {
  checkTextError("missing-end", ".method static f ()V\n.regs 0\nreturn\n",
                 "missing .end");
}

B2_TEST(rbc_text_error_switch_without_default) {
  checkTextError(
      "switch-no-default",
      ".method static f (I)I\n.regs 1\n.locals 1\n"
      ".const c0 = switch { 1 : L2 }\n"
      "iload r0 l0\nlookupswitch r0 c0\niconst r0 0\nireturn r0\n"
      "L2:\niconst r0 1\nireturn r0\n.end\n",
      "'default'");
}

B2_TEST(rbc_text_error_unknown_flag_word) {
  checkTextError("unknown-flag",
                 ".method statik f ()V\n.regs 0\nreturn\n.end\n",
                 "unknown method flag");
}

B2_TEST(rbc_text_error_label_must_own_line) {
  checkTextError("label-not-own-line",
                 ".method static f ()V\n.regs 0\nL0: return\n.end\n",
                 "own line");
}

}  // namespace
