// TurboScript Tier 0 — compiler_impl.h (internal; shared by compiler*.cpp)
#pragma once
#include "compiler.h"
#include "parser.h"
#include "runtime.h"
#include <unordered_map>
#include <vector>

namespace ts {

struct VarInfo {
    bool isLetConst = false;
    bool captured = false;
    uint16_t reg = 0;
    uint16_t ctxIdx = 0;
};

struct Scope {
    std::unordered_map<std::u16string, VarInfo> vars;
    bool needCtx = false;     // holds captured vars -> materialize Context
    bool isFnScope = false;
    bool isProgram = false;
    uint16_t regBase = 0;
    uint16_t nCtxCells = 0;
    uint32_t gDepth = 0;      // global ctx-chain depth (codegen-time, needCtx only)
    VarInfo* find(const std::u16string& n) {
        auto it = vars.find(n);
        return it == vars.end() ? nullptr : &it->second;
    }
};

struct FnUnit {
    BytecodeFunction* bf = new BytecodeFunction();
    Node* fnNode = nullptr;   // function AST node (for param names at init)
    std::vector<uint32_t> code;
    std::vector<Value> consts;
    std::vector<IC> ics;
    std::vector<Handler> handlers;
    std::vector<BytecodeFunction*> funcs;
    uint16_t nextReg = 0;
    uint16_t peakReg = 0;   // high-water mark: frame size must cover this
    uint16_t nParams = 0;
    uint16_t thisReg = 0;   // reserved register holding `this` for the frame
    bool hasThisReg = false;
    Scope* fnScope = nullptr;
    bool needOwnCtx = false;
    uint32_t rootGDepth = 0;
    uint32_t ctxDepthNow = 0;      // runtime-open context scopes in this function

    struct Loop {
        std::vector<uint32_t> breaks, continues;
        size_t tryCountAtEntry = 0;
        uint32_t ctxDepthAtEntry = 0;
        bool canBreak = true, canContinue = true;
    };
    std::vector<Loop> loops;
    struct TryRegion {
        bool hasFinally = false;
        uint32_t finEntry = 0;
        uint16_t flagReg = 0, valReg = 0, exReg = 0;
        std::vector<uint32_t> retJumps;  // return-site jumps to patch at finEntry
    };
    std::vector<TryRegion> openTrys;
};

enum class RKind : uint8_t { LOCAL, CTX, GLOBAL };

struct Resolved {
    RKind kind = RKind::GLOBAL;
    uint16_t reg = 0;
    uint16_t hops = 0;
    uint16_t idx = 0;
    bool letConst = false;
};

class CompilerImpl {
public:
    explicit CompilerImpl(Runtime* r) : rt(r) {}
    ~CompilerImpl();

    BytecodeFunction* compileProgram(Node* program);

    [[noreturn]] static void fail(const char* msg, Node* n = nullptr);

    // ---- pass 1: capture analysis (compiler.cpp) ----
    void analyzeProgram(Node* n);
    void analyzeFn(Node* f, bool bindNameEnclosing);
    void analyze(Node* n);
    void addVar(Scope* s, const std::u16string& name, bool letConst);

    // ---- resolution (compiler.cpp) ----
    bool resolve(const std::u16string& name, Resolved& out);

    // ---- codegen framework (compiler.cpp) ----
    uint16_t allocReg(FnUnit& u);
    void freeRegsTo(FnUnit& u, uint16_t n);
    uint32_t here(FnUnit& u);
    void emit1(FnUnit& u, uint8_t op, uint8_t a);
    void emit2(FnUnit& u, uint8_t op, uint8_t a, uint8_t b);
    void emit3(FnUnit& u, uint8_t op, uint8_t a, uint8_t b, uint8_t c);
    void emitW(FnUnit& u, uint32_t w);
    void emitExt(FnUnit& u, uint32_t idx);
    uint32_t emitJmp24p(FnUnit& u);
    void emitJmp24(FnUnit& u, int32_t offset);
    uint32_t emitCondJmp(FnUnit& u, uint8_t op, uint8_t cond);
    void patchJmp24(FnUnit& u, uint32_t at, uint32_t target);
    void patchCond(FnUnit& u, uint32_t at, uint32_t target);
    uint32_t addIC(FnUnit& u, String* key);
    uint32_t addConst(FnUnit& u, Value v);
    String* internU(const std::u16string& s);
    Scope* nearestFnScope();
    void enterScope(FnUnit& u, Scope* s);
    void exitScope(FnUnit& u, Scope* s);
    bool anyOpenFinally(FnUnit& u);

    // ---- expressions (compiler.cpp) ----
    void emitExpr(Node* n, uint16_t dst);
    void emitFuncNode(Node* f, uint16_t dst);
    void emitLoadVar(const Resolved& r, uint16_t dst);
    void emitStoreVar(const Resolved& r, uint16_t src);
    void emitIdent(Node* n, uint16_t dst);
    void emitBinaryOp(Node* n, uint16_t dst);
    void emitArith(Node* n, uint16_t dst);
    void emitCompound(FnUnit& u, uint8_t aop, uint16_t lhs, uint16_t rhs);
    bool simpleBaseReg(Node* base, uint16_t* outReg, FnUnit* u);
    void emitStoreToTarget(Node* target, uint16_t valReg);

    // ---- statements + functions (compiler2.cpp) ----
    void emitStmt(Node* n);
    void emitBlockScoped(Node* n);
    void emitVarDecl(Node* n);
    void emitTry(Node* n);
    void closeRegionsTo(FnUnit& u, FnUnit::Loop& L);
    void setupFunctionScope(FnUnit* u, Node* f);
    void initFnLocals(FnUnit* u);
    BytecodeFunction* compileFunction(Node* f);
    void finalizeFunction(FnUnit& u);

    Runtime* rt;
    std::vector<Scope*> scopes;
    std::vector<FnUnit*> fns;
    std::unordered_map<Node*, Scope*> nodeScope;  // persistent analysis scopes
    std::vector<Scope*> scopeArena;
    uint32_t ctxCounter = 0;
};

} // namespace ts
