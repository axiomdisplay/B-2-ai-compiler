// TurboScript — TSBC text assembler implementation.
// Grammar: turboscript/docs/bytecode_spec.md Section 10.
#include "ts_assembler.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ts {

namespace {

constexpr int32_t kShortBranchMin = -128;
constexpr int32_t kShortBranchMax = 127;
constexpr int32_t kLongJmpMin = -32768;
constexpr int32_t kLongJmpMax = 32767;

uint32_t w1(uint32_t op, uint32_t a, uint32_t b) {
  return (op & 0xFF) | ((a & 0xFF) << 8) | ((b & 0xFF) << 16);
}

uint32_t w2(uint32_t c, uint32_t d, uint32_t e) {
  return (c & 0xFF) | ((d & 0xFF) << 8) | ((e & 0xFF) << 16);
}

void appendImm24(std::vector<uint32_t>& code, uint32_t imm) {
  code.push_back(w2(imm & 0xFF, (imm >> 8) & 0xFF, (imm >> 16) & 0xFF));
}

}  // namespace

Assembler::Assembler(std::string file, SymbolTable& symbols)
    : symbols_(symbols), file_(std::move(file)) {}

TsDiagnostic Assembler::errorAt(uint32_t line, const std::string& msg,
                                const std::string& expected,
                                const std::string& actual,
                                const std::string& hint) const {
  TsDiagnostic d;
  d.file = file_;
  d.line = line;
  d.message = msg;
  d.expected = expected;
  d.actual = actual;
  d.hint = hint;
  return d;
}

std::vector<std::string> Assembler::tokenize(const std::string& line,
                                             size_t* consumedChars) {
  // Full-line tokenizer used for directive lines; instruction lines are
  // tokenized the same way (strings may contain spaces).
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' ||
                               line[i] == ',')) i++;  // comma = separator
    if (i >= line.size() || line[i] == ';' || line[i] == '#') break;
    if (line[i] == '"') {
      std::string tok;
      tok.push_back('"');
      i++;
      while (i < line.size() && line[i] != '"') {
        if (line[i] == '\\' && i + 1 < line.size()) {
          tok.push_back(line[i]);
          tok.push_back(line[i + 1]);
          i += 2;
        } else {
          tok.push_back(line[i]);
          i++;
        }
      }
      if (i < line.size()) {
        tok.push_back('"');
        i++;
      }
      tokens.push_back(tok);
    } else {
      size_t start = i;
      while (i < line.size() && line[i] != ' ' && line[i] != '\t' &&
             line[i] != ',' && line[i] != ';' && line[i] != '#') {
        i++;
      }
      tokens.push_back(line.substr(start, i - start));
    }
  }
  if (consumedChars != nullptr) *consumedChars = i;
  return tokens;
}

TsResult<Constant> Assembler::parseConstant(const std::string& text,
                                            uint32_t line) {
  if (text.empty()) {
    return std::unexpected(
        errorAt(line, "empty constant literal", "42 | 3.5 | \"str\" | 1n", "",
                "constants are: integer, double, string, or BigInt literal"));
  }
  // BigInt literal: digits (+ optional separators) with trailing 'n'.
  if (text.size() >= 2 && text.back() == 'n' &&
      text.find_first_not_of("-_0123456789") == text.size() - 1) {
    BigInt big;
    // Strip the trailing 'n' before parsing the magnitude.
    std::u16string wide(text.begin(), text.end() - 1);
    if (!BigInt::fromString(wide, &big)) {
      return std::unexpected(errorAt(line, "invalid BigInt literal '" + text + "'",
                                     "decimal digits with optional sign",
                                     text, "example: 123n"));
    }
    Constant c;
    c.kind = ValueKind::BigInt;
    c.bigintValue = std::move(big);
    return c;
  }
  // String literal
  if (text.front() == '"') {
    if (text.size() < 2 || text.back() != '"') {
      return std::unexpected(errorAt(line, "unterminated string literal",
                                     "closing quote", text));
    }
    std::u16string out;
    size_t i = 1;
    while (i + 1 < text.size()) {
      char ch = text[i];
      if (ch != '\\') {
        out.push_back(static_cast<char16_t>(static_cast<unsigned char>(ch)));
        i++;
        continue;
      }
      i++;
      if (i >= text.size()) break;
      char esc = text[i++];
      switch (esc) {
        case 'n': out.push_back(u'\n'); break;
        case 't': out.push_back(u'\t'); break;
        case 'r': out.push_back(u'\r'); break;
        case '0': out.push_back(u'\0'); break;
        case '\\': out.push_back(u'\\'); break;
        case '"': out.push_back(u'"'); break;
        case 'u': {
          if (i + 4 > text.size()) {
            return std::unexpected(errorAt(
                line, "\\u escape needs 4 hex digits", "\\uXXXX", text.substr(i - 2)));
          }
          std::string hex = text.substr(i, 4);
          i += 4;
          char* end = nullptr;
          unsigned long cp = std::strtoul(hex.c_str(), &end, 16);
          if (end != hex.c_str() + 4) {
            return std::unexpected(errorAt(line, "invalid \\u escape",
                                           "4 hex digits", hex));
          }
          out.push_back(static_cast<char16_t>(cp));
          break;
        }
        default:
          return std::unexpected(errorAt(line,
              std::string("unknown escape '\\") + esc + "'",
              "\\n \\t \\r \\0 \\\\ \\\" \\uXXXX", text.substr(i - 2)));
      }
    }
    Constant c;
    c.kind = ValueKind::String;
    c.text = std::move(out);
    return c;
  }
  // Integer literal (fits int32 -> Smi)
  {
    char* end = nullptr;
    long long v = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() + text.size() && end != text.c_str()) {
      if (v >= kSmiMin && v <= kSmiMax) {
        Constant c;
        c.kind = ValueKind::Smi;
        c.smi = static_cast<int32_t>(v);
        return c;
      }
      Constant c;
      c.kind = ValueKind::HeapNumber;
      c.num = static_cast<double>(v);
      return c;
    }
  }
  // Double literal
  {
    char* end = nullptr;
    double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() + text.size() && end != text.c_str()) {
      Constant c;
      c.kind = ValueKind::HeapNumber;
      c.num = v;
      return c;
    }
  }
  return std::unexpected(errorAt(line, "cannot parse constant '" + text + "'",
                                 "int | double | \"string\" | 123n", text));
}

