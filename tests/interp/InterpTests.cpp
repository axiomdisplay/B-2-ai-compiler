// B-2 interpreter tests - the Tier-0 semantic oracle suite (Task BE-4).
//
// WHY THIS FILE EXISTS:
// T0 is the reference implementation against the Java semantic oracle
// (Rule 67, Rule 133) and the differential baseline for every future tier.
// This suite executes RBC programs end to end through
// b2::interp::Interpreter and asserts Java semantics exactly as pinned by
// docs/rbc_spec.md SS3 (opcode semantics), the normative comments in
// include/b2/interp/{Interp,Frame,Runtime,Heap,Value}.h (the dispatch-loop,
// call-protocol, exception-algorithm and quickened-opcode pins), and the
// JVM's own observable behavior (exception classes and messages, IEEE 754
// arithmetic, JLS 5.1.3 conversion clamping - Rule 72).
//
// DISCIPLINE:
// - Programs are written as embedded RBC text (parseRbcText), the same
//   surface the corpus and b2rbc use; quickened-offset and deopt-resume
//   cases that the text grammar cannot spell are built as Ins records in
//   C++ (the interpreter's contract allows both entry paths).
// - Every test is deterministic: no clocks, no addresses, no iteration
//   over unordered structures whose order is observable.
// - Divergences pinned by the implementation comments (println value-tag
//   formatting for (Z)/(C), exact-descriptor array assignability, guards
//   are no-ops, deopt_trap -> InternalError, trailing multianewarray dims
//   are length-0 arrays) are asserted AS PINNED, with a comment at each.
// - KNOWN VERIFIER LIMITATION worked around throughout: the exception-edge
//   join in compiler/rbc/src/Verifier.cpp resets the seeded handler-entry
//   r0 = Ref back to Bottom whenever a protected range covers more than one
//   instruction (reported to the integrator; see the worklog). Tests that
//   READ r0 in a handler therefore pin their protected range to exactly
//   one instruction, which is legal RBC and exercises the same pin.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "TestHarness.h"
#include "b2/interp/Frame.h"
#include "b2/interp/Interp.h"
#include "b2/interp/Runtime.h"
#include "b2/rbc/Rbc.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

namespace {

using b2::interp::dumpFrames;
using b2::interp::Frame;
using b2::interp::InterpConfig;
using b2::interp::Interpreter;
using b2::interp::ObjRef;
using b2::interp::RunResult;
using b2::interp::RunStatus;
using b2::interp::Value;
namespace rbc = b2::rbc;
using b2::rbc::parseRbcText;
using b2::rbc::Program;

// ===========================================================================
// Harness: one run = program + interpreter + result, kept alive together
// (the Interpreter holds a const& to the Program; both must outlive it).
// ===========================================================================

struct RunCtx {
  std::unique_ptr<Program> program;
  std::unique_ptr<Interpreter> interp;
  RunResult result{};

  [[nodiscard]] bool ok() const { return program != nullptr; }
  [[nodiscard]] b2::interp::Runtime& rt() { return interp->runtime(); }
  [[nodiscard]] const b2::interp::Runtime& rt() const { return interp->runtime(); }
  [[nodiscard]] const RunResult& res() const { return result; }
};

// Formats everything a failed assertion wants to see (never crashes on a
// failed parse: the fields stay empty).
[[nodiscard]] std::string describe(const RunCtx& c) {
  if (!c.ok()) {
    return "program failed to parse";
  }
  std::string out = "status=";
  switch (c.result.status) {
  case RunStatus::Returned:
    out += "Returned";
    break;
  case RunStatus::Threw:
    out += "Threw(" + std::string(c.rt().classNameOf(c.result.exception)) +
           ", msg=\"" + std::string(c.rt().exceptionMessage(c.result.exception)) +
           "\")";
    break;
  case RunStatus::VerifyFailed:
    out += "VerifyFailed";
    break;
  }
  out += " result={type=" + std::to_string(static_cast<int>(c.result.result.type)) +
         " i=" + std::to_string(c.result.result.as.i) + "}";
  for (const auto& d : c.result.verifyDiags) {
    out += "\n  verify pc=" + std::to_string(d.pc) + ": " + d.message;
  }
  return out;
}

RunCtx runProgram(const std::string& text, const InterpConfig& cfg = {},
                  std::string_view name = "main",
                  std::string_view descriptor = "()I") {
  RunCtx ctx;
  auto parsed = parseRbcText(text);
  if (!parsed) {
    b2::test::recordFailure(__FILE__, __LINE__,
                            "program did not parse (offset " +
                                std::to_string(parsed.error().offset) + "): " +
                                parsed.error().message);
    return ctx;
  }
  ctx.program = std::make_unique<Program>(std::move(*parsed));
  ctx.interp = std::make_unique<Interpreter>(*ctx.program, cfg);
  ctx.result = ctx.interp->run(name, descriptor, {});
  return ctx;
}

// --- assertion helpers (record rich failures, keep tests one line each) ----

void checkReturned(const RunCtx& c, const char* what) {
  if (!c.ok() || c.result.status != RunStatus::Returned) {
    b2::test::recordFailure(__FILE__, __LINE__,
                            std::string(what) + ": expected Returned, got " +
                                describe(c));
  }
}

void checkResultInt(const RunCtx& c, std::int32_t want, std::string_view what) {
  if (!c.ok() || c.result.status != RunStatus::Returned ||
      c.result.result.type != b2::rbc::RType::Int ||
      c.result.result.as.i != want) {
    b2::test::recordFailure(__FILE__, __LINE__,
                            std::string(what) + ": expected int " +
                                std::to_string(want) + ", got " + describe(c));
  }
}

void checkResultLong(const RunCtx& c, std::int64_t want, const char* what) {
  if (!c.ok() || c.result.status != RunStatus::Returned ||
      c.result.result.type != b2::rbc::RType::Long ||
      c.result.result.as.l != want) {
    b2::test::recordFailure(__FILE__, __LINE__,
                            std::string(what) + ": expected long " +
                                std::to_string(want) + ", got " + describe(c));
  }
}

void checkResultFloatBits(const RunCtx& c, std::uint32_t wantBits,
                          const char* what) {
  const std::uint32_t gotBits =
      c.ok() ? std::bit_cast<std::uint32_t>(c.result.result.as.f) : 0u;
  if (!c.ok() || c.result.status != RunStatus::Returned ||
      c.result.result.type != b2::rbc::RType::Float || gotBits != wantBits) {
    b2::test::recordFailure(__FILE__, __LINE__,
                            std::string(what) + ": expected float bits 0x" +
                                std::to_string(wantBits) + ", got " + describe(c));
  }
}

void checkResultDoubleBits(const RunCtx& c, std::uint64_t wantBits,
                           const char* what) {
  const std::uint64_t gotBits =
      c.ok() ? std::bit_cast<std::uint64_t>(c.result.result.as.d) : 0ull;
  if (!c.ok() || c.result.status != RunStatus::Returned ||
      c.result.result.type != b2::rbc::RType::Double || gotBits != wantBits) {
    b2::test::recordFailure(__FILE__, __LINE__,
                            std::string(what) + ": expected double bits 0x" +
                                std::to_string(wantBits) + ", got " + describe(c));
  }
}

// Threw + exact class + exact message (the JVM message pins live in
// Runtime.h / Interp.cpp; Rule 74: exceptions are values).
void checkThrew(const RunCtx& c, std::string_view cls, std::string_view msg,
                std::string_view what) {
  if (!c.ok() || c.result.status != RunStatus::Threw) {
    b2::test::recordFailure(__FILE__, __LINE__,
                            std::string(what) + ": expected Threw " +
                                std::string(cls) + ", got " + describe(c));
    return;
  }
  const std::string gotCls(c.rt().classNameOf(c.result.exception));
  const std::string gotMsg(c.rt().exceptionMessage(c.result.exception));
  if (gotCls != cls || gotMsg != msg) {
    b2::test::recordFailure(
        __FILE__, __LINE__,
        std::string(what) + ": expected Threw " + std::string(cls) + " \"" +
            std::string(msg) + "\", got " + gotCls + " \"" + gotMsg + "\"");
  }
}

[[nodiscard]] std::uint32_t fBits(float f) {
  return std::bit_cast<std::uint32_t>(f);
}
[[nodiscard]] std::uint64_t dBits(double d) {
  return std::bit_cast<std::uint64_t>(d);
}

// ===========================================================================
// 1. Numeric edges (Rule 72: Java numeric semantics preserved exactly).
//    All 32-bit wraparound expectations are JLS 15.18.2 / 15.17.2 / 15.17.3.
// ===========================================================================

B2_TEST(interp_int_add_overflow_wraps) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 2147483647
iconst r1 1
iadd r2 r0 r1
ireturn r2
.end
)RBC");
  checkResultInt(c, -2147483648, "2147483647 + 1 wraps to INT_MIN");
}

B2_TEST(interp_int_sub_underflow_wraps) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 -2147483648
iconst r1 1
isub r2 r0 r1
ireturn r2
.end
)RBC");
  checkResultInt(c, 2147483647, "INT_MIN - 1 wraps to INT_MAX");
}

B2_TEST(interp_int_mul_overflow_wraps) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 65536
iconst r1 65536
imul r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, 0, "65536 * 65536 == 0 (mod 2^32)");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 100000
iconst r1 100000
imul r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, 1410065408, "100000 * 100000 == 1410065408 (mod 2^32)");
  }
}

B2_TEST(interp_idiv_int_min_over_minus_one) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 -2147483648
iconst r1 -1
idiv r2 r0 r1
ireturn r2
.end
)RBC");
  // JLS 15.17.2: INT_MIN / -1 == INT_MIN (overflow, no trap).
  checkResultInt(c, -2147483648, "INT_MIN / -1 == INT_MIN");
}

B2_TEST(interp_irem_int_min_over_minus_one) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 -2147483648
iconst r1 -1
irem r2 r0 r1
ireturn r2
.end
)RBC");
  // JLS 15.17.3: INT_MIN % -1 == 0 (and avoids the C++ UB case).
  checkResultInt(c, 0, "INT_MIN % -1 == 0");
}

B2_TEST(interp_idiv_irem_by_zero_throws_arithmetic) {
  // "/ by zero" with r0 delivery: the handler reads the delivered exception
  // (single-instruction protected range, see the file header note).
  for (const char* op : {"idiv", "irem"}) {
    const std::string prog = std::string(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 1
.const c0 = class "java/lang/ArithmeticException"
iconst r0 17
iconst r1 0
Ltry:
)RBC") + op + R"RBC( r2 r0 r1
Lend:
iconst r3 100
ireturn r3
Lh:
astore r0 l0
aload r1 l0
instanceof r2 r1 c0
ireturn r2
.catch c0 from Ltry to Lend handler Lh
.end
)RBC";
    const RunCtx c = runProgram(prog);
    checkResultInt(c, 1, std::string(op) + " by zero caught in r0 as ArithmeticException");
  }
}

B2_TEST(interp_ldiv_lrem_by_zero_throws_arithmetic) {
  for (const char* op : {"ldiv", "lrem"}) {
    const std::string prog = std::string(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 1
.const c0 = class "java/lang/ArithmeticException"
.const c1 = long 9223372036854775807
.const c2 = long 0
lconst r0 c1
lconst r1 c2
Ltry:
)RBC") + op + R"RBC( r2 r0 r1
Lend:
iconst r3 100
ireturn r3
Lh:
astore r0 l0
aload r1 l0
instanceof r2 r1 c0
ireturn r2
.catch c0 from Ltry to Lend handler Lh
.end
)RBC";
    const RunCtx c = runProgram(prog);
    checkResultInt(c, 1, std::string(op) + " by zero caught in r0 as ArithmeticException");
  }
}

B2_TEST(interp_int_div_by_zero_uncaught_message) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 1
iconst r1 0
idiv r2 r0 r1
ireturn r2
.end
)RBC");
  checkThrew(c, "java/lang/ArithmeticException", "/ by zero",
             "uncaught idiv by zero");
  CHECK(c.ok() && c.result.stats.exceptions == 1);
}

B2_TEST(interp_long_wrap) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 4
.locals 0
.const c0 = long 9223372036854775807
.const c1 = long 1
lconst r0 c0
lconst r1 c1
ladd r2 r0 r1
lreturn r2
.end
)RBC", InterpConfig{}, "main", "()J");
    checkResultLong(c, -9223372036854775807LL - 1,
                    "LONG_MAX + 1 wraps to LONG_MIN");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 4
.locals 0
.const c0 = long 4294967296
lconst r0 c0
lconst r1 c0
lmul r2 r0 r1
lreturn r2
.end
)RBC", InterpConfig{}, "main", "()J");
    checkResultLong(c, 0, "2^32 * 2^32 == 0 (mod 2^64)");
  }
}

B2_TEST(interp_long_min_div_rem_minus_one) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 4
.locals 0
.const c0 = long -9223372036854775808
.const c1 = long -1
lconst r0 c0
lconst r1 c1
ldiv r2 r0 r1
lreturn r2
.end
)RBC", InterpConfig{}, "main", "()J");
    checkResultLong(c, -9223372036854775807LL - 1, "LONG_MIN / -1 == LONG_MIN");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 4
