// TurboScript Tier 0 — interp.cpp
// Computed-goto direct-threaded register interpreter.
//
// Perf architecture (each decision targets a known interpreter bottleneck):
//   1. computed goto dispatch (indirect threading) - no switch bounds checks,
//      hardware indirect branch predictor trains per handler site.
//   2. fixed 32-bit instruction words - operand decode is 3 shifts, no
//      variable-length re-sync.
//   3. contiguous value stack, callee frame = caller base + caller nRegs -
//      call sequence is a memcpy of args, zero allocation.
//   4. NaN-boxed values with unboxed doubles - float benchmarks never touch
//      the heap for arithmetic (Ignition boxes non-Smi doubles).
//   5. monomorphic ICs on property access - shape check + slot load.
//   6. GC safepoints only at loop backedges and calls (Rule 88-style),
//      so mid-expression allocations never collect.
#include "interp.h"
#include "parser.h"
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

namespace ts {

Interp::Interp(Runtime* r) : rt(r) {
    stackCap = 1u << 16; // 64K words = 512 KB
    stack = (Value*)std::malloc(stackCap * sizeof(Value));
    for (size_t i = 0; i < stackCap; i++) stack[i] = Value::fromBits(0); // 0.0 doubles: GC-safe
    rt->gcCtx = this;
    rt->gcMarkRoots = [](void* ctx) { ((Interp*)ctx)->markAllRoots(); };
}

Interp::~Interp() { std::free(stack); }

void Interp::growStack() {
    size_t nc = stackCap * 2;
    Value* ns = (Value*)std::malloc(nc * sizeof(Value));
    std::memcpy(ns, stack, stackWords * sizeof(Value));
    for (size_t i = stackWords; i < nc; i++) ns[i] = Value::fromBits(0);
    std::free(stack);
    stack = ns;
    stackCap = nc;
}

Value Interp::callFunction(JSFunction* f, Value thisVal, Value* args, uint32_t argc) {
    if (!f || f->kind != (uint8_t)ObjKind::Function) rt->throwTypeError("not a function");
    if (f->fn.native) return f->fn.native(rt, thisVal, args, argc);
    BytecodeFunction* bf = f->fn.bf;
    uint32_t newBase = (uint32_t)stackWords;
    uint32_t need = newBase + bf->nRegs;
    if (need > stackCap) {
        stackWords = need;
        growStack();
    }
    stackWords = need;
    Value* base = stack + newBase;
    uint32_t np = argc < bf->nParams ? argc : bf->nParams;
    for (uint32_t i = 0; i < np; i++) base[i] = args[i];
    for (uint32_t i = np; i < bf->nParams; i++) base[i] = Value::undef();
    frames.emplace_back();
    Frame& nf = frames.back();
    nf.bf = bf;
    nf.ip = bf->code;
    nf.baseOff = newBase;
    nf.rootCtx = f->fn.ctx;
    nf.curCtx = f->fn.ctx;
    nf.fn = f;
    nf.thisVal = thisVal;
    nf.ctxDepth = 0;
    size_t entryDepth = frames.size();
    Value ret = runLoop(entryDepth);
    frames.pop_back();               // runLoop leaves the entry frame in place
    stackWords = newBase;            // restore stack high-water to caller end
    return ret;
}

Value Interp::runProgram(BytecodeFunction* bf) {
    uint32_t need = bf->nRegs;
    if (need > stackCap) { stackWords = need; growStack(); }
    stackWords = need;
    frames.emplace_back();
    Frame& nf = frames.back();
    nf.bf = bf;
    nf.ip = bf->code;
    nf.baseOff = 0;
    nf.rootCtx = nullptr;
    nf.curCtx = nullptr;
    nf.fn = nullptr;
    nf.thisVal = Value::undef();
    nf.ctxDepth = 0;
    size_t entryDepth = frames.size();
    Value ret = runLoop(entryDepth);
    frames.pop_back();
    stackWords = 0;
    return ret;
}

// ---------------------------------------------------------------- GC marking
void markDeep(Runtime* rt, Value v) {
    if (!v.isHeap()) return;
    std::vector<HeapObj*> wl;
    wl.push_back(v.asObj());
    while (!wl.empty()) {
        HeapObj* o = wl.back();
        wl.pop_back();
        if (o->marked) continue;
        o->marked = true;
        switch ((ObjKind)o->kind) {
            case ObjKind::String: break;
            case ObjKind::Context: {
                Context* c = (Context*)o;
                if (c->parent) wl.push_back((HeapObj*)c->parent);
                for (uint32_t i = 0; i < c->nCells; i++) {
                    Value cv = c->cells[i];
                    if (cv.isHeap()) wl.push_back(cv.asObj());
                }
                break;
            }
            case ObjKind::Object:
            case ObjKind::Function: {
                Object* ob = (Object*)o;
                if (ob->proto) wl.push_back((HeapObj*)ob->proto);
                if (!ob->dictMode) {
                    uint32_t n = ob->shape ? ob->shape->propCount : 0;
                    for (uint32_t i = 0; i < n && i < ob->slotCap; i++) {
                        Value sv = ob->slots[i];
                        if (sv.isHeap()) wl.push_back(sv.asObj());
                    }
                } else {
                    ob->dict->forEach([&](String* k, Value val) {
                        if (val.isHeap()) wl.push_back(val.asObj());
                    });
                }
                for (uint32_t i = 0; i < ob->elemLen && i < ob->elemCap; i++) {
                    Value ev = ob->elements[i];
                    if (ev.isHeap()) wl.push_back(ev.asObj());
                }
                if (ob->kind == (uint8_t)ObjKind::Function) {
                    JSFunction* f = (JSFunction*)ob;
                    if (f->fn.ctx) wl.push_back((HeapObj*)f->fn.ctx);
                    if (f->fn.hasCtorProto && f->fn.ctorProto)
                        wl.push_back((HeapObj*)f->fn.ctorProto);
                }
                break;
            }
        }
    }
}

void Interp::markAllRoots() {
    // global object (deep)
    if (rt->globalObj) markDeep(rt, Value::fromObj((HeapObj*)rt->globalObj));
    // interned strings (permanent keys)
    for (auto& kv : rt->internTable_) {
        HeapObj* s = (HeapObj*)kv.second;
        s->marked = true;
    }
    // value stack
    for (size_t i = 0; i < stackWords && i < stackCap; i++) {
        Value v = stack[i];
        if (v.isHeap()) markDeep(rt, v);
    }
    // frames: context chains + this + receiver + IC-cached proto holders
    for (Frame& fr : frames) {
        if (fr.rootCtx) markDeep(rt, Value::fromObj((HeapObj*)fr.rootCtx));
        if (fr.curCtx) markDeep(rt, Value::fromObj((HeapObj*)fr.curCtx));
        if (fr.fn) markDeep(rt, Value::fromObj((HeapObj*)fr.fn));
        markDeep(rt, fr.thisVal);
        markDeep(rt, fr.lastRecv);
        BytecodeFunction* b2 = fr.bf;
        for (uint32_t i = 0; i < b2->nICs; i++) {
            if (b2->ics[i].holder) markDeep(rt, Value::fromObj((HeapObj*)b2->ics[i].holder));
        }
    }
    // try-frame snapshots (this/receiver/ctx held for unwinding)
    for (TryFrame& tf : trys) {
        if (tf.frame.rootCtx) markDeep(rt, Value::fromObj((HeapObj*)tf.frame.rootCtx));
        if (tf.frame.curCtx) markDeep(rt, Value::fromObj((HeapObj*)tf.frame.curCtx));
        if (tf.frame.fn) markDeep(rt, Value::fromObj((HeapObj*)tf.frame.fn));
        markDeep(rt, tf.frame.thisVal);
        markDeep(rt, tf.frame.lastRecv);
    }
    // pending exception
    markDeep(rt, Value::fromBits((uint64_t)rt->pendingException.bits));
    // IC-cached prototype holders (conservative: keep until program end)
    // — reachable only via frames' bf; bfs are permanent so their ICs live;
    //   holders referenced there are marked here through a registry walk.
    // (See Interp::markICRoots wired below.)
}

// ---------------------------------------------------------------- throw
[[noreturn]] void Interp::throwValue(Value v) {
    rt->pendingException.bits = v.bits;
    if (trys.empty()) {
        String* s = nullptr;
        if (v.isObject()) {
            Value name, msg;
            if (getProperty(rt, v, rt->intern("name"), &name) &&
                getProperty(rt, v, rt->intern("message"), &msg)) {
                String* n = rt->toStringObj(name);
                String* m = rt->toStringObj(msg);
                s = stringConcat(rt, stringConcat(rt, n, rt->intern(": ")), m);
            }
        }
        if (!s) s = rt->toStringObj(v);
        std::fwrite("Uncaught exception: ", 1, 20, stderr);
        for (uint32_t i = 0; i < s->len; i++) {
            char c = (s->data()[i] < 128) ? (char)s->data()[i] : '?';
            std::fputc(c, stderr);
        }
        std::fputc('\n', stderr);
        std::exit(1);
    }
    TryFrame& tf = trys.back();
    BytecodeFunction* bf = tf.frame.bf;
    Handler* h = &bf->handlers[tf.handlerIdx];
    // snapshot info before mutating trys
    uint8_t creg = h->catchReg;
    uint16_t ctxDepthTarget = h->ctxDepth;
    uint32_t target = h->target;
    longjmp(tf.buf, 1);
    (void)creg; (void)ctxDepthTarget; (void)target; (void)bf;
}

// ---------------------------------------------------------------- main loop
#define TS_DISPATCH() do { goto *disp[*ip & 0xFF]; } while (0)
#define TS_NEXT() do { ip++; TS_DISPATCH(); } while (0)
#define TS_NEXT2() do { ip += 2; TS_DISPATCH(); } while (0)
#define TS_SEXT24(x) ((int32_t)(((uint32_t)(x) & 0x800000u) ? ((uint32_t)(x) | 0xFF000000u) : (uint32_t)(x)))
#define TS_SEXT16(x) ((int32_t)(int16_t)(uint16_t)(x))

static inline Value numResult(double d) {
    // prefer smi when the double is integral and in range (cheap div/mod win)
    if (d >= -2147483648.0 && d <= 2147483647.0 && d == (double)(int32_t)d)
        return Value::fromSmi((int32_t)d);
    return Value::fromDouble(d);
}

static inline int32_t toInt32(double d) {
    if (std::isnan(d) || std::isinf(d)) return 0;
    d = std::fmod(std::trunc(d), 4294967296.0);
    if (d < 0) d += 4294967296.0;
    return (int32_t)(uint32_t)d;
}

Value Interp::runLoop(size_t entryDepth) {
    // Dispatch table (GCC C++ lacks designated array initializers -> fill once)
    static const void* disp[OP_COUNT];
    static bool dispInit = false;
    if (!dispInit) {
        disp[OP_LdSmi] = &&L_OP_LdSmi; disp[OP_LdConst] = &&L_OP_LdConst;
        disp[OP_LdUndef] = &&L_OP_LdUndef; disp[OP_LdNull] = &&L_OP_LdNull;
        disp[OP_LdTrue] = &&L_OP_LdTrue; disp[OP_LdFalse] = &&L_OP_LdFalse;
        disp[OP_LdZero] = &&L_OP_LdZero; disp[OP_LdThis] = &&L_OP_LdThis;
        disp[OP_Mov] = &&L_OP_Mov; disp[OP_MovChk] = &&L_OP_MovChk;
        disp[OP_Add] = &&L_OP_Add; disp[OP_Sub] = &&L_OP_Sub; disp[OP_Mul] = &&L_OP_Mul;
        disp[OP_AddImm] = &&L_OP_AddImm; disp[OP_SubImm] = &&L_OP_SubImm;
        disp[OP_MulSmi] = &&L_OP_MulSmi;
        disp[OP_LtSmi] = &&L_OP_LtSmi; disp[OP_LeSmi] = &&L_OP_LeSmi;
        disp[OP_GtSmi] = &&L_OP_GtSmi; disp[OP_GeSmi] = &&L_OP_GeSmi;
        disp[OP_Div] = &&L_OP_Div; disp[OP_Mod] = &&L_OP_Mod;
        disp[OP_BitAnd] = &&L_OP_BitAnd; disp[OP_BitOr] = &&L_OP_BitOr;
        disp[OP_BitXor] = &&L_OP_BitXor; disp[OP_Shl] = &&L_OP_Shl;
        disp[OP_Shr] = &&L_OP_Shr; disp[OP_UShr] = &&L_OP_UShr;
        disp[OP_Neg] = &&L_OP_Neg; disp[OP_ToNum] = &&L_OP_ToNum;
        disp[OP_BitNot] = &&L_OP_BitNot; disp[OP_Not] = &&L_OP_Not;
        disp[OP_TypeOf] = &&L_OP_TypeOf; disp[OP_Inc] = &&L_OP_Inc; disp[OP_Dec] = &&L_OP_Dec;
        disp[OP_Eq] = &&L_OP_Eq; disp[OP_Ne] = &&L_OP_Ne;
        disp[OP_StrictEq] = &&L_OP_StrictEq; disp[OP_StrictNe] = &&L_OP_StrictNe;
        disp[OP_Lt] = &&L_OP_Lt; disp[OP_Le] = &&L_OP_Le; disp[OP_Gt] = &&L_OP_Gt; disp[OP_Ge] = &&L_OP_Ge;
        disp[OP_Jmp] = &&L_OP_Jmp; disp[OP_JmpIfFalse] = &&L_OP_JmpIfFalse;
        disp[OP_JmpIfTrue] = &&L_OP_JmpIfTrue; disp[OP_JmpIfNotNU] = &&L_OP_JmpIfNotNU;
        disp[OP_GetNamed] = &&L_OP_GetNamed; disp[OP_SetNamed] = &&L_OP_SetNamed;
        disp[OP_GetGlobal] = &&L_OP_GetGlobal; disp[OP_SetGlobal] = &&L_OP_SetGlobal;
        disp[OP_GetKeyed] = &&L_OP_GetKeyed; disp[OP_SetKeyed] = &&L_OP_SetKeyed;
        disp[OP_Delete] = &&L_OP_Delete; disp[OP_DeleteKeyed] = &&L_OP_DeleteKeyed;
        disp[OP_GetCtx] = &&L_OP_GetCtx; disp[OP_SetCtx] = &&L_OP_SetCtx;
        disp[OP_NewCtx] = &&L_OP_NewCtx; disp[OP_ExitCtx] = &&L_OP_ExitCtx;
        disp[OP_LdTDZ] = &&L_OP_LdTDZ; disp[OP_CheckTDZ] = &&L_OP_CheckTDZ;
        disp[OP_Call] = &&L_OP_Call; disp[OP_CallN] = &&L_OP_CallN; disp[OP_New] = &&L_OP_New;
        disp[OP_Return] = &&L_OP_Return; disp[OP_LdFn] = &&L_OP_LdFn;
        disp[OP_Closure] = &&L_OP_Closure;
        disp[OP_NewArray] = &&L_OP_NewArray; disp[OP_NewObject] = &&L_OP_NewObject;
        disp[OP_TryEnter] = &&L_OP_TryEnter; disp[OP_TryExit] = &&L_OP_TryExit;
        disp[OP_Throw] = &&L_OP_Throw;
        disp[OP_InstanceOf] = &&L_OP_InstanceOf; disp[OP_InOp] = &&L_OP_InOp;
        disp[OP_ForInKeys] = &&L_OP_ForInKeys; disp[OP_NOP] = &&L_OP_NOP;
        dispInit = true;
    }

    Frame* f = &frames.back();
    BytecodeFunction* bf = f->bf;
    const uint32_t* ip = f->ip;
    Value* r = stack + f->baseOff;

    TS_DISPATCH();

// ---- loads ----
L_OP_LdSmi: {
    uint32_t w = *ip;
    r[(w >> 8) & 0xFF] = Value::fromSmi((int8_t)((w >> 16) & 0xFF));
    TS_NEXT();
}
L_OP_LdConst: {
    uint32_t w = *ip;
    r[(w >> 8) & 0xFF] = bf->consts[(w >> 16) & 0xFFFF];
    TS_NEXT();
}
L_OP_LdUndef: { r[(*ip >> 8) & 0xFF] = Value::undef(); TS_NEXT(); }
L_OP_LdNull:  { r[(*ip >> 8) & 0xFF] = Value::null_(); TS_NEXT(); }
L_OP_LdTrue:  { r[(*ip >> 8) & 0xFF] = Value::true_(); TS_NEXT(); }
L_OP_LdFalse: { r[(*ip >> 8) & 0xFF] = Value::false_(); TS_NEXT(); }
L_OP_LdZero:  { r[(*ip >> 8) & 0xFF] = Value::fromSmi(0); TS_NEXT(); }
L_OP_LdThis:  { r[(*ip >> 8) & 0xFF] = f->thisVal; TS_NEXT(); }
L_OP_LdFn:    { r[(*ip >> 8) & 0xFF] = Value::fromObj((HeapObj*)f->fn); TS_NEXT(); }
L_OP_Mov: {
    uint32_t w = *ip;
    r[(w >> 8) & 0xFF] = r[(w >> 16) & 0xFF];
    TS_NEXT();
}
L_OP_MovChk: {
    uint32_t w = *ip;
    Value v = r[(w >> 16) & 0xFF];
    if (v.isTDZ()) rt->throwReferenceError("Cannot access before initialization");
    r[(w >> 8) & 0xFF] = v;
    TS_NEXT();
}

// ---- arithmetic ----
L_OP_Add: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF, c = (w >> 24) & 0xFF;
    Value x = r[b], y = r[c];
    if (x.isSmi() && y.isSmi()) {
        int64_t s = (int64_t)x.asSmi() + (int64_t)y.asSmi();
        r[a] = (s == (int32_t)s) ? Value::fromSmi((int32_t)s) : Value::fromDouble((double)s);
        TS_NEXT();
    }
    if (x.isDouble() && y.isDouble()) {
        r[a] = Value::fromDouble(x.asDouble() + y.asDouble());
        TS_NEXT();
    }
    if (x.isNumber() && y.isNumber()) {
        r[a] = Value::fromDouble(x.asNumber() + y.asNumber());
        TS_NEXT();
    }
    // string path
    if (x.isString() && y.isString()) {
        r[a] = Value::fromObj((HeapObj*)stringConcat(rt, x.asStringUnchecked(), y.asStringUnchecked()));
        TS_NEXT();
    }
    Value px = rt->toPrimitiveValue(x, "default");
    Value py = rt->toPrimitiveValue(y, "default");
    if (px.isString() || py.isString()) {
        String* sx = rt->toStringObj(px);
        String* sy = rt->toStringObj(py);
        r[a] = Value::fromObj((HeapObj*)stringConcat(rt, sx, sy));
    } else {
        r[a] = Value::fromDouble(rt->toDoubleForArith(px) + rt->toDoubleForArith(py));
    }
    TS_NEXT();
}
L_OP_Sub: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF, c = (w >> 24) & 0xFF;
    Value x = r[b], y = r[c];
    if (x.isSmi() && y.isSmi()) {
        int64_t s = (int64_t)x.asSmi() - (int64_t)y.asSmi();
        r[a] = (s == (int32_t)s) ? Value::fromSmi((int32_t)s) : Value::fromDouble((double)s);
        TS_NEXT();
    }
    if (x.isDouble() && y.isDouble()) { r[a] = Value::fromDouble(x.asDouble() - y.asDouble()); TS_NEXT(); }
    if (x.isNumber() && y.isNumber()) { r[a] = Value::fromDouble(x.asNumber() - y.asNumber()); TS_NEXT(); }
    r[a] = Value::fromDouble(rt->toDoubleForArith(rt->toPrimitiveValue(x, "number")) -
                             rt->toDoubleForArith(rt->toPrimitiveValue(y, "number")));
    TS_NEXT();
}
L_OP_Mul: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF, c = (w >> 24) & 0xFF;
    Value x = r[b], y = r[c];
    if (x.isSmi() && y.isSmi()) {
        int64_t s = (int64_t)x.asSmi() * (int64_t)y.asSmi();
        r[a] = (s == (int32_t)s) ? Value::fromSmi((int32_t)s) : Value::fromDouble((double)s);
        TS_NEXT();
    }
    if (x.isDouble() && y.isDouble()) { r[a] = Value::fromDouble(x.asDouble() * y.asDouble()); TS_NEXT(); }
    if (x.isNumber() && y.isNumber()) { r[a] = Value::fromDouble(x.asNumber() * y.asNumber()); TS_NEXT(); }
    r[a] = Value::fromDouble(rt->toDoubleForArith(rt->toPrimitiveValue(x, "number")) *
                             rt->toDoubleForArith(rt->toPrimitiveValue(y, "number")));
    TS_NEXT();
}
L_OP_Div: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF, c = (w >> 24) & 0xFF;
    Value x = r[b], y = r[c];
    if (x.isNumber() && y.isNumber())
        r[a] = numResult(x.asNumber() / y.asNumber());
    else
        r[a] = numResult(rt->toDoubleForArith(rt->toPrimitiveValue(x, "number")) /
                         rt->toDoubleForArith(rt->toPrimitiveValue(y, "number")));
    TS_NEXT();
}
L_OP_Mod: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF, c = (w >> 24) & 0xFF;
    Value x = r[b], y = r[c];
    if (x.isNumber() && y.isNumber()) {
        double m = std::fmod(x.asNumber(), y.asNumber());
        r[a] = numResult(m);
        TS_NEXT();
    }
    r[a] = numResult(std::fmod(rt->toDoubleForArith(rt->toPrimitiveValue(x, "number")),
                               rt->toDoubleForArith(rt->toPrimitiveValue(y, "number"))));
    TS_NEXT();
}
// ---- superinstructions (fused immediate operand) ----
L_OP_AddImm: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF;
    int32_t imm = (int8_t)((w >> 24) & 0xFF);
    Value x = r[b];
    if (x.isSmi()) {
        int64_t s = (int64_t)x.asSmi() + imm;
        r[a] = (s == (int32_t)s) ? Value::fromSmi((int32_t)s) : Value::fromDouble((double)s);
        TS_NEXT();
    }
    if (x.isDouble()) { r[a] = Value::fromDouble(x.asDouble() + imm); TS_NEXT(); }
    r[a] = Value::fromDouble(rt->toDoubleForArith(rt->toPrimitiveValue(x, "number")) + imm);
    TS_NEXT();
}
L_OP_SubImm: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF;
    int32_t imm = (int8_t)((w >> 24) & 0xFF);
    Value x = r[b];
    if (x.isSmi()) {
        int64_t s = (int64_t)x.asSmi() - imm;
        r[a] = (s == (int32_t)s) ? Value::fromSmi((int32_t)s) : Value::fromDouble((double)s);
        TS_NEXT();
    }
    if (x.isDouble()) { r[a] = Value::fromDouble(x.asDouble() - imm); TS_NEXT(); }
    r[a] = Value::fromDouble(rt->toDoubleForArith(rt->toPrimitiveValue(x, "number")) - imm);
    TS_NEXT();
}
L_OP_MulSmi: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF;
    int32_t imm = (int8_t)((w >> 24) & 0xFF);
    Value x = r[b];
    if (x.isSmi()) {
        int64_t s = (int64_t)x.asSmi() * imm;
        r[a] = (s == (int32_t)s) ? Value::fromSmi((int32_t)s) : Value::fromDouble((double)s);
        TS_NEXT();
    }
    if (x.isDouble()) { r[a] = Value::fromDouble(x.asDouble() * imm); TS_NEXT(); }
    r[a] = Value::fromDouble(rt->toDoubleForArith(rt->toPrimitiveValue(x, "number")) * imm);
    TS_NEXT();
}
L_OP_LtSmi: {
    uint32_t w = *ip;
    int32_t imm = (int8_t)((w >> 24) & 0xFF);
    Value x = r[(w >> 16) & 0xFF];
    if (x.isSmi()) { r[(w >> 8) & 0xFF] = Value::boolean(x.asSmi() < imm); TS_NEXT(); }
    Value res = lessThanValue(rt, x, Value::fromSmi(imm), false);
    r[(w >> 8) & 0xFF] = res.isUndef() ? Value::false_() : res;
    TS_NEXT();
}
L_OP_LeSmi: {
    uint32_t w = *ip;
    int32_t imm = (int8_t)((w >> 24) & 0xFF);
    Value x = r[(w >> 16) & 0xFF];
    if (x.isSmi()) { r[(w >> 8) & 0xFF] = Value::boolean(x.asSmi() <= imm); TS_NEXT(); }
    Value res = lessThanValue(rt, x, Value::fromSmi(imm), true);
    r[(w >> 8) & 0xFF] = res.isUndef() ? Value::false_() : res;
    TS_NEXT();
}
L_OP_GtSmi: {
    uint32_t w = *ip;
    int32_t imm = (int8_t)((w >> 24) & 0xFF);
    Value x = r[(w >> 16) & 0xFF];
    if (x.isSmi()) { r[(w >> 8) & 0xFF] = Value::boolean(x.asSmi() > imm); TS_NEXT(); }
    Value res = lessThanValue(rt, Value::fromSmi(imm), x, false);
    r[(w >> 8) & 0xFF] = res.isUndef() ? Value::false_() : res;
    TS_NEXT();
}
L_OP_GeSmi: {
    uint32_t w = *ip;
    int32_t imm = (int8_t)((w >> 24) & 0xFF);
    Value x = r[(w >> 16) & 0xFF];
    if (x.isSmi()) { r[(w >> 8) & 0xFF] = Value::boolean(x.asSmi() >= imm); TS_NEXT(); }
    Value res = lessThanValue(rt, Value::fromSmi(imm), x, true);
    r[(w >> 8) & 0xFF] = res.isUndef() ? Value::false_() : res;
    TS_NEXT();
}