TsResult<uint32_t> Assembler::parseRegister(const std::string& token,
                                            uint32_t line,
                                            uint32_t registerCount) {
  if (token.size() < 2 || token[0] != 'r' ||
      token.find_first_not_of("0123456789", 1) != std::string::npos) {
    return std::unexpected(errorAt(line, "expected register operand",
                                   "rN (0..253)", token));
  }
  unsigned long n = std::strtoul(token.c_str() + 1, nullptr, 10);
  if (n >= kMaxUsableRegister + 1) {
    return std::unexpected(errorAt(line, "register out of architectural range",
                                   "r0..r253", token,
                                   "r254/r255 are reserved by the spec"));
  }
  if (n >= registerCount) {
    return std::unexpected(errorAt(
        line, "register exceeds this function's declared register count",
        "r0..r" + std::to_string(registerCount - 1), token,
        "raise the register count in the .function line"));
  }
  return static_cast<uint32_t>(n);
}

TsResult<int32_t> Assembler::parseInt8(const std::string& token,
                                       uint32_t line) {
  char* end = nullptr;
  long v = std::strtol(token.c_str(), &end, 10);
  if (end != token.c_str() + token.size() || v < -128 || v > 127) {
    return std::unexpected(errorAt(line, "expected int8 immediate",
                                   "-128..127", token));
  }
  return static_cast<int32_t>(v);
}

TsResult<uint32_t> Assembler::constIndex(const std::string& label,
                                         uint32_t line) {
  auto it = constantIndex_.find(label);
  if (it == constantIndex_.end()) {
    return std::unexpected(errorAt(line, "unknown constant label '@" + label + "'",
                                   "a declared .const label", "@" + label,
                                   "declare it with: .const " + label + " = <literal>"));
  }
  return it->second;
}

TsResult<uint32_t> Assembler::functionIndex(const std::string& label,
                                            uint32_t line) {
  auto it = functionIndex_.find(label);
  if (it == functionIndex_.end()) {
    return std::unexpected(errorAt(line,
                                   "unknown function label '@" + label + "'",
                                   "a declared .function label", "@" + label));
  }
  return it->second;
}

TsResult<uint32_t> Assembler::globalIndex(const std::string& name,
                                          uint32_t line) {
  auto it = globalIndex_.find(name);
  if (it == globalIndex_.end()) {
    return std::unexpected(errorAt(line, "undeclared global '@" + name + "'",
                                   "a declared .global name", "@" + name,
                                   "declare it with: .global " + name));
  }
  return it->second;
}