.locals 0
.const c0 = long -9223372036854775808
.const c1 = long -1
lconst r0 c0
lconst r1 c1
lrem r2 r0 r1
lreturn r2
.end
)RBC", InterpConfig{}, "main", "()J");
    checkResultLong(c, 0, "LONG_MIN % -1 == 0");
  }
}

B2_TEST(interp_shift_counts_masked) {
  {
    // 1 << 33 == 1 << 1 == 2 (int shifts use the low 5 bits, JLS 15.19).
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 1
iconst r1 33
ishl r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, 2, "1 << 33 == 2");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 4
.locals 0
.const c0 = long 1
lconst r0 c0
iconst r1 65
lshl r2 r0 r1
lreturn r2
.end
)RBC", InterpConfig{}, "main", "()J");
    checkResultLong(c, 2, "1L << 65 == 2 (long shifts use the low 6 bits)");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 -16
iconst r1 2
ishr r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, -4, "-16 >> 2 == -4 (arithmetic)");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 -1
iconst r1 28
iushr r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, 15, "-1 >>> 28 == 15 (logical)");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 4
.locals 0
.const c0 = long -1
lconst r0 c0
iconst r1 60
lushr r2 r0 r1
lreturn r2
.end
)RBC", InterpConfig{}, "main", "()J");
    checkResultLong(c, 15, "-1L >>> 60 == 15");
  }
}

B2_TEST(interp_ineg_int_min) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r0 -2147483648
ineg r1 r0
ireturn r1
.end
)RBC");
  // JLS 15.15.4: -INT_MIN == INT_MIN.
  checkResultInt(c, -2147483648, "ineg INT_MIN == INT_MIN");
}

B2_TEST(interp_fneg_nan_and_negative_zero) {
  {
    // fneg flips the sign bit of NaN too (IEEE 754 negation).
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 2
.locals 0
fconst r0 2143289344
fneg r1 r0
freturn r1
.end
)RBC", InterpConfig{}, "main", "()F");
    checkResultFloatBits(c, 0xFFC00000u, "fneg(0x7FC00000) == 0xFFC00000");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 2
.locals 0
fconst r0 0
fneg r1 r0
freturn r1
.end
)RBC", InterpConfig{}, "main", "()F");
    checkResultFloatBits(c, 0x80000000u, "fneg(0.0f) == -0.0f");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()D
.regs 2
.locals 0
.const c0 = double 0.0
dconst r0 c0
dneg r1 r0
dreturn r1
.end
)RBC", InterpConfig{}, "main", "()D");
    checkResultDoubleBits(c, 0x8000000000000000ull, "dneg(0.0) == -0.0");
  }
}

B2_TEST(interp_float_ieee_ops) {
  {
    // 1.0f / 0.0f == +inf; -1.0f / 0.0f == -inf (no trap, SS3.7).
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 4
.locals 0
fconst r0 1065353216
fconst r1 0
fdiv r2 r0 r1
freturn r2
.end
)RBC", InterpConfig{}, "main", "()F");
    checkResultFloatBits(c, 0x7F800000u, "1.0f / 0.0f == +inf");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 4
.locals 0
fconst r0 -1082130432
fconst r1 0
fdiv r2 r0 r1
freturn r2
.end
)RBC", InterpConfig{}, "main", "()F");
    checkResultFloatBits(c, 0xFF800000u, "-1.0f / 0.0f == -inf");
  }
  {
    // inf + (-inf) == NaN.
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 4
.locals 0
fconst r0 2139095040
fconst r1 4286578688
fadd r2 r0 r1
freturn r2
.end
)RBC", InterpConfig{}, "main", "()F");
    CHECK(c.ok() && c.result.status == RunStatus::Returned);
    CHECK(c.ok() && c.result.result.type == b2::rbc::RType::Float);
    CHECK(c.ok() && std::isnan(c.result.result.as.f));
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()D
.regs 4
.locals 0
.const c0 = double 1.0
.const c1 = double 3.0
dconst r0 c0
dconst r1 c1
ddiv r2 r0 r1
dreturn r2
.end
)RBC", InterpConfig{}, "main", "()D");
    // The exact quotient of the two doubles 1.0 and 3.0.
    checkResultDoubleBits(c, dBits(1.0 / 3.0), "1.0 / 3.0 (double)");
  }
}

B2_TEST(interp_frem_drem_java_modulus) {
  {
    // JLS 15.17.3: Java's % is the C fmod (truncated quotient), so
    // 7.5 % 2.25 == 0.75 and -7.5 % 2.25 == -0.75 (sign of dividend).
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 4
.locals 0
fconst r0 1089470464
fconst r1 1074790400
frem r2 r0 r1
freturn r2
.end
)RBC", InterpConfig{}, "main", "()F");
    checkResultFloatBits(c, 0x3F400000u, "7.5f % 2.25f == 0.75f");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 4
.locals 0
fconst r0 -1058013184
fconst r1 1074790400
frem r2 r0 r1
freturn r2
.end
)RBC", InterpConfig{}, "main", "()F");
    checkResultFloatBits(c, 0xBF400000u, "-7.5f % 2.25f == -0.75f");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()D
.regs 4
.locals 0
.const c0 = double 7.5
.const c1 = double 2.25
dconst r0 c0
dconst r1 c1
drem r2 r0 r1
dreturn r2
.end
)RBC", InterpConfig{}, "main", "()D");
    checkResultDoubleBits(c, dBits(0.75), "7.5 % 2.25 == 0.75");
  }
}

B2_TEST(interp_fcmpl_fcmpg_nan_ordering) {
  // NaN compares less with fcmpl, greater with fcmpg (JLS 15.20.1).
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
fconst r0 2143289344
fconst r1 1065353216
fcmpl r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, -1, "fcmpl(NaN, 1.0f) == -1");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
fconst r0 2143289344
fconst r1 1065353216
fcmpg r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, 1, "fcmpg(NaN, 1.0f) == 1");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
fconst r0 1074790400
fconst r1 1065353216
fcmpl r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, 1, "fcmpl(2.25f, 1.0f) == 1 (both ops non-NaN)");
  }
}

B2_TEST(interp_dcmpl_dcmpg_nan_ordering) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = double nan
.const c1 = double 1.0
dconst r0 c0
dconst r1 c1
dcmpl r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, -1, "dcmpl(NaN, 1.0) == -1");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = double nan
.const c1 = double 1.0
dconst r0 c0
dconst r1 c1
dcmpg r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, 1, "dcmpg(NaN, 1.0) == 1");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = double -0.0
.const c1 = double 0.0
dconst r0 c0
dconst r1 c1
dcmpl r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, 0, "dcmpl(-0.0, 0.0) == 0");
  }
}

B2_TEST(interp_icmp_lcmp) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 -5
iconst r1 7
icmp r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, -1, "icmp(-5, 7) == -1");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = long -9223372036854775808
.const c1 = long 9223372036854775807
lconst r0 c0
lconst r1 c1
lcmp r2 r0 r1
ireturn r2
.end
)RBC");
    checkResultInt(c, -1, "lcmp(LONG_MIN, LONG_MAX) == -1");
  }
}

B2_TEST(interp_conversions_widening) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 2
.locals 0
iconst r0 -5
i2l r1 r0
lreturn r1
.end
)RBC", InterpConfig{}, "main", "()J");
    checkResultLong(c, -5, "i2l sign-extends");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()D
.regs 2
.locals 0
iconst r0 42
i2d r1 r0
dreturn r1
.end
)RBC", InterpConfig{}, "main", "()D");
    checkResultDoubleBits(c, dBits(42.0), "i2d exact");
  }
  {
    // i2f rounds to nearest: 2^24 + 1 is not representable, rounds to 2^24.
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 2
.locals 0
iconst r0 16777217
i2f r1 r0
freturn r1
.end
)RBC", InterpConfig{}, "main", "()F");
    checkResultFloatBits(c, fBits(16777216.0f), "i2f(16777217) == 16777216.0f");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 2
.locals 0
.const c0 = long 9007199254740993
lconst r0 c0
l2f r1 r0
freturn r1
.end
)RBC", InterpConfig{}, "main", "()F");
    checkResultFloatBits(c, fBits(9007199254740992.0f), "l2f(2^53+1) == 2^53f");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()D
.regs 2
.locals 0
.const c0 = long -1234567890123
lconst r0 c0
l2d r1 r0
dreturn r1
.end
)RBC", InterpConfig{}, "main", "()D");
    checkResultDoubleBits(c, dBits(-1234567890123.0), "l2d exact");
  }
}

B2_TEST(interp_l2i_truncates_low_32_bits) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
.const c0 = long 4886718345
lconst r0 c0
l2i r1 r0
ireturn r1
.end
)RBC");
  // 0x123456789 -> low 32 bits 0x23456789.
  checkResultInt(c, 0x23456789, "l2i keeps the low 32 bits");
}

B2_TEST(interp_f2i_nan_and_clamping) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
fconst r0 2143289344
f2i r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, 0, "f2i(NaN) == 0 (JLS 5.1.3)");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
fconst r0 2139095040
f2i r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, 2147483647, "f2i(+inf) == INT_MAX");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
fconst r0 4286578688
f2i r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, -2147483648, "f2i(-inf) == INT_MIN");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
fconst r0 1343554297
f2i r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, 2147483647, "f2i(1e10f) clamps to INT_MAX");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
fconst r0 -803929351
f2i r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, -2147483648, "f2i(-1e10f) clamps to INT_MIN");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
fconst r0 1089470464
f2i r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, 7, "f2i(7.5f) == 7 (round toward zero)");
  }
}

B2_TEST(interp_f2l_d2i_d2l_clamping) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 2
.locals 0
.const c0 = double 1e30
dconst r0 c0
d2l r1 r0
lreturn r1
.end
)RBC", InterpConfig{}, "main", "()J");
    checkResultLong(c, 9223372036854775807LL, "d2l(1e30) clamps to LONG_MAX");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 2
.locals 0
.const c0 = double nan
dconst r0 c0
d2l r1 r0
lreturn r1
.end
)RBC", InterpConfig{}, "main", "()J");
    checkResultLong(c, 0, "d2l(NaN) == 0");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
.const c0 = double 1000000000
dconst r0 c0
d2i r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, 1000000000, "d2i(1e9) == 1000000000 (in range)");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 2
.locals 0
.const c0 = double 0.1
dconst r0 c0
d2f r1 r0
freturn r1
.end
)RBC", InterpConfig{}, "main", "()F");
    checkResultFloatBits(c, fBits(0.1f), "d2f(0.1) == 0.1f");
  }
}

B2_TEST(interp_i2b_i2c_i2s_narrowing) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r0 300
i2b r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, 44, "i2b(300) == 44");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r0 -1
i2b r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, -1, "i2b(-1) == -1 (sign-extends)");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r0 -1
i2c r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, 65535, "i2c(-1) == 65535 (zero-extends)");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r0 65536
i2c r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, 0, "i2c(65536) == 0");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r0 65535
i2s r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, -1, "i2s(65535) == -1");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r0 32768
i2s r1 r0
ireturn r1
.end
)RBC");
    checkResultInt(c, -32768, "i2s(32768) == -32768");
  }
}

B2_TEST(interp_int_bitwise_ops) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 12
iconst r1 10
iand r2 r0 r1
ior r2 r2 r1
ixor r2 r2 r0
ireturn r2
.end
)RBC");
  // 12 & 10 = 8; 8 | 10 = 10; 10 ^ 12 = 6.
  checkResultInt(c, 6, "iand/ior/ixor chain");
}

B2_TEST(interp_long_bitwise_ops) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 4
.locals 0
.const c0 = long 281474976710656
.const c1 = long 4294967296
lconst r0 c0
lconst r1 c1
land r2 r0 r1
lor r2 r2 r1
lxor r2 r2 r0
lreturn r2
.end
)RBC", InterpConfig{}, "main", "()J");
  // 2^48 & 2^32 = 0; 0 | 2^32 = 2^32; 2^32 ^ 2^48 = 2^48 + 2^32.
  checkResultLong(c, 281474976710656LL + 4294967296LL, "land/lor/lxor chain");
}

// ===========================================================================
// 2. Constants, locals, moves (SS3.2 / SS3.3 / SS3.4).
// ===========================================================================

B2_TEST(interp_fconst_bit_pattern) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 2
.locals 0
fconst r0 1065353216
freturn r0
.end
)RBC", InterpConfig{}, "main", "()F");
  // imm 0x3F800000 is the IEEE 754 bit pattern of 1.0f (SS3.2).
  checkResultFloatBits(c, 0x3F800000u, "fconst 0x3F800000 == 1.0f");
}

B2_TEST(interp_lconst_dconst_from_pool) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 2
.locals 0
.const c0 = long -9007199254740993
lconst r0 c0
lreturn r0
.end
)RBC", InterpConfig{}, "main", "()J");
    checkResultLong(c, -9007199254740993LL, "lconst reads the cp Int64");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()D
