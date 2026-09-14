// TurboScript — verifier implementation (bytecode_spec.md Section 9).
// Flow analysis: worklist over branch + handler edges, definedness lattice
// with union join (monotonic, terminates — Rule 10 spirit applied to the
// verifier itself). Feedback layout is computed here (Section 8).
#include "ts_verifier.h"

#include <bitset>

namespace ts {

namespace {

constexpr uint32_t kBadJumpTarget = 0xFFFFFFFFu;

TsDiagnostic verr(const std::string& file, const Module& module,
                  uint32_t fnIndex, uint32_t pc, const std::string& msg,
                  const std::string& expected = "",
                  const std::string& actual = "",
                  const std::string& hint = "") {
  TsDiagnostic d;
  d.file = file;
  d.line = fnIndex < module.functionLine.size() ? module.functionLine[fnIndex]
                                                : 0;
  d.message = "fn@" + std::to_string(fnIndex) + " pc " + std::to_string(pc) +
              ": " + msg;
  d.expected = expected;
  d.actual = actual;
  d.hint = hint;
  return d;
}

std::vector<bool> computeStarts(const Function& fn, bool* aligned) {
  std::vector<bool> starts(fn.code.size(), false);
  *aligned = false;
  uint32_t pc = 0;
  while (pc < fn.code.size()) {
    starts[pc] = true;
    pc += functionWidth(fn, pc);
  }
  *aligned = (pc == fn.code.size());
  return starts;
}

std::vector<std::vector<uint32_t>> handlersCovering(const Function& fn) {
  std::vector<std::vector<uint32_t>> per(fn.code.size());
  for (uint32_t hi = 0; hi < fn.handlers.size(); hi++) {
    const Handler& h = fn.handlers[hi];
    for (uint32_t pc = h.startPc; pc < h.endPc && pc < per.size(); pc++) {
      per[pc].push_back(hi);
    }
  }
  return per;
}

using DefSet = std::bitset<kMaxRegisters>;

}  // namespace

TsResult<bool> Verifier::verify(const Module& module,
                                const SymbolTable& symbols,
                                const std::string& file) {
  // Symbol table participates in diagnostics in a later revision; the
  // signature is part of the verifier contract (bytecode_spec.md §9).
  (void)symbols;
  if (module.functions.empty()) {
    TsDiagnostic d;
    d.file = file;
    d.message = "module has no functions";
    return std::unexpected(d);
  }
  if (module.entryIndex >= module.functions.size()) {
    TsDiagnostic d;
    d.file = file;
    d.message = "entry function index out of range (expected < " +
                std::to_string(module.functions.size()) + ", got " +
                std::to_string(module.entryIndex) + ")";
    return std::unexpected(d);
  }
  for (const auto& fn : module.functions) {
    TsResult<bool> ok = verifyFunction(module, *fn, file);
    if (!ok) return std::unexpected(ok.error());
  }
  return true;
}

TsResult<bool> Verifier::verifyFunction(const Module& module, Function& fn,
                                        const std::string& file) {
  const uint32_t fnIndex = fn.index;
  if (fn.registerCount < 1 || fn.registerCount > kMaxUsableRegister + 1) {
    return std::unexpected(verr(file, module, fnIndex, 0,
                                "register count out of range", "1..254",
                                std::to_string(fn.registerCount)));
  }
  if (fn.paramCount > fn.registerCount) {
    return std::unexpected(verr(file, module, fnIndex, 0,
                                "paramCount exceeds registerCount",
                                "params <= regs",
                                std::to_string(fn.paramCount) + " > " +
                                    std::to_string(fn.registerCount)));
  }
  if (fn.code.empty()) {
    return std::unexpected(
        verr(file, module, fnIndex, 0, "function has no code"));
  }
  if (fn.code.size() > kMaxModuleCodeWords) {
    return std::unexpected(verr(file, module, fnIndex, 0,
                                "function code exceeds size budget"));
  }

  bool aligned = false;
  std::vector<bool> starts = computeStarts(fn, &aligned);
  if (!aligned) {
    return std::unexpected(verr(
        file, module, fnIndex, 0,
        "trailing truncated instruction (code length is not a sum of opcode "
        "widths)"));
  }

  // ---- Static operand checks: reserved/overflowing registers, windows. ----
  for (uint32_t pc = 0; pc < fn.code.size();) {
    uint32_t w = fn.code[pc];
    uint32_t opByte = w & 0xFF;
    if (!opcodeDefined(opByte)) {
      return std::unexpected(verr(file, module, fnIndex, pc,
                                  "undefined opcode byte",
                                  "a TSBC v0.1 opcode",
                                  std::to_string(opByte)));
    }
    Opcode opcode = static_cast<Opcode>(opByte);
    OpcodeInfo info = opcodeInfo(opcode);
    uint32_t width = formatWordCount(info.format);
    // fields[0] is the opcode byte; fields[1..5] are A,B,C,D,E.
    uint32_t fields[6] = {opByte, (w >> 8) & 0xFF, (w >> 16) & 0xFF, 0, 0, 0};
    if (width == 2) {
      uint32_t s = fn.code[pc + 1];
      fields[3] = s & 0xFF;          // C
      fields[4] = (s >> 8) & 0xFF;   // D
      fields[5] = (s >> 16) & 0xFF;  // E
    }
    // Operand view: indexed by kPosA..kPosE (0..4).
    const uint32_t ops[5] = {fields[1], fields[2], fields[3], fields[4],
                             fields[5]};

    if (info.format == OpFormat::W2_CALL) {
      // A=func B=argsBase C=argc D=this E=dst
      for (uint32_t f : {ops[0], ops[1], ops[3], ops[4]}) {
        if (f >= fn.registerCount) {
          return std::unexpected(verr(file, module, fnIndex, pc,
                                      std::string(info.name) +
                                          " register exceeds registerCount",
                                      "< " + std::to_string(fn.registerCount),
                                      "r" + std::to_string(f)));
        }
      }
      if (ops[1] + fields[3] > fn.registerCount) {
        return std::unexpected(verr(file, module, fnIndex, pc,
                                    "argument window exceeds registerCount",
                                    "argsBase + argc <= regs",
                                    std::to_string(ops[1]) + " + " +
                                        std::to_string(fields[3])));
      }
      pc += width;
      continue;
    }

    OperandRoles r = opcodeRoles(opcode);
    for (uint8_t i = 0; i < r.readCount; i++) {
      uint32_t f = ops[r.readPos[i]];
      if (f > kMaxUsableRegister) {
        return std::unexpected(verr(file, module, fnIndex, pc,
                                    "read of reserved register r254/r255",
                                    "r0..r253", "r" + std::to_string(f)));
      }
      if (f >= fn.registerCount) {
        return std::unexpected(verr(file, module, fnIndex, pc,
                                    "read register exceeds registerCount",
                                    "< " + std::to_string(fn.registerCount),
                                    "r" + std::to_string(f)));
      }
    }
    for (uint8_t i = 0; i < r.writeCount; i++) {
      uint32_t f = ops[r.writePos[i]];
      if (f > kMaxUsableRegister) {
        return std::unexpected(verr(file, module, fnIndex, pc,
                                    "write to reserved register r254/r255",
                                    "r0..r253", "r" + std::to_string(f)));
      }
      if (f >= fn.registerCount) {
        return std::unexpected(verr(file, module, fnIndex, pc,
                                    "write register exceeds registerCount",
                                    "< " + std::to_string(fn.registerCount),
                                    "r" + std::to_string(f)));
      }
    }
    pc += width;
  }

  // ---- Handler sanity. ----
  for (const Handler& h : fn.handlers) {
    if (h.startPc >= h.endPc || h.endPc > fn.code.size() ||
        !starts[h.startPc] ||
        (h.endPc < fn.code.size() && !starts[h.endPc])) {
      return std::unexpected(verr(file, module, fnIndex, h.startPc,
                                  "handler range invalid or misaligned",
                                  "[startPc, endPc) on instruction starts",
                                  "[" + std::to_string(h.startPc) + ", " +
                                      std::to_string(h.endPc) + ")"));
    }
    if (h.handlerPc >= fn.code.size() || !starts[h.handlerPc]) {
      return std::unexpected(verr(file, module, fnIndex, h.handlerPc,
                                  "handlerPc is not an instruction start"));
    }
    if (h.catchReg > kMaxUsableRegister || h.catchReg >= fn.registerCount) {
      return std::unexpected(verr(file, module, fnIndex, h.handlerPc,
                                  "catchReg exceeds registerCount"));
    }
  }

  // ---- Flow analysis: definedness + reachability to fixpoint. ----
  std::vector<DefSet> state(fn.code.size());
  std::vector<bool> queued(fn.code.size(), false);
  std::vector<bool> everReached(fn.code.size(), false);
  DefSet entry;
  for (uint32_t i = 0; i < fn.paramCount; i++) entry.set(i);

  std::vector<DefSet> handlerState(fn.handlers.size());
  std::vector<bool> handlerActive(fn.handlers.size(), false);
  std::vector<std::vector<uint32_t>> covering = handlersCovering(fn);
  std::vector<uint32_t> worklist;

  auto push = [&](uint32_t pc, const DefSet& st) {
    if (pc >= fn.code.size() || !starts[pc]) return;
    DefSet joined = state[pc] | st;
    if (joined != state[pc] || !everReached[pc]) {
      state[pc] = joined;
      everReached[pc] = true;
      if (!queued[pc]) {
        queued[pc] = true;
        worklist.push_back(pc);
      }
    }
  };

  auto recomputeHandler = [&](uint32_t hi) {
    const Handler& h = fn.handlers[hi];
    DefSet joined;
    bool any = false;
    for (uint32_t pc = h.startPc; pc < h.endPc; pc++) {
      if (!everReached[pc]) continue;
      any = true;
      joined |= state[pc];
    }
    if (!any) return;
    DefSet st = joined;
    st.set(h.catchReg);
    if (!handlerActive[hi] || st != handlerState[hi]) {
      handlerState[hi] = st;
      handlerActive[hi] = true;
      push(h.handlerPc, st);
    }
  };

  push(0, entry);
  while (!worklist.empty()) {
    uint32_t pc = worklist.back();
    worklist.pop_back();
    queued[pc] = false;
    DefSet st = state[pc];

    uint32_t w = fn.code[pc];
    Opcode opcode = static_cast<Opcode>(w & 0xFF);
    OpcodeInfo info = opcodeInfo(opcode);
    uint32_t width = formatWordCount(info.format);
    // Same layout as the static pass: fields[0]=op byte, fields[1..5]=A..E.
    uint32_t fields[6] = {w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF, 0, 0, 0};
    if (width == 2) {
      uint32_t s = fn.code[pc + 1];
      fields[3] = s & 0xFF;          // C
      fields[4] = (s >> 8) & 0xFF;   // D
      fields[5] = (s >> 16) & 0xFF;  // E
    }
    const uint32_t ops[5] = {fields[1], fields[2], fields[3], fields[4],
                             fields[5]};

    if (info.format == OpFormat::W2_CALL) {
      // Definedness for calls: func, the whole args window, and — for
      // CallMethod only — the receiver (Call's D field is a don't-care 0).
      if (!st.test(ops[0])) {
        return std::unexpected(verr(
            file, module, fnIndex, pc,
            std::string("read of possibly-undefined register r") +
                std::to_string(ops[0]) + " by " + info.name,
            "defined on all incoming paths", "undefined on some path",
            "initialize the register on every path reaching here"));
      }
      if (opcode == Opcode::kCallMethod && !st.test(ops[3])) {
        return std::unexpected(verr(
            file, module, fnIndex, pc,
            std::string("read of possibly-undefined register r") +
                std::to_string(ops[3]) + " (receiver) by CallMethod",
            "defined on all incoming paths", "undefined on some path",
            "initialize the register on every path reaching here"));
      }
      for (uint32_t k = 0; k < fields[3]; k++) {  // args window
        uint32_t f = ops[1] + k;
        if (!st.test(f)) {
          return std::unexpected(verr(
              file, module, fnIndex, pc,
              std::string("read of possibly-undefined register r") +
                  std::to_string(f) + " (call argument) by " + info.name,
              "defined on all incoming paths", "undefined on some path",
              "initialize the register on every path reaching here"));
        }
      }
    } else {
      OperandRoles r = opcodeRoles(opcode);
      for (uint8_t i = 0; i < r.readCount; i++) {
        uint32_t f = ops[r.readPos[i]];
        if (!st.test(f)) {
          return std::unexpected(verr(
              file, module, fnIndex, pc,
              std::string("read of possibly-undefined register r") +
                  std::to_string(f) + " by " + info.name,
              "defined on all incoming paths", "undefined on some path",
              "initialize the register on every path reaching here"));
        }
      }
    }

    DefSet after = st;
    if (info.format == OpFormat::W2_CALL) {
      after.set(ops[4]);  // dst (E)
    } else {
      OperandRoles r = opcodeRoles(opcode);
      for (uint8_t i = 0; i < r.writeCount; i++) {
        after.set(ops[r.writePos[i]]);
      }
    }

    auto jumpTarget = [&]() -> uint32_t {
      int32_t off = 0;
      if (info.format == OpFormat::W2_BR) {
        // imm24 in word2 bytes C,D,E (fields[3..5]).
        off = static_cast<int32_t>(fields[3] | (fields[4] << 8) |
                                   (fields[5] << 16));
        if (off >= (1 << 23)) off -= (1 << 24);
      } else if (info.format == OpFormat::W1_J) {
        // 16-bit signed offset in bytes A,B (fields[1..2]).
        off = static_cast<int16_t>(fields[1] | (fields[2] << 8));
      } else {  // W1_BR: off8 in B (fields[2])
        off = static_cast<int8_t>(static_cast<uint8_t>(fields[2]));
      }
      int64_t t = static_cast<int64_t>(pc) + off;
      if (t < 0 || t >= static_cast<int64_t>(fn.code.size()) ||
          !starts[static_cast<size_t>(t)]) {
        return kBadJumpTarget;
      }
      return static_cast<uint32_t>(t);
    };

    switch (opcode) {
      case Opcode::kReturn:
      case Opcode::kThrow:
      case Opcode::kRethrow:
        // Return: frame exits. Throw/Rethrow: exception edges only.
        break;
      case Opcode::kJmp:
      case Opcode::kJmpWide: {
        uint32_t target = jumpTarget();
        if (target == kBadJumpTarget) {
          return std::unexpected(verr(file, module, fnIndex, pc,
                                      "jump target outside function or "
                                      "misaligned"));
        }
        push(target, after);
        break;
      }
      case Opcode::kJmpTrue:
      case Opcode::kJmpFalse:
      case Opcode::kJmpTrueWide:
      case Opcode::kJmpFalseWide: {
        uint32_t target = jumpTarget();
        if (target == kBadJumpTarget) {
          return std::unexpected(verr(file, module, fnIndex, pc,
                                      "branch target outside function or "
                                      "misaligned"));
        }
        push(target, after);
        if (pc + width >= fn.code.size()) {
          return std::unexpected(verr(file, module, fnIndex, pc,
                                      "conditional branch falls off the end "
                                      "of the function"));
        }
        push(pc + width, after);
        break;
      }
      default: {
        if (pc + width >= fn.code.size()) {
          return std::unexpected(verr(file, module, fnIndex, pc,
                                      "execution falls off the end of the "
                                      "function without Return",
                                      "every path ends in Return",
                                      "fallthrough past the last "
                                      "instruction"));
        }
        push(pc + width, after);
        break;
      }
    }

    // Exception edges: covering handlers observe this pc's state.
    for (uint32_t hi : covering[pc]) {
      recomputeHandler(hi);
    }
  }

  // Unreachable code (Rule 60: no untested paths; dead bytecode is an error).
  for (uint32_t pc = 0; pc < fn.code.size();) {
    if (!everReached[pc]) {
      return std::unexpected(verr(file, module, fnIndex, pc,
                                  "unreachable instruction",
                                  "all instructions reachable from entry",
                                  "pc " + std::to_string(pc) +
                                      " is never executed; remove dead code "
                                      "or make it reachable"));
    }
    pc += functionWidth(fn, pc);
  }

  // ---- Feedback layout (bytecode_spec.md Section 8). ----
  fn.feedbackLayout.clear();
  fn.slotOfPc.assign(fn.code.size(), kNoFeedbackSlot);
  for (uint32_t pc = 0; pc < fn.code.size();) {
    Opcode opcode = static_cast<Opcode>(fn.code[pc] & 0xFF);
    OpcodeInfo info = opcodeInfo(opcode);
    if (info.ic) {
      FeedbackKind kind;
      if (info.format == OpFormat::W2_CALL) {
        kind = FeedbackKind::Call;
      } else if (info.format == OpFormat::W1_BR ||
                 info.format == OpFormat::W2_BR) {
        kind = FeedbackKind::Branch;
      } else if (opcode == Opcode::kGetProperty ||
                 opcode == Opcode::kSetProperty) {
        kind = FeedbackKind::Property;
      } else if (opcode == Opcode::kGetElement ||
                 opcode == Opcode::kSetElement) {
        kind = FeedbackKind::Element;
      } else {
        kind = FeedbackKind::Binary;
      }
      fn.slotOfPc[pc] = static_cast<uint32_t>(fn.feedbackLayout.size());
      fn.feedbackLayout.push_back(kind);
    }
    pc += formatWordCount(info.format);
  }
  return true;
}

}  // namespace ts
