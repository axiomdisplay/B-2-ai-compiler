// TurboScript Tier 0 — compiler.h
#pragma once
#include "bytecode.h"

namespace ts {
class Node;
class Runtime;

class Compiler {
public:
    // Compiles an AST program (NK::Block) to a BytecodeFunction.
    // Throws const char* on unsupported constructs (registered scope gaps).
    static BytecodeFunction* compile(Runtime* rt, Node* program);
};

} // namespace ts