// ---- bitwise (smi == int32 in our representation) ----
// bitwise: smi == int32 in our representation, so smi operands skip ToInt32
// entirely (the smi&&smi path is the hot path for masking/flag work).
L_OP_BitAnd: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF], y = r[(w >> 24) & 0xFF];
    if (x.isSmi() && y.isSmi()) {
        r[(w >> 8) & 0xFF] = Value::fromSmi(x.asSmi() & y.asSmi());
        TS_NEXT();
    }
    r[(w >> 8) & 0xFF] = Value::fromSmi(toInt32(rt->toDoubleForArith(x)) & toInt32(rt->toDoubleForArith(y)));
    TS_NEXT();
}
L_OP_BitOr: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF], y = r[(w >> 24) & 0xFF];
    if (x.isSmi() && y.isSmi()) {
        r[(w >> 8) & 0xFF] = Value::fromSmi(x.asSmi() | y.asSmi());
        TS_NEXT();
    }
    r[(w >> 8) & 0xFF] = Value::fromSmi(toInt32(rt->toDoubleForArith(x)) | toInt32(rt->toDoubleForArith(y)));
    TS_NEXT();
}
L_OP_BitXor: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF], y = r[(w >> 24) & 0xFF];
    if (x.isSmi() && y.isSmi()) {
        r[(w >> 8) & 0xFF] = Value::fromSmi(x.asSmi() ^ y.asSmi());
        TS_NEXT();
    }
    r[(w >> 8) & 0xFF] = Value::fromSmi(toInt32(rt->toDoubleForArith(x)) ^ toInt32(rt->toDoubleForArith(y)));
    TS_NEXT();
}
L_OP_Shl: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF], y = r[(w >> 24) & 0xFF];
    if (x.isSmi() && y.isSmi()) {
        r[(w >> 8) & 0xFF] = Value::fromSmi(x.asSmi() << (y.asSmi() & 31));
        TS_NEXT();
    }
    r[(w >> 8) & 0xFF] = Value::fromSmi(toInt32(rt->toDoubleForArith(x)) << (toInt32(rt->toDoubleForArith(y)) & 31));
    TS_NEXT();
}
L_OP_Shr: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF], y = r[(w >> 24) & 0xFF];
    if (x.isSmi() && y.isSmi()) {
        r[(w >> 8) & 0xFF] = Value::fromSmi(x.asSmi() >> (y.asSmi() & 31));
        TS_NEXT();
    }
    r[(w >> 8) & 0xFF] = Value::fromSmi(toInt32(rt->toDoubleForArith(x)) >> (toInt32(rt->toDoubleForArith(y)) & 31));
    TS_NEXT();
}
L_OP_UShr: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF], y = r[(w >> 24) & 0xFF];
    if (x.isSmi() && y.isSmi()) {
        uint32_t u = (uint32_t)x.asSmi() >> (y.asSmi() & 31);
        r[(w >> 8) & 0xFF] = (u <= 0x7FFFFFFF) ? Value::fromSmi((int32_t)u) : Value::fromDouble((double)u);
        TS_NEXT();
    }
    uint32_t u = (uint32_t)toInt32(rt->toDoubleForArith(x)) >> (toInt32(rt->toDoubleForArith(y)) & 31);
    r[(w >> 8) & 0xFF] = (u <= 0x7FFFFFFF) ? Value::fromSmi((int32_t)u) : Value::fromDouble((double)u);
    TS_NEXT();
}

