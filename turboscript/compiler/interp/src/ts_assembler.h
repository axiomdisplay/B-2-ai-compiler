// TurboScript — TSBC text assembler (.tsbc -> Module).
// Rule 47 diagnostics (file:line, expected vs actual, hint); Rule 52
// (self-contained inputs); deterministic output (Rule 124).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ts_core.h"
#include "ts_module.h"
#include "ts_object.h"

namespace ts {

// Opcode names in Opcode enum order (X-macro generated).
static constexpr const char* kOpcodeNames[] = {
#define TS_OP(name, fmt, effect, ic, roles_expr) #name,
#include "ts_opcodes.inc"
#undef TS_OP
};

class Assembler {
 public:
  Assembler(std::string file, SymbolTable& symbols);

  // Assembles the whole source text into a verified-shape module (the
  // verifier still must run; the assembler only guarantees encodability).
  [[nodiscard]] TsResult<std::unique_ptr<Module>> assemble(
      std::string_view source);

 private:
  struct PendingCatch {
    uint32_t catchReg = 0;
    std::string startLabel;
    std::string endLabel;
    std::string handlerLabel;
    uint32_t line = 0;
  };

  // One parsed instruction waiting for label resolution / encoding.
  struct PendingOp {
    Opcode op = Opcode::kNop;
    std::vector<std::string> args;  // raw operand tokens
    std::vector<uint32_t> argLines;
    uint32_t line = 0;
  };

  struct FunctionSource {
    std::string label;
    uint32_t nameSymbol = kInvalidSymbol;
    uint32_t paramCount = 0;
    uint32_t registerCount = 0;
    uint32_t line = 0;
    std::vector<PendingOp> ops;
    // label -> op index (branches resolve to "before op i")
    std::unordered_map<std::string, uint32_t> labels;
    std::vector<PendingCatch> catches;
  };

  struct ConstantSource {
    std::string label;
    Constant constant;
    uint32_t line = 0;
  };

  SymbolTable& symbols_;
  std::string file_;
  std::unique_ptr<Module> module_;
  std::vector<FunctionSource> functions_;
  std::vector<ConstantSource> constants_;
  std::unordered_map<std::string, uint32_t> constantIndex_;
  std::unordered_map<std::string, uint32_t> functionIndex_;
  std::vector<SymbolId> globalNames_;
  std::unordered_map<std::string, uint32_t> globalIndex_;
  int entryLine_ = -1;
  std::string entryLabel_;
  uint32_t line_ = 1;

  [[nodiscard]] TsDiagnostic errorAt(uint32_t line, const std::string& msg,
                                     const std::string& expected = "",
                                     const std::string& actual = "",
                                     const std::string& hint = "") const;

  // Split a source line into tokens honoring strings and comments.
  static std::vector<std::string> tokenize(const std::string& line,
                                           size_t* consumedChars);
  // Encode one function (with branch-width fixpoint).
  [[nodiscard]] TsResult<bool> encodeFunction(FunctionSource& fn);
  [[nodiscard]] TsResult<uint32_t> constIndex(const std::string& label,
                                              uint32_t line);
  [[nodiscard]] TsResult<uint32_t> functionIndex(const std::string& label,
                                                 uint32_t line);
  [[nodiscard]] TsResult<uint32_t> globalIndex(const std::string& name,
                                               uint32_t line);
  [[nodiscard]] TsResult<Constant> parseConstant(const std::string& text,
                                                 uint32_t line);
  // Parse one register token "rN"; error with Rule 47 fields on failure.
  [[nodiscard]] TsResult<uint32_t> parseRegister(const std::string& token,
                                                 uint32_t line,
                                                 uint32_t registerCount);
  [[nodiscard]] TsResult<int32_t> parseInt8(const std::string& token,
                                            uint32_t line);

  // Scratch output of encodeFunction (moved into Function::code by assemble).
  std::vector<uint32_t> encodedFnCode_;
};

}  // namespace ts