.regs 2
.locals 0
.const c0 = double 0.5
dconst r0 c0
dreturn r0
.end
)RBC", InterpConfig{}, "main", "()D");
    checkResultDoubleBits(c, dBits(0.5), "dconst reads the cp Double");
  }
}

B2_TEST(interp_local_store_load_roundtrip_single_slot) {
  // long/double locals occupy ONE slot (RBC divergence, rbc_spec.md SS1.2):
  // the round-trips below store and load adjacent locals without pairing.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 8
.locals 4
.const c0 = long 1234567890123
.const c1 = double 3.5
lconst r0 c0
lstore r0 l1
dconst r0 c1
dstore r0 l2
iconst r0 77
istore r0 l3
lload r1 l1
lconst r2 c0
lcmp r3 r1 r2
ifne r3 Lbad
dload r1 l2
dconst r2 c1
dcmpl r3 r1 r2
ifne r3 Lbad
iload r1 l3
imove r2 r1
iadd r3 r1 r2
ireturn r3
Lbad:
iconst r3 -1
ireturn r3
.end
)RBC");
  checkResultInt(c, 154, "long/double/int locals + imove round-trip");
}

B2_TEST(interp_iinc_read_modify_write) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r0 10
iinc r0 5
iinc r0 -7
ireturn r0
.end
)RBC");
    checkResultInt(c, 8, "iinc 10 +5 -7 == 8");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r0 2147483647
iinc r0 1
ireturn r0
.end
)RBC");
    checkResultInt(c, -2147483648, "iinc wraps at INT_MAX");
  }
}

// ===========================================================================
// 3. Control flow (SS3.11).
// ===========================================================================

B2_TEST(interp_if_zero_family) {
  struct Case {
    const char* op;
    int value;
    bool taken;
  };
  const Case cases[] = {
      {"ifeq", 0, true},    {"ifeq", 5, false},   {"ifne", 0, false},
      {"ifne", 5, true},    {"iflt", -3, true},   {"iflt", 0, false},
      {"iflt", 4, false},   {"ifge", 0, true},    {"ifge", 4, true},
      {"ifge", -1, false},  {"ifgt", 9, true},    {"ifgt", 0, false},
      {"ifgt", -2, false},  {"ifle", 0, true},    {"ifle", -8, true},
      {"ifle", 3, false},
  };
  for (const Case& tc : cases) {
    const std::string prog = std::string(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
iconst r0 )RBC") +
                             std::to_string(tc.value) + "\n" + tc.op +
                             R"RBC( r0 Ltaken
iconst r1 0
ireturn r1
Ltaken:
iconst r1 1
ireturn r1
.end
)RBC";
    const RunCtx c = runProgram(prog);
    checkResultInt(c, tc.taken ? 1 : 0,
                   (std::string(tc.op) + "(" + std::to_string(tc.value) + ")")
                       .c_str());
  }
}

B2_TEST(interp_if_icmp_family) {
  struct Case {
    int a;
    int b;
    int eq;   // a == b
    int ne;   // a != b
    int lt;   // a < b
    int ge;   // a >= b
    int gt;   // a > b
    int le;   // a <= b
  };
  const Case cases[] = {
      {0, 0, 1, 0, 0, 1, 0, 1},
      {-5, 7, 0, 1, 1, 0, 0, 1},
      {7, -5, 0, 1, 0, 1, 1, 0},
      {-2147483648, 2147483647, 0, 1, 1, 0, 0, 1},
  };
  for (const Case& tc : cases) {
    const int expected[6] = {tc.eq, tc.ne, tc.lt, tc.ge, tc.gt, tc.le};
    const char* ops[6] = {"if_icmpeq", "if_icmpne", "if_icmplt",
                          "if_icmpge", "if_icmpgt", "if_icmple"};
    for (int i = 0; i < 6; ++i) {
      const std::string prog = std::string(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
iconst r0 )RBC") + std::to_string(tc.a) + "\niconst r1 " +
                               std::to_string(tc.b) + "\n" + ops[i] +
                               R"RBC( r0 r1 Ltaken
iconst r2 0
ireturn r2
Ltaken:
iconst r2 1
ireturn r2
.end
)RBC";
      const RunCtx c = runProgram(prog);
      checkResultInt(c, expected[i],
                     (std::string(ops[i]) + "(" + std::to_string(tc.a) + "," +
                      std::to_string(tc.b) + ")")
                         .c_str());
    }
  }
}

B2_TEST(interp_if_null_family_and_acmp_interning) {
  // Bit 0: two ldc of the SAME string constant are the same object (JVMS
  // 5.1 interning). Bit 1: null == null. Bit 2: object != null. Bit 3:
  // two DIFFERENT constants are different objects.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 8
.locals 0
.const c0 = string "x"
.const c1 = string "y"
ldc r0 c0
ldc r1 c0
if_acmpeq r0 r1 Lsame
iconst r2 0
goto Lm1
Lsame:
iconst r2 1
Lm1:
aconst_null r3 0
aconst_null r4 0
if_acmpeq r3 r4 Lnn
iconst r5 0
goto Lm2
Lnn:
iconst r5 1
Lm2:
ldc r6 c0
if_acmpne r6 r3 Lnp
iconst r6 0
goto Lm3
Lnp:
iconst r6 1
Lm3:
ldc r0 c0
ldc r1 c1
if_acmpne r0 r1 Lxy
iconst r7 0
goto Lm4
Lxy:
iconst r7 1
Lm4:
iconst r0 2
imul r5 r5 r0
iconst r0 4
imul r6 r6 r0
iconst r0 8
imul r7 r7 r0
iadd r2 r2 r5
iadd r2 r2 r6
iadd r2 r2 r7
ireturn r2
.end
)RBC");
  checkResultInt(c, 1 + 2 + 4 + 8, "acmp identity + interning bitmask");
}

B2_TEST(interp_goto_backward_loop) {
  // sum(1..5) = 15 via a backward goto (also exercises the backedge poll
  // convention with an explicit safepoint_poll).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 2
iconst r0 0
istore r0 l0
iconst r0 1
istore r0 l1
Lcond:
iload r0 l1
iconst r1 6
if_icmpge r0 r1 Ldone
iload r0 l0
iload r1 l1
iadd r2 r0 r1
istore r2 l0
iload r3 l1
iinc r3 1
istore r3 l1
safepoint_poll
goto Lcond
Ldone:
iload r0 l0
ireturn r0
.end
)RBC");
  checkResultInt(c, 15, "goto backward loop sums 1..5");
}

B2_TEST(interp_tableswitch_dense_dispatch) {
  for (int sel = 0; sel <= 2; ++sel) {
    const std::string prog = std::string(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = switch { 0:Lzero 1:Lone 2:Ltwo default:Ld }
iconst r0 )RBC") + std::to_string(sel) +
                             R"RBC(
tableswitch r0 c0
Ld:
iconst r1 9
ireturn r1
Lzero:
iconst r1 0
ireturn r1
Lone:
iconst r1 1
ireturn r1
Ltwo:
iconst r1 2
ireturn r1
.end
)RBC";
    const RunCtx c = runProgram(prog);
    checkResultInt(c, sel, "tableswitch dense dispatch");
  }
}

B2_TEST(interp_tableswitch_default_fallthrough_pc_plus_1) {
  // The default target is pinned to the fall-through instruction pc+1
  // (rbc_spec.md SS3.11): the instruction right after the switch runs.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = switch { 0:Lzero default:Ld }
iconst r0 7
tableswitch r0 c0
Ld:
iconst r1 77
ireturn r1
Lzero:
iconst r1 1
ireturn r1
.end
)RBC");
  checkResultInt(c, 77, "tableswitch default falls through to pc+1");
}

B2_TEST(interp_lookupswitch_sparse_and_default) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = switch { 10:Lten 20:Ltwenty 35:Lthirtyfive default:Ld }
iconst r0 20
lookupswitch r0 c0
Ld:
iconst r1 0
ireturn r1
Lten:
iconst r1 10
ireturn r1
Ltwenty:
iconst r1 20
ireturn r1
Lthirtyfive:
iconst r1 35
ireturn r1
.end
)RBC");
    checkResultInt(c, 20, "lookupswitch hits a sparse key");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = switch { 10:Lten 20:Ltwenty 35:Lthirtyfive default:Ld }
iconst r0 21
lookupswitch r0 c0
Ld:
iconst r1 -1
ireturn r1
Lten:
iconst r1 10
ireturn r1
Ltwenty:
iconst r1 20
ireturn r1
Lthirtyfive:
iconst r1 35
ireturn r1
.end
)RBC");
    checkResultInt(c, -1, "lookupswitch misses -> default");
  }
}

B2_TEST(interp_lookupswitch_empty_payload_always_default) {
  // An empty switch payload (N = 0) can only take the default.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = switch { default:Ld }
iconst r0 42
lookupswitch r0 c0
Ld:
iconst r1 5
ireturn r1
.end
)RBC");
  checkResultInt(c, 5, "empty lookupswitch always takes the default");
}

// ===========================================================================
// 4. Arrays (SS3.13).
// ===========================================================================

B2_TEST(interp_newarray_zero_fill_all_atypes) {
  // Fresh arrays are zero-filled with the ELEMENT TYPE's zero (Heap.h):
  // 0 for int-ish, 0L, 0.0f, 0.0, null for refs.
  struct Case {
    const char* atype;    // display name (numeric atype code in parens)
    const char* code;     // numeric atype code (RbcText.cpp spelling)
    const char* load;
    const char* store;    // typed store for the loaded element
    const char* compare;  // compares the loaded local against the zero
  };
  const Case cases[] = {
      {"boolean(4)", "4", "baload", "istore", "iload r0 l0\nifeq r0 Lzero\n"},
      {"byte(8)", "8", "baload", "istore", "iload r0 l0\nifeq r0 Lzero\n"},
      {"char(5)", "5", "caload", "istore", "iload r0 l0\nifeq r0 Lzero\n"},
      {"short(9)", "9", "saload", "istore", "iload r0 l0\nifeq r0 Lzero\n"},
      {"int(10)", "10", "iaload", "istore", "iload r0 l0\nifeq r0 Lzero\n"},
      {"long(11)", "11", "laload", "lstore",
       "lload r0 l0\nlconst r1 c0\nlcmp r2 r0 r1\nifeq r2 Lzero\n"},
      {"float(6)", "6", "faload", "fstore",
       "fload r0 l0\nfconst r1 0\nfcmpl r2 r0 r1\nifeq r2 Lzero\n"},
      {"double(7)", "7", "daload", "dstore",
       "dload r0 l0\ndconst r1 c1\ndcmpl r2 r0 r1\nifeq r2 Lzero\n"},
  };
  for (const Case& tc : cases) {
    const std::string prog =
        std::string(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 1
.const c0 = long 0
.const c1 = double 0.0
iconst r0 2
newarray r1 r0 )RBC") + tc.code + "\n" +
        "iconst r2 1\n" + tc.load + " r3 r1 r2\n" + tc.store + " r3 l0\n" +
        tc.compare +
        R"RBC(iconst r4 0
ireturn r4
Lzero:
iconst r4 1
ireturn r4
.end
)RBC";
    const RunCtx c = runProgram(prog);
    checkResultInt(c, 1, (std::string("newarray atype ") + tc.atype +
                          " zero-fill")
                             .c_str());
  }
}

B2_TEST(interp_anewarray_ref_elements_null) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = class "java/lang/String"
iconst r0 3
anewarray r1 r0 c0
iconst r2 2
aaload r3 r1 r2
ifnull r3 Lnull
iconst r3 0
ireturn r3
Lnull:
iconst r3 1
ireturn r3
.end
)RBC");
  checkResultInt(c, 1, "fresh ref[] elements are null");
}

B2_TEST(interp_arraylength_and_iastore_iaload_roundtrip) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 6
.locals 0
iconst r0 3
newarray r1 r0 10
arraylength r2 r1
iconst r3 -7
iconst r4 1
iastore r3 r1 r4
iconst r4 1
iaload r5 r1 r4
imul r5 r2 r5
ireturn r5
.end
)RBC");
  checkResultInt(c, -21, "arraylength(3) * iaload[1](-7)");
}

B2_TEST(interp_bastore_truncates_to_byte) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 6
.locals 0
iconst r0 2
newarray r1 r0 8
iconst r2 300
iconst r3 0
bastore r2 r1 r3
iconst r2 -1
iconst r3 1
bastore r2 r1 r3
iconst r3 0
baload r4 r1 r3
iconst r3 1
baload r5 r1 r3
iadd r5 r4 r5
ireturn r5
.end
)RBC");
  // 300 narrows to (byte)44; -1 stays -1; the loads sign-extend.
  checkResultInt(c, 43, "bastore(300) -> 44, bastore(-1) -> -1");
}

B2_TEST(interp_castore_sastore_narrowing) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 8
.locals 0
iconst r0 2
newarray r1 r0 5
iconst r2 70000
iconst r3 0
castore r2 r1 r3
iconst r3 0
caload r4 r1 r3
iconst r0 2
newarray r5 r0 9
iconst r2 40000
iconst r3 1
sastore r2 r5 r3
iconst r3 1
saload r6 r5 r3
iadd r7 r4 r6
ireturn r7
.end
)RBC");
  // castore(70000) -> 4464 (uint16); sastore(40000) -> -25536 (int16).
  checkResultInt(c, 4464 + (-25536), "castore/sastore narrowing");
}

