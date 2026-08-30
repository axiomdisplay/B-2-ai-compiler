#pragma once
// B-2 Frontend lowering tests - shared parse+lower+verify+execute helper.
//
// The strongest oracle for lowering output is the middle end's own
// verifier (every method must pass rbc::verify) followed by T0 execution
// (Rule 36 discipline in miniature: the interpreter is the reference
// semantics). This helper wires the three stages together for tests.

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ParseUtil.h"
#include "TestHarness.h"

#include "b2/frontend/Lower.h"
#include "b2/interp/Interp.h"
#include "b2/rbc/RbcText.h"
#include "b2/rbc/Verifier.h"

namespace b2::test {

// Verifies every method of a program; true iff ALL pass. Failures are
// recorded with the method name and pc so the failing test points at the
// exact RBC defect.
[[nodiscard]] inline bool verifyAll(const b2::rbc::Program& p) {
  for (const b2::rbc::Method& m : p.methods) {
    const b2::rbc::VerifyResult r = b2::rbc::verify(m);
    if (!r.ok) {
      for (const auto& d : r.diags) {
        b2::test::recordFailure(
            "LowerUtil.h", 0,
            "verify failed for method '" + m.name + "' pc " +
                std::to_string(d.pc) + ": " + d.message);
      }
      return false;
    }
  }
  return true;
}

struct ExecOutcome {
  bool ok = false;  // the interpreter ran (not: returned cleanly)
  b2::interp::RunStatus status = b2::interp::RunStatus::Returned;
  std::string stdoutText;
};

// Parse + lower one source; `lowered` is valid when lowering ran at all.
struct LowerSession : ParseSession {
  std::optional<b2::frontend::LoweredUnit> lowered;

  void runLower(const std::string& name, const std::string& source) {
    run(name, source);
    if (hasErrors() || !unit) {
      return;
    }
    lowered = b2::frontend::lowerUnit(*unit, sm, *diags);
  }

  [[nodiscard]] bool lowerOk() const {
    return lowered.has_value() && lowered->ok && !hasErrors();
  }

  // Rule 47 shape check: an error diagnostic containing `needle`.
  [[nodiscard]] bool expectError(std::string_view needle) const {
    if (!diags) return false;
    for (const auto& d : diags->diagnostics()) {
      if (d.severity == b2::frontend::Severity::Error &&
          d.message.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool expectWarning(std::string_view needle) const {
    if (!diags) return false;
    for (const auto& d : diags->diagnostics()) {
      if (d.severity == b2::frontend::Severity::Warning &&
          d.message.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  // Runs the program's main([Ljava/lang/String;)V (or ()V) on T0.
  [[nodiscard]] ExecOutcome executeMain() {
    ExecOutcome out;
    if (!lowerOk()) {
      return out;
    }
    const b2::rbc::Program& p = lowered->program;
    b2::interp::InterpConfig cfg;
    b2::interp::Interpreter interp(p, cfg);
    std::string entryDesc = "()V";
    if (p.find("main", entryDesc) == nullptr) {
      entryDesc = "([Ljava/lang/String;)V";
    }
    std::vector<b2::interp::Value> args;
    if (entryDesc == "([Ljava/lang/String;)V") {
      auto arr = interp.runtime().newRefArray(
          interp.runtime().stringClass(), 0);
      args.push_back(b2::interp::Value::refVal(arr));
    }
    const b2::interp::RunResult r = interp.run("main", entryDesc, args);
    out.ok = true;
    out.status = r.status;
    out.stdoutText = interp.runtime().stdout();
    return out;
  }
};

}  // namespace b2::test
