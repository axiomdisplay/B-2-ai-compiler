// TurboScript Tier 0 — compiler.cpp
// Pass 1 (capture analysis), resolution, codegen framework, expression codegen.
#include "compiler_impl.h"
#include <cstdio>
#include <cstdlib>
#include <cstdlib>

namespace ts {

void CompilerImpl::fail(const char* msg, Node* n) {
    std::fprintf(stderr, "CompileError: %s (line %u)\n", msg, n ? n->line : 0);
    std::abort();
}

// ================================================================ pass 1
void CompilerImpl::addVar(Scope* s, const std::u16string& name, bool letConst) {
    if (!s->find(name)) {
        VarInfo v; v.isLetConst = letConst;
        s->vars[name] = v;
    }
}

void CompilerImpl::analyzeProgram(Node* program) {
    Scope* prog = new Scope();
    prog->isFnScope = true;
    prog->isProgram = true;
    scopeArena.push_back(prog);
    nodeScope[program] = prog;
    scopes.push_back(prog);
    analyze(program);
    scopes.pop_back();
}

void CompilerImpl::analyzeFn(Node* f, bool bindNameEnclosing) {
    bool isClass = (f->num == 1.0);
    Scope* own = new Scope();
    own->isFnScope = true;
    scopeArena.push_back(own);
    nodeScope[f] = own;
    if (f->params) for (auto& p : *f->params) addVar(own, p, false);
    if (f->fname) {
        if (bindNameEnclosing && !scopes.empty()) {
            Scope* enc = scopes.back();
            addVar(enc, *f->fname, false);
        }
        addVar(own, *f->fname, false); // self-name (function expr / class)
    }
    scopes.push_back(own);
    if (isClass) {
        for (Node* m : *f->kids) {
            if (*m->fname == u"constructor") { analyze(m->body); break; }
        }
        for (Node* m : *f->kids) {
            if (*m->fname != u"constructor") analyzeFn(m, false);
        }
    } else {
        // body statements bind into the function scope directly
        if (f->body && f->body->kids) for (Node* k : *f->body->kids) analyze(k);
    }
    scopes.pop_back();
}

void CompilerImpl::analyze(Node* n) {
    if (!n) return;
    switch (n->k) {
        case NK::Ident: {
            const std::u16string& name = *n->str;
            int myFn = -1;
            for (int i = (int)scopes.size() - 1; i >= 0; i--)
                if (scopes[i]->isFnScope) { myFn = i; break; }
            for (int i = (int)scopes.size() - 1; i >= 0; i--) {
                if (VarInfo* v = scopes[i]->find(name)) {
                    if (i < myFn) {
                        v->captured = true;
                        scopes[i]->needCtx = true;
                    }
                    return;
                }
            }
            return; // global
        }
        case NK::Var: {
            for (auto& d : *n->decls) {
                Scope* target;
                if (d.isLet || d.isConst) {
                    target = scopes.back();
                } else {
                    target = nearestFnScope();
                }
                bool globalVar = target->isProgram && !(d.isLet || d.isConst);
                if (target && !globalVar) addVar(target, d.name, d.isLet || d.isConst);
                if (d.init) analyze(d.init);
            }
            return;
        }
        case NK::Func: {
            // declaration vs expression: if parent statement wraps it with a name
            // and it appears in statement position. We approximate: bind enclosing
            // when invoked from statement context (emitStmt path also knows this);
            // for analysis, bind enclosing iff fname exists AND node is reached in
            // decl position — we mark decl fns by setting n->op = 1 in emitStmt;
            // here we check a parallel marker set by the parser wrapper.
            bool isDecl = (n->op == 1);
            analyzeFn(n, isDecl && n->fname != nullptr);
            return;
        }
        case NK::Block: {
            Scope* s = new Scope();
            scopeArena.push_back(s);
            nodeScope[n] = s;
            scopes.push_back(s);
            if (n->kids) for (Node* k : *n->kids) analyze(k);
            scopes.pop_back();
            return;
        }
        default: break;
    }
    if (n->a) analyze(n->a);
    if (n->b) analyze(n->b);
    if (n->c) analyze(n->c);
    if (n->kids) for (Node* k : *n->kids) analyze(k);
}

Scope* CompilerImpl::nearestFnScope() {
    for (int i = (int)scopes.size() - 1; i >= 0; i--)
        if (scopes[i]->isFnScope) return scopes[i];
    return nullptr;
}

// ================================================================ resolution
bool CompilerImpl::resolve(const std::u16string& name, Resolved& out) {
    uint32_t myRoot = fns.empty() ? 0 : fns.back()->rootGDepth;
    for (int i = (int)scopes.size() - 1; i >= 0; i--) {
        if (VarInfo* v = scopes[i]->find(name)) {
            if (v->captured) {
                out.kind = RKind::CTX;
                out.letConst = v->isLetConst;
                uint32_t varDepth = scopes[i]->gDepth;
                if (varDepth == 0 || varDepth > myRoot) fail("internal: ctx depth");
                out.hops = (uint16_t)(myRoot - varDepth);
                out.idx = v->ctxIdx;
            } else {
                out.kind = RKind::LOCAL;
                out.letConst = v->isLetConst;
                out.reg = v->reg;
            }
            return true;
        }
    }
    return false; // global
}

// ================================================================ codegen framework
uint16_t CompilerImpl::allocReg(FnUnit& u) {
    if (u.nextReg >= 255) fail("too many registers (256 vreg limit, Laws Part I Tier 0)");
    uint16_t r = u.nextReg++;
    if (u.nextReg > u.peakReg) u.peakReg = u.nextReg;
    return r;
}
void CompilerImpl::freeRegsTo(FnUnit& u, uint16_t n) { u.nextReg = n; }
uint32_t CompilerImpl::here(FnUnit& u) { return (uint32_t)u.code.size(); }
void CompilerImpl::emitW(FnUnit& u, uint32_t w) { u.code.push_back(w); }
void CompilerImpl::emit1(FnUnit& u, uint8_t op, uint8_t a) { emitW(u, op | ((uint32_t)a << 8)); }
void CompilerImpl::emit2(FnUnit& u, uint8_t op, uint8_t a, uint8_t b) {
    emitW(u, op | ((uint32_t)a << 8) | ((uint32_t)b << 16));
}
void CompilerImpl::emit3(FnUnit& u, uint8_t op, uint8_t a, uint8_t b, uint8_t c) {
    emitW(u, op | ((uint32_t)a << 8) | ((uint32_t)b << 16) | ((uint32_t)c << 24));
}
void CompilerImpl::emitExt(FnUnit& u, uint32_t idx) {
    emitW(u, makeExt(idx));
}

void CompilerImpl::emitJmp24(FnUnit& u, int32_t offset) {
    uint32_t off = (uint32_t)offset & 0xFFFFFF;
    emitW(u, (uint32_t)OP_Jmp | ((off & 0xFF) << 8) | (((off >> 8) & 0xFF) << 16) | (((off >> 16) & 0xFF) << 24));
}
uint32_t CompilerImpl::emitJmp24p(FnUnit& u) {
    emit3(u, OP_Jmp, 0, 0, 0);
    return here(u) - 1;
}
uint32_t CompilerImpl::emitCondJmp(FnUnit& u, uint8_t op, uint8_t cond) {
    emit2(u, op, cond, 0);
    return here(u) - 1;
}
void CompilerImpl::patchJmp24(FnUnit& u, uint32_t at, uint32_t target) {
    int32_t off = (int32_t)target - (int32_t)(at + 1);
    uint32_t offb = (uint32_t)off & 0xFFFFFF;
    uint32_t w = u.code[at];
    w = (w & 0xFF) | ((offb & 0xFF) << 8) | (((offb >> 8) & 0xFF) << 16) | (((offb >> 16) & 0xFF) << 24);
    u.code[at] = w;
}
void CompilerImpl::patchCond(FnUnit& u, uint32_t at, uint32_t target) {
    int32_t off = (int32_t)target - (int32_t)(at + 1);
    uint32_t w = u.code[at];
    w = (w & 0xFFFF) | (((uint32_t)off & 0xFFFF) << 16);
    u.code[at] = w;
}
uint32_t CompilerImpl::addIC(FnUnit& u, String* key) {
    IC ic; ic.key = key;
    u.ics.push_back(ic);
    return (uint32_t)u.ics.size() - 1;
}
uint32_t CompilerImpl::addConst(FnUnit& u, Value v) {
    for (uint32_t i = 0; i < u.consts.size(); i++)
        if (u.consts[i].bits == v.bits) return i;
    u.consts.push_back(v);
    return (uint32_t)u.consts.size() - 1;
}
String* CompilerImpl::internU(const std::u16string& s) {
    return rt->internUTF16(s.data(), (uint32_t)s.size());
}

void CompilerImpl::enterScope(FnUnit& u, Scope* s) {
    s->regBase = u.nextReg;
    scopes.push_back(s);
    if (s->needCtx) {
        ctxCounter++;
        s->gDepth = ctxCounter;
        emit1(u, OP_NewCtx, (uint8_t)s->nCtxCells);
        u.ctxDepthNow++;
    }
}
void CompilerImpl::exitScope(FnUnit& u, Scope* s) {
    if (s->needCtx) {
        emit1(u, OP_ExitCtx, 0);
        ctxCounter--;
        u.ctxDepthNow--;
    }
    scopes.pop_back();
    u.nextReg = s->regBase;
}
bool CompilerImpl::anyOpenFinally(FnUnit& u) {
    for (auto& tr : u.openTrys) if (tr.hasFinally) return true;
    return false;
}

CompilerImpl::~CompilerImpl() {
    for (Scope* s : scopeArena) delete s;
}

// ================================================================ var load/store
void CompilerImpl::emitLoadVar(const Resolved& r, uint16_t dst) {
    FnUnit& u = *fns.back();
    switch (r.kind) {
        case RKind::LOCAL:
            if (r.letConst) emit2(u, OP_MovChk, dst, r.reg);
            else if (dst != r.reg) emit2(u, OP_Mov, dst, r.reg);
            break;
        case RKind::CTX:
            emit3(u, OP_GetCtx, dst, (uint8_t)r.hops, (uint8_t)r.idx);
            if (r.letConst) emit1(u, OP_CheckTDZ, dst);
            break;
        case RKind::GLOBAL:
            break; // handled by caller (needs name)
    }
}

void CompilerImpl::emitStoreVar(const Resolved& r, uint16_t src) {
    FnUnit& u = *fns.back();
    switch (r.kind) {
        case RKind::LOCAL:
            if (src != r.reg) emit2(u, OP_Mov, r.reg, src);
            break;
        case RKind::CTX:
            emit3(u, OP_SetCtx, (uint8_t)r.hops, (uint8_t)r.idx, src);
            break;
        case RKind::GLOBAL:
            break;
    }
}

void CompilerImpl::emitIdent(Node* n, uint16_t dst) {
    const std::u16string& name = *n->str;
    FnUnit& u = *fns.back();
    // global constants (Part 0 numeric semantics)
    if (name == u"undefined") { emit1(u, OP_LdUndef, dst); return; }
    if (name == u"NaN") { emit2(u, OP_LdConst, dst, (uint8_t)addConst(u, Value::nan())); return; }
    if (name == u"Infinity") { emit2(u, OP_LdConst, dst, (uint8_t)addConst(u, Value::fromDouble(HUGE_VAL))); return; }
    Resolved r;
    if (resolve(name, r)) {
        emitLoadVar(r, dst);
        return;
    }
    // global property with IC
    String* key = internU(name);
    uint32_t ic = addIC(u, key);
    emit2(u, OP_GetGlobal, dst, 0);
    emitExt(u, ic);
}

// ================================================================ expressions
void CompilerImpl::emitExpr(Node* n, uint16_t dst) {
    FnUnit& u = *fns.back();
    switch (n->k) {
        case NK::Num: {
            double d = n->num;
            if (d == (double)(int32_t)d && d >= -2147483648.0 && d <= 2147483647.0) {
                emit2(u, OP_LdConst, dst, (uint8_t)addConst(u, Value::fromSmi((int32_t)d)));
            } else {
                emit2(u, OP_LdConst, dst, (uint8_t)addConst(u, Value::fromDouble(d)));
            }
            return;
        }
        case NK::Str:
            emit2(u, OP_LdConst, dst, (uint8_t)addConst(u, Value::fromObj(internU(*n->str))));
            return;
        case NK::Tpl: {
            // kids: [piece0, expr0, piece1, expr1, ..., pieceN] (odd count)
            // result = ((piece0 + expr0) + piece1) + expr1) ... + pieceN
            size_t cnt = n->kids->size();
            emit2(u, OP_LdConst, dst, (uint8_t)addConst(u, Value::fromObj(internU(*(*n->kids)[0]->str))));
            uint16_t mark = u.nextReg;
            uint16_t tmp = allocReg(u);
            for (size_t k = 1; k < cnt; k += 2) {
                emitExpr((*n->kids)[k], tmp);           // expr
                emit3(u, OP_Add, dst, dst, tmp);
                if (k + 1 < cnt) {
                    Node* piece = (*n->kids)[k + 1];
                    emit2(u, OP_LdConst, tmp, (uint8_t)addConst(u, Value::fromObj(internU(*piece->str))));
                    emit3(u, OP_Add, dst, dst, tmp);
                }
            }
            freeRegsTo(u, mark);
            return;
        }
        case NK::Ident:
            emitIdent(n, dst);
            return;
        case NK::Null: emit1(u, OP_LdNull, dst); return;
        case NK::Undef: emit1(u, OP_LdUndef, dst); return;
        case NK::True: emit1(u, OP_LdTrue, dst); return;
        case NK::False: emit1(u, OP_LdFalse, dst); return;
        case NK::This:
            if (fns.back()->hasThisReg && dst != fns.back()->thisReg)
                emit2(u, OP_Mov, dst, fns.back()->thisReg);
            else if (fns.back()->hasThisReg) return; // already in dst
            else emit1(u, OP_LdThis, dst);
            return;
        case NK::Func:
            emitFuncNode(n, dst);
            return;
        default: break;
    }
    emitBinaryOp(n, dst);
}

void CompilerImpl::emitBinaryOp(Node* n, uint16_t dst) {
    FnUnit& u = *fns.back();
    switch (n->k) {
        case NK::Array: {
            uint16_t mark = u.nextReg;
            for (Node* e : *n->kids) {
                uint16_t r = allocReg(u);
                emitExpr(e, r);
            }
            // elements occupy consecutive regs starting at `mark`
            emit3(u, OP_NewArray, dst, (uint8_t)n->kids->size(), mark);
            freeRegsTo(u, mark);
            return;
        }
        case NK::Object: {
            emit1(u, OP_NewObject, dst);
            for (Node* prop : *n->kids) {
                if (prop->op == 1) { // numeric key
                    uint16_t mark = u.nextReg;
                    uint16_t keyR = allocReg(u);
                    double d = prop->a->num;
                    if (d == (double)(int32_t)d)
                        emit2(u, OP_LdConst, keyR, (uint8_t)addConst(u, Value::fromSmi((int32_t)d)));
                    else
                        emit2(u, OP_LdConst, keyR, (uint8_t)addConst(u, Value::fromDouble(d)));
                    uint16_t valR = allocReg(u);
                    emitExpr(prop->b, valR);
                    emit3(u, OP_SetKeyed, dst, keyR, valR);
                    freeRegsTo(u, mark);
                } else if (prop->str) { // string key
                    uint16_t mark = u.nextReg;
                    uint16_t valR = allocReg(u);
                    emitExpr(prop->a, valR);
                    String* key = internU(*prop->str);
                    uint32_t ic = addIC(u, key);
                    emit2(u, OP_SetNamed, dst, valR);
                    emitExt(u, ic);
                    freeRegsTo(u, mark);
                } else {
                    fail("internal: bad object literal prop");
                }
            }
            return;
        }
        case NK::Prop: fail("internal: bare Prop");
        case NK::Member: {
            uint16_t mark = u.nextReg;
            uint16_t objR;
            if (simpleBaseReg(n->a, &objR, &u)) {
                // base already lives in a register (local/param/this): no copy
                String* key = internU(*n->str);
                uint32_t ic = addIC(u, key);
                emit2(u, OP_GetNamed, dst, objR);
                emitExt(u, ic);
                return; // nothing to free: no temp was allocated
            }
            objR = allocReg(u);
            emitExpr(n->a, objR);
            String* key = internU(*n->str);
            uint32_t ic = addIC(u, key);
            emit2(u, OP_GetNamed, dst, objR);
            emitExt(u, ic);
            freeRegsTo(u, mark);
            return;
        }
        case NK::Index: {
            uint16_t mark = u.nextReg;
            uint16_t objR = allocReg(u);
            uint16_t keyR = allocReg(u);
            emitExpr(n->a, objR);
            emitExpr(n->b, keyR);
            emit3(u, OP_GetKeyed, dst, objR, keyR);
            freeRegsTo(u, mark);
            return;
        }
        case NK::Call: {
            uint16_t mark = u.nextReg;
            uint16_t fnR = dst;
            // args must live at consecutive regs; never move the allocator DOWN
            // onto live locals (dst may be a low local register).
            if (u.nextReg < (uint16_t)(dst + 1)) {
                u.nextReg = (uint16_t)(dst + 1);
                if (u.nextReg > u.peakReg) u.peakReg = u.nextReg;
            }
            emitExpr(n->a, fnR);
            uint8_t argc = 0;
            uint16_t firstArg = fnR + 1;
            bool hasArg = false;
            if (n->kids) {
                for (Node* a : *n->kids) {
                    if (argc >= 254) fail("too many arguments");
                    uint16_t ar = allocReg(u);
                    if (!hasArg) { firstArg = ar; hasArg = true; }
                    emitExpr(a, ar);
                    argc++;
                }
            }
            emit3(u, OP_Call, fnR, (uint8_t)firstArg, argc);
            freeRegsTo(u, mark);
            return;
        }
        case NK::New: {
            uint16_t mark = u.nextReg;
            uint16_t fnR = dst;
            if (u.nextReg < (uint16_t)(dst + 1)) {
                u.nextReg = (uint16_t)(dst + 1);
                if (u.nextReg > u.peakReg) u.peakReg = u.nextReg;
            }
            emitExpr(n->a, fnR);
            uint8_t argc = 0;
            uint16_t firstArg = fnR + 1;
            bool hasArg = false;
            if (n->kids) {
                for (Node* a : *n->kids) {
                    if (argc >= 254) fail("too many arguments");
                    uint16_t ar = allocReg(u);
                    if (!hasArg) { firstArg = ar; hasArg = true; }
                    emitExpr(a, ar);
                    argc++;
                }
            }
            emit3(u, OP_New, fnR, (uint8_t)firstArg, argc);
            freeRegsTo(u, mark);
            return;
        }
        case NK::Unary: {
            uint8_t op = n->op;
            if (op == (uint8_t)P::Inc || op == (uint8_t)P::Dec) {
                // prefix ++/--
                Node* t = n->a;
                if (t->k == NK::Ident) {
                    Resolved r;
                    if (!resolve(*t->str, r)) {
                        // global inc
                        uint16_t mark = u.nextReg;
                        uint16_t cur = allocReg(u);
                        emitIdent(t, cur);
                        emit1(u, op == (uint8_t)P::Inc ? OP_Inc : OP_Dec, cur);
                        String* key = internU(*t->str);
                        uint32_t ic = addIC(u, key);
                        emit2(u, OP_SetGlobal, cur, 0);
                        emitExt(u, ic);
                        if (dst != cur) emit2(u, OP_Mov, dst, cur);
                        freeRegsTo(u, mark);
                    } else if (r.kind == RKind::LOCAL) {
                        emit1(u, op == (uint8_t)P::Inc ? OP_Inc : OP_Dec, r.reg);
                        emitLoadVar(r, dst);
                    } else if (r.kind == RKind::CTX) {
                        emitLoadVar(r, dst); // loads + TDZ check
                        emit1(u, op == (uint8_t)P::Inc ? OP_Inc : OP_Dec, dst);
                        emitStoreVar(r, dst);
                    }
                    return;
                }
                if (t->k == NK::Member || t->k == NK::Index) {
                    uint16_t mark = u.nextReg;
                    uint16_t objR = allocReg(u);
                    emitExpr(t->a, objR);
                    uint16_t cur = allocReg(u);
                    if (t->k == NK::Member) {
                        String* key = internU(*t->str);
                        uint32_t ic = addIC(u, key);
                        emit2(u, OP_GetNamed, cur, objR);
                        emitExt(u, ic);
                        emit1(u, op == (uint8_t)P::Inc ? OP_Inc : OP_Dec, cur);
                        emit2(u, OP_SetNamed, objR, cur);
                        emitExt(u, ic);
                    } else {
                        uint16_t keyR = allocReg(u);
                        emitExpr(t->b, keyR);
                        emit3(u, OP_GetKeyed, cur, objR, keyR);
                        emit1(u, op == (uint8_t)P::Inc ? OP_Inc : OP_Dec, cur);
                        emit3(u, OP_SetKeyed, objR, keyR, cur);
                    }
                    if (dst != cur) emit2(u, OP_Mov, dst, cur);
                    freeRegsTo(u, mark);
                    return;
                }
                fail("invalid ++/-- target");
            }
            uint16_t mark = u.nextReg;
            uint16_t src = allocReg(u);
            emitExpr(n->a, src);
            switch (op) {
                case (uint8_t)P::Sub: emit2(u, OP_Neg, dst, src); break;
                case (uint8_t)P::Add: emit2(u, OP_ToNum, dst, src); break;
                case (uint8_t)P::Not: emit2(u, OP_Not, dst, src); break;
                case (uint8_t)P::BitNot: emit2(u, OP_BitNot, dst, src); break;
                case (uint8_t)P::QMark: emit2(u, OP_TypeOf, dst, src); break;  // typeof
                case (uint8_t)P::Colon: emit1(u, OP_LdUndef, dst); break;      // void
                case (uint8_t)P::Comma: { // delete
                    if (n->a->k == NK::Member) {
                        Node* m = n->a;
                        uint16_t objR = allocReg(u);
                        emitExpr(m->a, objR);
                        String* key = internU(*m->str);
                        uint32_t ic = addIC(u, key);
                        emit2(u, OP_Delete, dst, objR);
                        emitExt(u, ic);
                    } else {
                        uint16_t objR = allocReg(u);
                        uint16_t keyR = allocReg(u);
                        emitExpr(n->a->a, objR);
                        emitExpr(n->a->b, keyR);
                        emit3(u, OP_DeleteKeyed, dst, objR, keyR);
                    }
                    break;
                }
                default: fail("internal: unknown unary op");
            }
            freeRegsTo(u, mark);
            return;
        }
        case NK::Postfix: {
            // x++ / x-- : value = old
            uint8_t op = n->op;
            Node* t = n->a;
            if (t->k == NK::Ident) {
                Resolved r;
                if (!resolve(*t->str, r)) {
                    uint16_t mark = u.nextReg;
                    uint16_t cur = allocReg(u);
                    emitIdent(t, cur);
                    if (dst != cur) emit2(u, OP_Mov, dst, cur);
                    emit1(u, op == (uint8_t)P::Inc ? OP_Inc : OP_Dec, cur);
                    String* key = internU(*t->str);
                    uint32_t ic = addIC(u, key);
                    emit2(u, OP_SetGlobal, cur, 0);
                    emitExt(u, ic);
                    freeRegsTo(u, mark);
                } else if (r.kind == RKind::LOCAL) {
                    if (dst != r.reg) emit2(u, OP_Mov, dst, r.reg);
                    emit1(u, op == (uint8_t)P::Inc ? OP_Inc : OP_Dec, r.reg);
                } else {
                    emitLoadVar(r, dst);
                    uint16_t mark = u.nextReg;
                    uint16_t tmp = allocReg(u);
                    emit2(u, OP_Mov, tmp, dst);
                    emit1(u, op == (uint8_t)P::Inc ? OP_Inc : OP_Dec, tmp);
                    emitStoreVar(r, tmp);
                    freeRegsTo(u, mark);
                }
                return;
            }
            if (t->k == NK::Member || t->k == NK::Index) {
                uint16_t mark = u.nextReg;
                uint16_t objR = allocReg(u);
                emitExpr(t->a, objR);
                uint16_t cur = allocReg(u);
                if (t->k == NK::Member) {
                    String* key = internU(*t->str);
                    uint32_t ic = addIC(u, key);
                    emit2(u, OP_GetNamed, cur, objR);
                    emitExt(u, ic);
                    if (dst != cur) emit2(u, OP_Mov, dst, cur);
                    emit1(u, op == (uint8_t)P::Inc ? OP_Inc : OP_Dec, cur);
                    emit2(u, OP_SetNamed, objR, cur);
                    emitExt(u, ic);
                } else {
                    uint16_t keyR = allocReg(u);
                    emitExpr(t->b, keyR);
                    emit3(u, OP_GetKeyed, cur, objR, keyR);
                    if (dst != cur) emit2(u, OP_Mov, dst, cur);
                    emit1(u, op == (uint8_t)P::Inc ? OP_Inc : OP_Dec, cur);
                    emit3(u, OP_SetKeyed, objR, keyR, cur);
                }
                freeRegsTo(u, mark);
                return;
            }
            fail("invalid postfix target");
        }
        case NK::Logical: {
            uint8_t op = n->op;
            emitExpr(n->a, dst);
            uint32_t at;
            if (op == (uint8_t)P::AndAnd) at = emitCondJmp(u, OP_JmpIfFalse, dst);
            else if (op == (uint8_t)P::OrOr) at = emitCondJmp(u, OP_JmpIfTrue, dst);
            else at = emitCondJmp(u, OP_JmpIfNotNU, dst); // ??
            emitExpr(n->b, dst);
            patchCond(u, at, here(u));
            return;
        }
        case NK::Cond: {
            uint16_t mark = u.nextReg;
            uint16_t condR = allocReg(u);
            emitExpr(n->a, condR);
            uint32_t jf = emitCondJmp(u, OP_JmpIfFalse, condR);
            freeRegsTo(u, mark);
            emitExpr(n->b, dst);
            uint32_t jend = emitJmp24p(u);
            patchCond(u, jf, here(u));
            emitExpr(n->c, dst);
            patchJmp24(u, jend, here(u));
            return;
        }
        case NK::Assign: {
            Node* t = n->a;
            uint8_t aop = n->op;
            bool compound = aop != (uint8_t)P::Assign;
            if (t->k == NK::Ident) {
                Resolved r;
                bool found = resolve(*t->str, r);
                if (!found) r.kind = RKind::GLOBAL;
                if (!compound) {
                    emitExpr(n->b, dst);
                    if (r.kind == RKind::LOCAL) emitStoreVar(r, dst);
                    else if (r.kind == RKind::CTX) emitStoreVar(r, dst);
                    else {
                        String* key = internU(*t->str);
                        uint32_t ic = addIC(u, key);
                        emit2(u, OP_SetGlobal, dst, 0);
                        emitExt(u, ic);
                    }
                } else {
                    // load-modify-store
                    if (r.kind == RKind::LOCAL) {
                        emitLoadVar(r, dst);
                        uint16_t mark = u.nextReg;
                        uint16_t rhs = allocReg(u);
                        emitExpr(n->b, rhs);
                        emitCompound(u, aop, dst, rhs);
                        freeRegsTo(u, mark);
                        emitStoreVar(r, dst);
                    } else if (r.kind == RKind::CTX) {
                        emitLoadVar(r, dst);
                        uint16_t mark = u.nextReg;
                        uint16_t rhs = allocReg(u);
                        emitExpr(n->b, rhs);
                        emitCompound(u, aop, dst, rhs);
                        freeRegsTo(u, mark);
                        emitStoreVar(r, dst);
                    } else {
                        emitIdent(t, dst);
                        uint16_t mark = u.nextReg;
                        uint16_t rhs = allocReg(u);
                        emitExpr(n->b, rhs);
                        emitCompound(u, aop, dst, rhs);
                        freeRegsTo(u, mark);
                        String* key = internU(*t->str);
                        uint32_t ic = addIC(u, key);
                        emit2(u, OP_SetGlobal, dst, 0);
                        emitExt(u, ic);
                    }
                }
                return;
            }
            if (t->k == NK::Member) {
                uint16_t mark = u.nextReg;
                uint16_t objR = allocReg(u);
                emitExpr(t->a, objR);
                if (!compound) {
                    emitExpr(n->b, dst);
                    String* key = internU(*t->str);
                    uint32_t ic = addIC(u, key);
                    emit2(u, OP_SetNamed, objR, dst);
                    emitExt(u, ic);
                } else {
                    String* key = internU(*t->str);
                    uint32_t ic = addIC(u, key);
                    emit2(u, OP_GetNamed, dst, objR);
                    emitExt(u, ic);
                    uint16_t rhs = allocReg(u);
                    emitExpr(n->b, rhs);
                    emitCompound(u, aop, dst, rhs);
                    emit2(u, OP_SetNamed, objR, dst);
                    emitExt(u, ic);
                }
                freeRegsTo(u, mark);
                return;
            }
            if (t->k == NK::Index) {
                uint16_t mark = u.nextReg;
                uint16_t objR = allocReg(u);
                uint16_t keyR = allocReg(u);
                emitExpr(t->a, objR);
                emitExpr(t->b, keyR);
                if (!compound) {
                    emitExpr(n->b, dst);
                    emit3(u, OP_SetKeyed, objR, keyR, dst);
                } else {
                    emit3(u, OP_GetKeyed, dst, objR, keyR);
                    uint16_t rhs = allocReg(u);
                    emitExpr(n->b, rhs);
                    emitCompound(u, aop, dst, rhs);
                    emit3(u, OP_SetKeyed, objR, keyR, dst);
                }
                freeRegsTo(u, mark);
                return;
            }
            fail("invalid assignment target");
        }
        case NK::Binary: emitArith(n, dst); return;
        default: fail("internal: unhandled expression node");
    }
}

// Zero-copy member base: returns true and sets *outReg when the base
// expression already lives in a register we may read directly (no copy, no
// allocation). Covers `this` (reserved reg), plain locals and params.
bool CompilerImpl::simpleBaseReg(Node* base, uint16_t* outReg, FnUnit* u) {
    if (!base) return false;
    if (base->k == NK::This) {
        *outReg = u->thisReg;
        return true;
    }
    if (base->k == NK::Ident) {
        Resolved r;
        if (resolve(*base->str, r) && r.kind == RKind::LOCAL && !r.letConst) {
            *outReg = r.reg;
            return true;
        }
    }
    return false;
}

void CompilerImpl::emitCompound(FnUnit& u, uint8_t aop, uint16_t lhs, uint16_t rhs) {
    // maps compound assignment to the arithmetic op
    uint8_t op;
    switch ((P)aop) {
        case P::AddA: op = OP_Add; break;
        case P::SubA: op = OP_Sub; break;
        case P::MulA: op = OP_Mul; break;
        case P::DivA: op = OP_Div; break;
        case P::ModA: op = OP_Mod; break;
        case P::AndA: op = OP_BitAnd; break;
        case P::OrA:  op = OP_BitOr; break;
        case P::XorA: op = OP_BitXor; break;
        case P::ShlA: op = OP_Shl; break;
        case P::ShrA: op = OP_Shr; break;
        case P::UShrA: op = OP_UShr; break;
        default: fail("internal: bad compound op");
    }
    emit3(u, op, lhs, lhs, rhs);
}

void CompilerImpl::emitArith(Node* n, uint16_t dst) {
    FnUnit& u = *fns.back();
    uint8_t opv = n->op;
    // 'in' (0xFF) / instanceof (0xFE) keywords
    if (opv == 0xFF || opv == 0xFE) {
        uint16_t mark = u.nextReg;
        uint16_t lhs = allocReg(u), rhs = allocReg(u);
        emitExpr(n->a, lhs);
        emitExpr(n->b, rhs);
        if (opv == 0xFF) emit3(u, OP_InOp, dst, lhs, rhs);
        else emit3(u, OP_InstanceOf, dst, lhs, rhs);
        freeRegsTo(u, mark);
        return;
    }
    // peephole: literal RHS on the hot paths fuses into one instruction
    // (dst = lhs op imm8) — removes the LdConst + arg Mov from loops.
    if (n->b->k == NK::Num) {
        double d = n->b->num;
        if (d == (double)(int32_t)d && d >= -128 && d <= 127) {
            int8_t imm = (int8_t)(int32_t)d;
            switch ((P)opv) {
                case P::Add: emitExpr(n->a, dst); emit3(u, OP_AddImm, dst, dst, (uint8_t)imm); return;
                case P::Sub: emitExpr(n->a, dst); emit3(u, OP_SubImm, dst, dst, (uint8_t)imm); return;
                case P::Mul: emitExpr(n->a, dst); emit3(u, OP_MulSmi, dst, dst, (uint8_t)imm); return;
                case P::Lt: emitExpr(n->a, dst); emit3(u, OP_LtSmi, dst, dst, (uint8_t)imm); return;
                case P::Le: emitExpr(n->a, dst); emit3(u, OP_LeSmi, dst, dst, (uint8_t)imm); return;
                case P::Gt: emitExpr(n->a, dst); emit3(u, OP_GtSmi, dst, dst, (uint8_t)imm); return;
                case P::Ge: emitExpr(n->a, dst); emit3(u, OP_GeSmi, dst, dst, (uint8_t)imm); return;
                default: break;
            }
        }
    }
    uint16_t mark = u.nextReg;
    emitExpr(n->a, dst);
    uint16_t rhs = allocReg(u);
    emitExpr(n->b, rhs);
    uint8_t op;
    switch ((P)opv) {
        case P::Add: op = OP_Add; break;
        case P::Sub: op = OP_Sub; break;
        case P::Mul: op = OP_Mul; break;
        case P::Div: op = OP_Div; break;
        case P::Mod: op = OP_Mod; break;
        case P::Pow: op = OP_NOP; break; // ** via runtime helper below
        case P::BitAnd: op = OP_BitAnd; break;
        case P::BitOr:  op = OP_BitOr; break;
        case P::BitXor: op = OP_BitXor; break;
        case P::Shl: op = OP_Shl; break;
        case P::Shr: op = OP_Shr; break;
        case P::UShr: op = OP_UShr; break;
        case P::Eq: op = OP_Eq; break;
        case P::Ne: op = OP_Ne; break;
        case P::StrictEq: op = OP_StrictEq; break;
        case P::StrictNe: op = OP_StrictNe; break;
        case P::Lt: op = OP_Lt; break;
        case P::Le: op = OP_Le; break;
        case P::Gt: op = OP_Gt; break;
        case P::Ge: op = OP_Ge; break;
        default: fail("internal: unknown binary op");
    }
    if (op == OP_NOP) {
        // ** -> Math.pow fallback through builtin call
        uint16_t mathR = allocReg(u), powR = allocReg(u);
        emit2(u, OP_GetGlobal, mathR, 0);
        emitExt(u, addIC(u, rt->intern("Math")));
        emit2(u, OP_GetNamed, powR, mathR);
        emitExt(u, addIC(u, rt->intern("pow")));
        uint16_t a0 = allocReg(u), a1 = allocReg(u);
        emit2(u, OP_Mov, a0, dst);
        emit2(u, OP_Mov, a1, rhs);
        emit3(u, OP_Call, powR, a0, 2);
        emit2(u, OP_Mov, dst, powR);
        freeRegsTo(u, mark);
        return;
    }
    emit3(u, op, dst, dst, rhs);
    freeRegsTo(u, mark);
}

// __PART3__ (functions + program entry)

} // namespace ts
