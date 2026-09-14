// TurboScript — bytecode verifier. Enforces bytecode_spec.md Section 9:
// structure, register legality, definedness dataflow (no dead code, no
// undefined reads, no fall-off), handler ranges, feedback layout.
// Rule 105: inputs are untrusted; Rule 47: exact diagnostics.
#pragma once

#include "ts_core.h"
#include "ts_module.h"
#include "ts_object.h"

namespace ts {

class Verifier {
 public:
  // Verify every function + module structure.
  [[nodiscard]] static TsResult<bool> verify(const Module& module,
                                             const SymbolTable& symbols,
                                             const std::string& file);

  // Verify one function (flow analysis + feedback layout). Fills
  // fn.feedbackLayout and fn.slotOfPc on success.
  [[nodiscard]] static TsResult<bool> verifyFunction(const Module& module,
                                                     Function& fn,
                                                     const std::string& file);
};

}  // namespace ts