// ---- unary ----
L_OP_Neg: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF];
    if (x.isNumber()) r[(w >> 8) & 0xFF] = Value::fromDouble(-x.asNumber());
    else r[(w >> 8) & 0xFF] = Value::fromDouble(-rt->toDoubleForArith(rt->toPrimitiveValue(x, "number")));
    TS_NEXT();
}
L_OP_ToNum: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF];
    if (x.isNumber()) r[(w >> 8) & 0xFF] = x;
    else r[(w >> 8) & 0xFF] = Value::fromDouble(rt->toDoubleForArith(rt->toPrimitiveValue(x, "number")));
    TS_NEXT();
}
L_OP_BitNot: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF];
    if (x.isSmi()) { r[(w >> 8) & 0xFF] = Value::fromSmi(~x.asSmi()); TS_NEXT(); }
    r[(w >> 8) & 0xFF] = Value::fromSmi(~toInt32(rt->toDoubleForArith(x)));
    TS_NEXT();
}
L_OP_Not: {
    uint32_t w = *ip;
    r[(w >> 8) & 0xFF] = Value::boolean(!r[(w >> 16) & 0xFF].toBoolean());
    TS_NEXT();
}
L_OP_TypeOf: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF];
    const char* s;
    if (x.isUndef()) s = "undefined";
    else if (x.isNull()) s = "object";
    else if (x.isBool()) s = "boolean";
    else if (x.isNumber()) s = "number";
    else if (x.isString()) s = "string";
    else if (x.isFunction()) s = "function";
    else s = "object";
    r[(w >> 8) & 0xFF] = Value::fromObj((HeapObj*)rt->intern(s));
    TS_NEXT();
}
L_OP_Inc: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF;
    Value x = r[a];
    if (x.isSmi()) {
        int32_t v = x.asSmi();
        r[a] = (v == 2147483647) ? Value::fromDouble(2147483648.0) : Value::fromSmi(v + 1);
        TS_NEXT();
    }
    if (x.isDouble()) { r[a] = Value::fromDouble(x.asDouble() + 1.0); TS_NEXT(); }
    r[a] = Value::fromDouble(rt->toDoubleForArith(rt->toNumericValue(x)) + 1.0);
    TS_NEXT();
}
L_OP_Dec: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF;
    Value x = r[a];
    if (x.isSmi()) {
        int32_t v = x.asSmi();
        r[a] = (v == -2147483647 - 1) ? Value::fromDouble(-2147483648.0) : Value::fromSmi(v - 1);
        TS_NEXT();
    }
    if (x.isDouble()) { r[a] = Value::fromDouble(x.asDouble() - 1.0); TS_NEXT(); }
    r[a] = Value::fromDouble(rt->toDoubleForArith(rt->toNumericValue(x)) - 1.0);
    TS_NEXT();
}

