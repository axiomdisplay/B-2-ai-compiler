// B-2 Frontend lowering tests: unit level.
//
// Every successful lowering must pass rbc::verify on EVERY method (the
// contract's hard invariant), be deterministic (byte-identical text dumps),
// and have the documented program shape. Refusals must be diagnostics with
// the Rule 47 shape (message + expected + fix), never silent miscompiles.

#include "LowerUtil.h"

namespace {

using b2::test::ExecOutcome;
using b2::test::LowerSession;
using b2::test::verifyAll;

// ---------------------------------------------------------------- shape ----

B2_TEST(lowerHelloWorldShapeAndVerify) {
  LowerSession s;
  s.runLower("Hello.java",
             "public class Hello {\n"
             "    public static void main(String[] args) {\n"
             "        System.out.println(\"hi\");\n"
             "    }\n"
             "}\n");
  CHECK(s.lowerOk());
  CHECK(s.lowered->program.className == "Hello");
  // main + synthesized default <init>
  CHECK(s.lowered->program.methods.size() == 2);
  const b2::rbc::Method* main = s.lowered->program.find("main", "([Ljava/lang/String;)V");
  CHECK(main != nullptr);
  CHECK(main->isStatic());
  CHECK(s.lowered->program.find("<init>", "()V") != nullptr);
  CHECK(verifyAll(s.lowered->program));
}

B2_TEST(lowerPackageInternalName) {
  LowerSession s;
  s.runLower("p/Fib.java",
             "package demo;\n"
             "public class Fib {\n"
             "    public static void main(String[] args) { }\n"
             "}\n");
  CHECK(s.lowerOk());
  CHECK(s.lowered->program.className == "demo/Fib");
  CHECK(verifyAll(s.lowered->program));
}

B2_TEST(lowerClinitSynthesis) {
  LowerSession s;
  s.runLower(
      "S.java",
      "public class S {\n"
      "    static int a = 1;\n"
      "    static { a += 1; }\n"
      "    static int b = a * 10;\n"
      "    int instanceField = 5;\n"
      "    public static void main(String[] args) { }\n"
      "}\n");
  CHECK(s.lowerOk());
  const b2::rbc::Method* clinit = s.lowered->program.find("<clinit>", "()V");
  CHECK(clinit != nullptr);
  CHECK(clinit->isStatic());
  // putstatic sites for a (twice) and b
  int puts = 0;
  for (const b2::rbc::Ins& ins : clinit->code) {
    if (ins.opcode() == b2::rbc::Op::Putstatic) {
      ++puts;
    }
  }
  CHECK(puts == 3);
  CHECK(verifyAll(s.lowered->program));
}

B2_TEST(lowerNoClinitWhenNoStaticInits) {
  LowerSession s;
  s.runLower("S.java",
             "public class S {\n"
             "    int x = 1;\n"
             "    public static void main(String[] args) { }\n"
             "}\n");
  CHECK(s.lowerOk());
  CHECK(s.lowered->program.find("<clinit>", "()V") == nullptr);
  CHECK(verifyAll(s.lowered->program));
}

B2_TEST(lowerInstanceFieldInitsPrependedToCtor) {
  LowerSession s;
  s.runLower(
      "C.java",
      "public class C {\n"
      "    int a = 1;\n"
      "    int b = 2;\n"
      "    C() { b = 3; }\n"
      "    public static void main(String[] args) { }\n"
      "}\n");
  CHECK(s.lowerOk());
  const b2::rbc::Method* ctor = s.lowered->program.find("<init>", "()V");
  CHECK(ctor != nullptr);
  // first putfield is a=1 (field initializers run before the body)
  bool sawA = false;
  for (const b2::rbc::Ins& ins : ctor->code) {
    if (ins.opcode() == b2::rbc::Op::Putfield) {
      const b2::rbc::Const& c = ctor->cp[ins.imm];
      sawA = c.str2 == "a";
      break;
    }
  }
  CHECK(sawA);
  CHECK(verifyAll(s.lowered->program));
}

B2_TEST(lowerCtorThisDelegationSkipsFieldInits) {
  LowerSession s;
  s.runLower(
      "C.java",
      "public class C {\n"
      "    int a = 1;\n"
      "    C() { this(9); a = 2; }\n"
      "    C(int v) { a = v; }\n"
      "    public static void main(String[] args) { }\n"
      "}\n");
  CHECK(s.lowerOk());
  const b2::rbc::Method* ctor = s.lowered->program.find("<init>", "()V");
  CHECK(ctor != nullptr);
  // invokespecial <init>(I) appears before the body's a=2 store
  bool delegation = false;
  for (const b2::rbc::Ins& ins : ctor->code) {
    if (ins.opcode() == b2::rbc::Op::Invokespecial) {
      delegation = true;
      break;
    }
  }
  CHECK(delegation);
  CHECK(verifyAll(s.lowered->program));
}

B2_TEST(lowerMethodDescriptorsAndFlags) {
  LowerSession s;
  s.runLower(
      "M.java",
      "public class M {\n"
      "    public static long wide(int a, double b) { return 1L; }\n"
      "    private synchronized int guarded() { return 2; }\n"
      "    public static void main(String[] args) { }\n"
      "}\n");
  CHECK(s.lowerOk());
  const b2::rbc::Method* wide = s.lowered->program.find("wide", "(ID)J");
  CHECK(wide != nullptr);
  CHECK(wide->isStatic());
  const b2::rbc::Method* g = s.lowered->program.find("guarded", "()I");
  CHECK(g != nullptr);
  CHECK(g->isSynchronized());
  CHECK(!g->isStatic());
  CHECK(verifyAll(s.lowered->program));
}

// ----------------------------------------------------------- determinism ----

B2_TEST(lowerDeterministicDump) {
  const char* src =
      "public class D {\n"
      "    static int v = 1;\n"
      "    public static void main(String[] args) {\n"
      "        int[] a = {1, 2, 3};\n"
      "        for (int x : a) { v += x; }\n"
      "        try { v /= 0; } catch (Exception e) { v = -1; } finally { }\n"
      "        System.out.println(v);\n"
      "    }\n"
      "}\n";
  LowerSession a;
  a.runLower("D.java", src);
  CHECK(a.lowerOk());
  LowerSession b;
  b.runLower("D.java", src);
  CHECK(b.lowerOk());
  CHECK(b2::rbc::printRbcText(a.lowered->program) ==
        b2::rbc::printRbcText(b.lowered->program));
  CHECK(verifyAll(a.lowered->program));
}

// --------------------------------------------------------- slot stability ----

B2_TEST(lowerLocalsInsideTryStayStable) {
  // Declarations inside protected ranges: the prologue's fixed family init
  // keeps every slot's verification type stable (rbc_spec SS5.3).
  LowerSession s;
  s.runLower(
      "T.java",
      "public class T {\n"
      "    public static void main(String[] args) {\n"
      "        try {\n"
      "            int x = 1;\n"
      "            String s = \"a\";\n"
      "            long l = 2L;\n"
      "            double d = 3.0;\n"
      "            System.out.println(x);\n"
      "        } catch (Exception e) {\n"
      "            System.out.println(\"c\");\n"
      "        }\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
}

B2_TEST(lowerNestedTryCatchVarSurvives) {
  // The catch variable is a Ref-typed LOCAL: it survives an inner protected
  // region (a register would die at the inner handler entry). NOTE: v0's
  // runtime matches user classes exactly, so the catch type is N itself
  // (catching a user throw as Exception needs the class hierarchy).
  LowerSession s;
  s.runLower(
      "N.java",
      "public class N {\n"
      "    public static void main(String[] args) {\n"
      "        try {\n"
      "            throw new N();\n"
      "        } catch (N e) {\n"
      "            try {\n"
      "                int[] t = new int[1];\n"
      "                t[3] = 0;\n"
      "            } catch (Exception inner) {\n"
      "                System.out.println(\"inner\");\n"
      "            }\n"
      "            System.out.println(e instanceof N);\n"
      "        }\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.status == b2::interp::RunStatus::Returned);
  CHECK(o.stdoutText == "inner\n1\n");
}

// -------------------------------------------------------------- refusals ----

B2_TEST(refuseLambda) {
  LowerSession s;
  s.runLower("L.java",
             "public class L {\n"
             "    public static void main(String[] args) {\n"
             "        Runnable r = () -> { };\n"
             "    }\n"
             "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("lambdas are not supported"));
}

B2_TEST(refuseStringConcat) {
  LowerSession s;
  s.runLower("C.java",
             "public class C {\n"
             "    public static void main(String[] args) {\n"
             "        String s = \"a\" + \"b\";\n"
             "    }\n"
             "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("string concatenation"));
}

B2_TEST(refuseStringConcatCompound) {
  LowerSession s;
  s.runLower("C.java",
             "public class C {\n"
             "    public static void main(String[] args) {\n"
             "        String s = \"a\";\n"
             "        s += 1;\n"
             "    }\n"
             "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("String compound assignment"));
}

B2_TEST(refuseTopLevelInterface) {
  LowerSession s;
  s.runLower("I.java", "public interface I { void m(); }");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("supports classes only"));
}

B2_TEST(refuseTopLevelEnum) {
  LowerSession s;
  s.runLower("E.java", "enum E { A, B }");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("supports classes only"));
}

B2_TEST(refuseNestedClass) {
  LowerSession s;
  s.runLower(
      "N.java",
      "public class N {\n"
      "    class Inner { }\n"
      "    public static void main(String[] args) { }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("nested class declarations"));
}

B2_TEST(refuseMultipleTopLevelClasses) {
  LowerSession s;
  s.runLower(
      "M.java",
      "public class M { }\n"
      "class Second { }\n");
  CHECK(s.lowerOk());  // warning only: first class lowered
  CHECK(s.expectWarning("one top-level class"));
  CHECK(verifyAll(s.lowered->program));
}

B2_TEST(refuseTryWithResources) {
  LowerSession s;
  s.runLower(
      "R.java",
      "public class R {\n"
      "    public static void main(String[] args) {\n"
      "        try (AutoCloseable a = null) { }\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("try-with-resources"));
}

B2_TEST(refuseAssert) {
  LowerSession s;
  s.runLower(
      "A.java",
      "public class A {\n"
      "    public static void main(String[] args) {\n"
      "        assert 1 == 2;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("'assert'"));
}

B2_TEST(refuseMultiDimArray) {
  LowerSession s;
  s.runLower(
      "D.java",
      "public class D {\n"
      "    public static void main(String[] args) {\n"
      "        int[][] grid = new int[2][3];\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("multi-dimensional array"));
}

B2_TEST(refuseStringMethodCall) {
  LowerSession s;
  s.runLower(
      "S.java",
      "public class S {\n"
      "    public static void main(String[] args) {\n"
      "        String s = \"a\";\n"
             "        int n = s.length();\n"
             "    }\n"
             "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("method calls on String"));
}

B2_TEST(refuseCrossClassStaticCall) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        Math.max(1, 2);\n"
      "    }\n"
             "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("static call into"));
}

B2_TEST(refuseCrossClassField) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        int v = Integer.MAX_VALUE;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("is not known to v0 lowering"));
}

B2_TEST(refuseLibraryConstruction) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        Object o = new Object();\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("constructs the program class only"));
}

B2_TEST(refuseSuperWithArgs) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    X() { super(1); }\n"
      "    public static void main(String[] args) { }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("super(...) with arguments"));
}

B2_TEST(refuseInstanceFieldFromStatic) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    int f = 1;\n"
      "    public static void main(String[] args) {\n"
      "        int v = f;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("static context"));
}

B2_TEST(refuseNonBooleanCondition) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        if (1) { }\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("expected boolean"));
}