B2_TEST(interp_lastore_fastore_dastore_roundtrip) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 6
.locals 0
iconst r0 2
newarray r1 r0 11
.const c0 = long -1234567890123
lconst r2 c0
iconst r3 1
lastore r2 r1 r3
iconst r3 1
laload r4 r1 r3
lreturn r4
.end
)RBC", InterpConfig{}, "main", "()J");
    checkResultLong(c, -1234567890123LL, "lastore/laload round-trip");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()F
.regs 6
.locals 0
iconst r0 2
newarray r1 r0 6
fconst r2 1089470464
iconst r3 1
fastore r2 r1 r3
iconst r3 1
faload r4 r1 r3
freturn r4
.end
)RBC", InterpConfig{}, "main", "()F");
    checkResultFloatBits(c, 0x40F00000u, "fastore/faload round-trip (7.5f)");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()D
.regs 6
.locals 0
iconst r0 2
newarray r1 r0 7
.const c0 = double 2.718281828459045
dconst r2 c0
iconst r3 1
dastore r2 r1 r3
iconst r3 1
daload r4 r1 r3
dreturn r4
.end
)RBC", InterpConfig{}, "main", "()D");
    checkResultDoubleBits(c, dBits(2.718281828459045), "dastore/daload round-trip");
  }
}

B2_TEST(interp_aastore_aaload_identity) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 6
.locals 0
.const c0 = class "java/lang/String"
.const c1 = string "elem"
iconst r0 2
anewarray r1 r0 c0
ldc r2 c1
iconst r3 1
aastore r2 r1 r3
iconst r3 1
aaload r4 r1 r3
if_acmpeq r2 r4 Lsame
iconst r5 0
ireturn r5
Lsame:
iconst r5 1
ireturn r5
.end
)RBC");
  checkResultInt(c, 1, "aastore/aaload preserve object identity");
}

B2_TEST(interp_array_access_npe) {
  // NPE on null arrays for every load/store family + arraylength. Each
  // program guards exactly the faulting instruction (single-instruction
  // protected ranges, see the file header note).
  struct Case {
    const char* name;
    const char* body;
  };
  const Case cases[] = {
      {"arraylength", "arraylength r1 r0\n"},
      {"iaload", "iconst r1 0\niaload r2 r0 r1\n"},
      {"laload", "iconst r1 0\nlaload r2 r0 r1\n"},
      {"faload", "iconst r1 0\nfaload r2 r0 r1\n"},
      {"daload", "iconst r1 0\ndaload r2 r0 r1\n"},
      {"aaload", "iconst r1 0\naaload r2 r0 r1\n"},
      {"baload", "iconst r1 0\nbaload r2 r0 r1\n"},
      {"caload", "iconst r1 0\ncaload r2 r0 r1\n"},
      {"saload", "iconst r1 0\nsaload r2 r0 r1\n"},
      {"iastore", "iconst r1 0\niconst r2 5\niastore r2 r0 r1\n"},
      {"lastore", "iconst r1 0\n.const c0 = long 1\nlconst r2 c0\nlastore r2 r0 r1\n"},
      {"fastore", "iconst r1 0\nfconst r2 0\nfastore r2 r0 r1\n"},
      {"dastore", "iconst r1 0\n.const c0 = double 1.0\ndconst r2 c0\ndastore r2 r0 r1\n"},
      {"aastore", "iconst r1 0\naconst_null r2 0\naastore r2 r0 r1\n"},
      {"bastore", "iconst r1 0\niconst r2 5\nbastore r2 r0 r1\n"},
  };
  for (const Case& tc : cases) {
    const std::string prog = std::string(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 0
aconst_null r0 0
Ltry:
)RBC") + tc.body + R"RBC(
Lend:
iconst r3 0
ireturn r3
Lh:
iconst r3 1
ireturn r3
.catch all from Ltry to Lend handler Lh
.end
)RBC";
    const RunCtx c = runProgram(prog);
    checkResultInt(c, 1, (std::string("NPE on null array for ") + tc.name).c_str());
  }
}

B2_TEST(interp_array_index_oob_exact_messages) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 0
iconst r0 3
newarray r1 r0 10
iconst r2 5
iaload r3 r1 r2
ireturn r3
.end
)RBC");
    checkThrew(c, "java/lang/ArrayIndexOutOfBoundsException",
               "Index 5 out of bounds for length 3", "iaload OOB high");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 0
iconst r0 3
newarray r1 r0 10
iconst r2 -1
iaload r3 r1 r2
ireturn r3
.end
)RBC");
    checkThrew(c, "java/lang/ArrayIndexOutOfBoundsException",
               "Index -1 out of bounds for length 3", "iaload OOB negative");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 0
iconst r0 3
newarray r1 r0 10
iconst r2 3
iconst r3 3
iastore r2 r1 r3
ireturn r2
.end
)RBC");
    checkThrew(c, "java/lang/ArrayIndexOutOfBoundsException",
               "Index 3 out of bounds for length 3", "iastore OOB");
  }
}

B2_TEST(interp_negative_array_size_message) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
iconst r0 -3
newarray r1 r0 10
ireturn r0
.end
)RBC");
    // JVM pin: the message is the decimal size.
    checkThrew(c, "java/lang/NegativeArraySizeException", "-3",
               "newarray(-3)");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = class "java/lang/String"
iconst r0 -3
anewarray r1 r0 c0
ireturn r0
.end
)RBC");
    checkThrew(c, "java/lang/NegativeArraySizeException", "-3",
               "anewarray(-3)");
  }
}

B2_TEST(interp_multianewarray_two_dims) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 6
.locals 0
.const c0 = class "[[I"
iconst r1 3
iconst r2 4
multianewarray r0 r1 r2 c0
arraylength r3 r0
iconst r4 0
aaload r5 r0 r4
arraylength r5 r5
imul r5 r3 r5
ireturn r5
.end
)RBC");
  // 3 x 4 nest: outer length 3, inner length 4.
  checkResultInt(c, 12, "multianewarray [[I 3x4");
}

B2_TEST(interp_multianewarray_partial_dims_trailing_zero) {
  // v0 pin (BE-2): trailing unspecified dimensions are LENGTH-0 arrays,
  // not nulls (documented divergence from Java, Heap.h/Runtime.cpp).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 6
.locals 0
.const c0 = class "[[I"
iconst r1 3
multianewarray r0 r1 r1 c0
arraylength r3 r0
iconst r4 0
aaload r5 r0 r4
arraylength r5 r5
imul r5 r3 r5
ireturn r5
.end
)RBC");
  checkResultInt(c, 0, "partial dims: inner arrays have length 0");
}

B2_TEST(interp_multianewarray_negative_dim) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 0
.const c0 = class "[[I"
iconst r1 -1
iconst r2 2
multianewarray r0 r1 r2 c0
ireturn r1
.end
)RBC");
  checkThrew(c, "java/lang/NegativeArraySizeException", "-1",
             "multianewarray with negative first dim");
}

B2_TEST(interp_aastore_exact_descriptor_assignability) {
  // v0 pin: array assignability is EXACT-DESCRIPTOR-ONLY (no covariance;
  // Runtime.cpp isAssignableFrom). Storing a Main instance into a
  // String[] raises ArrayStoreException with the JVM message shape.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 0
.const c0 = class "java/lang/String"
.const c1 = class "Main"
iconst r0 2
anewarray r1 r0 c0
new r2 c1
iconst r3 0
aastore r2 r1 r3
ireturn r3
.end
)RBC");
  checkThrew(c, "java/lang/ArrayStoreException", "class Main",
             "aastore Main into String[]");
}

B2_TEST(interp_instanceof_array_exact_descriptor) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = class "[I"
iconst r0 2
newarray r1 r0 10
instanceof r2 r1 c0
ireturn r2
.end
)RBC");
  checkResultInt(c, 1, "int[] instanceof [I (exact descriptor)");
}

// ===========================================================================
// 5. Objects and fields (SS3.12 / SS3.14).
// ===========================================================================

B2_TEST(interp_new_putfield_getfield_lazy_layout) {
  // The instance is created BEFORE the field is first resolved (lazy layout
  // grow path, Heap.h): new runs while the class layout is still empty and
  // the store grows the object on first access.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 6
.locals 1
.const c0 = class "Main"
.const c1 = field Main value I
.const c2 = field Main ref Ljava/lang/String;
.const c3 = string "payload"
new r0 c0
astore r0 l0
iconst r1 31
aload r2 l0
putfield r0 r2 r1 c1
ldc r1 c3
aload r2 l0
putfield r0 r2 r1 c2
aload r2 l0
getfield r3 r2 c1
aload r2 l0
getfield r4 r2 c2
if_acmpeq r1 r4 Lsame
iconst r5 -1
ireturn r5
Lsame:
ireturn r3
.end
)RBC");
  checkResultInt(c, 31, "putfield/getfield round-trip incl. lazy layout + ref field");
}

B2_TEST(interp_long_field_roundtrip) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()J
.regs 5
.locals 1
.const c0 = class "Main"
.const c1 = field Main wide J
.const c2 = long 1234567890123456789
new r0 c0
astore r0 l0
lconst r1 c2
aload r2 l0
putfield r0 r2 r1 c1
aload r2 l0
getfield r3 r2 c1
lreturn r3
.end
)RBC", InterpConfig{}, "main", "()J");
  checkResultLong(c, 1234567890123456789LL, "long field round-trip");
}

B2_TEST(interp_getfield_putfield_npe) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = field Main f I
aconst_null r0 0
Ltry:
getfield r1 r0 c0
Lend:
iconst r2 0
ireturn r2
Lh:
iconst r2 1
ireturn r2
.catch all from Ltry to Lend handler Lh
.end
)RBC");
    checkResultInt(c, 1, "getfield on null traps NPE");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = field Main f I
aconst_null r0 0
iconst r1 5
Ltry:
putfield r0 r0 r1 c0
Lend:
iconst r2 0
ireturn r2
Lh:
iconst r2 1
ireturn r2
.catch all from Ltry to Lend handler Lh
.end
)RBC");
    checkResultInt(c, 1, "putfield on null traps NPE");
  }
}

B2_TEST(interp_unwritten_instance_field_reads_type_zero) {
  // A5 pin: an unwritten instance field reads the FIELD TYPE's zero
  // (JLS 4.12.5), never Bottom.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = class "Main"
.const c1 = field Main f I
new r0 c0
getfield r1 r0 c1
ireturn r1
.end
)RBC");
  checkResultInt(c, 0, "unwritten int field reads 0");
}

B2_TEST(interp_statics_accumulate_across_calls) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
.const c0 = method Main bump ()I
invokestatic r1 r0 r0 c0
invokestatic r1 r0 r0 c0
ireturn r1
.end
.method static bump ()I
.regs 3
.locals 0
.const c0 = field Main total I
getstatic r0 c0
iconst r1 5
iadd r2 r0 r1
putstatic r2 c0
getstatic r0 c0
ireturn r0
.end
)RBC");
  // Statics live in the Runtime, so they persist across method calls and
  // even across run() calls on one Interpreter (Interp.h).
  checkResultInt(c, 10, "static field accumulates across two calls");
}

B2_TEST(interp_clinit_lazy_class_init_on_getstatic) {
  // JVMS 5.5 first-use: getstatic pushes <clinit>()V, the trigger
  // re-executes after it returns. <clinit> is spelled as a plain method
  // name in the text format (word token; not a '(' descriptor).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static <clinit> ()V
.regs 1
.locals 0
.const c0 = field Main value I
iconst r0 41
iinc r0 1
putstatic r0 c0
return
.end
.method static main ()I
.regs 4
.locals 0
.const c0 = field Main value I
getstatic r0 c0
ireturn r0
.end
)RBC");
  checkResultInt(c, 42, "<clinit> runs once on first getstatic");
}

B2_TEST(interp_clinit_runs_only_once_across_runs) {
  // The Runtime (and its initialized flags) persists across run() calls:
  // the second getstatic must NOT re-run <clinit> (it would re-store 5 and
  // the counter below would end at 10, not 15).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static <clinit> ()V
.regs 1
.locals 0
.const c0 = field Main value I
iconst r0 5
putstatic r0 c0
return
.end
.method static read ()I
.regs 2
.locals 0
.const c0 = field Main value I
getstatic r0 c0
iconst r1 5
iadd r1 r0 r1
putstatic r1 c0
getstatic r0 c0
ireturn r0
.end
)RBC", InterpConfig{}, "read", "()I");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  CHECK(c.result.status == RunStatus::Returned);
  CHECK(c.result.result.as.i == 10);  // clinit stored 5, read adds 5
  const RunResult second = c.interp->run("read", "()I", {});
  CHECK(second.status == RunStatus::Returned);
  // 10 (persisting static) + 5. If <clinit> re-ran, this would be 5 + 5 = 10.
  CHECK(second.result.as.i == 15);
}