// ---- comparison ----
L_OP_Lt: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF], y = r[(w >> 24) & 0xFF];
    if (x.isSmi() && y.isSmi()) { r[(w >> 8) & 0xFF] = Value::boolean(x.asSmi() < y.asSmi()); TS_NEXT(); }
    Value res = lessThanValue(rt, x, y, false);
    r[(w >> 8) & 0xFF] = res.isUndef() ? Value::false_() : res;
    TS_NEXT();
}
L_OP_Le: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF], y = r[(w >> 24) & 0xFF];
    if (x.isSmi() && y.isSmi()) { r[(w >> 8) & 0xFF] = Value::boolean(x.asSmi() <= y.asSmi()); TS_NEXT(); }
    Value res = lessThanValue(rt, x, y, true);
    r[(w >> 8) & 0xFF] = res.isUndef() ? Value::false_() : res;
    TS_NEXT();
}
L_OP_Gt: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF], y = r[(w >> 24) & 0xFF];
    if (x.isSmi() && y.isSmi()) { r[(w >> 8) & 0xFF] = Value::boolean(x.asSmi() > y.asSmi()); TS_NEXT(); }
    Value res = lessThanValue(rt, y, x, false); // x > y  <=>  y < x
    r[(w >> 8) & 0xFF] = res.isUndef() ? Value::false_() : res;
    TS_NEXT();
}
L_OP_Ge: {
    uint32_t w = *ip;
    Value x = r[(w >> 16) & 0xFF], y = r[(w >> 24) & 0xFF];
    if (x.isSmi() && y.isSmi()) { r[(w >> 8) & 0xFF] = Value::boolean(x.asSmi() >= y.asSmi()); TS_NEXT(); }
    Value res = lessThanValue(rt, y, x, true); // x >= y <=> y <= x
    r[(w >> 8) & 0xFF] = res.isUndef() ? Value::false_() : res;
    TS_NEXT();
}
L_OP_Eq: {
    uint32_t w = *ip;
    r[(w >> 8) & 0xFF] = Value::boolean(abstractEquals(rt, r[(w >> 16) & 0xFF], r[(w >> 24) & 0xFF]));
    TS_NEXT();
}
L_OP_Ne: {
    uint32_t w = *ip;
    r[(w >> 8) & 0xFF] = Value::boolean(!abstractEquals(rt, r[(w >> 16) & 0xFF], r[(w >> 24) & 0xFF]));
    TS_NEXT();
}
L_OP_StrictEq: {
    uint32_t w = *ip;
    r[(w >> 8) & 0xFF] = Value::boolean(strictEquals(r[(w >> 16) & 0xFF], r[(w >> 24) & 0xFF]));
    TS_NEXT();
}
L_OP_StrictNe: {
    uint32_t w = *ip;
    r[(w >> 8) & 0xFF] = Value::boolean(!strictEquals(r[(w >> 16) & 0xFF], r[(w >> 24) & 0xFF]));
    TS_NEXT();
}

