// Unit test — deopt_enter_at_reconstructs_registers (interp_contract.md 6,
// Rules 4/83): enterAt rebuilds a Tier 0 frame at an exact pc with an exact
// register file, and rejects untrusted entry inputs (Rule 105).
#include <cstdio>
#include <vector>

#include "ts_assembler.h"
#include "ts_interpreter.h"
#include "ts_verifier.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "UNIT FAIL: %s\n", what);
    failures++;
  } else {
    std::printf("ok: %s\n", what);
  }
}

const char* kSource = R"TSBC(
.module unit
.global @print
.const @K10 = 10
.entry @main

; f(a, b): return (a + b) * 10   -- pcs: Add@0, LoadConst@1, Mul@3, Mov@4, Ret@5
.function @f 2 5
  Add r0, r1
  LoadConst r2, @K10
  Mul r0, r2
  Mov r3, r0
  Return r3
.end

.function @main 0 4
  LoadGlobal r1, @print
  LoadConstS r2, 0
  Call r1, r2, 1, r0
  Return r0
.end
)TSBC";

}  // namespace

int main() {
  ts::SymbolTable symbols;
  ts::Assembler assembler("unit.tsbc", symbols);
  ts::TsResult<std::unique_ptr<ts::Module>> module = assembler.assemble(kSource);
  if (!module) {
    ts::TsDiagnostic d = module.error();
    std::fprintf(stderr, "assemble failed: %s\n", d.message.c_str());
    return 1;
  }
  ts::TsResult<bool> verified = ts::Verifier::verify(**module, symbols, "unit.tsbc");
  if (!verified) {
    ts::TsDiagnostic d = verified.error();
    std::fprintf(stderr, "verify failed: %s\n", d.message.c_str());
    return 1;
  }

  ts::Isolate isolate;
  if (!isolate.loadModule(**module, symbols)) {
    std::fprintf(stderr, "loadModule failed\n");
    return 1;
  }
  // f is function 0 (main is the entry, index 1).
  ts::Closure* closure = isolate.makeClosureFor(0, nullptr);
  check(closure != nullptr, "makeClosureFor(f) yields a closure");

  // 1. Whole-frame entry at pc 0: (2 + 3) * 10 = 50.
  {
    std::vector<ts::Value> regs(module->get()->functions[0]->registerCount,
                                ts::Value::undefined());
    regs[0] = ts::Value::smi(2);
    regs[1] = ts::Value::smi(3);
    ts::JsResult<ts::Value> r = isolate.enterAt(closure, 0, regs);
    check(r && r->i32 == 50, "enterAt at entry pc computes (2+3)*10 = 50");
  }

  // 2. Mid-frame entry at pc 3 (Mul): a deoptimizer hands back the exact
  //    post-Add register state; execution continues seamlessly.
  {
    std::vector<ts::Value> regs(module->get()->functions[0]->registerCount,
                                ts::Value::undefined());
    regs[0] = ts::Value::smi(5);   // a + b, already computed
    regs[2] = ts::Value::smi(10);  // constant, already loaded
    ts::JsResult<ts::Value> r = isolate.enterAt(closure, 3, regs);
    check(r && r->i32 == 50, "enterAt at Mul pc reconstructs registers -> 50");
  }

  // 3. Non-boundary pc (2 is LoadConst's suffix word) is rejected (Rule 105).
  {
    std::vector<ts::Value> regs(module->get()->functions[0]->registerCount,
                                ts::Value::undefined());
    ts::JsResult<ts::Value> r = isolate.enterAt(closure, 2, regs);
    check(!r, "enterAt rejects a pc that is not an instruction boundary");
  }

  // 4. Wrong register-file size is rejected.
  {
    std::vector<ts::Value> regs(3, ts::Value::undefined());
    ts::JsResult<ts::Value> r = isolate.enterAt(closure, 0, regs);
    check(!r, "enterAt rejects a mismatched register file");
  }

  if (failures != 0) {
    std::fprintf(stderr, "unit: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("unit: all enterAt checks passed\n");
  return 0;
}