B2_TEST(interp_getstatic_system_out_println) {
  RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 6
.locals 0
.const c0 = field java/lang/System out Ljava/io/PrintStream;
.const c1 = method java/io/PrintStream println (I)V
.const c2 = method java/io/PrintStream println (Ljava/lang/String;)V
.const c3 = string "hi"
getstatic r0 c0
iconst r1 42
invokevirtual r2 r0 r2 c1
getstatic r0 c0
ldc r1 c3
invokevirtual r2 r0 r2 c2
iconst r3 0
ireturn r3
.end
)RBC");
  checkReturned(c, "println program returns");
  CHECK(c.ok() && c.rt().stdout() == "42\nhi\n");
  CHECK(c.ok() && c.rt().stderr().empty());
}

B2_TEST(interp_getstatic_system_err_println) {
  RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = field java/lang/System err Ljava/io/PrintStream;
.const c1 = method java/io/PrintStream println (I)V
getstatic r0 c0
iconst r1 1
invokevirtual r2 r0 r2 c1
iconst r3 0
ireturn r3
.end
)RBC");
  checkReturned(c, "System.err println returns");
  CHECK(c.ok() && c.rt().stderr() == "1\n");
  CHECK(c.ok() && c.rt().stdout().empty());
}

B2_TEST(interp_println_formatting_java_shapes) {
  // println output uses Java's exact formatting (JDK Double.toString /
  // Float.toString). Corpus tests must use (I)(J)(F)(D)(String) descriptors
  // because of the value-tag formatting pin (see the corpus README note).
  RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 8
.locals 0
.const c0 = field java/lang/System out Ljava/io/PrintStream;
.const c1 = method java/io/PrintStream println (J)V
.const c2 = method java/io/PrintStream println (F)V
.const c3 = method java/io/PrintStream println (D)V
.const c4 = long -9223372036854775808
.const c5 = double 1e7
getstatic r0 c0
lconst r1 c4
invokevirtual r2 r0 r2 c1
getstatic r0 c0
fconst r1 1074790400
invokevirtual r2 r0 r2 c2
getstatic r0 c0
dconst r1 c5
invokevirtual r2 r0 r2 c3
iconst r4 0
ireturn r4
.end
)RBC");
  checkReturned(c, "println(J/F/D) program returns");
  CHECK(c.ok() && c.rt().stdout() ==
                     "-9223372036854775808\n2.25\n1.0E7\n");
}

B2_TEST(interp_checkcast_null_passes_and_success) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = class "java/lang/String"
aconst_null r0 0
checkcast r1 r0 c0
ifnull r1 Lnull
iconst r2 0
ireturn r2
Lnull:
iconst r2 1
ireturn r2
.end
)RBC");
    checkResultInt(c, 1, "checkcast(null) passes null through (JLS 5.5)");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = class "java/lang/String"
.const c1 = string "s"
ldc r0 c1
checkcast r1 r0 c0
if_acmpeq r0 r1 Lsame
iconst r2 0
ireturn r2
Lsame:
iconst r2 1
ireturn r2
.end
)RBC");
    checkResultInt(c, 1, "checkcast success preserves identity");
  }
}

B2_TEST(interp_checkcast_classcast_exception_message) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = class "Main"
.const c1 = class "java/lang/String"
new r0 c0
checkcast r1 r0 c1
iconst r2 0
ireturn r2
.end
)RBC");
  // JVM pin: dotted class names both sides.
  checkThrew(c, "java/lang/ClassCastException",
             "class Main cannot be cast to class java.lang.String",
             "checkcast Main -> String");
}

B2_TEST(interp_instanceof_results) {
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = class "java/lang/String"
aconst_null r0 0
instanceof r1 r0 c0
ireturn r1
.end
)RBC");
    checkResultInt(c, 0, "null instanceof anything == 0");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = class "java/lang/String"
.const c1 = string "s"
ldc r0 c1
instanceof r1 r0 c0
ireturn r1
.end
)RBC");
    checkResultInt(c, 1, "String instanceof String == 1");
  }
  {
    const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = class "Main"
.const c1 = class "java/lang/String"
new r0 c0
instanceof r1 r0 c1
ireturn r1
.end
)RBC");
    checkResultInt(c, 0, "Main instanceof String == 0 (no user hierarchy)");
  }
}

B2_TEST(interp_instanceof_builtin_hierarchy) {
  // An ArithmeticException object is instanceof java/lang/Exception via the
  // builtin exception hierarchy (Runtime.h). Delivered to the handler in r0
  // (single-instruction protected range, see the file header note).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 1
.const c0 = class "java/lang/Exception"
.const c1 = class "java/lang/ArithmeticException"
iconst r0 1
iconst r1 0
Ltry:
idiv r2 r0 r1
Lend:
iconst r3 0
ireturn r3
Lh:
astore r0 l0
aload r1 l0
instanceof r2 r1 c0
aload r1 l0
instanceof r3 r1 c1
iadd r3 r2 r3
ireturn r3
.catch all from Ltry to Lend handler Lh
.end
)RBC");
  checkResultInt(c, 2, "ArithmeticException instanceof Exception and itself");
}

// ===========================================================================
// 6. Calls (SS3.15; THE CALL PROTOCOL in Interp.h).
// ===========================================================================

B2_TEST(interp_invokestatic_recursion_fib) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
.const c0 = method Main fib (I)I
iconst r0 10
invokestatic r1 r0 r1 c0
ireturn r1
.end
.method static fib (I)I
.regs 4
.locals 2
.const c0 = method Main fib (I)I
iload r0 l0
iconst r1 2
if_icmplt r0 r1 Lbase
iload r0 l0
iconst r1 1
isub r2 r0 r1
invokestatic r3 r2 r1 c0
iload r0 l0
iconst r1 2
isub r2 r0 r1
invokestatic r1 r2 r1 c0
iadd r2 r3 r1
ireturn r2
Lbase:
iload r0 l0
ireturn r0
.end
)RBC");
  checkResultInt(c, 55, "recursive fib(10) == 55");
}

B2_TEST(interp_invokestatic_args_and_result_register) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 0
.const c0 = method Main add (II)I
iconst r0 20
iconst r1 22
invokestatic r3 r0 r2 c0
ireturn r3
.end
.method static add (II)I
.regs 3
.locals 2
iload r0 l0
iload r1 l1
iadd r2 r0 r1
ireturn r2
.end
)RBC");
  checkResultInt(c, 42, "args in consecutive registers, result in dst");
}

B2_TEST(interp_call_arguments_not_clobbered) {
  // CALL PROTOCOL step 5: arguments are READ, never clobbered.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 0
.const c0 = method Main add (II)I
iconst r0 10
iconst r1 32
invokestatic r3 r0 r2 c0
iadd r4 r0 r1
ireturn r4
.end
.method static add (II)I
.regs 3
.locals 2
iload r0 l0
iload r1 l1
iadd r2 r0 r1
ireturn r2
.end
)RBC");
  checkResultInt(c, 42, "caller arg registers unchanged after the call");
}

B2_TEST(interp_void_invokestatic_leaves_dst_untouched) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = method Main noop ()V
iconst r2 123
invokestatic r2 r0 r0 c0
ireturn r2
.end
.method static noop ()V
.regs 1
.locals 0
iconst r0 1
return
.end
)RBC");
  checkResultInt(c, 123, "void callee leaves the dst register untouched");
}

B2_TEST(interp_invokespecial_direct_call_with_receiver) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 1
.const c0 = class "Main"
.const c1 = method Main setVal ()V
.const c2 = field Main val I
new r0 c0
astore r0 l0
aload r0 l0
invokespecial r1 r0 r1 c1
aload r2 l0
getfield r3 r2 c2
ireturn r3
.end
.method setVal ()V
.regs 3
.locals 1
.const c0 = field Main val I
iconst r1 55
aload r0 l0
putfield r0 r0 r1 c0
return
.end
)RBC");
  checkResultInt(c, 55, "invokespecial receiver becomes callee local l0");
}

B2_TEST(interp_invokevirtual_ic_miss_then_hit) {
  // One virtual call site executed exactly twice: the first execution is an
  // IC miss (resolution + fill), the second an IC hit (Interp.h site-IC
  // contract). No println in the program: builtin sites are permanent
  // misses and would pollute the counters.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 8
.locals 3
.const c0 = class "Main"
.const c1 = method Main bump (I)I
new r0 c0
astore r0 l0
iconst r1 0
istore r1 l1
iconst r1 0
istore r1 l2
Lloop:
iload r1 l2
iconst r2 2
if_icmpge r1 r2 Ldone
aload r3 l0
iconst r4 5
invokevirtual r5 r3 r2 c1
istore r5 l1
iload r6 l2
iinc r6 1
istore r6 l2
goto Lloop
Ldone:
iload r0 l1
ireturn r0
.end
.method bump (I)I
.regs 3
.locals 2
iload r0 l1
iconst r1 1
iadd r2 r0 r1
ireturn r2
.end
)RBC");
  checkResultInt(c, 6, "invokevirtual site returns bump(5)+1 twice");
  CHECK(c.ok() && c.result.stats.icMisses == 1);
  CHECK(c.ok() && c.result.stats.icHits == 1);
  CHECK(c.ok() && c.result.stats.calls == 2);
}

B2_TEST(interp_invokevirtual_null_receiver_npe) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = method Main bump (I)I
aconst_null r0 0
iconst r1 5
invokevirtual r2 r0 r2 c0
ireturn r2
.end
.method static bump (I)I
.regs 3
.locals 1
iload r0 l0
iconst r1 1
iadd r2 r0 r1
ireturn r2
.end
)RBC");
  // A2 pin: the receiver NPE precedes the inline cache.
  checkThrew(c, "java/lang/NullPointerException", "",
             "invokevirtual on null receiver");
}

B2_TEST(interp_invokestatic_missing_method_error_message) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
.const c0 = method Main nosuch (I)I
iconst r0 1
invokestatic r1 r0 r1 c0
ireturn r1
.end
)RBC");
  // v0 pin: "<class>.<name><descriptor>" with the internal class name.
  checkThrew(c, "java/lang/NoSuchMethodError", "Main.nosuch(I)I",
             "invokestatic of a missing method");
}

B2_TEST(interp_missing_entry_method_throws) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 1
.locals 0
iconst r0 0
ireturn r0
.end
)RBC", InterpConfig{}, "nope", "()V");
  // run() resolution failure: "<name><descriptor>".
  checkThrew(c, "java/lang/NoSuchMethodError", "nope()V",
             "run of a missing entry method");
}

B2_TEST(interp_stack_overflow_error_on_deep_recursion) {
  InterpConfig cfg;
  cfg.maxFrames = 64;  // small cap: the trap must come quickly
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
.const c0 = method Main main ()I
invokestatic r1 r0 r0 c0
ireturn r1
.end
)RBC", cfg);
  checkThrew(c, "java/lang/StackOverflowError", "",
             "unbounded recursion traps StackOverflowError");
  // Every push was counted before the trap fired.
  CHECK(c.ok() && c.result.stats.calls >= 63);
  CHECK(c.ok() && c.interp->frames().empty());
}

// ===========================================================================
// 7. Exceptions (THE EXCEPTION ALGORITHM, Interp.h; Rule 74).
// ===========================================================================

B2_TEST(interp_handler_receives_exception_in_r0) {
  // SS5.3 / SS1.3: on catch, ALL registers die and r0 carries the object.
  // The handler stores r0 to a local and proves it is the thrown object by
  // identity (acmp against the athrow operand).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 1
.const c0 = class "java/lang/ArithmeticException"
new r0 c0
astore r0 l0
aload r1 l0
Ltry:
athrow r1
Lend:
iconst r2 0
ireturn r2
Lh:
aload r3 l0
if_acmpeq r0 r3 Lsame
iconst r2 0
ireturn r2
Lsame:
iconst r2 1
ireturn r2
.catch all from Ltry to Lend handler Lh
.end
)RBC");
  checkResultInt(c, 1, "caught object is the thrown object (r0 delivery)");
}

B2_TEST(interp_catch_builtin_hierarchy_supertype) {
  // catch java/lang/Exception catches ArithmeticException (builtin chain).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = class "java/lang/Exception"
iconst r0 1
iconst r1 0
Ltry:
idiv r2 r0 r1
Lend:
iconst r3 0
ireturn r3
Lh:
iconst r3 1
ireturn r3
.catch c0 from Ltry to Lend handler Lh
.end
)RBC");
  checkResultInt(c, 1, "catch Exception catches ArithmeticException");
}

B2_TEST(interp_catch_non_matching_type_propagates) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = class "java/lang/String"
iconst r0 1
iconst r1 0
Ltry:
idiv r2 r0 r1
Lend:
iconst r3 0
ireturn r3
Lh:
iconst r3 1
ireturn r3
.catch c0 from Ltry to Lend handler Lh
.end
)RBC");
  checkThrew(c, "java/lang/ArithmeticException", "/ by zero",
             "catch String does not catch ArithmeticException");
}

B2_TEST(interp_catch_all_catches_everything) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = class "java/lang/ArithmeticException"
new r0 c0
Ltry:
athrow r0
Lend:
iconst r1 0
ireturn r1
Lh:
iconst r1 7
ireturn r1
.catch all from Ltry to Lend handler Lh
.end
)RBC");
  checkResultInt(c, 7, "catch-all (catchType -1) catches any exception");
}