// ---- control flow ----
L_OP_Jmp: {
    uint32_t w = *ip;
    int32_t off = TS_SEXT24((w >> 8) & 0xFFFFFF);
    ip += 1 + off;
    if (off < 0 && rt->heap.gcRequested()) rt->runGC();  // loop backedge safepoint (Rule 88-style)
    TS_DISPATCH();
}
L_OP_JmpIfFalse: {
    uint32_t w = *ip;
    int32_t off = TS_SEXT16(w >> 16);
    if (!r[(w >> 8) & 0xFF].toBoolean()) ip += 1 + off;
    else ip++;
    TS_DISPATCH();
}
L_OP_JmpIfTrue: {
    uint32_t w = *ip;
    int32_t off = TS_SEXT16(w >> 16);
    if (r[(w >> 8) & 0xFF].toBoolean()) ip += 1 + off;
    else ip++;
    TS_DISPATCH();
}
L_OP_JmpIfNotNU: {
    uint32_t w = *ip;
    int32_t off = TS_SEXT16(w >> 16);
    Value v = r[(w >> 8) & 0xFF];
    if (!(v.isNull() || v.isUndef())) ip += 1 + off;
    else ip++;
    TS_DISPATCH();
}

// ---- property access with inline caches ----
L_OP_GetNamed: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF;
    uint32_t icIdx = ip[1] & 0xFFFF;
    Value recv = r[b];
    f->lastRecv = recv; // receiver tracking for method calls (Call op)
    IC* ic = &bf->ics[icIdx];
    if (recv.isObject()) {
        Object* o = (Object*)recv.asObj();
        if (ic->state == IC_MONO && o->shape == (Shape*)ic->recvShape) {
            if (ic->depth == 0) { r[a] = o->slots[ic->slot]; TS_NEXT2(); }
            Object* h = o->proto;
            for (uint8_t d = 1; d < ic->depth && h; d++) h = h->proto;
            if (h && (Shape*)h->shape == (Shape*)ic->holderShape) {
                r[a] = h->slots[ic->slot];
                TS_NEXT2();
            }
            r[a] = getNamedSlow(o, ic->key, ic); // proto chain changed: re-resolve
            TS_NEXT2();
        }
        r[a] = getNamedSlow(o, ic->key, ic);
        TS_NEXT2();
    }
    if (recv.isUndef() || recv.isNull()) {
        rt->throwTypeError("Cannot read properties of null/undefined");
    }
    r[a] = Value::undef();
    getProperty(rt, recv, ic->key, &r[a]);
    TS_NEXT2();
}
L_OP_SetNamed: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF;
    uint32_t icIdx = ip[1] & 0xFFFF;
    Value recv = r[a];
    IC* ic = &bf->ics[icIdx];
    if (recv.isObject()) {
        Object* o = (Object*)recv.asObj();
        if (ic->state == IC_MONO && !o->dictMode && o->shape == (Shape*)ic->recvShape) {
            o->slots[ic->slot] = r[b];
            TS_NEXT2();
        }
        setNamedSlow(o, ic->key, r[b], ic);
        TS_NEXT2();
    }
    setProperty(rt, recv, ic->key, r[b]); // primitives: silently ignored (sloppy)
    TS_NEXT2();
}
L_OP_GetGlobal: {
    uint32_t w = *ip;
    uint32_t icIdx = ip[1] & 0xFFFF;
    f->lastRecv = Value::undef(); // plain global: not a method receiver
    IC* ic = &bf->ics[icIdx];
    Object* g = rt->globalObj;
    if (ic->state == IC_MONO && g->shape == (Shape*)ic->recvShape) {
        r[(w >> 8) & 0xFF] = g->slots[ic->slot];
        TS_NEXT2();
    }
    bool found = false;
    uint32_t slot = 0;
    if (!g->dictMode) slot = g->ownSlotIndex(ic->key, &found);
    else {
        if (Value* v = g->dict->find(ic->key)) { r[(w >> 8) & 0xFF] = *v; TS_NEXT2(); }
    }
    if (found) {
        ic->state = IC_MONO;
        ic->recvShape = g->shape;
        ic->slot = slot;
        ic->holder = nullptr;
        ic->depth = 0;
        r[(w >> 8) & 0xFF] = g->slots[slot];
    } else {
        rt->throwReferenceError("identifier not defined");
    }
    TS_NEXT2();
}
L_OP_SetGlobal: {
    uint32_t w = *ip;
    uint32_t icIdx = ip[1] & 0xFFFF;
    IC* ic = &bf->ics[icIdx];
    Object* g = rt->globalObj;
    if (ic->state == IC_MONO && !g->dictMode && g->shape == (Shape*)ic->recvShape) {
        g->slots[ic->slot] = r[(w >> 8) & 0xFF];
        TS_NEXT2();
    }
    setProperty(rt, Value::fromObj((HeapObj*)g), ic->key, r[(w >> 8) & 0xFF]);
    bool found = false;
    uint32_t slot = g->ownSlotIndex(ic->key, &found);
    if (found && !g->dictMode) {
        ic->state = IC_MONO;
        ic->recvShape = g->shape;
        ic->slot = slot;
        ic->holder = nullptr;
        ic->depth = 0;
    } else ic->state = IC_MEGA;
    TS_NEXT2();
}
L_OP_GetKeyed: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF, c = (w >> 24) & 0xFF;
    Value recv = r[b], key = r[c];
    f->lastRecv = recv; // receiver tracking
    if (recv.isObject()) {
        Object* o = (Object*)recv.asObj();
        if (key.isSmi()) {
            int32_t ki = key.asSmi();
            if (ki >= 0 && (uint32_t)ki < o->elemLen) { r[a] = o->elements[ki]; TS_NEXT(); }
        } else if (key.isString()) {
            uint32_t idx;
            if (strToIndex(key.asStringUnchecked(), &idx) && idx < o->elemLen) {
                r[a] = o->elements[idx];
                TS_NEXT();
            }
        }
        r[a] = Value::undef();
        getElement(rt, recv, key, &r[a]);
        TS_NEXT();
    }
    if (recv.isString()) {
        String* s = recv.asStringUnchecked();
        if (key.isSmi()) {
            int32_t ki = key.asSmi();
            if (ki >= 0 && (uint32_t)ki < s->len) {
                char16_t ch = s->data()[ki];
                r[a] = Value::fromObj((HeapObj*)stringFromUTF16(rt, &ch, 1));
                TS_NEXT();
            }
        }
    }
    if (recv.isUndef() || recv.isNull()) {
        rt->throwTypeError("Cannot read properties of null/undefined");
    }
    r[a] = Value::undef();
    getElement(rt, recv, key, &r[a]);
    TS_NEXT();
}
L_OP_SetKeyed: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF, c = (w >> 24) & 0xFF;
    Value recv = r[a], key = r[b], val = r[c];
    if (recv.isObject()) {
        Object* o = (Object*)recv.asObj();
        if (key.isSmi()) {
            int32_t ki = key.asSmi();
            if (ki >= 0 && (uint32_t)ki < o->elemLen) { o->elements[ki] = val; TS_NEXT(); }
            if (ki >= 0 && (uint32_t)ki == o->elemLen && o->elemLen < o->elemCap) {
                o->elements[o->elemLen++] = val;
                TS_NEXT();
            }
        }
        setElement(rt, recv, key, val);
        TS_NEXT();
    }
    setElement(rt, recv, key, val); // primitives ignored
    TS_NEXT();
}
L_OP_Delete: {
    uint32_t w = *ip;
    uint32_t icIdx = ip[1] & 0xFFFF;
    Value recv = r[(w >> 16) & 0xFF];
    if (!recv.isObject()) { r[(w >> 8) & 0xFF] = Value::true_(); TS_NEXT2(); }
    bool ok = deleteProperty(rt, (Object*)recv.asObj(), bf->ics[icIdx].key);
    r[(w >> 8) & 0xFF] = Value::boolean(ok);
    TS_NEXT2();
}
L_OP_DeleteKeyed: {
    uint32_t w = *ip;
    Value recv = r[(w >> 16) & 0xFF], key = r[(w >> 24) & 0xFF];
    if (!recv.isObject()) { r[(w >> 8) & 0xFF] = Value::true_(); TS_NEXT(); }
    Object* o = (Object*)recv.asObj();
    if (key.isString()) {
        bool ok = deleteProperty(rt, o, key.asStringUnchecked());
        r[(w >> 8) & 0xFF] = Value::boolean(ok);
    } else {
        r[(w >> 8) & 0xFF] = Value::true_(); // numeric-key delete: registered gap
    }
    TS_NEXT();
}

