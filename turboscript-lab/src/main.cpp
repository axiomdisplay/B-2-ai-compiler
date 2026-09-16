// TurboScript Tier 0 — main.cpp
// CLI: ts <file.js> [--bench N] [--dis] [--dump-value]
#include "parser.h"
#include "compiler.h"
#include "interp.h"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

using namespace ts;

static const char* opName(uint8_t op) {
    static const char* names[] = {
    "LdSmi",
    "LdConst",
    "LdUndef",
    "LdNull",
    "LdTrue",
    "LdFalse",
    "LdZero",
    "LdThis",
    "Mov",
    "MovChk",
    "Add",
    "Sub",
    "Mul",
    "Div",
    "Mod",
    "AddImm",
    "SubImm",
    "MulSmi",
    "LtSmi",
    "LeSmi",
    "GtSmi",
    "GeSmi",
    "BitAnd",
    "BitOr",
    "BitXor",
    "Shl",
    "Shr",
    "UShr",
    "Neg",
    "ToNum",
    "BitNot",
    "Not",
    "TypeOf",
    "Inc",
    "Dec",
    "Eq",
    "Ne",
    "StrictEq",
    "StrictNe",
    "Lt",
    "Le",
    "Gt",
    "Ge",
    "Jmp",
    "JmpIfFalse",
    "JmpIfTrue",
    "JmpIfNotNU",
    "GetNamed",
    "SetNamed",
    "GetGlobal",
    "SetGlobal",
    "GetKeyed",
    "SetKeyed",
    "Delete",
    "GetCtx",
    "SetCtx",
    "NewCtx",
    "ExitCtx",
    "LdTDZ",
    "CheckTDZ",
    "Call",
    "CallN",
    "New",
    "Return",
    "LdFn",
    "Closure",
    "NewArray",
    "NewObject",
    "TryEnter",
    "TryExit",
    "Throw",
    "InstanceOf",
    "InOp",
    "DeleteKeyed",
    "ForInKeys",
    "NOP"
    };
    return op < sizeof(names) / sizeof(names[0]) ? names[op] : "?";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.js> [--bench N] [--dis]\n", argv[0]);
        return 2;
    }
    const char* path = nullptr;
    long benchIters = 0;
    bool dis = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--dis")) { dis = true; continue; }
        if (!std::strcmp(argv[i], "--bench") && i + 1 < argc) { benchIters = std::atol(argv[++i]); continue; }
        if (!path) path = argv[i];
    }
    if (!path) { std::fprintf(stderr, "usage: %s <file.js> [--bench N] [--dis]\n", argv[0]); return 2; }
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "--bench") && i + 1 < argc) benchIters = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--dis")) dis = true;
    }

    // read file
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 2; }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> src((size_t)len + 1);
    if (len && std::fread(src.data(), 1, (size_t)len, f) != (size_t)len) { /* short read ok */ }
    std::fclose(f);

    Node* ast = nullptr;
    try {
        ast = Parser::parseProgram(src.data(), (size_t)len);
    } catch (const SyntaxErr& e) {
        std::string msg;
        for (char16_t c : e.msg) msg += (c < 128) ? (char)c : '?';
        std::fprintf(stderr, "%s:%u: %s\n", path, e.line, msg.c_str());
        return 65;
    }

    Runtime* rt = new Runtime();
    BytecodeFunction* bf = Compiler::compile(rt, ast);

    if (dis) {
        std::vector<BytecodeFunction*> stack;
        stack.push_back(bf);
        uint32_t uid = 0;
        while (!stack.empty()) {
            BytecodeFunction* b = stack.back(); stack.pop_back();
            std::printf("=== func#%u: %u words, %u consts, %u ics, %u handlers, %u regs\n",
                        uid++, b->codeLen, b->nConsts, b->nICs, b->nHandlers, b->nRegs);
            for (uint32_t i = 0; i < b->codeLen; i++) {
                uint32_t w = b->code[i];
                std::printf("%4u: %-12s a=%3u b=%3u c=%3u   ; %08x\n", i, opName(w & 0xFF),
                            (w >> 8) & 0xFF, (w >> 16) & 0xFF, (w >> 24) & 0xFF, w);
            }
            for (uint32_t i = 0; i < b->nFuncs; i++) stack.push_back(b->funcs[i]);
        }
        return 0;
    }

    Interp interp(rt);
    rt->interp = &interp;

    if (benchIters > 0) {
        using clk = std::chrono::steady_clock;
        double bestMs = 1e18, sumMs = 0;
        for (long it = 0; it < benchIters; it++) {
            // fresh function instances per run via fresh closure creation:
            // re-run the whole program from scratch
            auto t0 = clk::now();
            interp.runProgram(bf);
            auto t1 = clk::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (ms < bestMs) bestMs = ms;
            sumMs += ms;
        }
        std::fprintf(stderr, "[bench] best=%.3fms avg=%.3fms (n=%ld)\n",
                     bestMs, sumMs / benchIters, benchIters);
    } else {
        interp.runProgram(bf);
    }
    return 0;
}
