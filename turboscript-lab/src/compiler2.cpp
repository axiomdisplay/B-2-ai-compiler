// TurboScript Tier 0 — compiler2.cpp
// Statement codegen, function compilation, program entry.
// Registered MVP gaps: classes, function hoisting, labels, switch,
// break/continue across finally, per-iteration loop closures.
#include "compiler_impl.h"
#include <cstdio>
#include <cstdlib>

namespace ts {

// --------------------------------------------------------------- statements
void CompilerImpl::emitVarDecl(Node* n) {
    FnUnit& u = *fns.back();
    for (auto& d : *n->decls) {
        Resolved r;
        bool found = resolve(d.name, r);
        if (!found || r.kind == RKind::GLOBAL) {
            // global binding (program top level or implicit global)
            if (d.init) {
                uint16_t mark = u.nextReg;
                uint16_t tmp = allocReg(u);
                emitExpr(d.init, tmp);
                String* key = internU(d.name);
                uint32_t ic = addIC(u, key);
                emit2(u, OP_SetGlobal, tmp, 0);
                emitExt(u, ic);
                freeRegsTo(u, mark);
            }
            continue;
        }
        if (d.init) {
            uint16_t mark = u.nextReg;
            if (r.kind == RKind::LOCAL) {
                emitExpr(d.init, r.reg);
            } else {
                uint16_t tmp = allocReg(u);
                emitExpr(d.init, tmp);
                emitStoreVar(r, tmp);
            }
            freeRegsTo(u, mark);
        }
        // no init: pre-init (undef/TDZ) already emitted at scope entry
    }
}

void CompilerImpl::emitBlockScoped(Node* n) {
    FnUnit& u = *fns.back();
    Scope* s = nodeScope[n];
    if (!s) { emitStmt(n); return; } // safety
    // pre-init let/const at scope entry (TDZ until declared)
    for (auto& kv : s->vars) {
        VarInfo& v = kv.second;
        if (!v.isLetConst) continue;
        if (v.captured) {
            v.ctxIdx = s->nCtxCells++;
        } else {
            v.reg = allocReg(u);
            emit1(u, OP_LdTDZ, (uint8_t)v.reg);
        }
    }
    enterScope(u, s);
    if (n->kids) for (Node* k : *n->kids) emitStmt(k);
    exitScope(u, s);
}

void CompilerImpl::emitStoreToTarget(Node* t, uint16_t valReg) {
    FnUnit& u = *fns.back();
    if (t->k == NK::Ident) {
        Resolved r;
        if (!resolve(*t->str, r)) {
            String* key = internU(*t->str);
            uint32_t ic = addIC(u, key);
            emit2(u, OP_SetGlobal, valReg, 0);
            emitExt(u, ic);
        } else {
            emitStoreVar(r, valReg);
        }
        return;
    }
    if (t->k == NK::Member) {
        uint16_t mark = u.nextReg;
        uint16_t objR = allocReg(u);
        emitExpr(t->a, objR);
        String* key = internU(*t->str);
        uint32_t ic = addIC(u, key);
        emit2(u, OP_SetNamed, objR, valReg);
        emitExt(u, ic);
        freeRegsTo(u, mark);
        return;
    }
    if (t->k == NK::Index) {
        uint16_t mark = u.nextReg;
        uint16_t objR = allocReg(u);
        uint16_t keyR = allocReg(u);
        emitExpr(t->a, objR);
        emitExpr(t->b, keyR);
        emit3(u, OP_SetKeyed, objR, keyR, valReg);
        freeRegsTo(u, mark);
        return;
    }
    fail("invalid assignment target");
}

void CompilerImpl::closeRegionsTo(FnUnit& u, FnUnit::Loop& L) {
    for (size_t i = u.openTrys.size(); i > L.tryCountAtEntry; i--)
        emit1(u, OP_TryExit, 0);
    for (uint32_t d = u.ctxDepthNow; d > L.ctxDepthAtEntry; d--)
        emit1(u, OP_ExitCtx, 0);
}

void CompilerImpl::emitStmt(Node* n) {
    FnUnit& u = *fns.back();
    switch (n->k) {
        case NK::Empty: return;
        case NK::Block: emitBlockScoped(n); return;
        case NK::Var: emitVarDecl(n); return;
        case NK::ExprStmt: {
            uint16_t mark = u.nextReg;
            uint16_t tmp = allocReg(u);
            emitExpr(n->a, tmp);
            freeRegsTo(u, mark);
            return;
        }
        case NK::Func: {
            bool isDecl = (n->op == 1);
            uint16_t mark = u.nextReg;
            uint16_t tmp = allocReg(u);
            emitFuncNode(n, tmp);
            if (isDecl && n->fname) {
                Resolved r;
                if (resolve(*n->fname, r)) {
                    emitStoreVar(r, tmp);
                } else {
                    String* key = internU(*n->fname);
                    uint32_t ic = addIC(u, key);
                    emit2(u, OP_SetGlobal, tmp, 0);
                    emitExt(u, ic);
                }
            }
            freeRegsTo(u, mark);
            return;
        }
        case NK::If: {
            uint16_t mark = u.nextReg;
            uint16_t c = allocReg(u);
            emitExpr(n->a, c);
            uint32_t jf = emitCondJmp(u, OP_JmpIfFalse, c);
            freeRegsTo(u, mark);
            emitStmt(n->b);
            if (n->c) {
                uint32_t jend = emitJmp24p(u);
                patchCond(u, jf, here(u));
                emitStmt(n->c);
                patchJmp24(u, jend, here(u));
            } else {
                patchCond(u, jf, here(u));
            }
            return;
        }
        case NK::While: {
            FnUnit::Loop L;
            L.tryCountAtEntry = u.openTrys.size();
            L.ctxDepthAtEntry = u.ctxDepthNow;
            L.canBreak = L.canContinue = !anyOpenFinally(u);
            u.loops.push_back(L);
            uint32_t start = here(u);
            uint16_t mark = u.nextReg;
            uint16_t c = allocReg(u);
            emitExpr(n->a, c);
            uint32_t jf = emitCondJmp(u, OP_JmpIfFalse, c);
            freeRegsTo(u, mark);
            emitStmt(n->b);
            for (uint32_t at : u.loops.back().continues) patchJmp24(u, at, start);
            emitJmp24(u, (int32_t)((int64_t)start - (int64_t)(here(u) + 1)));
            patchCond(u, jf, here(u));
            for (uint32_t at : u.loops.back().breaks) patchJmp24(u, at, here(u));
            u.loops.pop_back();
            return;
        }
        case NK::DoWhile: {
            FnUnit::Loop L;
            L.tryCountAtEntry = u.openTrys.size();
            L.ctxDepthAtEntry = u.ctxDepthNow;
            L.canBreak = L.canContinue = !anyOpenFinally(u);
            u.loops.push_back(L);
            uint32_t start = here(u);
            emitStmt(n->b);
            for (uint32_t at : u.loops.back().continues) patchJmp24(u, at, here(u));
            uint16_t mark = u.nextReg;
            uint16_t c = allocReg(u);
            emitExpr(n->a, c);
            uint32_t jt = emitCondJmp(u, OP_JmpIfTrue, c);
            freeRegsTo(u, mark);
            patchCond(u, jt, start); // backedge when condition true; fallthrough = exit
            for (uint32_t at : u.loops.back().breaks) patchJmp24(u, at, here(u));
            u.loops.pop_back();
            return;
        }
        case NK::For: {
            if (n->a) {
                if (n->a->k == NK::Var) emitVarDecl(n->a);
                else {
                    uint16_t mark = u.nextReg;
                    uint16_t tmp = allocReg(u);
                    emitExpr(n->a, tmp);
                    freeRegsTo(u, mark);
                }
            }
            FnUnit::Loop L;
            L.tryCountAtEntry = u.openTrys.size();
            L.ctxDepthAtEntry = u.ctxDepthNow;
            L.canBreak = L.canContinue = !anyOpenFinally(u);
            u.loops.push_back(L);
            uint32_t start = here(u);
            uint32_t jf = 0;
            if (n->b) {
                uint16_t mark = u.nextReg;
                uint16_t c = allocReg(u);
                emitExpr(n->b, c);
                jf = emitCondJmp(u, OP_JmpIfFalse, c);
                freeRegsTo(u, mark);
            }
            emitStmt((*n->kids)[0]);
            // continue target: step
            for (uint32_t at : u.loops.back().continues) patchJmp24(u, at, here(u));
            if (n->c) {
                uint16_t mark = u.nextReg;
                uint16_t tmp = allocReg(u);
                emitExpr(n->c, tmp);
                freeRegsTo(u, mark);
            }
            emitJmp24(u, (int32_t)((int64_t)start - (int64_t)(here(u) + 1)));
            if (n->b) patchCond(u, jf, here(u));
            for (uint32_t at : u.loops.back().breaks) patchJmp24(u, at, here(u));
            u.loops.pop_back();
            return;
        }
        case NK::ForIn: {
            // desugar: keys=ForInKeys(src); for (i=0; i<keys.length; i++) { key=keys[i]; ... }
            Node* varDecl = (n->a->k == NK::Var) ? n->a : nullptr;
            Node* assignTarget = varDecl ? nullptr : n->a->a; // Empty wrapper
            FnUnit::Loop L;
            L.tryCountAtEntry = u.openTrys.size();
            L.ctxDepthAtEntry = u.ctxDepthNow;
            L.canBreak = L.canContinue = !anyOpenFinally(u);
            u.loops.push_back(L);
            uint16_t mark = u.nextReg;
            uint16_t keys = allocReg(u);
            uint16_t idx = allocReg(u);
            uint16_t len = allocReg(u);
            uint16_t keyR = allocReg(u);
            emitExpr(n->b, keys);
            emit2(u, OP_ForInKeys, keys, keys);
            emit1(u, OP_LdZero, (uint8_t)idx);
            uint32_t start = here(u);
            String* lenKey = rt->intern("length");
            uint32_t lenIc = addIC(u, lenKey);
            emit2(u, OP_GetNamed, len, keys);
            emitExt(u, lenIc);
            emit3(u, OP_Lt, keyR, idx, len);
            uint32_t jf = emitCondJmp(u, OP_JmpIfFalse, keyR);
            emit3(u, OP_GetKeyed, keyR, keys, idx);
            if (varDecl) {
                const std::u16string& vname = (*varDecl->decls)[0].name;
                Resolved r;
                if (!resolve(vname, r)) {
                    String* key = internU(vname);
                    uint32_t ic = addIC(u, key);
                    emit2(u, OP_SetGlobal, keyR, 0);
                    emitExt(u, ic);
                } else {
                    emitStoreVar(r, keyR);
                }
            } else {
                emitStoreToTarget(assignTarget, keyR);
            }
            emitStmt(n->c); // loop body (parser stores it in c for ForIn)
            for (uint32_t at : u.loops.back().continues) patchJmp24(u, at, here(u));
            emit1(u, OP_Inc, (uint8_t)idx);
            emitJmp24(u, (int32_t)((int64_t)start - (int64_t)(here(u) + 1)));
            patchCond(u, jf, here(u));
            for (uint32_t at : u.loops.back().breaks) patchJmp24(u, at, here(u));
            u.loops.pop_back();
            freeRegsTo(u, mark);
            return;
        }
        case NK::Return: {
            for (auto it = u.openTrys.rbegin(); it != u.openTrys.rend(); ++it) {
                if (it->hasFinally) {
                    uint16_t mark = u.nextReg;
                    uint16_t t = allocReg(u);
                    if (n->a) emitExpr(n->a, t); else emit1(u, OP_LdUndef, t);
                    emit1(u, OP_LdTrue, (uint8_t)it->flagReg);
                    emit2(u, OP_Mov, (uint8_t)it->valReg, (uint8_t)t);
                    freeRegsTo(u, mark);
                    it->retJumps.push_back(emitJmp24p(u)); // patched once finEntry exists
                    return;
                }
            }
            uint16_t mark = u.nextReg;
            uint16_t t = allocReg(u);
            if (n->a) emitExpr(n->a, t); else emit1(u, OP_LdUndef, t);
            emit1(u, OP_Return, (uint8_t)t);
            freeRegsTo(u, mark);
            return;
        }
        case NK::Break: {
            if (u.loops.empty()) fail("'break' outside loop", n);
            FnUnit::Loop& L = u.loops.back();
            if (!L.canBreak) fail("break crossing finally (registered MVP gap)", n);
            closeRegionsTo(u, L);
            L.breaks.push_back(here(u));
            emit3(u, OP_Jmp, 0, 0, 0);
            return;
        }
        case NK::Continue: {
            if (u.loops.empty()) fail("'continue' outside loop", n);
            FnUnit::Loop& L = u.loops.back();
            if (!L.canContinue) fail("continue crossing finally (registered MVP gap)", n);
            closeRegionsTo(u, L);
            L.continues.push_back(here(u));
            emit3(u, OP_Jmp, 0, 0, 0);
            return;
        }
        case NK::Throw: {
            uint16_t mark = u.nextReg;
            uint16_t t = allocReg(u);
            emitExpr(n->a, t);
            emit1(u, OP_Throw, (uint8_t)t);
            freeRegsTo(u, mark);
            return;
        }
        case NK::Try: emitTry(n); return;
        default: fail("internal: unhandled statement");
    }
}

// --------------------------------------------------------------- try/catch/finally
void CompilerImpl::emitTry(Node* n) {
    FnUnit& u = *fns.back();
    bool hasCatch = n->b != nullptr;
    bool hasFinally = n->c != nullptr;
    uint16_t mark = u.nextReg;
    uint32_t ctxDepthAtEnter = u.ctxDepthNow;

    auto registerHandler = [&](uint32_t start, uint32_t end, uint32_t target,
                               uint8_t creg, bool isFinally) {
        Handler h;
        h.start = start; h.end = end; h.target = target;
        h.ctxDepth = (uint16_t)ctxDepthAtEnter;
        h.catchReg = creg;
        h.isFinally = isFinally;
        u.handlers.push_back(h);
        return (uint32_t)u.handlers.size() - 1;
    };

    if (!hasFinally) {
        // try { body } catch (e) { catchBody }
        uint16_t catchReg = allocReg(u);
        uint32_t hIdx = (uint32_t)u.handlers.size(); // reserved slot
        u.handlers.push_back({});
        uint32_t enterAt = here(u);
        emit2(u, OP_TryEnter, 0, 0);
        emitExt(u, hIdx);
        u.openTrys.push_back({});
        uint32_t bodyStart = here(u);
        emitStmt(n->a);
        emit1(u, OP_TryExit, 0);
        u.openTrys.pop_back();
        uint32_t bodyEnd = here(u);
        uint32_t jend = emitJmp24p(u);
        uint32_t catchTarget = here(u);
        u.handlers[hIdx] = {bodyStart, bodyEnd, catchTarget,
                            (uint16_t)ctxDepthAtEnter, (uint8_t)catchReg, false};
        u.code[enterAt + 1] = makeExt(hIdx);
        // bind exception to catch variable
        Resolved r;
        if (resolve(*n->str, r)) {
            emitStoreVar(r, catchReg);
        } else {
            String* key = internU(*n->str);
            uint32_t ic = addIC(u, key);
            emit2(u, OP_SetGlobal, catchReg, 0);
            emitExt(u, ic);
        }
        emitStmt(n->b);
        patchJmp24(u, jend, here(u));
        freeRegsTo(u, mark);
        return;
    }

    // ---- finally is present ----
    FnUnit::TryRegion tr;
    tr.hasFinally = true;
    tr.flagReg = allocReg(u);
    tr.valReg = allocReg(u);
    tr.exReg = allocReg(u);
    uint16_t catchReg = 0;
    if (hasCatch) catchReg = allocReg(u);

    uint32_t hFinIdx = (uint32_t)u.handlers.size(); // reserved
    u.handlers.push_back({});
    uint32_t enterAt = here(u);
    emit2(u, OP_TryEnter, 0, 0);
    emitExt(u, hFinIdx);
    u.openTrys.push_back(tr);

    uint32_t bodyStart = here(u);
    if (hasCatch) {
        uint32_t hCatchIdx = (uint32_t)u.handlers.size();
        u.handlers.push_back({});
        uint32_t enterAt2 = here(u);
        emit2(u, OP_TryEnter, 0, 0);
        emitExt(u, hCatchIdx);
        u.openTrys.push_back({});
        uint32_t innerStart = here(u);
        emitStmt(n->a);
        emit1(u, OP_TryExit, 0);
        u.openTrys.pop_back();
        uint32_t innerEnd = here(u);
        uint32_t jafter = emitJmp24p(u);
        uint32_t catchTarget = here(u);
        u.handlers[hCatchIdx] = {innerStart, innerEnd, catchTarget,
                                 (uint16_t)ctxDepthAtEnter, (uint8_t)catchReg, false};
        u.code[enterAt2 + 1] = makeExt(hCatchIdx);
        Resolved r;
        if (resolve(*n->str, r)) emitStoreVar(r, catchReg);
        else {
            String* key = internU(*n->str);
            uint32_t ic = addIC(u, key);
            emit2(u, OP_SetGlobal, catchReg, 0);
            emitExt(u, ic);
        }
        emitStmt(n->b);
        patchJmp24(u, jafter, here(u));
    } else {
        emitStmt(n->a);
    }
    uint32_t bodyEnd = here(u);

    emit1(u, OP_TryExit, 0);
    std::vector<uint32_t> retJumps = u.openTrys.back().retJumps; // recorded by Return sites
    u.openTrys.pop_back();

    // normal path into finally (outside the try region)
    emit1(u, OP_LdFalse, (uint8_t)tr.flagReg);
    uint32_t finEntry = here(u);
    for (uint32_t at : retJumps) patchJmp24(u, at, finEntry);
    emitStmt(n->c);
    // dispatch: if flag -> return value
    uint32_t jnotret = emitCondJmp(u, OP_JmpIfFalse, (uint8_t)tr.flagReg);
    emit1(u, OP_Return, (uint8_t)tr.valReg);
    patchCond(u, jnotret, here(u));
    uint32_t jend = emitJmp24p(u);

    // exception path: finally, then rethrow
    uint32_t finTarget = here(u);
    u.handlers[hFinIdx] = {bodyStart, bodyEnd, finTarget,
                           (uint16_t)ctxDepthAtEnter, (uint8_t)tr.exReg, true};
    u.code[enterAt + 1] = makeExt(hFinIdx);
    emitStmt(n->c);
    emit1(u, OP_Throw, (uint8_t)tr.exReg);
    patchJmp24(u, jend, here(u));
    freeRegsTo(u, mark);
}

// --------------------------------------------------------------- functions
void CompilerImpl::emitFuncNode(Node* f, uint16_t dst) {
    FnUnit& u = *fns.back();
    BytecodeFunction* bf = compileFunction(f);
    u.funcs.push_back(bf);
    uint32_t idx = (uint32_t)u.funcs.size() - 1;
    emit2(u, OP_Closure, dst, 0);
    emitExt(u, idx);
}

void CompilerImpl::finalizeFunction(FnUnit& u) {
    BytecodeFunction* bf = u.bf;
    bf->codeLen = (uint32_t)u.code.size();
    bf->code = new uint32_t[bf->codeLen ? bf->codeLen : 1];
    for (uint32_t i = 0; i < bf->codeLen; i++) bf->code[i] = u.code[i];
    bf->nConsts = (uint32_t)u.consts.size();
    bf->consts = new Value[bf->nConsts ? bf->nConsts : 1];
    for (uint32_t i = 0; i < bf->nConsts; i++) bf->consts[i] = u.consts[i];
    bf->nICs = (uint32_t)u.ics.size();
    bf->ics = new IC[bf->nICs ? bf->nICs : 1];
    for (uint32_t i = 0; i < bf->nICs; i++) bf->ics[i] = u.ics[i];
    bf->nHandlers = (uint32_t)u.handlers.size();
    bf->handlers = new Handler[bf->nHandlers ? bf->nHandlers : 1];
    for (uint32_t i = 0; i < bf->nHandlers; i++) bf->handlers[i] = u.handlers[i];
    bf->nFuncs = (uint32_t)u.funcs.size();
    bf->funcs = new BytecodeFunction*[bf->nFuncs ? bf->nFuncs : 1];
    for (uint32_t i = 0; i < bf->nFuncs; i++) bf->funcs[i] = u.funcs[i];
    bf->nRegs = u.peakReg > u.nextReg ? u.peakReg : u.nextReg;
    bf->nParams = u.nParams;
}

void CompilerImpl::setupFunctionScope(FnUnit* u, Node* f) {
    Scope* fs = nodeScope[f];
    u->fnScope = fs;
    if (f->params) {
        u->nParams = (uint16_t)f->params->size();
    }
    u->needOwnCtx = fs->needCtx;
    if (u->needOwnCtx) {
        ctxCounter++;
        fs->gDepth = ctxCounter;
        u->rootGDepth = ctxCounter;
    } else {
        u->rootGDepth = ctxCounter;
    }
    scopes.push_back(fs);
    // params: non-captured -> reg = index; captured -> cell
    uint16_t ri = 0;
    if (f->params) {
        for (auto& p : *f->params) {
            VarInfo* v = fs->find(p);
            if (v && v->captured) v->ctxIdx = fs->nCtxCells++;
            else if (v) v->reg = ri;
            ri++;
        }
    }
    u->nextReg = u->nParams;
    // reserve `this` register (loaded once at entry; zero-copy member bases)
    u->hasThisReg = true;
    u->thisReg = u->nextReg;
    u->nextReg++;
    if (u->nextReg > u->peakReg) u->peakReg = u->nextReg;
    // remaining fn-scope vars (var-hoisted, fn-level let/const, self-name)
    for (auto& kv : fs->vars) {
        VarInfo& v = kv.second;
        bool isParam = false;
        if (f->params) {
            for (auto& p : *f->params) if (p == kv.first) { isParam = true; break; }
        }
        if (isParam) continue;
        if (v.captured) {
            v.ctxIdx = fs->nCtxCells++;
        } else {
            v.reg = u->nextReg;
            u->nextReg++;
        }
    }
}

void CompilerImpl::initFnLocals(FnUnit* u) {
    FnUnit& uu = *u;
    Scope* fs = u->fnScope;
    Node* f = nullptr;
    // find the function node for param names (walk nodeScope inverse — store on unit)
    f = u->fnNode;
    if (u->needOwnCtx) {
        emit1(uu, OP_NewCtx, (uint8_t)fs->nCtxCells);
        u->ctxDepthNow = 1; // matches runtime frame state for handler tables
    }
    if (u->hasThisReg) emit1(uu, OP_LdThis, (uint8_t)u->thisReg);
    // captured params: copy arg from param reg into cell (cells start as TDZ)
    if (f && f->params) {
        uint16_t i = 0;
        for (auto& p : *f->params) {
            VarInfo* v = fs->find(p);
            if (v && v->captured) {
                emit3(uu, OP_SetCtx, 0, (uint8_t)v->ctxIdx, (uint8_t)i);
            }
            i++;
        }
    }
    // init remaining vars
    for (auto& kv : fs->vars) {
        VarInfo& v = kv.second;
        bool isParam = false;
        if (f && f->params) {
            for (auto& p : *f->params) if (p == kv.first) { isParam = true; break; }
        }
        if (isParam) continue;
        bool isSelfName = (f && f->fname && kv.first == *f->fname);
        if (v.captured) {
            if (isSelfName) {
                uint16_t t = allocReg(uu);
                emit1(uu, OP_LdFn, (uint8_t)t);
                emit3(uu, OP_SetCtx, 0, (uint8_t)v.ctxIdx, (uint8_t)t);
            } else if (!v.isLetConst) {
                uint16_t t = allocReg(uu);
                emit1(uu, OP_LdUndef, (uint8_t)t);
                emit3(uu, OP_SetCtx, 0, (uint8_t)v.ctxIdx, (uint8_t)t);
            }
        } else {
            if (isSelfName) emit1(uu, OP_LdFn, (uint8_t)v.reg);
            else if (v.isLetConst) emit1(uu, OP_LdTDZ, (uint8_t)v.reg);
            else emit1(uu, OP_LdUndef, (uint8_t)v.reg);
        }
    }
}

BytecodeFunction* CompilerImpl::compileFunction(Node* f) {
    if (f->num == 1.0) fail("class syntax not supported in Tier 0 MVP (registered scope gap)", f);
    uint32_t savedCtxCounter = ctxCounter; // context scopes don't outlive the fn compile
    FnUnit u;
    u.fnNode = f;
    fns.push_back(&u);
    setupFunctionScope(&u, f);
    initFnLocals(&u);
    if (f->body && f->body->kids) for (Node* k : *f->body->kids) emitStmt(k);
    uint16_t t = allocReg(u);
    emit1(u, OP_LdUndef, (uint8_t)t);
    emit1(u, OP_Return, (uint8_t)t);
    finalizeFunction(u);
    scopes.pop_back();
    fns.pop_back();
    ctxCounter = savedCtxCounter;
    return u.bf;
}

// program entry
BytecodeFunction* CompilerImpl::compileProgram(Node* program) {
    FnUnit u;
    u.fnNode = nullptr;
    fns.push_back(&u);
    Scope* prog = nodeScope[program];
    u.fnScope = prog;
    u.needOwnCtx = prog->needCtx;
    scopes.push_back(prog);
    if (u.needOwnCtx) {
        ctxCounter++;
        prog->gDepth = ctxCounter;
        u.rootGDepth = ctxCounter;
    } else {
        u.rootGDepth = 0;
    }
    // bind program-scope let/const (vars are global and never bound here)
    for (auto& kv : prog->vars) {
        VarInfo& v = kv.second;
        if (v.captured) v.ctxIdx = prog->nCtxCells++;
        else { v.reg = u.nextReg; u.nextReg++; }
    }
    if (u.needOwnCtx) {
        emit1(u, OP_NewCtx, (uint8_t)prog->nCtxCells);
        u.ctxDepthNow = 1; // matches runtime frame state for handler tables
    }
    for (auto& kv : prog->vars) {
        VarInfo& v = kv.second;
        if (!v.captured) emit1(u, OP_LdTDZ, (uint8_t)v.reg);
    }
    if (program->kids) for (Node* k : *program->kids) emitStmt(k);
    uint16_t t = allocReg(u);
    emit1(u, OP_LdUndef, (uint8_t)t);
    emit1(u, OP_Return, (uint8_t)t);
    finalizeFunction(u);
    scopes.pop_back();
    fns.pop_back();
    return u.bf;
}

BytecodeFunction* Compiler::compile(Runtime* rt, Node* program) {
    CompilerImpl impl(rt);
    impl.analyzeProgram(program);
    impl.scopes.clear();
    return impl.compileProgram(program);
}

} // namespace ts