B2_TEST(interp_uncaught_exception_class_and_message) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
iconst r0 5
iconst r1 0
irem r2 r0 r1
ireturn r2
.end
)RBC");
  checkThrew(c, "java/lang/ArithmeticException", "/ by zero",
             "uncaught irem by zero");
  CHECK(c.ok() && c.interp->frames().empty());
}

B2_TEST(interp_unwind_through_three_frames) {
  // throw in depth-2 callee, caught in depth-0 caller; the intermediate
  // frames unwind and the caught object keeps its identity.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 2
.const c0 = class "java/lang/ArithmeticException"
.const c1 = method Main mid (Ljava/lang/Object;)V
new r0 c0
astore r0 l1
aload r0 l1
Ltry:
invokestatic r1 r0 r1 c1
Lend:
iconst r2 0
ireturn r2
Lh:
aload r1 l1
if_acmpeq r0 r1 Lsame
iconst r2 0
ireturn r2
Lsame:
iconst r2 1
ireturn r2
.catch all from Ltry to Lend handler Lh
.end
.method static mid (Ljava/lang/Object;)V
.regs 2
.locals 1
.const c0 = method Main leaf (Ljava/lang/Object;)V
aload r0 l0
invokestatic r1 r0 r1 c0
return
.end
.method static leaf (Ljava/lang/Object;)V
.regs 1
.locals 1
aload r0 l0
athrow r0
return
.end
)RBC");
  checkResultInt(c, 1, "exception unwinds leaf->mid->main preserving identity");
}

B2_TEST(interp_athrow_null_traps_npe) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
aconst_null r0 0
athrow r0
.end
)RBC");
  // JLS 14.18: athrow null raises NullPointerException.
  checkThrew(c, "java/lang/NullPointerException", "", "athrow(null)");
}

B2_TEST(interp_nested_try_inner_rethrows_outer_catches) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 1
.const c0 = class "java/lang/ArithmeticException"
iconst r0 1
iconst r1 0
Louter:
Linner:
idiv r2 r0 r1
LinnerEnd:
iconst r3 1
ireturn r3
LinnerH:
athrow r0
LouterEnd:
iconst r3 5
ireturn r3
LouterH:
iconst r3 42
ireturn r3
.catch c0 from Linner to LinnerEnd handler LinnerH
.catch all from Louter to LouterEnd handler LouterH
.end
)RBC");
  checkResultInt(c, 42, "inner catch rethrows, outer catch-all catches");
}

B2_TEST(interp_exception_stats_counts_rethrows) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 1
.const c0 = class "java/lang/ArithmeticException"
iconst r0 1
iconst r1 0
Louter:
Linner:
idiv r2 r0 r1
LinnerEnd:
iconst r3 1
ireturn r3
LinnerH:
athrow r0
LouterEnd:
iconst r3 5
ireturn r3
LouterH:
iconst r3 42
ireturn r3
.catch c0 from Linner to LinnerEnd handler LinnerH
.catch all from Louter to LouterEnd handler LouterH
.end
)RBC");
  CHECK(c.ok() && c.result.status == RunStatus::Returned);
  // One trap + one rethrow = two exceptions counted (Interp.h: "incl.
  // rethrows").
  CHECK(c.ok() && c.result.stats.exceptions == 2);
}

B2_TEST(interp_unwind_preserves_caller_locals) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 2
.const c0 = method Main thrower ()V
iconst r0 777
istore r0 l1
Ltry:
invokestatic r1 r0 r0 c0
Lend:
iconst r2 0
ireturn r2
Lh:
iload r2 l1
ireturn r2
.catch all from Ltry to Lend handler Lh
.end
.method static thrower ()V
.regs 2
.locals 0
.const c0 = class "java/lang/ArithmeticException"
new r0 c0
athrow r0
return
.end
)RBC");
  checkResultInt(c, 777, "caller locals survive the callee's unwind");
}

B2_TEST(interp_handler_table_order_first_match_wins) {
  // Two handlers cover the same single instruction; the FIRST in table
  // order (the catch-all) must win over the later typed entry.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = class "java/lang/ArithmeticException"
iconst r0 1
iconst r1 0
Ltry:
idiv r2 r0 r1
Lend:
iconst r3 0
ireturn r3
Lh1:
iconst r3 1
ireturn r3
Lh2:
iconst r3 2
ireturn r3
.catch all from Ltry to Lend handler Lh1
.catch c0 from Ltry to Lend handler Lh2
.end
)RBC");
  checkResultInt(c, 1, "first matching handler in table order wins");
}

B2_TEST(interp_catch_message_via_detail_message_field) {
  // The trap object's message is a real java/lang/String stored in
  // Throwable's detailMessage field (Runtime.cpp makeException): RBC can
  // read it back with getfield and println it.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 1
.const c0 = field java/lang/Throwable detailMessage Ljava/lang/String;
iconst r0 1
iconst r1 0
Ltry:
idiv r2 r0 r1
Lend:
iconst r3 0
ireturn r3
Lh:
astore r0 l0
aload r1 l0
getfield r1 r1 c0
ifnull r1 Lnullmsg
iconst r3 1
ireturn r3
Lnullmsg:
iconst r3 0
ireturn r3
.catch all from Ltry to Lend handler Lh
.end
)RBC");
  checkResultInt(c, 1, "detailMessage field readable from RBC");
}

// ===========================================================================
// 8. Monitors (SS3.14; reentrant, single-owner v0).
// ===========================================================================

B2_TEST(interp_monitor_balanced_enter_exit) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 1
.const c0 = class "Main"
new r0 c0
astore r0 l0
aload r0 l0
monitorenter r0
iconst r1 20
iconst r2 22
iadd r3 r1 r2
aload r0 l0
monitorexit r0
ireturn r3
.end
)RBC");
  checkResultInt(c, 42, "balanced monitorenter/monitorexit around a computation");
}

B2_TEST(interp_monitor_reentrant_then_imse_on_extra_exit) {
  // Reentrancy: enter twice, exit twice is fine; the THIRD exit is
  // IllegalMonitorStateException (JVM pin: no message).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 1
.const c0 = class "Main"
new r0 c0
astore r0 l0
aload r0 l0
monitorenter r0
aload r0 l0
monitorenter r0
aload r0 l0
monitorexit r0
aload r0 l0
monitorexit r0
aload r0 l0
Ltry:
monitorexit r0
Lend:
iconst r1 0
ireturn r1
Lh:
iconst r1 1
ireturn r1
.catch all from Ltry to Lend handler Lh
.end
)RBC");
  checkResultInt(c, 1, "third monitorexit traps IllegalMonitorStateException");
}

B2_TEST(interp_monitorexit_unowned_traps_imse) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 1
.const c0 = class "Main"
new r0 c0
astore r0 l0
aload r0 l0
Ltry:
monitorexit r0
Lend:
iconst r1 0
ireturn r1
Lh:
iconst r1 1
ireturn r1
.catch all from Ltry to Lend handler Lh
.end
)RBC");
  checkResultInt(c, 1, "monitorexit without enter traps IllegalMonitorStateException");
}

B2_TEST(interp_monitorexit_unowned_uncaught_message_empty) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 1
.const c0 = class "Main"
new r0 c0
astore r0 l0
aload r0 l0
monitorexit r0
iconst r1 0
ireturn r1
.end
)RBC");
  checkThrew(c, "java/lang/IllegalMonitorStateException", "",
             "uncaught unowned monitorexit");
}

B2_TEST(interp_monitor_enter_exit_null_traps_npe) {
  for (const char* op : {"monitorenter", "monitorexit"}) {
    const std::string prog = std::string(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
aconst_null r0 0
Ltry:
)RBC") + op + R"RBC( r0
Lend:
iconst r1 0
ireturn r1
Lh:
iconst r1 1
ireturn r1
.catch all from Ltry to Lend handler Lh
.end
)RBC";
    const RunCtx c = runProgram(prog);
    checkResultInt(c, 1, std::string(op) + " on null traps NPE");
  }
}

B2_TEST(interp_synchronized_static_method_releases_class_monitor) {
  // The class-object monitor is entered at entry and released on return
  // (JVMS 8.4.3.6). OBSERVABLE proof: after the run, a bare monitorexit on
  // the class object (materialized by ldc-of-Class, the cached
  // Runtime::classObject) must FAIL with IllegalMonitorStateException - it
  // would succeed if the synchronized method had leaked the monitor.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static synchronized syncm ()V
.regs 1
.locals 0
return
.end
.method static probe ()I
.regs 2
.locals 0
.const c0 = class "Main"
ldc r0 c0
Ltry:
monitorexit r0
Lend:
iconst r1 0
ireturn r1
Lh:
iconst r1 1
ireturn r1
.catch all from Ltry to Lend handler Lh
.end
)RBC", InterpConfig{}, "syncm", "()V");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  CHECK(c.result.status == RunStatus::Returned);
  const RunResult probe = c.interp->run("probe", "()I", {});
  CHECK(probe.status == RunStatus::Returned);
  CHECK(probe.result.as.i == 1);  // monitorexit failed: monitor was released
}

B2_TEST(interp_synchronized_instance_method_releases_receiver_monitor) {
  RunCtx c = runProgram(R"RBC(.class Main
.method synchronized synci ()V
.regs 1
.locals 1
return
.end
.method static probe (Ljava/lang/Object;)I
.regs 2
.locals 1
aload r0 l0
Ltry:
monitorexit r0
Lend:
iconst r1 0
ireturn r1
Lh:
iconst r1 1
ireturn r1
.catch all from Ltry to Lend handler Lh
.end
)RBC");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  const ObjRef receiver = c.rt().newInstance(c.rt().classId("Main"));
  const Value recv[1] = {Value::refVal(receiver)};
  const RunResult first = c.interp->run("synci", "()V", recv);
  CHECK(first.status == RunStatus::Returned);
  const RunResult probe =
      c.interp->run("probe", "(Ljava/lang/Object;)I", recv);
  CHECK(probe.status == RunStatus::Returned);
  CHECK(probe.result.as.i == 1);  // receiver monitor released on return
}

B2_TEST(interp_monitor_released_on_unwind_through_frames) {
  // inner() enters a monitor and throws; the unwind releases it (JVMS
  // monitorexit-on-throw, Interp.h call-protocol step 4). OBSERVABLE proof:
  // after catching, a bare monitorexit in main must FAIL with
  // IllegalMonitorStateException (it would succeed if the unwind had leaked
  // the monitor).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 1
.const c0 = class "Main"
.const c1 = method Main inner (Ljava/lang/Object;)V
.const c2 = class "java/lang/IllegalMonitorStateException"
new r0 c0
astore r0 l0
aload r0 l0
Ltry:
invokestatic r1 r0 r1 c1
Lend:
iconst r2 0
ireturn r2
Lh1:
nop
Ltry2:
aload r1 l0
monitorexit r1
Lend2:
iconst r2 2
ireturn r2
Lh2:
iconst r2 1
ireturn r2
.catch all from Ltry to Lend handler Lh1
.catch c2 from Ltry2 to Lend2 handler Lh2
.end
.method static inner (Ljava/lang/Object;)V
.regs 2
.locals 1
.const c0 = class "java/lang/ArithmeticException"
aload r0 l0
monitorenter r0
new r1 c0
athrow r1
return
.end
)RBC");
  // v0 single-threaded monitors make "was it released" only partially
  // observable (re-enter would just bump the count); the bare-exit probe
  // above IS the observable half (documented test limitation, see report).
  checkResultInt(c, 1, "monitor held by an unwound frame is released");
}

// ===========================================================================
// 9. Deopt hooks and resume() (Rule 4; docs/deopt_backend.md Part A).
// ===========================================================================

B2_TEST(interp_guard_non_null_noop_on_null) {
  // v0 pin: T0 holds no speculative state, so guards can never fail here -
  // guard_non_null is a NO-OP even on null (Interp.cpp).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
aconst_null r0 0
guard_non_null r0 1
iconst r1 5
ireturn r1
.end
)RBC");
  checkResultInt(c, 5, "guard_non_null(null) continues execution");
}

B2_TEST(interp_guard_class_noop) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = class "Main"
.const c1 = class "java/lang/String"
new r0 c0
guard_class r0 2 c1
iconst r1 6
ireturn r1
.end
)RBC");
  // The guard would fail class-checking in a tier that emitted it; T0's pin
  // is a no-op, so the program continues.
  checkResultInt(c, 6, "guard_class never deopts in T0");
}

B2_TEST(interp_deopt_trap_throws_internal_error) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 1
.locals 0
iconst r0 0
deopt_trap 7
ireturn r0
.end
)RBC");
  checkThrew(c, "java/lang/InternalError",
             "deopt_trap executed in T0 without deopt metadata (v0)",
             "deopt_trap without metadata");
}

