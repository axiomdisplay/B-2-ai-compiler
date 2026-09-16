// TurboScript Tier 0 — interp.h
// The Direct-Threaded Register Interpreter (Laws Part I Tier 0).
#pragma once
#include "runtime.h"
#include "bytecode.h"
#include <deque>

namespace ts {

struct Frame {
    BytecodeFunction* bf = nullptr;
    const uint32_t* ip = nullptr;
    uint32_t baseOff = 0;      // base offset in the value stack (words)
    Context* rootCtx = nullptr; // function root context (own ctx or closure ctx)
    Context* curCtx = nullptr;  // deepest open scope context
    JSFunction* fn = nullptr;
    Value thisVal = Value::undef();
    Value lastRecv = Value::undef();
    Value ctorThis = Value::undef();  // [[Construct]]: the freshly created `this`
    bool constructing = false;        // frame was entered via OP_New
    uint8_t ctxDepth = 0;
};

class Interp {
public:
    explicit Interp(Runtime* rt);
    ~Interp();

    Value runProgram(BytecodeFunction* bf);
    Value callFunction(JSFunction* f, Value thisVal, Value* args, uint32_t argc);

    // GC root marking (called by Runtime::runGC via callback)
    void markAllRoots();

    [[noreturn]] void throwValue(Value v);

    Runtime* rt;
    Value* stack = nullptr;
    size_t stackCap = 0;       // words
    size_t stackWords = 0;     // words live (top frame end)
    std::deque<Frame> frames;

    struct TryFrame {
        jmp_buf buf;
        Frame frame;           // snapshot of the frame that entered the try
        const uint32_t* ip;    // ip to resume at on normal flow
        uint32_t handlerIdx;
        size_t frameIndex;
        size_t tryStackSize;   // trys.size() before this push
        size_t stackWordsSnap;
    };
    std::deque<TryFrame> trys;

private:
    void growStack();
    Value getNamedSlow(Object* o, String* key, IC* ic);
    void setNamedSlow(Object* o, String* key, Value v, IC* ic);
    Value runLoop(size_t entryDepth);
    friend class Runtime;
};

// shared helpers
Value makeError(Runtime* rt, const char* ctor, const char* msg);
void markDeep(Runtime* rt, Value v);

} // namespace ts