// ---- lexical scope ----
L_OP_GetCtx: {
    uint32_t w = *ip;
    Context* c = f->rootCtx;
    for (uint8_t d = 0; d < ((w >> 16) & 0xFF); d++) c = c->parent;
    r[(w >> 8) & 0xFF] = c->cells[(w >> 24) & 0xFF];
    TS_NEXT();
}
L_OP_SetCtx: {
    uint32_t w = *ip;
    Context* c = f->rootCtx;
    for (uint8_t d = 0; d < ((w >> 8) & 0xFF); d++) c = c->parent;
    c->cells[(w >> 16) & 0xFF] = r[(w >> 24) & 0xFF];
    TS_NEXT();
}
L_OP_NewCtx: {
    f->curCtx = Context::create(rt, f->curCtx, (*ip >> 8) & 0xFF);
    if (f->ctxDepth == 0) f->rootCtx = f->curCtx; // first ctx = frame root
    f->ctxDepth++;
    TS_NEXT();
}
L_OP_ExitCtx: {
    f->curCtx = f->curCtx->parent;
    f->ctxDepth--;
    TS_NEXT();
}
L_OP_LdTDZ: { r[(*ip >> 8) & 0xFF] = Value::tdz(); TS_NEXT(); }
L_OP_CheckTDZ: {
    uint8_t a = (*ip >> 8) & 0xFF;
    if (r[a].isTDZ()) rt->throwReferenceError("Cannot access before initialization");
    TS_NEXT();
}