B2_TEST(interp_resume_mid_method_loop) {
  // THE DEOPT ENTRY POINT: reconstruct a frame mid-method by hand (loop
  // counter already advanced, regs all Bottom) and resume() to the correct
  // final result. sum(n) with n=10, s=30, i=4 already folded in: the
  // remaining loop adds 4..10 -> 79 (a fresh run of sum(10) gives 55).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static sum (I)I
.regs 4
.locals 3
iconst r0 0
istore r0 l1
iconst r0 1
istore r0 l2
Lcond:
iload r0 l2
iload r1 l0
if_icmpgt r0 r1 Ldone
iload r0 l1
iload r1 l2
iadd r2 r0 r1
istore r2 l1
iload r3 l2
iinc r3 1
istore r3 l2
goto Lcond
Ldone:
iload r0 l1
ireturn r0
.end
)RBC", InterpConfig{}, "sum", "()I");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  // Direct-run oracle.
  const Value arg[1] = {Value::intVal(10)};
  const RunResult direct = c.interp->run("sum", "(I)I", arg);
  CHECK(direct.status == RunStatus::Returned);
  CHECK(direct.result.as.i == 55);

  // Mid-method resume: pc 4 is Lcond (the loop head).
  const rbc::Method* sum = c.program->find("sum", "(I)I");
  CHECK(sum != nullptr);
  if (sum == nullptr) {
    return;
  }
  Frame f;
  f.method = sum;
  f.pc = 4;
  f.locals = {Value::intVal(10), Value::intVal(30), Value::intVal(4)};
  f.regs.assign(sum->numRegs, Value::bottom());
  const RunResult resumed = c.interp->resume(std::move(f));
  CHECK(resumed.status == RunStatus::Returned);
  CHECK(resumed.result.as.i == 79);
}

B2_TEST(interp_resume_with_pending_exception_enters_handler) {
  // Exception deopt (Part A SS2.3): resume() with pendingException set
  // starts INSIDE the exception algorithm at the frame's pc - here a pc
  // covered by the handler, so dispatch lands in the handler.
  RunCtx c = runProgram(R"RBC(.class Main
.method static guarded (I)I
.regs 4
.locals 2
.const c0 = class "java/lang/ArithmeticException"
iload r0 l0
Ltry:
idiv r1 r0 r0
Lend:
iconst r2 5
ireturn r2
Lh:
iconst r3 9
ireturn r3
.catch c0 from Ltry to Lend handler Lh
.end
)RBC", InterpConfig{}, "guarded", "()I");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  const rbc::Method* m = c.program->find("guarded", "(I)I");
  CHECK(m != nullptr);
  if (m == nullptr) {
    return;
  }
  const ObjRef exc = c.rt().makeException("java/lang/ArithmeticException",
                                          "/ by zero");
  Frame f;
  f.method = m;
  f.pc = 1;  // the covered idiv (pc 0 is the iload)
  f.locals = {Value::intVal(7), Value::bottom()};
  f.regs.assign(m->numRegs, Value::bottom());
  f.pendingException = exc;
  const RunResult resumed = c.interp->resume(std::move(f));
  CHECK(resumed.status == RunStatus::Returned);
  CHECK(resumed.result.as.i == 9);
}

B2_TEST(interp_resume_rejects_mismatched_frame) {
  RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 1
.locals 0
iconst r0 0
ireturn r0
.end
)RBC");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  const rbc::Method* m = c.program->find("main", "()I");
  CHECK(m != nullptr);
  if (m == nullptr) {
    return;
  }
  Frame f;
  f.method = m;
  f.pc = 999;  // out of range: hostile reconstruction
  f.regs.assign(m->numRegs, Value::bottom());
  const RunResult resumed = c.interp->resume(std::move(f));
  RunCtx out;
  out.program = std::move(c.program);
  out.interp = std::move(c.interp);
  out.result = resumed;
  checkThrew(out, "java/lang/InternalError",
             "resume frame does not match the program",
             "resume with out-of-range pc");
}

// ===========================================================================
// 10. State dumps, safepoints, and statistics (Rules 88, 111, 114, 119, 124).
// ===========================================================================

B2_TEST(interp_safepoint_trace_golden) {
  // traceSafepoints emits the pinned v1 state-dump format (Frame.h) at
  // every safepoint_poll retirement; the stream is the deopt-fixture
  // generator and must be byte-deterministic (Rule 124). This golden string
  // IS the fixture other tiers' deopt tests will be built against.
  InterpConfig cfg;
  cfg.traceSafepoints = true;
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 3
iconst r0 0
istore r0 l1
iconst r0 2
istore r0 l2
Lcond:
iload r0 l2
ifle r0 Ldone
iload r0 l1
iload r1 l2
iadd r2 r0 r1
istore r2 l1
iload r3 l2
iinc r3 -1
istore r3 l2
safepoint_poll
goto Lcond
Ldone:
iload r0 l1
ireturn r0
.end
)RBC", cfg);
  checkResultInt(c, 3, "loop result");
  // NOTE on the pinned spellings this golden locks in (Frame.cpp): the
  // dump emits <kind>=<payload> for EVERY slot, so an empty slot is
  // "bot=bot" and null is "null=null"; the poll dump is taken at the
  // safepoint_poll's own pc (13) before it retires.
  const std::string want =
      "frame 0 method=main()I pc=13\n"
      "  local l0:bot=bot l1:int=2 l2:int=1\n"
      "  reg r0:int=0 r1:int=2 r2:int=2 r3:int=1\n"
      "  monitors=0\n"
      "frame 0 method=main()I pc=13\n"
      "  local l0:bot=bot l1:int=3 l2:int=0\n"
      "  reg r0:int=2 r1:int=1 r2:int=3 r3:int=0\n"
      "  monitors=0\n";
  CHECK(c.ok() && c.result.safepointTrace == want);
  // Byte-determinism: a second run produces the identical trace.
  const RunCtx c2 = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 3
iconst r0 0
istore r0 l1
iconst r0 2
istore r0 l2
Lcond:
iload r0 l2
ifle r0 Ldone
iload r0 l1
iload r1 l2
iadd r2 r0 r1
istore r2 l1
iload r3 l2
iinc r3 -1
istore r3 l2
safepoint_poll
goto Lcond
Ldone:
iload r0 l1
ireturn r0
.end
)RBC", cfg);
  CHECK(c2.ok() && c2.result.safepointTrace == want);
}

B2_TEST(interp_stats_nonzero_and_deterministic) {
  const auto make = [] {
    return runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 3
iconst r0 0
istore r0 l1
iconst r0 1
istore r0 l2
Lcond:
iload r0 l2
iconst r1 101
if_icmpge r0 r1 Ldone
iload r0 l1
iload r1 l2
iadd r2 r0 r1
istore r2 l1
iload r3 l2
iinc r3 1
istore r3 l2
safepoint_poll
goto Lcond
Ldone:
iload r0 l1
ireturn r0
.end
)RBC");
  };
  const RunCtx a = make();
  const RunCtx b = make();
  checkResultInt(a, 5050, "sum 1..100");
  CHECK(a.ok());
  CHECK(b.ok());
  if (!a.ok() || !b.ok()) {
    return;
  }
  CHECK(a.result.stats.instructions > 0);
  CHECK(a.result.stats.polls == 100);  // one per backedge
  CHECK(a.result.stats.calls == 0);
  CHECK(a.result.stats.exceptions == 0);
  CHECK(a.result.stats.allocations >= 2);  // System.out/err singletons
  // Deterministic across identical runs (Rule 124 discipline).
  CHECK(a.result.stats.instructions == b.result.stats.instructions);
  CHECK(a.result.stats.polls == b.result.stats.polls);
  CHECK(a.result.stats.allocations == b.result.stats.allocations);
}

B2_TEST(interp_stats_instructions_counted_at_fetch) {
  // 4 instructions retire before the return; the return itself is fetched
  // and counted too (Interp.cpp counts at FETCH).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r0 2
iconst r1 3
iadd r1 r0 r1
ireturn r1
.end
)RBC");
  checkResultInt(c, 5, "2 + 3");
  CHECK(c.ok() && c.result.stats.instructions == 4);
}

B2_TEST(interp_dumpframes_pinned_format) {
  // Direct dumpFrames() on a hand-built frame vector: every kind spelling
  // (int/long/float/double/null/bot/ref), the String payload append, and
  // the most-recent-first monitor record (Frame.h v1 pin).
  Program prog;
  prog.className = "Main";
  rbc::Method m;
  m.name = "m";
  m.descriptor = "()V";
  prog.methods.push_back(m);

  b2::interp::Runtime rt(prog);
  // Object ids are deterministic: System.out = 1, System.err = 2 (allocated
  // by the Runtime constructor), then this interned String = 3.
  const ObjRef str = rt.internString("hi");
  CHECK(str.id == 3);

  Frame f;
  f.method = &prog.methods[0];
  f.pc = 3;
  f.locals = {Value::intVal(42),      Value::longVal(-9),   Value::floatVal(1.5f),
              Value::doubleVal(0.25), Value::nullVal(),     Value::bottom(),
              Value::refVal(str)};
  f.regs = {Value::intVal(-1), Value::bottom()};
  f.monitors = {ObjRef{7}, ObjRef{3}};

  std::string out;
  const Frame frames[] = {f};
  dumpFrames(frames, rt, out);  // one-frame stack: no trailing blank line
  const std::string want =
      "frame 0 method=m()V pc=3\n"
      "  local l0:int=42 l1:long=-9 l2:float=1.5 l3:double=0.25"
      " l4:null=null l5:bot=bot l6:ref=java/lang/String@3 str=hi\n"
      "  reg r0:int=-1 r1:bot=bot\n"
      "  monitors=2,7,3\n";
  CHECK(out == want);

  // Byte-determinism (Rule 124): dumping the same state twice is identical.
  std::string again;
  dumpFrames(frames, rt, again);
  CHECK(again == want);
}

B2_TEST(interp_safepoint_request_flag_mechanics) {
  // Rule 111 handshake, v0 form: the flag is global, settable by tests, and
  // a set flag never interrupts execution (single-threaded v0: record-only
  // poll - the run completes normally).
  Interpreter::clearSafepointRequest();
  CHECK(!Interpreter::safepointRequested());
  Interpreter::requestSafepoint();
  CHECK(Interpreter::safepointRequested());
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 3
iconst r0 0
istore r0 l1
iconst r0 1
istore r0 l2
Lcond:
iload r0 l2
iconst r1 11
if_icmpge r0 r1 Ldone
iload r0 l1
iload r1 l2
iadd r2 r0 r1
istore r2 l1
iload r3 l2
iinc r3 1
istore r3 l2
safepoint_poll
goto Lcond
Ldone:
iload r0 l1
ireturn r0
.end
)RBC");
  checkResultInt(c, 55, "run completes with the safepoint flag set");
  CHECK(Interpreter::safepointRequested());
  Interpreter::clearSafepointRequest();
  CHECK(!Interpreter::safepointRequested());
}

B2_TEST(interp_profiles_backedge_bumps) {
  // Backward branches bump the saturating per-method backedge counter
  // (Runtime::profiles; Rule 114).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 1
iconst r0 0
istore r0 l0
Lloop:
iload r0 l0
iconst r1 5
if_icmpge r0 r1 Ldone
iload r0 l0
iinc r0 1
istore r0 l0
goto Lloop
Ldone:
iload r0 l0
ireturn r0
.end
)RBC");
  checkResultInt(c, 5, "simple loop");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  const auto& profiles = c.rt().profiles();
  CHECK(profiles.size() == 1);
  CHECK(profiles[0].backedges >= 5);
  CHECK(profiles[0].invocations >= 1);
}

// ===========================================================================
// 11. Verify gate (verifier before execution, Interp.h step 1).
// ===========================================================================

B2_TEST(interp_verify_gate_type_error) {
  // iadd on ref-typed registers: a semantic verify error in otherwise
  // well-formed text. run() must fail the WHOLE program before executing
  // anything (the hard gate).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = string "s"
ldc r0 c0
ldc r1 c0
iadd r2 r0 r1
ireturn r2
.end
)RBC");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  CHECK(c.result.status == RunStatus::VerifyFailed);
  CHECK(!c.result.verifyDiags.empty());
  bool found = false;
  for (const auto& d : c.result.verifyDiags) {
    if (d.message.find("iadd expects int") != std::string::npos) {
      found = true;
    }
  }
  CHECK(found);
  CHECK(c.result.stats.instructions == 0);  // nothing executed
}

B2_TEST(interp_verify_gate_register_out_of_range) {
  // .regs 2 but the method uses r5: the structural check fires.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r5 1
ireturn r5
.end
)RBC");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  CHECK(c.result.status == RunStatus::VerifyFailed);
  CHECK(!c.result.verifyDiags.empty());
  bool found = false;
  for (const auto& d : c.result.verifyDiags) {
    if (d.message.find("out of range") != std::string::npos) {
      found = true;
    }
  }
  CHECK(found);
}

// ===========================================================================
// 12. Quickened execution (INTERIM QUICKENED-OPCODE PINS, Interp.h).
//    Field offset = slot * sizeof(Value) = slot * 16; invokestatic_quick
//    imm = method index; invokevirtual_quick imm = IC site id (== call pc),
//    cold-cache resolution through cp[imm].
// ===========================================================================