TsResult<std::unique_ptr<Module>> Assembler::assemble(
    std::string_view source) {
  module_ = std::make_unique<Module>();
  functions_.clear();
  constants_.clear();
  constantIndex_.clear();
  functionIndex_.clear();
  globalNames_.clear();
  globalIndex_.clear();
  entryLabel_.clear();
  entryLine_ = -1;

  const std::string text(source);
  size_t pos = 0;
  line_ = 1;
  bool inFunction = false;
  FunctionSource* current = nullptr;

  while (pos <= text.size()) {
    size_t eol = text.find('\n', pos);
    std::string line = text.substr(pos, eol == std::string::npos
                                             ? std::string::npos
                                             : eol - pos);
    uint32_t thisLine = line_;
    line_++;
    pos = eol == std::string::npos ? text.size() + 1 : eol + 1;

    std::vector<std::string> tokens = tokenize(line, nullptr);
    if (tokens.empty()) continue;
    const std::string& t0 = tokens[0];

    // Labels: "X:" possibly alone on a line.
    auto isLabelToken = [](const std::string& t) {
      return t.size() >= 2 && t.back() == ':';
    };

    if (!inFunction) {
      if (t0 == ".module") {
        if (tokens.size() < 2) {
          return std::unexpected(errorAt(thisLine, ".module needs a name",
                                         ".module <name>", line));
        }
        module_->name = tokens[1];
      } else if (t0 == ".const") {
        // .const <label> = <literal tokens...>
        if (tokens.size() < 4 || tokens[2] != "=") {
          return std::unexpected(errorAt(
              thisLine, "malformed .const",
              ".const <label> = <literal>", line,
              "label must start with @; string literals may contain spaces"));
        }
        std::string label = tokens[1];
        if (label.size() < 2 || label[0] != '@') {
          return std::unexpected(errorAt(thisLine,
              "constant label must start with '@'", "@label", label));
        }
        std::string literal;
        for (size_t i = 3; i < tokens.size(); i++) {
          if (i > 3) literal.push_back(' ');
          literal += tokens[i];
        }
        TsResult<Constant> c = parseConstant(literal, thisLine);
        if (!c) return std::unexpected(c.error());
        if (constantIndex_.count(label)) {
          return std::unexpected(errorAt(thisLine,
                                         "duplicate constant label " + label,
                                         "unique label", label));
        }
        // Keys stored without the leading '@' (lookup convention).
        constantIndex_.emplace(label.substr(1), constants_.size());
        constants_.push_back(
            ConstantSource{label.substr(1), std::move(*c), thisLine});
      } else if (t0 == ".global") {
        if (tokens.size() < 2) {
          return std::unexpected(errorAt(thisLine, ".global needs a name",
                                         ".global <name>", line));
        }
        std::string name = tokens[1];
        if (name.size() >= 1 && name[0] == '@') name = name.substr(1);
        if (globalIndex_.count(name)) {
          return std::unexpected(errorAt(thisLine, "duplicate global " + name,
                                         "unique name", name));
        }
        SymbolId sym = symbols_.intern(
            std::u16string(name.begin(), name.end()));
        globalIndex_.emplace(name, globalNames_.size());
        globalNames_.push_back(sym);
      } else if (t0 == ".entry") {
        if (tokens.size() < 2) {
          return std::unexpected(errorAt(thisLine, ".entry needs a label",
                                         ".entry <function-label>", line));
        }
        entryLabel_ = tokens[1];
        entryLine_ = static_cast<int>(thisLine);
      } else if (t0 == ".function") {
        if (tokens.size() < 4) {
          return std::unexpected(errorAt(
              thisLine, ".function needs: label, paramCount, registerCount",
              ".function <@label> <nparams> <nregs>", line));
        }
        std::string label = tokens[1];
        if (label.size() < 2 || label[0] != '@') {
          return std::unexpected(errorAt(thisLine,
              "function label must start with '@'", "@label", label));
        }
        FunctionSource fs;
        fs.label = label.substr(1);
        if (functionIndex_.count(fs.label)) {
          return std::unexpected(errorAt(thisLine,
                                         "duplicate function label " + label,
                                         "unique label", label));
        }
        fs.nameSymbol = symbols_.intern(
            std::u16string(fs.label.begin(), fs.label.end()));
        char* end = nullptr;
        long params = std::strtol(tokens[2].c_str(), &end, 10);
        if (end != tokens[2].c_str() + tokens[2].size() || params < 0 ||
            params > kMaxParams) {
          return std::unexpected(errorAt(thisLine,
              "invalid param count", "0.." + std::to_string(kMaxParams),
              tokens[2]));
        }
        long regs = std::strtol(tokens[3].c_str(), &end, 10);
        if (end != tokens[3].c_str() + tokens[3].size() || regs < 1 ||
            regs > kMaxUsableRegister + 1) {
          return std::unexpected(errorAt(thisLine, "invalid register count",
                                         "1..254", tokens[3]));
        }
        if (static_cast<uint32_t>(params) > static_cast<uint32_t>(regs)) {
          return std::unexpected(errorAt(thisLine,
              "param count exceeds register count",
              "nparams <= nregs", tokens[2] + " > " + tokens[3]));
        }
        fs.paramCount = static_cast<uint32_t>(params);
        fs.registerCount = static_cast<uint32_t>(regs);
        fs.line = thisLine;
        functionIndex_.emplace(fs.label, functions_.size());
        functions_.push_back(std::move(fs));
        current = &functions_.back();
        inFunction = true;
      } else if (isLabelToken(t0)) {
        return std::unexpected(errorAt(thisLine,
                                       "function-local label outside function",
                                       "label inside .function block", t0));
      } else {
        return std::unexpected(errorAt(thisLine,
                                       "unexpected directive outside function",
                                       ".module | .const | .global | .entry | "
                                       ".function",
                                       t0));
      }
      continue;
    }

    // ----- inside a function -----
    if (t0 == ".end") {
      inFunction = false;
      current = nullptr;
      continue;
    }
    if (isLabelToken(t0)) {
      // Store labels WITHOUT the leading '.' (branch fixpoint and .catch
      // resolution both normalize to dot-less keys).
      std::string label = t0.substr(0, t0.size() - 1);
      if (!label.empty() && label[0] == '.') label = label.substr(1);
      if (current->labels.count(label)) {
        return std::unexpected(errorAt(thisLine, "duplicate label " + label,
                                       "unique label", label));
      }
      current->labels.emplace(label,
                              static_cast<uint32_t>(current->ops.size()));
      // Trailing tokens after a label on the same line are not supported
      // (keeps diagnostics exact).
      if (tokens.size() > 1) {
        return std::unexpected(errorAt(thisLine,
            "instruction must be on its own line after a label",
            "LABEL: newline INSTRUCTION", line));
      }
      continue;
    }
    if (t0 == ".catch") {
      // .catch rX, .Lstart, .Lend, .Lhandler
      if (tokens.size() != 5) {
        return std::unexpected(errorAt(thisLine, "malformed .catch",
                                       ".catch rX, .Lstart, .Lend, .Lhandler",
                                       line));
      }
      PendingCatch pc;
      TsResult<uint32_t> reg =
          parseRegister(tokens[1], thisLine, current->registerCount);
      if (!reg) return std::unexpected(reg.error());
      pc.catchReg = *reg;
      auto strip = [](const std::string& t) {
        return t.size() > 1 && t[0] == '.' ? t.substr(1) : t;
      };
      pc.startLabel = strip(tokens[2]);
      pc.endLabel = strip(tokens[3]);
      pc.handlerLabel = strip(tokens[4]);
      pc.line = thisLine;
      current->catches.push_back(std::move(pc));
      continue;
    }

    // Instruction: <opcode> <args...>
    auto opIt = std::find_if(
        std::begin(kOpcodeNames), std::end(kOpcodeNames),
        [&](const char* n) { return t0 == n; });
    if (opIt == std::end(kOpcodeNames)) {
      return std::unexpected(errorAt(thisLine, "unknown opcode '" + t0 + "'",
                                     "a TSBC v0.1 opcode", t0,
                                     "see turboscript/docs/bytecode_spec.md"));
    }
    uint32_t opIdx = static_cast<uint32_t>(opIt - std::begin(kOpcodeNames));
    PendingOp po;
    po.op = static_cast<Opcode>(opIdx);
    po.line = thisLine;
    for (size_t i = 1; i < tokens.size(); i++) {
      po.args.push_back(tokens[i]);
      po.argLines.push_back(thisLine);
    }
    current->ops.push_back(std::move(po));
  }

  if (inFunction) {
    return std::unexpected(errorAt(line_ - 1, "unterminated .function block",
                                   ".end at function end", "<eof>"));
  }
  if (entryLabel_.empty() || entryLine_ < 0) {
    return std::unexpected(errorAt(1, "module has no .entry directive",
                                   ".entry <function-label>", "<missing>"));
  }
  // Keys are stored without the leading '@' (fs.label convention).
  std::string entryKey = entryLabel_[0] == '@' ? entryLabel_.substr(1)
                                               : entryLabel_;
  auto entryIt = functionIndex_.find(entryKey);
  if (entryIt == functionIndex_.end()) {
    return std::unexpected(errorAt(static_cast<uint32_t>(entryLine_),
                                   "unknown entry function '@" + entryLabel_ + "'",
                                   "a declared .function label",
                                   "@" + entryLabel_));
  }
  module_->entryIndex = entryIt->second;

  // Materialize module tables.
  module_->constants.resize(constants_.size());
  for (size_t i = 0; i < constants_.size(); i++) {
    module_->constants[i] = std::move(constants_[i].constant);
  }
  module_->globalNames = globalNames_;

  module_->functions.resize(functions_.size());
  module_->functionLine.resize(functions_.size());
  for (size_t i = 0; i < functions_.size(); i++) {
    FunctionSource& src = functions_[i];
    auto fn = std::make_unique<Function>();
    fn->name = src.nameSymbol;
    fn->index = static_cast<uint32_t>(i);
    fn->paramCount = src.paramCount;
    fn->registerCount = src.registerCount;
    module_->functionLine[i] = src.line;
    TsResult<bool> encoded = encodeFunction(src);
    if (!encoded) return std::unexpected(encoded.error());
    // encodeFunction writes into a temporary; retrieve via fn->code below.
    fn->code = std::move(encodedFnCode_);
    // Resolve handler table (dot-less label keys).
    for (const PendingCatch& pc : src.catches) {
      Handler h;
      auto stripDot = [](const std::string& s) {
        return !s.empty() && s[0] == '.' ? s.substr(1) : s;
      };
      auto sIt = src.labels.find(stripDot(pc.startLabel));
      auto eIt = src.labels.find(stripDot(pc.endLabel));
      auto hIt = src.labels.find(stripDot(pc.handlerLabel));
      if (sIt == src.labels.end() || eIt == src.labels.end() ||
          hIt == src.labels.end()) {
        return std::unexpected(errorAt(pc.line, "unknown .catch label",
                                       ".Lstart/.Lend/.Lhandler in function",
                                       "." + pc.startLabel));
      }
      // Labels record op INDICES; the handler table stores word pcs. Convert
      // by walking the encoded code (encoded widths include widened branches).
      auto opIndexToPc = [&](uint32_t idx) -> uint32_t {
        uint32_t p = 0;
        for (uint32_t oi = 0; oi < idx && p < fn->code.size(); oi++) {
          uint32_t w = fn->code[p];
          p += formatWordCount(
              opcodeInfo(static_cast<Opcode>(w & 0xFF)).format);
        }
        return p;
      };
      h.startPc = opIndexToPc(sIt->second);
      h.endPc = opIndexToPc(eIt->second);
      h.handlerPc = opIndexToPc(hIt->second);
      h.catchReg = pc.catchReg;
      if (h.endPc <= h.startPc) {
        return std::unexpected(errorAt(pc.line,
                                       "empty try range (.Lend must come after .Lstart)",
                                       "Lstart < Lend",
                                       "[" + std::to_string(h.startPc) + ", " +
                                           std::to_string(h.endPc) + ")"));
      }
      fn->handlers.push_back(h);
    }
    module_->functions[i] = std::move(fn);
  }

  return std::move(module_);
}