// ---- calls ----
#define TS_DO_CALL(withRecv) { \
    uint32_t w = *ip; \
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF, c = (w >> 24) & 0xFF; \
    Value cal = r[a]; \
    if (!cal.isFunction()) rt->throwTypeError("value is not a function"); \
    JSFunction* callee = (JSFunction*)cal.asObj(); \
    if (rt->heap.gcRequested()) rt->runGC(); /* call safepoint */ \
    if (callee->fn.native) { \
        Value tv = (withRecv) ? f->lastRecv : Value::undef(); \
        r[a] = callee->fn.native(rt, tv, &r[b], c); \
        TS_NEXT(); \
    } \
    BytecodeFunction* cbf = callee->fn.bf; \
    uint32_t newBase = f->baseOff + bf->nRegs; \
    if (newBase + cbf->nRegs > stackCap) { \
        stackWords = newBase + cbf->nRegs; \
        growStack(); \
        r = stack + f->baseOff; \
    } \
    stackWords = newBase + cbf->nRegs; \
    frames.back().ip = ip + 1; \
    uint32_t np = c < cbf->nParams ? c : cbf->nParams; \
    Value* nb = stack + newBase; \
    for (uint32_t i = 0; i < np; i++) nb[i] = r[b + i]; \
    for (uint32_t i = np; i < cbf->nParams; i++) nb[i] = Value::undef(); \
    Frame nf; \
    nf.bf = cbf; nf.ip = cbf->code; nf.baseOff = newBase; \
    nf.rootCtx = callee->fn.ctx; nf.curCtx = callee->fn.ctx; \
    nf.fn = callee; \
    nf.thisVal = (withRecv) ? f->lastRecv : Value::undef(); \
    nf.lastRecv = Value::undef(); nf.ctxDepth = 0; \
    frames.push_back(nf); \
    f = &frames.back(); bf = cbf; ip = cbf->code; r = stack + newBase; \
    TS_DISPATCH(); \
}
L_OP_Call:  TS_DO_CALL(true)
L_OP_CallN: TS_DO_CALL(false)
L_OP_New: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF, c = (w >> 24) & 0xFF;
    Value cal = r[a];
    if (!cal.isFunction()) rt->throwTypeError("value is not a constructor");
    JSFunction* callee = (JSFunction*)cal.asObj();
    if (!callee->fn.isCtor) rt->throwTypeError("value is not a constructor");
    if (rt->heap.gcRequested()) rt->runGC();
    if (!callee->fn.hasCtorProto) {
        callee->fn.ctorProto = Object::create(rt, rt->objectProto, false);
        callee->fn.hasCtorProto = true;
    }
    Object* newObj = Object::create(rt, callee->fn.ctorProto, false);
    Value res;
    if (callee->fn.native) {
        res = callee->fn.native(rt, Value::fromObj((HeapObj*)newObj), &r[b], c);
    } else {
        BytecodeFunction* cbf = callee->fn.bf;
        uint32_t newBase = f->baseOff + bf->nRegs;
        if (newBase + cbf->nRegs > stackCap) {
            stackWords = newBase + cbf->nRegs;
            growStack();
            r = stack + f->baseOff;
        }
        stackWords = newBase + cbf->nRegs;
        frames.back().ip = ip + 1;
        uint32_t np = c < cbf->nParams ? c : cbf->nParams;
        Value* nb = stack + newBase;
        for (uint32_t i = 0; i < np; i++) nb[i] = r[b + i];
        for (uint32_t i = np; i < cbf->nParams; i++) nb[i] = Value::undef();
        Frame& nf = frames.emplace_back();
        nf.bf = cbf; nf.ip = cbf->code; nf.baseOff = newBase;
        nf.rootCtx = callee->fn.ctx; nf.curCtx = callee->fn.ctx;
        nf.fn = callee;
        nf.thisVal = Value::fromObj((HeapObj*)newObj);
        nf.ctorThis = Value::fromObj((HeapObj*)newObj);
        nf.constructing = true;
        nf.lastRecv = Value::undef(); nf.ctxDepth = 0;
        f = &nf; bf = cbf; ip = cbf->code; r = stack + newBase;
        TS_DISPATCH();
    }
    // native constructor path
    r[a] = res.isObject() ? res : Value::fromObj((HeapObj*)newObj);
    TS_NEXT();
}
L_OP_Return: {
    uint32_t w = *ip;
    Value ret = r[(w >> 8) & 0xFF];
    // [[Construct]] semantics: primitive/undefined return => the created `this`
    bool wasCtor = frames.back().constructing;
    Value ctorThis = frames.back().ctorThis;
    if (wasCtor && !ret.isObject()) ret = ctorThis;
    while (!trys.empty() && trys.back().frameIndex == frames.size() - 1) trys.pop_back();
    if (frames.size() <= entryDepth) return ret;
    size_t calleeEnd = stackWords;
    frames.pop_back();
    f = &frames.back();
    bf = f->bf;
    r = stack + f->baseOff;
    stackWords = f->baseOff + bf->nRegs; // restore stack high-water to caller end
    (void)calleeEnd;
    uint8_t dst = (f->ip[-1] >> 8) & 0xFF; // call instruction's dst
    r[dst] = ret;
    ip = f->ip;
    TS_DISPATCH();
}
L_OP_Closure: {
    uint32_t w = *ip;
    uint32_t fidx = ip[1] & 0xFFFF;
    BytecodeFunction* cbf = bf->funcs[fidx];
    JSFunction* fnObj = JSFunction::createInterpreted(rt, cbf, f->curCtx);
    r[(w >> 8) & 0xFF] = Value::fromObj((HeapObj*)fnObj);
    TS_NEXT2();
}

// ---- literals ----
L_OP_NewArray: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF, c = (w >> 24) & 0xFF;
    Object* arr = Object::create(rt, rt->arrayProto, true);
    if (b) {
        uint32_t cap = b < 4 ? 4 : b;
        arr->elements = (Value*)std::malloc(cap * sizeof(Value));
        arr->elemCap = cap;
        arr->elemLen = b;
        for (uint32_t i = 0; i < b; i++) arr->elements[i] = r[c + i];
    }
    r[a] = Value::fromObj((HeapObj*)arr);
    TS_NEXT();
}
L_OP_NewObject: {
    uint8_t a = (*ip >> 8) & 0xFF;
    Object* o = Object::create(rt, rt->objectProto, false);
    r[a] = Value::fromObj((HeapObj*)o);
    TS_NEXT();
}

// ---- exceptions ----
L_OP_TryEnter: {
    uint32_t hIdx = ip[1] & 0xFFFF;
    TryFrame tf;
    tf.frame = *f;
    tf.ip = ip + 2;
    tf.handlerIdx = hIdx;
    tf.frameIndex = frames.size() - 1;
    tf.tryStackSize = trys.size();
    tf.stackWordsSnap = f->baseOff + (size_t)bf->nRegs;
    trys.push_back(tf);
    volatile int v = setjmp(trys.back().buf);
    if (v == 0) { ip += 2; TS_DISPATCH(); }
    // ---- exception landing: state restored from the snapshot ----
    TryFrame& tf2 = trys.back();
    size_t fidx2 = tf2.frameIndex;
    size_t keep = tf2.tryStackSize;
    Frame snap = tf2.frame;
    uint32_t hIdx2 = tf2.handlerIdx;
    trys.resize(keep);
    frames.resize(fidx2 + 1);
    f = &frames.back();
    *f = snap;
    bf = f->bf;
    r = stack + f->baseOff;
    // Restore the stack high-water to the snapshot so the dead region of
    // unwound callee frames is not scanned (or kept alive) by the GC.
    for (size_t i = tf2.stackWordsSnap; i < stackWords && i < stackCap; i++)
        stack[i] = Value::fromBits(0);
    stackWords = tf2.stackWordsSnap;
    Handler* h = &bf->handlers[hIdx2];
    while (f->ctxDepth > h->ctxDepth) { f->curCtx = f->curCtx->parent; f->ctxDepth--; }
    Value exc = Value::fromBits((uint64_t)rt->pendingException.bits);
    if (h->catchReg != 0xFF) r[h->catchReg] = exc;
    ip = bf->code + h->target;
    TS_DISPATCH();
}
L_OP_TryExit: {
    trys.pop_back();
    TS_NEXT();
}
L_OP_Throw: {
    throwValue(r[(*ip >> 8) & 0xFF]);
}

