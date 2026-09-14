// TurboScript — module/function representation loaded from TSBC text.
// Rule 15 spirit: index-based tables. Rule 124: deterministic layout.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ts_bytecode.h"
#include "ts_core.h"
#include "ts_object.h"
#include "ts_value.h"

namespace ts {

// A constant-pool entry. Strings/BigInts are materialized into the Isolate
// heap at module load (LoadConst returns stable heap pointers).
struct Constant {
  ValueKind kind = ValueKind::Undefined;  // Smi | HeapNumber | String | BigInt
  int32_t smi = 0;
  double num = 0.0;
  std::u16string text;   // String
  BigInt bigintValue;    // BigInt
};

struct Handler {
  uint32_t startPc = 0;    // inclusive, word index
  uint32_t endPc = 0;      // exclusive
  uint32_t handlerPc = 0;
  uint32_t catchReg = 0;   // register receiving the exception value
};

// Feedback slot kinds (bytecode_spec.md Section 8).
enum class FeedbackKind : uint8_t { Property, Binary, Branch, Call };

struct Function {
  SymbolId name = kInvalidSymbol;
  uint32_t index = 0;                 // position in module function table
  uint32_t paramCount = 0;
  uint32_t registerCount = 0;
  std::vector<uint32_t> code;         // 24-bit words stored in uint32
  std::vector<Handler> handlers;
  std::vector<FeedbackKind> feedbackLayout;  // verifier-computed, pc order

  // pc -> feedback slot index (kNoFeedbackSlot when none). Built with the
  // layout above after verification.
  std::vector<uint32_t> slotOfPc;
};

constexpr uint32_t kNoFeedbackSlot = 0xFFFFFFFFu;

struct Module {
  std::string name;
  std::vector<Constant> constants;
  std::vector<SymbolId> globalNames;  // interned in the Isolate at load
  std::vector<std::unique_ptr<Function>> functions;
  uint32_t entryIndex = 0;
  // Source line of each function definition (Rule 47 diagnostics).
  std::vector<uint32_t> functionLine;
};

// Instruction start bitmap for a function: which word indices begin an
// instruction. Used by enterAt (Rule 105: validate untrusted pc) and the
// verifier. Cached on first request (single-threaded T0, Rule 119).
[[nodiscard]] const std::vector<bool>& instructionStarts(const Function& fn);
[[nodiscard]] uint32_t functionWidth(const Function& fn, uint32_t pc);

// Disassembler (Rule 140 replay artifact; `tsrun --dump`).
[[nodiscard]] std::string disassembleModule(const Module& module,
                                            const SymbolTable& symbols);
[[nodiscard]] std::string disassembleFunction(const Function& fn,
                                              const SymbolTable& symbols,
                                              const Module& module,
                                              int indent);

}  // namespace ts