// ---------------------------------------------------------------------------
// Encoding with branch-width fixpoint (short branches assumed first; widen
// monotonic until all offsets fit — bounded iterations, Rule 10 spirit).
// ---------------------------------------------------------------------------
TsResult<bool> Assembler::encodeFunction(FunctionSource& src) {
  enum class BranchState : uint8_t { Unknown, Short, Long };
  std::vector<BranchState> branchState(src.ops.size(), BranchState::Unknown);
  // Map op index -> pc (recomputed each iteration).
  std::vector<uint32_t> opPc(src.ops.size() + 1, 0);

  auto opWidth = [&](size_t i) -> uint32_t {
    const PendingOp& po = src.ops[i];
    OpFormat fmt = opcodeInfo(po.op).format;
    switch (po.op) {
      case Opcode::kJmp:
        // Short unless it cannot fit in 16 bits; decided by fixpoint.
        return branchState[i] == BranchState::Long
                   ? formatWordCount(OpFormat::W2_BR)
                   : formatWordCount(OpFormat::W1_J);
      case Opcode::kJmpTrue:
      case Opcode::kJmpFalse:
        return branchState[i] == BranchState::Long
                   ? formatWordCount(OpFormat::W2_BR)
                   : formatWordCount(OpFormat::W1_BR);
      default:
        return formatWordCount(fmt);
    }
  };

  bool changed = true;
  int iterations = 0;
  constexpr int kMaxBranchFixpoint = 8;  // named bound (Rule 23)
  while (changed) {
    if (++iterations > kMaxBranchFixpoint) {
      return std::unexpected(errorAt(src.line,
                                     "branch widening did not converge",
                                     "<= " + std::to_string(kMaxBranchFixpoint) +
                                         " iterations",
                                     "diverged"));
    }
    changed = false;
    uint32_t pc = 0;
    for (size_t i = 0; i < src.ops.size(); i++) {
      opPc[i] = pc;
      pc += opWidth(i);
    }
    opPc[src.ops.size()] = pc;

    for (size_t i = 0; i < src.ops.size(); i++) {
      Opcode op = src.ops[i].op;
      if (op != Opcode::kJmp && op != Opcode::kJmpTrue &&
          op != Opcode::kJmpFalse) {
        continue;
      }
      if (branchState[i] == BranchState::Long) continue;
      // Target label is the last argument (Jmp) or second (JmpTrue/JmpFalse).
      const std::string& label =
          op == Opcode::kJmp ? src.ops[i].args[0] : src.ops[i].args[1];
      std::string name =
          label.size() > 1 && label[0] == '.' ? label.substr(1) : label;
      auto it = src.labels.find(name);
      if (it == src.labels.end()) {
        return std::unexpected(errorAt(src.ops[i].line,
                                       "unknown label '" + label + "'",
                                       "a label in this function", label));
      }
      int64_t target = static_cast<int64_t>(opPc[it->second]);
      int64_t rel = target - static_cast<int64_t>(opPc[i]);
      int32_t limit = op == Opcode::kJmp ? kLongJmpMax : kShortBranchMax;
      int32_t floor = op == Opcode::kJmp ? kLongJmpMin : kShortBranchMin;
      if (rel < floor || rel > limit) {
        branchState[i] = BranchState::Long;
        changed = true;
      }
    }
  }

  // Final encode.
  encodedFnCode_.clear();
  auto labelPc = [&](const std::string& token, uint32_t line,
                     uint32_t fromOp) -> TsResult<int32_t> {
    std::string name =
        token.size() > 1 && token[0] == '.' ? token.substr(1) : token;
    auto it = src.labels.find(name);
    if (it == src.labels.end()) {
      return std::unexpected(errorAt(line, "unknown label '" + token + "'",
                                     "a label in this function", token));
    }
    int64_t rel = static_cast<int64_t>(opPc[it->second]) -
                  static_cast<int64_t>(opPc[fromOp]);
    return static_cast<int32_t>(rel);
  };

  for (size_t i = 0; i < src.ops.size(); i++) {
    const PendingOp& po = src.ops[i];
    const uint32_t line = po.line;
    Opcode op = po.op;
    auto argc = [&]() {
      return static_cast<uint32_t>(po.args.size());
    };
    auto need = [&](uint32_t n) -> TsResult<bool> {
      if (argc() != n) {
        return std::unexpected(errorAt(
            line,
            std::string("opcode '") + opcodeInfo(op).name + "' expects " +
                std::to_string(n) + " operand(s)",
            std::to_string(n), std::to_string(argc())));
      }
      return true;
    };
    auto reg = [&](uint32_t argIdx) -> TsResult<uint32_t> {
      return parseRegister(po.args[argIdx], po.argLines[argIdx],
                           src.registerCount);
    };

    switch (op) {
      case Opcode::kNop:
      case Opcode::kRethrow: {
        TsResult<bool> ok = need(0);
        if (!ok) return std::unexpected(ok.error());
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), 0, 0));
        break;
      }
      case Opcode::kLoadUndefined:
      case Opcode::kLoadNull:
      case Opcode::kLoadTrue:
      case Opcode::kLoadFalse:
      case Opcode::kNewObject:
      case Opcode::kNewArray:
      case Opcode::kGetContext:
      case Opcode::kReturn:
      case Opcode::kThrow: {
        TsResult<bool> ok = need(1);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> d = reg(0);
        if (!d) return std::unexpected(d.error());
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *d, 0));
        break;
      }
      case Opcode::kMov:
      case Opcode::kGetPrototype: {
        TsResult<bool> ok = need(2);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> d = reg(0);
        if (!d) return std::unexpected(d.error());
        TsResult<uint32_t> s = reg(1);
        if (!s) return std::unexpected(s.error());
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *d, *s));
        break;
      }
      case Opcode::kLoadConstS: {
        TsResult<bool> ok = need(2);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> d = reg(0);
        if (!d) return std::unexpected(d.error());
        TsResult<int32_t> imm = parseInt8(po.args[1], line);
        if (!imm) return std::unexpected(imm.error());
        encodedFnCode_.push_back(
            w1(static_cast<uint32_t>(op), *d,
               static_cast<uint32_t>(static_cast<uint8_t>(*imm))));
        break;
      }
      case Opcode::kLoadConst: {
        TsResult<bool> ok = need(2);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> d = reg(0);
        if (!d) return std::unexpected(d.error());
        std::string lbl = po.args[1];
        if (lbl.empty() || lbl[0] != '@') {
          return std::unexpected(errorAt(line, "LoadConst needs @const",
                                         "@label", lbl));
        }
        TsResult<uint32_t> ci = constIndex(lbl.substr(1), line);
        if (!ci) return std::unexpected(ci.error());
        if (*ci >= kMaxConstantPool) {
          return std::unexpected(errorAt(line, "constant index exceeds imm24",
                                         "< 2^24", std::to_string(*ci)));
        }
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *d, 0));
        appendImm24(encodedFnCode_, *ci);
        break;
      }
      case Opcode::kNewClosure: {
        TsResult<bool> ok = need(2);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> d = reg(0);
        if (!d) return std::unexpected(d.error());
        std::string lbl = po.args[1];
        if (lbl.empty() || lbl[0] != '@') {
          return std::unexpected(errorAt(line, "NewClosure needs @function",
                                         "@label", lbl));
        }
        TsResult<uint32_t> fi = functionIndex(lbl.substr(1), line);
        if (!fi) return std::unexpected(fi.error());
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *d, 0));
        appendImm24(encodedFnCode_, *fi);
        break;
      }
      case Opcode::kDefineGlobalVar: {
        TsResult<bool> ok = need(1);
        if (!ok) return std::unexpected(ok.error());
        std::string lbl = po.args[0];
        if (lbl.empty() || lbl[0] != '@') {
          return std::unexpected(errorAt(line, "DefineGlobalVar needs @global",
                                         "@name", lbl));
        }
        TsResult<uint32_t> gi = globalIndex(lbl.substr(1), line);
        if (!gi) return std::unexpected(gi.error());
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), 0, 0));
        appendImm24(encodedFnCode_, *gi);
        break;
      }
      case Opcode::kLoadGlobal:
      case Opcode::kStoreGlobal: {
        TsResult<bool> ok = need(2);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> r = reg(0);
        if (!r) return std::unexpected(r.error());
        std::string lbl = po.args[1];
        if (lbl.empty() || lbl[0] != '@') {
          return std::unexpected(errorAt(line, "global op needs @name",
                                         "@name", lbl));
        }
        TsResult<uint32_t> gi = globalIndex(lbl.substr(1), line);
        if (!gi) return std::unexpected(gi.error());
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *r, 0));
        appendImm24(encodedFnCode_, *gi);
        break;
      }
      case Opcode::kGetProperty:
      case Opcode::kGetElement:
      case Opcode::kDeleteProperty:
      case Opcode::kHasProperty:
      case Opcode::kSetPrototype:
      case Opcode::kInstanceof:
      case Opcode::kIn:
      case Opcode::kCharCodeAt: {
        TsResult<bool> ok = need(3);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> d = reg(0);
        if (!d) return std::unexpected(d.error());
        TsResult<uint32_t> a = reg(1);
        if (!a) return std::unexpected(a.error());
        TsResult<uint32_t> b = reg(2);
        if (!b) return std::unexpected(b.error());
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *d, *a));
        encodedFnCode_.push_back(w2(*b, 0, 0));
        break;
      }
      case Opcode::kSetProperty:
      case Opcode::kSetElement: {
        TsResult<bool> ok = need(3);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> o = reg(0);
        if (!o) return std::unexpected(o.error());
        TsResult<uint32_t> k = reg(1);
        if (!k) return std::unexpected(k.error());
        TsResult<uint32_t> v = reg(2);
        if (!v) return std::unexpected(v.error());
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *o, *k));
        encodedFnCode_.push_back(w2(*v, 0, 0));
        break;
      }
      case Opcode::kLoadContext:
      case Opcode::kNewContext: {
        TsResult<bool> ok = need(3);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> d = reg(0);
        if (!d) return std::unexpected(d.error());
        TsResult<uint32_t> c = reg(1);
        if (!c) return std::unexpected(c.error());
        TsResult<int32_t> imm = parseInt8(po.args[2], line);
        if (!imm) return std::unexpected(imm.error());
        if (*imm < 0) {
          return std::unexpected(errorAt(line, "cell count/index must be >= 0",
                                         "0..127", po.args[2]));
        }
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *d, *c));
        encodedFnCode_.push_back(
            w2(static_cast<uint32_t>(*imm), 0, 0));
        break;
      }
      case Opcode::kStoreContext: {
        TsResult<bool> ok = need(3);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> c = reg(0);
        if (!c) return std::unexpected(c.error());
        TsResult<int32_t> imm = parseInt8(po.args[1], line);
        if (!imm) return std::unexpected(imm.error());
        if (*imm < 0) {
          return std::unexpected(errorAt(line, "cell index must be >= 0",
                                         "0..127", po.args[1]));
        }
        TsResult<uint32_t> v = reg(2);
        if (!v) return std::unexpected(v.error());
        encodedFnCode_.push_back(
            w1(static_cast<uint32_t>(op), *c,
               static_cast<uint32_t>(static_cast<uint8_t>(*imm))));
        encodedFnCode_.push_back(w2(*v, 0, 0));
        break;
      }
      case Opcode::kCall:
      case Opcode::kConstruct: {
        TsResult<bool> ok = need(4);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> f = reg(0);
        if (!f) return std::unexpected(f.error());
        TsResult<uint32_t> b = reg(1);
        if (!b) return std::unexpected(b.error());
        char* end = nullptr;
        long argc = std::strtol(po.args[2].c_str(), &end, 10);
        if (end != po.args[2].c_str() + po.args[2].size() || argc < 0 ||
            argc > static_cast<long>(kMaxCallArgs)) {
          return std::unexpected(errorAt(line, "invalid argc",
                                         "0..255", po.args[2]));
        }
        TsResult<uint32_t> d = reg(3);
        if (!d) return std::unexpected(d.error());
        if (*b + static_cast<uint32_t>(argc) > src.registerCount) {
          return std::unexpected(errorAt(line,
              "argument window exceeds register count",
              "argsBase + argc <= " + std::to_string(src.registerCount),
              std::to_string(*b) + " + " + std::to_string(argc)));
        }
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *f, *b));
        encodedFnCode_.push_back(
            w2(static_cast<uint32_t>(argc), 0, *d));
        break;
      }
      case Opcode::kCallMethod: {
        TsResult<bool> ok = need(5);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> f = reg(0);
        if (!f) return std::unexpected(f.error());
        TsResult<uint32_t> t = reg(1);
        if (!t) return std::unexpected(t.error());
        TsResult<uint32_t> b = reg(2);
        if (!b) return std::unexpected(b.error());
        char* end = nullptr;
        long argc = std::strtol(po.args[3].c_str(), &end, 10);
        if (end != po.args[3].c_str() + po.args[3].size() || argc < 0 ||
            argc > static_cast<long>(kMaxCallArgs)) {
          return std::unexpected(errorAt(line, "invalid argc",
                                         "0..255", po.args[3]));
        }
        TsResult<uint32_t> d = reg(4);
        if (!d) return std::unexpected(d.error());
        if (*b + static_cast<uint32_t>(argc) > src.registerCount) {
          return std::unexpected(errorAt(line,
              "argument window exceeds register count",
              "argsBase + argc <= " + std::to_string(src.registerCount),
              std::to_string(*b) + " + " + std::to_string(argc)));
        }
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *f, *b));
        encodedFnCode_.push_back(
            w2(static_cast<uint32_t>(argc), *t, *d));
        break;
      }
      case Opcode::kJmp: {
        TsResult<bool> ok = need(1);
        if (!ok) return std::unexpected(ok.error());
        TsResult<int32_t> rel = labelPc(po.args[0], line,
                                        static_cast<uint32_t>(i));
        if (!rel) return std::unexpected(rel.error());
        if (branchState[i] == BranchState::Long) {
          encodedFnCode_.push_back(w1(static_cast<uint32_t>(Opcode::kJmpWide), 0, 0));
          uint32_t off = static_cast<uint32_t>(*rel) & 0xFFFFFF;
          appendImm24(encodedFnCode_, off);
        } else {
          int16_t off = static_cast<int16_t>(*rel);
          encodedFnCode_.push_back(w1(static_cast<uint32_t>(op),
                                      static_cast<uint32_t>(off & 0xFF),
                                      static_cast<uint32_t>((off >> 8) & 0xFF)));
        }
        break;
      }
      case Opcode::kJmpTrue:
      case Opcode::kJmpFalse: {
        TsResult<bool> ok = need(2);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> c = reg(0);
        if (!c) return std::unexpected(c.error());
        TsResult<int32_t> rel = labelPc(po.args[1], line,
                                        static_cast<uint32_t>(i));
        if (!rel) return std::unexpected(rel.error());
        if (branchState[i] == BranchState::Long) {
          uint32_t wide = static_cast<uint32_t>(
              op == Opcode::kJmpTrue ? Opcode::kJmpTrueWide
                                     : Opcode::kJmpFalseWide);
          encodedFnCode_.push_back(w1(wide, *c, 0));
          uint32_t off = static_cast<uint32_t>(*rel) & 0xFFFFFF;
          appendImm24(encodedFnCode_, off);
        } else {
          encodedFnCode_.push_back(
              w1(static_cast<uint32_t>(op), *c,
                 static_cast<uint32_t>(static_cast<uint8_t>(*rel))));
        }
        break;
      }
      case Opcode::kNeg:
      case Opcode::kInc:
      case Opcode::kDec:
      case Opcode::kBitNot:
      case Opcode::kLogicalNot:
      case Opcode::kToBoolean:
      case Opcode::kTypeOf:
      case Opcode::kToNumber:
      case Opcode::kToNumeric:
      case Opcode::kToString:
      case Opcode::kToInt32:
      case Opcode::kToUint32:
      case Opcode::kToBigInt:
      case Opcode::kToPrimitive:
      case Opcode::kStringFromCharCode:
      case Opcode::kStringLength:
      case Opcode::kBigIntNeg: {
        // Unary W1_R family: single register operand, in-place transform.
        TsResult<bool> ok = need(1);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> d = reg(0);
        if (!d) return std::unexpected(d.error());
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *d, 0));
        break;
      }
      default: {
        // Two-address arithmetic/comparison/conversion family: r, r.
        TsResult<bool> ok = need(2);
        if (!ok) return std::unexpected(ok.error());
        TsResult<uint32_t> d = reg(0);
        if (!d) return std::unexpected(d.error());
        TsResult<uint32_t> s = reg(1);
        if (!s) return std::unexpected(s.error());
        encodedFnCode_.push_back(w1(static_cast<uint32_t>(op), *d, *s));
        break;
      }
    }
  }
  return true;
}

}  // namespace ts