// ---- misc ----
L_OP_InstanceOf: {
    uint32_t w = *ip;
    Value lhs = r[(w >> 16) & 0xFF], rhs = r[(w >> 24) & 0xFF];
    bool res = false;
    if (rhs.isFunction() && lhs.isObject()) {
        JSFunction* rf = (JSFunction*)rhs.asObj();
        if (!rf->fn.hasCtorProto) {
            rf->fn.ctorProto = Object::create(rt, rt->objectProto, false);
            rf->fn.hasCtorProto = true;
        }
        Object* p = lhs.asObj()->proto;
        while (p) {
            if (p == rf->fn.ctorProto) { res = true; break; }
            p = p->proto;
        }
    }
    r[(w >> 8) & 0xFF] = Value::boolean(res);
    TS_NEXT();
}
L_OP_InOp: {
    uint32_t w = *ip;
    Value key = r[(w >> 16) & 0xFF], obj = r[(w >> 24) & 0xFF];
    bool res = false;
    if (obj.isObject()) {
        Object* o = (Object*)obj.asObj();
        if (key.isString()) res = hasProperty(rt, obj, key.asStringUnchecked());
        else if (key.isSmi()) {
            int32_t ki = key.asSmi();
            res = ki >= 0 && (uint32_t)ki < o->elemLen;
            if (!res) res = hasProperty(rt, obj, rt->toStringObj(key));
        }
    }
    r[(w >> 8) & 0xFF] = Value::boolean(res);
    TS_NEXT();
}
L_OP_ForInKeys: {
    uint32_t w = *ip;
    uint8_t a = (w >> 8) & 0xFF, b = (w >> 16) & 0xFF;
    Value src = r[b];
    Object* out = Object::create(rt, rt->arrayProto, true);
    std::vector<Value> keys;
    if (src.isObject()) {
        Object* o = (Object*)src.asObj();
        // element indices first, ascending, canonical strings
        char buf[16];
        for (uint32_t i = 0; i < o->elemLen; i++) {
            std::snprintf(buf, sizeof buf, "%u", i);
            keys.push_back(Value::fromObj((HeapObj*)rt->intern(buf)));
        }
        // own named props in insertion order (shape chain reversed)
        if (!o->dictMode) {
            std::vector<String*> names;
            for (Shape* s = o->shape; s && s->key; s = s->parent) names.push_back(s->key);
            for (size_t i = names.size(); i-- > 0;) keys.push_back(Value::fromObj((HeapObj*)names[i]));
        } else {
            o->dict->forEach([&](String* k, Value) {
                keys.push_back(Value::fromObj((HeapObj*)k));
            });
        }
    } else if (src.isString()) {
        String* s = src.asStringUnchecked();
        char buf[16];
        for (uint32_t i = 0; i < s->len; i++) {
            std::snprintf(buf, sizeof buf, "%u", i);
            keys.push_back(Value::fromObj((HeapObj*)rt->intern(buf)));
        }
    }
    if (!keys.empty()) {
        out->elements = (Value*)std::malloc(keys.size() * sizeof(Value));
        out->elemCap = (uint32_t)keys.size();
        out->elemLen = (uint32_t)keys.size();
        for (size_t i = 0; i < keys.size(); i++) out->elements[i] = keys[i];
    }
    r[a] = Value::fromObj((HeapObj*)out);
    TS_NEXT();
}
L_OP_NOP: TS_NEXT();

    return Value::undef(); // unreachable
}

// ---- IC slow paths ----
Value Interp::getNamedSlow(Object* o, String* key, IC* ic) {
    Value recv = Value::fromObj((HeapObj*)o);
    // specials (not shape-cacheable)
    if (o->kind == (uint8_t)ObjKind::Function && key == rt->intern("prototype")) {
        JSFunction* f = (JSFunction*)o;
        if (!f->fn.hasCtorProto) {
            f->fn.ctorProto = Object::create(rt, rt->objectProto, false);
            f->fn.hasCtorProto = true;
        }
        ic->state = IC_MEGA;
        return Value::fromObj((HeapObj*)f->fn.ctorProto);
    }
    if (o->isArray && key == rt->intern("length")) {
        ic->state = IC_MEGA;
        return Value::fromSmi((int32_t)o->elemLen);
    }
    // own
    if (!o->dictMode) {
        bool found; uint32_t slot = o->ownSlotIndex(key, &found);
        if (found) {
            ic->state = IC_MONO;
            ic->recvShape = o->shape;
            ic->slot = slot;
            ic->holder = nullptr;
            ic->depth = 0;
            return o->slots[slot];
        }
    } else if (Value* v = o->dict->find(key)) {
        ic->state = IC_MEGA;
        return *v;
    }
    // prototype chain (data props only)
    Object* h = o->proto;
    for (uint8_t d = 1; h && d < 255; d++, h = h->proto) {
        if (!h->dictMode) {
            bool found; uint32_t slot = h->ownSlotIndex(key, &found);
            if (found) {
                ic->state = IC_MONO;
                ic->recvShape = o->shape;
                ic->holder = h;
                ic->holderShape = h->shape;
                ic->slot = slot;
                ic->depth = d;
                return h->slots[slot];
            }
        } else if (Value* v = h->dict->find(key)) {
            ic->state = IC_MEGA;
            return *v;
        }
        if (h->kind == (uint8_t)ObjKind::Function && key == rt->intern("prototype")) {
            ic->state = IC_MEGA;
            JSFunction* hf = (JSFunction*)h;
            if (!hf->fn.hasCtorProto) {
                hf->fn.ctorProto = Object::create(rt, rt->objectProto, false);
                hf->fn.hasCtorProto = true;
            }
            return Value::fromObj((HeapObj*)hf->fn.ctorProto);
        }
        if (h->isArray && key == rt->intern("length")) {
            ic->state = IC_MEGA;
            return Value::fromSmi((int32_t)h->elemLen);
        }
    }
    ic->state = IC_MEGA; // negative lookup stays generic
    return Value::undef();
}

void Interp::setNamedSlow(Object* o, String* key, Value v, IC* ic) {
    if (o->isArray && key == rt->intern("length")) {
        ic->state = IC_MEGA;
        setProperty(rt, Value::fromObj((HeapObj*)o), key, v);
        return;
    }
    // own data prop?
    if (!o->dictMode) {
        bool found; uint32_t slot = o->ownSlotIndex(key, &found);
        if (found) {
            o->slots[slot] = v;
            ic->state = IC_MONO;
            ic->recvShape = o->shape;
            ic->slot = slot;
            ic->holder = nullptr;
            ic->depth = 0;
            return;
        }
    } else {
        o->dict->set(key, v);
        ic->state = IC_MEGA;
        return;
    }
    // add / shadow: generic setProperty performs the shape transition
    setProperty(rt, Value::fromObj((HeapObj*)o), key, v);
    if (!o->dictMode) {
        bool found; uint32_t slot = o->ownSlotIndex(key, &found);
        if (found) {
            ic->state = IC_MONO;
            ic->recvShape = o->shape; // post-transition shape
            ic->slot = slot;
            ic->holder = nullptr;
            ic->depth = 0;
            return;
        }
    }
    ic->state = IC_MEGA;
}

// canonical numeric string -> uint32 index ("0", "42"; no leading zeros)
bool strToIndex(String* s, uint32_t* out) {
    uint32_t n = s->len;
    if (n == 0 || n > 10) return false;
    const char16_t* p = s->data();
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = (uint32_t)(p[i] - '0');
        if (d > 9) return false;
        v = v * 10 + d;
    }
    if (n > 1 && p[0] == '0') return false; // leading zero: not canonical
    if (v > 0xFFFFFFFEull) return false;
    *out = (uint32_t)v;
    return true;
}

} // namespace ts