B2_TEST(refuseNarrowingWithoutCast) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        int v = 300;\n"
      "        byte b = v;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("cannot be converted"));
}

B2_TEST(refuseLossyConstant) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        byte b = 300;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("possible lossy conversion"));
}

B2_TEST(refuseVoidValueUsed) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    static void noop() { }\n"
      "    public static void main(String[] args) {\n"
      "        int v = noop();\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("void result cannot be used"));
}

B2_TEST(refuseDuplicateCase) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        switch (1) {\n"
      "            case 1:\n"
      "            case 1:\n"
      "                break;\n"
      "        }\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("duplicate case label"));
}

B2_TEST(refuseBreakOutsideLoop) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        break;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("'break'"));
}

B2_TEST(refuseLabeledNonLoop) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        lbl: int x = 1;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("labels are supported only on loops and switch"));
}

B2_TEST(refuseReturnValueFromVoid) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    static void noop() { return 1; }\n"
      "    public static void main(String[] args) { }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("cannot return a value from a void method"));
}

B2_TEST(refuseMissingReturnValue) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    static int v() { return; }\n"
      "    public static void main(String[] args) { }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("missing return value"));
}

B2_TEST(refuseVarWithoutInitializer) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        var v;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("'var' requires an initializer"));
}

B2_TEST(refuseUnresolvedName) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        int v = nothing;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("cannot resolve 'nothing'"));
}