// Hand-built quickened program: the text grammar spells quickened field
// offsets as cN references (their resolved pool index), which cannot
// express offset 16 with a small pool; building Ins records directly is the
// interpreter's other sanctioned entry path.
[[nodiscard]] Program buildQuickFieldProgram() {
  Program p;
  p.className = "Main";
  rbc::Method m;
  m.name = "main";
  m.descriptor = "()I";
  m.flags = b2::rbc::method_flags::Static;
  m.numRegs = 4;
  m.numLocals = 0;
  rbc::Const cls;
  cls.kind = rbc::Const::Kind::Class;
  cls.str = "Main";
  m.cp.push_back(cls);  // c0 (also the new target)
  for (std::int32_t i = 1; i <= 16; ++i) {
    rbc::Const filler;
    filler.kind = rbc::Const::Kind::Int32;
    filler.i32 = i;
    m.cp.push_back(filler);  // c1..c16 (imm anchors; kind unconstrained)
  }
  m.code.push_back(rbc::Ins(rbc::Op::New, 1, 0, 0, 0));          // pc0: r1 = new Main
  m.code.push_back(rbc::Ins(rbc::Op::Iconst, 2, 0, 0, 7));       // pc1: r2 = 7
  // pc2: putfield_quick obj=r1 value=r2 imm=0 (slot 0 -> offset 0)
  m.code.push_back(rbc::Ins(rbc::Op::PutfieldQuick, 0, 1, 2, 0));
  m.code.push_back(rbc::Ins(rbc::Op::Iconst, 2, 0, 0, 9));       // pc3: r2 = 9
  // pc4: putfield_quick obj=r1 value=r2 imm=16 (slot 1 -> offset 16)
  m.code.push_back(rbc::Ins(rbc::Op::PutfieldQuick, 0, 1, 2, 16));
  m.code.push_back(rbc::Ins(rbc::Op::Iconst, 3, 0, 0, 0));       // pc5: pre-type r3 Int
  // pc6: getfield_quick r3 = r1.slot0 (offset 0)
  m.code.push_back(rbc::Ins(rbc::Op::GetfieldQuick, 3, 1, 0, 0));
  // pc7: getfield_quick r2 = r1.slot1 (offset 16)
  m.code.push_back(rbc::Ins(rbc::Op::GetfieldQuick, 2, 1, 0, 16));
  m.code.push_back(rbc::Ins(rbc::Op::Iadd, 3, 3, 2, 0));         // pc8: 7 + 9
  m.code.push_back(rbc::Ins(rbc::Op::Ireturn, 0, 3, 0, 0));      // pc9 (a = r3)
  p.methods.push_back(m);
  return p;
}

B2_TEST(interp_quickened_field_offsets_slot_times_16) {
  // THE PIN: getfield_quick/putfield_quick imm is the BYTE offset and the
  // v0 layout is Value slots, so offset = slot * 16. Slot 0 -> 0, slot 1
  // -> 16 (two lazily-grown fields on a fresh instance).
  RunCtx c;
  c.program = std::make_unique<Program>(buildQuickFieldProgram());
  c.interp = std::make_unique<Interpreter>(*c.program);
  c.result = c.interp->run("main", "()I", {});
  checkResultInt(c, 16, "quickened field stores/loads at offsets 0 and 16");
}

B2_TEST(interp_quickened_getfield_unwritten_traps_internal_error) {
  // v0 corner pin (Interp.cpp A5 note): a quickened getfield of an
  // unwritten field cannot know the default value (the descriptor is gone
  // after quickening) - honest InternalError refusal.
  RunCtx c;
  c.program = std::make_unique<Program>(buildQuickFieldProgram());
  // Remove the two putfield_quick stores (pcs 2 and 4) by rebuilding the
  // program without them.
  Program p = buildQuickFieldProgram();
  p.methods[0].code.erase(p.methods[0].code.begin() + 4);  // drop putfield_quick slot1
  p.methods[0].code.erase(p.methods[0].code.begin() + 2);  // drop putfield_quick slot0
  c.program = std::make_unique<Program>(std::move(p));
  c.interp = std::make_unique<Interpreter>(*c.program);
  c.result = c.interp->run("main", "()I", {});
  // NOTE: the pc numbering above shifts; the remaining stream still reads
  // slot 0 (never written) at the first getfield_quick.
  checkThrew(c, "java/lang/InternalError",
             "quickened getfield of unwritten field (v0)",
             "getfield_quick of an unwritten field");
}

B2_TEST(interp_invokestatic_quick_imm_is_method_index) {
  // THE PIN: invokestatic_quick imm = MethodId (index into the Program's
  // method table). Method 0 is main, method 1 is bump; the quick call
  // carries imm=1 with no MethodRef at all.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
iconst r0 40
iconst r2 0
invokestatic_quick r2 r0 r1 1
ireturn r2
.end
.method static bump (I)I
.regs 3
.locals 1
iload r0 l0
iconst r1 2
iadd r2 r0 r1
ireturn r2
.end
)RBC");
  checkResultInt(c, 42, "invokestatic_quick imm=1 calls method index 1");
}

B2_TEST(interp_invokestatic_quick_out_of_range_traps) {
  // Defensive pin: a quickened method id past the table is a Java-visible
  // InternalError, never UB (the dummy-method path would be).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
iconst r0 40
iconst r1 0
invokestatic_quick r1 r0 r1 9
ireturn r1
.end
.method static bump (I)I
.regs 3
.locals 1
iload r0 l0
iconst r1 2
iadd r2 r0 r1
ireturn r2
.end
)RBC");
  checkThrew(c, "java/lang/InternalError", "quickened method id out of range",
             "invokestatic_quick with a hostile method id");
}

B2_TEST(interp_invokevirtual_quick_site_id_is_call_pc) {
  // THE PIN: invokevirtual_quick imm is the IC site id, and the v0 site id
  // of a call site is the pc of the call instruction itself. The program is
  // laid out so the invokevirtual_quick sits at pc 4 AND the MethodRef sits
  // at pool index 4 (c4 is the 5th declared const): a cold cache resolves
  // through cp[imm] (ambiguity resolution A4), fills the IC keyed by
  // (method, imm), and the SECOND execution hits it.
  //
  //   pc0 iconst r0 2
  //   pc1 istore r0 l0       (loop counter)
  //   pc2 new r1 c0          (receiver)
  //   pc3 astore r1 l1
  //   pc4 aload r3 l1
  //   pc5 iconst r4 5
  //   pc6 invokevirtual_quick r5 r3 r2 6    <- the call site (imm = 6)
  //   pc7.. loop tail: ifne back to pc4
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 8
.locals 3
.const c0 = class "Main"
.const c1 = int 0
.const c2 = int 0
.const c3 = int 0
.const c4 = int 0
.const c5 = int 0
.const c6 = method Main bump (I)I
iconst r0 2
istore r0 l0
new r1 c0
astore r1 l1
iconst r5 0
istore r5 l2
Lcall:
aload r3 l1
iconst r4 5
invokevirtual_quick r5 r3 r2 6
istore r5 l2
iload r6 l0
iinc r6 -1
istore r6 l0
ifne r6 Lcall
iload r0 l2
ireturn r0
.end
.method bump (I)I
.regs 3
.locals 2
iload r0 l1
iconst r1 1
iadd r2 r0 r1
ireturn r2
.end
)RBC");
  checkResultInt(c, 6, "invokevirtual_quick site-pc resolution + IC");
  CHECK(c.ok() && c.result.stats.icMisses == 1);
  CHECK(c.ok() && c.result.stats.icHits == 1);
  CHECK(c.ok() && c.result.stats.calls == 2);
}

B2_TEST(interp_putfield_getfield_quick_text_form_offset_zero) {
  // The TEXT form of quickened field ops: imm is spelled as a cN reference
  // whose resolved pool index equals the byte offset (the parser maps cN to
  // its pool index; quick ops accept any const kind). c0 is pool index 0 =
  // slot 0 = offset 0. The same const doubles as the `new` target.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 1
.const c0 = class "Main"
new r0 c0
astore r0 l0
iconst r1 63
aload r2 l0
putfield_quick r0 r2 r1 c0
iconst r3 0
aload r2 l0
getfield_quick r3 r2 c0
ireturn r3
.end
)RBC");
  checkResultInt(c, 63, "text-form putfield_quick/getfield_quick offset 0");
}

B2_TEST(interp_quick_field_ops_use_ic_free_path) {
  // Quickened field ops bypass the field IC entirely (no cp resolution, no
  // site cache): a program of ONLY quick field ops must report zero IC
  // traffic.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 5
.locals 1
.const c0 = class "Main"
new r0 c0
astore r0 l0
iconst r1 8
aload r2 l0
putfield_quick r0 r2 r1 c0
iconst r3 0
aload r2 l0
getfield_quick r3 r2 c0
ireturn r3
.end
)RBC");
  checkResultInt(c, 8, "quick-only field program result");
  CHECK(c.ok() && c.result.stats.icHits == 0);
  CHECK(c.ok() && c.result.stats.icMisses == 0);
}

B2_TEST(interp_ldc_of_class_materializes_class_object) {
  // ldc of a Class constant hands out the cached java/lang/Class instance
  // (Runtime::classObject); two ldc of the same class are the same object.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 4
.locals 0
.const c0 = class "Main"
ldc r0 c0
ldc r1 c0
if_acmpeq r0 r1 Lsame
iconst r2 0
ireturn r2
Lsame:
iconst r2 1
ireturn r2
.end
)RBC");
  checkResultInt(c, 1, "ldc of Class is the cached class object");
}

B2_TEST(interp_nop_and_misc_ops) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
nop
nop
iconst r0 3
ireturn r0
.end
)RBC");
  checkResultInt(c, 3, "nop is a no-op");
}

B2_TEST(interp_entry_args_bound_to_locals) {
  // Static entries take the parameters in order (run() step 2): call an
  // (II)I entry directly with two argument values.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static add (II)I
.regs 3
.locals 2
iload r0 l0
iload r1 l1
iadd r2 r0 r1
ireturn r2
.end
)RBC", InterpConfig{}, "add", "(II)I");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  const Value args[2] = {Value::intVal(19), Value::intVal(23)};
  const RunResult r = c.interp->run("add", "(II)I", args);
  CHECK(r.status == RunStatus::Returned);
  CHECK(r.result.as.i == 42);
}

B2_TEST(interp_entry_insufficient_args_internal_error) {
  const RunCtx c = runProgram(R"RBC(.class Main
.method static add (II)I
.regs 3
.locals 2
iload r0 l0
iload r1 l1
iadd r2 r0 r1
ireturn r2
.end
)RBC", InterpConfig{}, "add", "(II)I");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  const Value args[1] = {Value::intVal(19)};  // one short
  const RunResult r = c.interp->run("add", "(II)I", args);
  CHECK(r.status == RunStatus::Threw);
  CHECK(std::string(c.rt().classNameOf(r.exception)) ==
        "java/lang/InternalError");
}

B2_TEST(interp_invokedynamic_throws_bootstrap_method_error) {
  // v0 is verifier-only for invokedynamic (SS10.1): honest refusal.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 2
.locals 0
.const c0 = indy add (I)I
iconst r0 1
invokedynamic r1 r0 r1 c0
ireturn r1
.end
)RBC");
  checkThrew(c, "java/lang/BootstrapMethodError",
             "invokedynamic not supported in v0", "invokedynamic in T0");
}

B2_TEST(interp_putstatic_to_builtin_static_refused) {
  // Defensive pin: storing to System.out would corrupt the singleton
  // identity every getstatic depends on - refuse with InternalError.
  const RunCtx c = runProgram(R"RBC(.class Main
.method static main ()I
.regs 3
.locals 0
.const c0 = field java/lang/System out Ljava/io/PrintStream;
.const c1 = class "Main"
new r0 c1
putstatic r0 c0
iconst r1 0
ireturn r1
.end
)RBC");
  checkThrew(c, "java/lang/InternalError", "store to builtin static",
             "putstatic to System.out");
}

B2_TEST(interp_two_runtimes_are_independent) {
  // Two Interpreters over the same program: statics and the interned pool
  // must not leak between them (one Runtime per Interpreter).
  const RunCtx c = runProgram(R"RBC(.class Main
.method static bump ()I
.regs 3
.locals 0
.const c0 = field Main total I
getstatic r0 c0
iconst r1 5
iadd r2 r0 r1
putstatic r2 c0
getstatic r0 c0
ireturn r0
.end
)RBC", InterpConfig{}, "bump", "()I");
  CHECK(c.ok());
  if (!c.ok()) {
    return;
  }
  // runProgram already executed bump once (result 5). A SECOND run on the
  // same interpreter must see the persisted static (5 + 5 = 10)...
  const RunResult again = c.interp->run("bump", "()I", {});
  CHECK(again.status == RunStatus::Returned);
  CHECK(again.result.as.i == 10);
  // ...while a fresh Interpreter over the same program starts from zero.
  Interpreter other(*c.program);
  const RunResult fresh = other.run("bump", "()I", {});
  CHECK(fresh.status == RunStatus::Returned);
  CHECK(fresh.result.as.i == 5);  // not 10: no cross-Runtime leakage
}

}  // namespace
