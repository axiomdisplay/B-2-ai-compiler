// TurboScript — disassembler + instruction-start bitmap.
// Rule 140: replay artifacts; Rule 47: readable diagnostics support.
#include "ts_module.h"

#include <cstdio>
#include <unordered_map>

namespace ts {

uint32_t functionWidth(const Function& fn, uint32_t pc) {
  uint32_t op = fn.code[pc] & 0xFF;
  if (op >= kOpcodeSpace) return 1;
  Opcode opcode = static_cast<Opcode>(op);
  return formatWordCount(opcodeInfo(opcode).format);
}

const std::vector<bool>& instructionStarts(const Function& fn) {
  // Lazy per-function cache; T0 is single-threaded (Rule 119).
  static std::unordered_map<const Function*, std::vector<bool>> cache;
  auto it = cache.find(&fn);
  if (it != cache.end()) return it->second;
  std::vector<bool> starts(fn.code.size(), false);
  uint32_t pc = 0;
  while (pc < fn.code.size()) {
    starts[pc] = true;
    pc += functionWidth(fn, pc);
  }
  auto inserted = cache.emplace(&fn, std::move(starts));
  return inserted.first->second;
}

namespace {

std::string u16ToUtf8(const std::u16string& s) {
  std::string out;
  for (char16_t c : s) {
    if (c >= 0x20 && c < 0x7F) {
      out.push_back(static_cast<char>(c));
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\r') {
      out += "\\r";
    } else {
      char buf[16];
      std::snprintf(buf, sizeof buf, "\\u%04X", c);
      out += buf;
    }
  }
  return out;
}

std::string constantText(const Module& module, uint32_t idx) {
  if (idx >= module.constants.size()) return "<oob>";
  const Constant& c = module.constants[idx];
  switch (c.kind) {
    case ValueKind::Smi:
      return std::to_string(c.smi);
    case ValueKind::HeapNumber:
      return pure::doubleToString(c.num);
    case ValueKind::String:
      return "\"" + u16ToUtf8(c.text) + "\"";
    case ValueKind::BigInt:
      return u16ToUtf8(c.bigintValue.toString()) + "n";
    default:
      return "<?>";
  }
}

std::string opName(uint32_t op) {
  if (op >= kOpcodeSpace) return "Invalid";
  return opcodeInfo(static_cast<Opcode>(op)).name;
}

}  // namespace

std::string disassembleFunction(const Function& fn, const SymbolTable& symbols,
                                const Module& module, int indent) {
  std::string pad(indent, ' ');
  std::string out;
  char line[160];
  const auto& starts = instructionStarts(fn);

  uint32_t pc = 0;
  while (pc < fn.code.size()) {
    if (!starts[pc]) {  // suffix word: only print when reached as start
      pc++;
      continue;
    }
    uint32_t w = fn.code[pc];
    uint32_t op = w & 0xFF;
    uint32_t a = (w >> 8) & 0xFF;
    uint32_t b = (w >> 16) & 0xFF;
    Opcode opcode = static_cast<Opcode>(op);
    OpcodeInfo info = opcodeInfo(opcode);
    std::string args;
    uint32_t width = formatWordCount(info.format);

    switch (info.format) {
      case OpFormat::W0:
        break;
      case OpFormat::W1_R:
        args = "r" + std::to_string(a);
        break;
      case OpFormat::W1_RI8:
        args = "r" + std::to_string(a) + ", " + std::to_string(
                         static_cast<int8_t>(b));
        break;
      case OpFormat::W1_RR:
        args = "r" + std::to_string(a) + ", r" + std::to_string(b);
        break;
      case OpFormat::W1_J: {
        int16_t off = static_cast<int16_t>(a | (b << 8));
        args = std::to_string(pc) + " -> " + std::to_string(
                          static_cast<int32_t>(pc) + off);
        break;
      }
      case OpFormat::W1_BR: {
        int8_t off = static_cast<int8_t>(b);
        args = "r" + std::to_string(a) + ", " + std::to_string(pc) + " -> " +
               std::to_string(static_cast<int32_t>(pc) + off);
        break;
      }
      case OpFormat::W2_I24: {
        uint32_t c = fn.code[pc + 1] & 0xFF;
        uint32_t d = (fn.code[pc + 1] >> 8) & 0xFF;
        uint32_t e = (fn.code[pc + 1] >> 16) & 0xFF;
        uint32_t imm = c | (d << 8) | (e << 16);
        args = std::to_string(imm);
        if (op == static_cast<uint32_t>(Opcode::kDefineGlobalVar) &&
            imm < module.globalNames.size()) {
          args += " (" + u16ToUtf8(symbols.text(module.globalNames[imm])) + ")";
        }
        break;
      }
      case OpFormat::W2_I24D: {
        uint32_t c = fn.code[pc + 1] & 0xFF;
        uint32_t d = (fn.code[pc + 1] >> 8) & 0xFF;
        uint32_t e = (fn.code[pc + 1] >> 16) & 0xFF;
        uint32_t imm = c | (d << 8) | (e << 16);
        if (op == static_cast<uint32_t>(Opcode::kLoadConst)) {
          args = "r" + std::to_string(a) + ", @" + std::to_string(imm) + " (" +
                 constantText(module, imm) + ")";
        } else if (op == static_cast<uint32_t>(Opcode::kNewClosure)) {
          args = "r" + std::to_string(a) + ", fn@" + std::to_string(imm);
        } else {
          args = "r" + std::to_string(a) + ", " + std::to_string(imm);
        }
        break;
      }
      case OpFormat::W2_I24S: {
        uint32_t c = fn.code[pc + 1] & 0xFF;
        uint32_t d = (fn.code[pc + 1] >> 8) & 0xFF;
        uint32_t e = (fn.code[pc + 1] >> 16) & 0xFF;
        uint32_t imm = c | (d << 8) | (e << 16);
        args = "r" + std::to_string(a) + ", " + std::to_string(imm);
        if (op == static_cast<uint32_t>(Opcode::kStoreGlobal) &&
            imm < module.globalNames.size()) {
          args += " (" + u16ToUtf8(symbols.text(module.globalNames[imm])) + ")";
        }
        break;
      }
      case OpFormat::W2_RR:
      case OpFormat::W2_RR_D:
      case OpFormat::W2_RR_V:
      case OpFormat::W2_RR_C:
      case OpFormat::W2_RC_V: {
        uint32_t c = fn.code[pc + 1] & 0xFF;
        args = "r" + std::to_string(a) + ", r" + std::to_string(b) + ", " +
               std::to_string(c);
        break;
      }
      case OpFormat::W2_CALL: {
        uint32_t c = fn.code[pc + 1] & 0xFF;
        uint32_t d = (fn.code[pc + 1] >> 8) & 0xFF;
        uint32_t e = (fn.code[pc + 1] >> 16) & 0xFF;
        args = "r" + std::to_string(a) + ", r" + std::to_string(b) + ", argc=" +
               std::to_string(c) + ", this=r" + std::to_string(d) + " -> r" +
               std::to_string(e);
        break;
      }
      case OpFormat::W2_BR: {
        uint32_t c = fn.code[pc + 1] & 0xFF;
        uint32_t d = (fn.code[pc + 1] >> 8) & 0xFF;
        uint32_t e = (fn.code[pc + 1] >> 16) & 0xFF;
        int32_t off = static_cast<int32_t>(c | (d << 8) | (e << 16));
        if (off >= (1 << 23)) off -= (1 << 24);
        args = op == static_cast<uint32_t>(Opcode::kJmpWide)
                   ? std::to_string(pc) + " -> " +
                         std::to_string(static_cast<int32_t>(pc) + off)
                   : "r" + std::to_string(a) + ", " + std::to_string(pc) +
                         " -> " + std::to_string(static_cast<int32_t>(pc) + off);
        break;
      }
    }

    std::snprintf(line, sizeof line, "%s%6u: %-12s %s\n", pad.c_str(), pc,
                  opName(op).c_str(), args.c_str());
    out += line;
    pc += width;
  }

  for (const Handler& h : fn.handlers) {
    std::snprintf(line, sizeof line,
                  "%s  .catch r%u, [%u, %u) -> %u\n", pad.c_str(), h.catchReg,
                  h.startPc, h.endPc, h.handlerPc);
    out += line;
  }
  return out;
}

std::string disassembleModule(const Module& module, const SymbolTable& symbols) {
  std::string out = "module " + module.name + "\n";
  out += "entry: fn@" + std::to_string(module.entryIndex) + "\n";
  char line[160];
  for (size_t i = 0; i < module.constants.size(); i++) {
    std::snprintf(line, sizeof line, "  const @%zu = %s\n", i,
                  constantText(module, static_cast<uint32_t>(i)).c_str());
    out += line;
  }
  for (size_t i = 0; i < module.globalNames.size(); i++) {
    std::snprintf(line, sizeof line, "  global @%zu = %s\n", i,
                  u16ToUtf8(symbols.text(module.globalNames[i])).c_str());
    out += line;
  }
  for (const auto& fn : module.functions) {
    out += "function fn@" + std::to_string(fn->index) + " " +
           (fn->name < symbols.size()
                ? u16ToUtf8(symbols.text(fn->name))
                : "<anon>") +
           " params=" + std::to_string(fn->paramCount) + " regs=" +
           std::to_string(fn->registerCount) + "\n";
    out += disassembleFunction(*fn, symbols, module, 2);
  }
  return out;
}

}  // namespace ts