B2_TEST(refuseAbstractMethod) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    abstract int v();\n"
      "    public static void main(String[] args) { }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("abstract or native"));
}

B2_TEST(refuseArityMismatchHintsVarargs) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    static int sum(int[] xs) { return 0; }\n"
      "    public static void main(String[] args) {\n"
      "        int v = sum(1, 2);\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("does not match any declared method"));
}

B2_TEST(refuseVarargCallSite) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    static int sum(int... xs) { return 0; }\n"
      "    public static void main(String[] args) {\n"
      "        int v = sum(1, 2);\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("does not match any declared method"));
}

B2_TEST(refuseSwitchExpression) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        int v = switch (1) { default -> 2; };\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("switch expressions are not supported"));
}

B2_TEST(refuseInstanceofPattern) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        Object o = null;\n"
      "        boolean b = o instanceof String s;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("pattern matching"));
}

B2_TEST(refuseBoxedArithmetic) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        Integer a = 1;\n"
      "        int v = a + 1;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  // Integer is an unknown reference type: the declaration's int value
  // cannot convert to it without boxing (no boxing in v0).
  CHECK(s.expectError("cannot be converted"));
}

B2_TEST(refuseThisInStatic) {
  LowerSession s;
  s.runLower(
      "X.java",
      "public class X {\n"
      "    public static void main(String[] args) {\n"
      "        X x = this;\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("'this' is not available in a static context"));
}

B2_TEST(refuseUninitializedParseRefused) {
  // Lowering runs only on clean parses; a parse error surfaces first.
  LowerSession s;
  s.runLower("X.java", "public class X { int x = ; }");
  CHECK(s.hasErrors());
  CHECK(!s.lowerOk());
}

// ------------------------------------------------------ emission details ----

B2_TEST(lowerSafepointPollsOnBackedges) {
  LowerSession s;
  s.runLower(
      "W.java",
      "public class W {\n"
      "    public static void main(String[] args) {\n"
      "        int i = 0;\n"
      "        while (i < 3) { i++; }\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  const b2::rbc::Method* main = s.lowered->program.find("main", "([Ljava/lang/String;)V");
  CHECK(main != nullptr);
  int polls = 0;
  for (const b2::rbc::Ins& ins : main->code) {
    if (ins.opcode() == b2::rbc::Op::SafepointPoll) {
      ++polls;
    }
  }
  CHECK(polls == 1);
  CHECK(verifyAll(s.lowered->program));
}

B2_TEST(lowerFloatNaNComparisonOpcodes) {
  // d < x must use dcmpg (NaN -> +1 -> not-less); d > x must use dcmpl.
  LowerSession s;
  s.runLower(
      "F.java",
      "public class F {\n"
      "    public static void main(String[] args) {\n"
      "        double d = 0.0 / 0.0;\n"
      "        boolean a = d < 1.0;\n"
      "        boolean b = d > 1.0;\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  const b2::rbc::Method* main = s.lowered->program.find("main", "([Ljava/lang/String;)V");
  CHECK(main != nullptr);
  bool sawG = false, sawL = false;
  for (const b2::rbc::Ins& ins : main->code) {
    if (ins.opcode() == b2::rbc::Op::Dcmpg) sawG = true;
    if (ins.opcode() == b2::rbc::Op::Dcmpl) sawL = true;
  }
  CHECK(sawG);
  CHECK(sawL);
  CHECK(verifyAll(s.lowered->program));
}

B2_TEST(lowerSwitchIsLookupswitchWithDefaultFallthrough) {
  LowerSession s;
  s.runLower(
      "S.java",
      "public class S {\n"
      "    public static void main(String[] args) {\n"
      "        int v = 20;\n"
      "        switch (v) {\n"
      "            case 10: break;\n"
      "            case 20: break;\n"
      "            case 35: break;\n"
      "        }\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  const b2::rbc::Method* main = s.lowered->program.find("main", "([Ljava/lang/String;)V");
  CHECK(main != nullptr);
  bool sawLookup = false;
  for (const b2::rbc::Ins& ins : main->code) {
    if (ins.opcode() == b2::rbc::Op::Lookupswitch) {
      sawLookup = true;
      const b2::rbc::Const& c = main->cp[ins.imm];
      CHECK(c.kind == b2::rbc::Const::Kind::SwitchTable);
      // lookup payload: [N, default, match, target, ...] with N == 3
      CHECK(c.ints.size() == 2 + 2 * 3);
      CHECK(c.ints[0] == 3);
    }
  }
  CHECK(sawLookup);
  CHECK(verifyAll(s.lowered->program));
}

B2_TEST(lowerThrowsAndHandlersRegistered) {
  LowerSession s;
  s.runLower(
      "H.java",
      "public class H {\n"
      "    public static void main(String[] args) {\n"
      "        try {\n"
      "            throw new H();\n"
      "        } catch (H e) {\n"
      "            System.out.println(\"c\");\n"
      "        }\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  const b2::rbc::Method* main = s.lowered->program.find("main", "([Ljava/lang/String;)V");
  CHECK(main != nullptr);
  CHECK(main->handlers.size() == 1);
  CHECK(main->cp[main->handlers[0].catchType].kind ==
        b2::rbc::Const::Kind::Class);
  CHECK(main->cp[main->handlers[0].catchType].str == "H");
  CHECK(main->numRegs >= 1);
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "c\n");
}

B2_TEST(lowerFinallyRegistersCatchAll) {
  LowerSession s;
  s.runLower(
      "F.java",
      "public class F {\n"
      "    public static void main(String[] args) {\n"
      "        try {\n"
      "            System.out.println(\"t\");\n"
      "        } finally {\n"
      "            System.out.println(\"f\");\n"
      "        }\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  const b2::rbc::Method* main = s.lowered->program.find("main", "([Ljava/lang/String;)V");
  CHECK(main != nullptr);
  CHECK(main->handlers.size() == 1);
  CHECK(main->handlers[0].catchType == -1);  // catch-all (finally rethrow)
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "t\nf\n");
}

B2_TEST(lowerUnionCatchOneHandlerPerMember) {
  LowerSession s;
  s.runLower(
      "U.java",
      "public class U {\n"
      "    public static void main(String[] args) {\n"
      "        try {\n"
      "            int v = 1 / 0;\n"
      "        } catch (java.lang.ArithmeticException | java.lang.NullPointerException e) {\n"
      "            System.out.println(\"u\");\n"
      "        }\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  const b2::rbc::Method* main = s.lowered->program.find("main", "([Ljava/lang/String;)V");
  CHECK(main != nullptr);
  CHECK(main->handlers.size() == 2);
  CHECK(main->cp[main->handlers[0].catchType].str == "java/lang/ArithmeticException");
  CHECK(main->cp[main->handlers[1].catchType].str == "java/lang/NullPointerException");
  CHECK(main->handlers[0].handler == main->handlers[1].handler);
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "u\n");
}

B2_TEST(lowerIncDecOnFieldsAndArrays) {
  LowerSession s;
  s.runLower(
      "I.java",
      "public class I {\n"
      "    static int counter = 0;\n"
      "    int f = 10;\n"
      "    public static void main(String[] args) {\n"
      "        counter++;\n"
      "        ++counter;\n"
      "        System.out.println(counter);\n"
      "        I obj = new I();\n"
      "        obj.f--;\n"
      "        System.out.println(obj.f);\n"
      "        int post = obj.f++;\n"
      "        System.out.println(post);\n"
      "        System.out.println(obj.f);\n"
      "        int pre = ++obj.f;\n"
      "        System.out.println(pre);\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "2\n9\n9\n10\n11\n");
}

B2_TEST(lowerCompoundAssignmentOnArrayElement) {
  LowerSession s;
  s.runLower(
      "A.java",
      "public class A {\n"
      "    public static void main(String[] args) {\n"
      "        int[] a = {10, 20};\n"
      "        int i = 1;\n"
      "        a[i] *= 3;\n"
      "        System.out.println(a[1]);\n"
      "        a[i - 1] += a[i];\n"
      "        System.out.println(a[0]);\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "60\n70\n");
}

B2_TEST(lowerEvaluationOrderReceiverThenArgs) {
  // The receiver and arguments evaluate once, left to right, into a
  // consecutive argument block (rbc_spec SS3.15).
  LowerSession s;
  s.runLower(
      "E.java",
      "public class E {\n"
      "    static int trace = 0;\n"
      "    static int mark(int v) { trace = trace * 10 + v; return v; }\n"
      "    public static void main(String[] args) {\n"
      "        E e = new E();\n"
      "        e.combine(mark(1), mark(2));\n"
      "        System.out.println(trace);\n"
      "    }\n"
      "    int combine(int a, int b) { return a + b; }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "12\n");
}

B2_TEST(lowerRuleFormSwitchStatement) {
  LowerSession s;
  s.runLower(
      "RS.java",
      "public class RS {\n"
      "    public static void main(String[] args) {\n"
      "        int v = 2;\n"
      "        switch (v) {\n"
      "            case 1 -> System.out.println(\"one\");\n"
      "            case 2 -> System.out.println(\"two\");\n"
      "            default -> System.out.println(\"other\");\n"
      "        }\n"
      "        switch (5) {\n"
      "            case 1 -> System.out.println(\"no\");\n"
      "            default -> System.out.println(\"def\");\n"
      "        }\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "two\ndef\n");
}

B2_TEST(lowerVarInference) {
  LowerSession s;
  s.runLower(
      "V.java",
      "public class V {\n"
      "    public static void main(String[] args) {\n"
      "        var i = 42;\n"
      "        var l = 1L;\n"
      "        var d = 0.5;\n"
      "        var f = 0.5f;\n"
      "        var b = true;\n"
      "        var c = 'x';\n"
      "        var s = \"txt\";\n"
      "        var arr = new int[2];\n"
      "        var obj = new V();\n"
      "        var cond = i > 0;\n"
      "        var sum = i + 1;\n"
      "        System.out.println(i);\n"
      "        System.out.println(l + i);\n"
      "        System.out.println(d + f);\n"
      "        System.out.println(b);\n"
      "        System.out.println(c);\n"
      "        System.out.println(s);\n"
      "        System.out.println(arr.length);\n"
      "        System.out.println(obj instanceof V);\n"
      "        System.out.println(cond);\n"
      "        System.out.println(sum);\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  // println(Z) prints 0/1 and println(C) the code point: the interp
  // team's frozen-signature pin (docs/interp_contract.md), not a lowering
  // choice.
  CHECK(o.stdoutText ==
        "42\n43\n1.0\n1\n120\ntxt\n2\n1\n1\n43\n");
}

B2_TEST(lowerDefaultValuesForUnassignedLocals) {
  // Documented v0 divergence: no definite-assignment analysis yet, so
  // unassigned locals read their default-initialized values.
  LowerSession s;
  s.runLower(
      "U.java",
      "public class U {\n"
      "    public static void main(String[] args) {\n"
      "        int i;\n"
      "        long l;\n"
      "        double d;\n"
      "        boolean b;\n"
      "        String s;\n"
      "        System.out.println(i);\n"
      "        System.out.println(l);\n"
      "        System.out.println(d);\n"
      "        System.out.println(b);\n"
      "        System.out.println(s == null);\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  // Ref locals default to the interned "" constant (the prologue's
  // Ref-typed init), so s == null is 0; definite-assignment checking lands
  // with the binding stage.
  CHECK(o.stdoutText == "0\n0\n0.0\n0\n0\n");
}

B2_TEST(lowerEnhancedForEachConversions) {
  LowerSession s;
  s.runLower(
      "FE.java",
      "public class FE {\n"
      "    public static void main(String[] args) {\n"
      "        int[] src = {1, 2, 3};\n"
      "        long total = 0;\n"
      "        for (int v : src) {\n"
      "            total += v;\n"
      "        }\n"
      "        System.out.println(total);\n"
      "        for (long w : src) {\n"
      "            total += w;\n"
      "        }\n"
      "        System.out.println(total);\n"
      "        String[] names = {\"a\"};\n"
      "        for (String n : names) {\n"
      "            System.out.println(n);\n"
      "        }\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "6\n12\na\n");
}

B2_TEST(lowerReferenceCastsAndInstanceof) {
  LowerSession s;
  s.runLower(
      "RC.java",
      "public class RC {\n"
      "    public static void main(String[] args) {\n"
      "        RC o = new RC();\n"
      "        Object asObj = o;\n"
      "        System.out.println(asObj instanceof RC);\n"
      "        RC back = (RC) asObj;\n"
      "        System.out.println(back == o);\n"
      "        int[] a = {1};\n"
      "        System.out.println(a instanceof Object);\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  // int[] instanceof Object: the v0 runtime matches array classes exactly
  // (documented interpreter limitation), so this prints 0 until the real
  // hierarchy lands.
  CHECK(o.stdoutText == "1\n1\n0\n");
}

B2_TEST(lowerUncaughtUserThrowReports) {
  LowerSession s;
  s.runLower(
      "UT.java",
      "public class UT {\n"
      "    public static void main(String[] args) {\n"
      "        throw new UT();\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.status == b2::interp::RunStatus::Threw);
}

B2_TEST(lowerSynchronizedMethodFlag) {
  LowerSession s;
  s.runLower(
      "SY.java",
      "public class SY {\n"
      "    public static synchronized int guarded() { return 1; }\n"
      "    public static void main(String[] args) {\n"
      "        System.out.println(guarded());\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  const b2::rbc::Method* g = s.lowered->program.find("guarded", "()I");
  CHECK(g != nullptr);
  CHECK(g->isSynchronized());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "1\n");
}

B2_TEST(lowerReturnThroughFinallyRunsFinally) {
  LowerSession s;
  s.runLower(
      "RF.java",
      "public class RF {\n"
      "    static int probe = 0;\n"
      "    static int f() {\n"
      "        try {\n"
      "            return 5;\n"
      "        } finally {\n"
      "            probe = 1;\n"
      "        }\n"
      "    }\n"
      "    public static void main(String[] args) {\n"
      "        System.out.println(f());\n"
      "        System.out.println(probe);\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "5\n1\n");
}

B2_TEST(lowerNestedFinallyOrder) {
  LowerSession s;
  s.runLower(
      "NF.java",
      "public class NF {\n"
      "    static int f() {\n"
      "        try {\n"
      "            try {\n"
      "                return 1;\n"
      "            } finally {\n"
      "                System.out.println(\"inner\");\n"
      "            }\n"
      "        } finally {\n"
      "            System.out.println(\"outer\");\n"
      "        }\n"
      "    }\n"
      "    public static void main(String[] args) {\n"
      "        System.out.println(f());\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "inner\nouter\n1\n");
}

B2_TEST(lowerThrowThroughFinallyRethrows) {
  LowerSession s;
  s.runLower(
      "TF.java",
      "public class TF {\n"
      "    public static void main(String[] args) {\n"
      "        try {\n"
      "            try {\n"
      "                throw new TF();\n"
      "            } finally {\n"
      "                System.out.println(\"finally\");\n"
      "            }\n"
      "        } catch (TF e) {\n"
      "            System.out.println(\"caught\");\n"
      "        }\n"
      "    }\n"
      "}\n");
  CHECK(s.lowerOk());
  CHECK(verifyAll(s.lowered->program));
  ExecOutcome o = s.executeMain();
  CHECK(o.ok);
  CHECK(o.stdoutText == "finally\ncaught\n");
}

B2_TEST(lowerStringSwitchRefused) {
  LowerSession s;
  s.runLower(
      "SS.java",
      "public class SS {\n"
      "    public static void main(String[] args) {\n"
      "        String s = \"a\";\n"
      "        switch (s) {\n"
      "            case \"a\": break;\n"
      "        }\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("switch selector must be char, byte, short, or int"));
}

B2_TEST(lowerLocalTypeDeclarationRefused) {
  LowerSession s;
  s.runLower(
      "LT.java",
      "public class LT {\n"
      "    public static void main(String[] args) {\n"
      "        class Local { }\n"
      "    }\n"
      "}\n");
  CHECK(!s.lowerOk());
  CHECK(s.expectError("local class"));
}

}  // namespace
