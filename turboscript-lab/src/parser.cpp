// TurboScript Tier 0 — parser.cpp (part 2: recursive descent)
#include "parser.h"
#include <cstdio>

namespace ts {

namespace {
// precedence for binary operators (higher binds tighter)
static int precOf(P p, bool& rightAssoc) {
    switch (p) {
        case P::OrOr: return 1;
        case P::Nullish: return 2;
        case P::AndAnd: return 3;
        case P::BitOr: return 4;
        case P::BitXor: return 5;
        case P::BitAnd: return 6;
        case P::Eq: case P::Ne: case P::StrictEq: case P::StrictNe: return 7;
        case P::Lt: case P::Le: case P::Gt: case P::Ge: return 8;
        case P::Shl: case P::Shr: case P::UShr: return 9;
        case P::Add: case P::Sub: return 10;
        case P::Mul: case P::Div: case P::Mod: return 11;
        case P::Pow: rightAssoc = true; return 12;
        default: return 0;
    }
}

class P2 {
public:
    std::vector<Token> toks;
    size_t pos = 0;
    uint32_t lastLine = 1;

    Token& cur() { return toks[pos]; }
    Token& advance() { Token& t = toks[pos]; if (toks[pos].kind != TokK::Eof) pos++; lastLine = t.line; return t; }
    bool at(P p) { return cur().kind == TokK::Punc && cur().punc == p; }
    bool atKw(const char* w) {
        if (cur().kind != TokK::Kw) return false;
        size_t n = std::strlen(w);
        if (cur().text.size() != n) return false;
        for (size_t i = 0; i < n; i++) if (cur().text[i] != (char16_t)w[i]) return false;
        return true;
    }
    bool eat(P p) { if (at(p)) { advance(); return true; } return false; }
    bool eatKw(const char* w) { if (atKw(w)) { advance(); return true; } return false; }
    void expect(P p) {
        if (!at(p)) fail(u"unexpected token (expected punctuation)");
        advance();
    }
    [[noreturn]] void fail(const char16_t* msg) {
        throw SyntaxErr{std::u16string(msg) + u" (got: " + kindName() + u")", cur().line};
    }
    std::u16string kindName() {
        switch (cur().kind) {
            case TokK::Eof: return u"<eof>";
            case TokK::Num: return u"<num>";
            case TokK::Str: return u"<string>";
            case TokK::Ident: return cur().text;
            case TokK::Kw: return cur().text;
            default: return u"<punc>";
        }
    }
    static std::u16string* dupStr(const std::u16string& s) { return new std::u16string(s); }

    bool newlineBefore() { return cur().line > lastLine; }

    // ------------------------------------------------ statements
    Node* parseProgram() {
        Node* block = new Node(NK::Block);
        block->kids = new std::vector<Node*>();
        while (cur().kind != TokK::Eof) block->kids->push_back(parseStatement());
        return block;
    }

    Node* parseStatement() {
        switch (cur().kind) {
            case TokK::Punc: {
                if (at(P::LC)) return parseBlock();
                if (at(P::Semi)) { advance(); return new Node(NK::Empty); }
                break;
            }
            case TokK::Kw: {
                if (atKw("var") || atKw("let") || atKw("const")) return parseVarDecl();
                if (atKw("function")) { Node* f = parseFunction(true); f->op = 1; return f; }
                if (atKw("class")) { Node* f = parseClass(); f->op = 1; return f; }
                if (atKw("if")) return parseIf();
                if (atKw("while")) return parseWhile();
                if (atKw("do")) return parseDoWhile();
                if (atKw("for")) return parseFor();
                if (atKw("return")) return parseReturn();
                if (atKw("break")) {
                    advance();
                    Node* n = new Node(NK::Break);
                    eatSemiASI();
                    return n;
                }
                if (atKw("continue")) {
                    advance();
                    Node* n = new Node(NK::Continue);
                    eatSemiASI();
                    return n;
                }
                if (atKw("try")) return parseTry();
                if (atKw("throw")) {
                    advance();
                    Node* n = new Node(NK::Throw);
                    if (newlineBefore()) fail(u"newline after throw");
                    n->a = parseExpression();
                    eatSemiASI();
                    return n;
                }
                break;
            }
            default: break;
        }
        Node* n = new Node(NK::ExprStmt);
        n->a = parseExpression();
        eatSemiASI();
        return n;
    }

    Node* wrapStmt(Node* f) { f->op = 1; Node* n = new Node(NK::ExprStmt); n->a = f; return n; }

    void eatSemiASI() {
        if (eat(P::Semi)) return;
        if (at(P::RC) || cur().kind == TokK::Eof) return;
        if (newlineBefore()) return; // ASI heuristic
        fail(u"expected ';'");
    }

    Node* parseBlock() {
        expect(P::LC);
        Node* n = new Node(NK::Block);
        n->kids = new std::vector<Node*>();
        while (!at(P::RC)) {
            if (cur().kind == TokK::Eof) fail(u"unterminated block");
            n->kids->push_back(parseStatement());
        }
        expect(P::RC);
        return n;
    }

    Node* parseVarDecl() {
        bool isLet = atKw("let"), isConst = atKw("const");
        advance();
        Node* n = new Node(NK::Var);
        n->decls = new std::vector<Decl>();
        for (;;) {
            if (cur().kind != TokK::Ident) fail(u"expected variable name");
            Decl d;
            d.name = cur().text;
            d.isLet = isLet; d.isConst = isConst;
            advance();
            if (eat(P::Assign)) d.init = parseAssign();
            n->decls->push_back(d);
            if (!eat(P::Comma)) break;
        }
        eatSemiASI();
        return n;
    }

    Node* parseIf() {
        advance();
        expect(P::LP);
        Node* n = new Node(NK::If);
        n->a = parseExpression();
        expect(P::RP);
        n->b = parseStatement();
        if (eatKw("else")) n->c = parseStatement();
        return n;
    }

    Node* parseWhile() {
        advance();
        expect(P::LP);
        Node* n = new Node(NK::While);
        n->a = parseExpression();
        expect(P::RP);
        n->b = parseStatement();
        return n;
    }

    Node* parseDoWhile() {
        advance();
        Node* n = new Node(NK::DoWhile);
        n->b = parseStatement();
        if (!eatKw("while")) fail(u"expected 'while'");
        expect(P::LP);
        n->a = parseExpression();
        expect(P::RP);
        eat(P::Semi);
        return n;
    }

    Node* parseFor() {
        advance();
        expect(P::LP);
        Node* n = new Node(NK::For);
        // init: var decl | expression | empty
        if (at(P::Semi)) { advance(); }
        else if (atKw("var") || atKw("let") || atKw("const")) {
            bool isLet = atKw("let"), isConst = atKw("const");
            advance();
            Node* vd = new Node(NK::Var);
            vd->decls = new std::vector<Decl>();
            if (cur().kind != TokK::Ident) fail(u"expected variable name");
            Decl d; d.name = cur().text; d.isLet = isLet; d.isConst = isConst; advance();
            if (eat(P::Assign)) d.init = parseAssign();
            vd->decls->push_back(d);
            n->a = vd;
            if (atKw("in")) {
                advance();
                Node* fi = new Node(NK::ForIn);
                fi->a = vd; fi->b = parseExpression();
                expect(P::RP);
                fi->c = parseStatement();
                return fi;
            }
            if (atKw("of")) fail(u"for-of not supported in Tier 0 MVP scope");
            expect(P::Semi);
        } else {
            Node* e = parseExpression();
            if (atKw("in")) {
                advance();
                Node* fi = new Node(NK::ForIn);
                Node* tgt = new Node(NK::Empty); tgt->a = e; // marker: expression target
                fi->a = tgt; fi->b = parseExpression();
                expect(P::RP);
                fi->c = parseStatement();
                return fi;
            }
            if (atKw("of")) fail(u"for-of not supported in Tier 0 MVP scope");
            n->a = e;
            expect(P::Semi);
        }
        if (!at(P::Semi)) n->b = parseExpression(); else n->b = nullptr;
        expect(P::Semi);
        if (!at(P::RP)) n->c = parseExpression(); else n->c = nullptr;
        expect(P::RP);
        n->kids = new std::vector<Node*>();
        (*n->kids).push_back(parseStatement());
        return n;
    }

    Node* parseReturn() {
        advance();
        Node* n = new Node(NK::Return);
        if (at(P::Semi) || at(P::RC) || cur().kind == TokK::Eof || newlineBefore()) {
            n->a = nullptr;
        } else {
            n->a = parseExpression();
        }
        eatSemiASI();
        return n;
    }

    Node* parseTry() {
        advance();
        Node* n = new Node(NK::Try);
        if (!at(P::LC)) fail(u"expected '{' after try");
        n->a = parseBlock();
        if (eatKw("catch")) {
            if (eat(P::LP)) {
                if (cur().kind != TokK::Ident) fail(u"expected catch parameter");
                n->str = dupStr(cur().text);
                advance();
                expect(P::RP);
            }
            if (!at(P::LC)) fail(u"expected '{' after catch");
            n->b = parseBlock();
        }
        if (eatKw("finally")) {
            if (!at(P::LC)) fail(u"expected '{' after finally");
            n->c = parseBlock();
        }
        if (!n->b && !n->c) fail(u"try without catch/finally");
        return n;
    }

    // ------------------------------------------------ expressions
    Node* parseExpression() {
        Node* n = parseAssign();
        while (eat(P::Comma)) {
            Node* seq = new Node(NK::Binary);
            seq->op = (uint8_t)P::Comma;
            // fold into left-nested comma chain
            seq->a = n; seq->b = parseAssign();
            n = seq;
        }
        return n;
    }

    Node* parseAssign() {
        // arrow function fast paths
        size_t save = pos;
        if (cur().kind == TokK::Ident && toks[pos + 1].kind == TokK::Punc && toks[pos + 1].punc == P::Arrow) {
            std::u16string* pname = dupStr(cur().text);
            advance(); advance();
            return finishArrow1(pname);
        }
        if (at(P::LP)) {
            if (scanParenIsArrow()) {
                advance(); // (
                Node* f = new Node(NK::Func);
                f->isArrowF = true;
                f->params = new std::vector<std::u16string>();
                if (!at(P::RP)) {
                    for (;;) {
                        if (cur().kind != TokK::Ident) fail(u"expected arrow parameter");
                        f->params->push_back(cur().text);
                        advance();
                        if (!eat(P::Comma)) break;
                    }
                }
                expect(P::RP);
                expect(P::Arrow);
                return finishArrow(f);
            }
            (void)save;
        }
        Node* lhs = parseCond();
        P ops[] = {P::Assign, P::AddA, P::SubA, P::MulA, P::DivA, P::ModA,
                   P::AndA, P::OrA, P::XorA, P::ShlA, P::ShrA, P::UShrA};
        for (P op : ops) {
            if (at(op)) {
                if (lhs->k != NK::Ident && lhs->k != NK::Member && lhs->k != NK::Index)
                    fail(u"invalid assignment target");
                advance();
                Node* n = new Node(NK::Assign);
                n->op = (uint8_t)op;
                n->a = lhs;
                n->b = parseAssign();
                return n;
            }
        }
        return lhs;
    }

    bool scanParenIsArrow() {
        // walk from current LP to matching RP; true iff followed by =>
        size_t p = pos;
        int depth = 0;
        while (p < toks.size()) {
            const Token& t = toks[p];
            if (t.kind == TokK::Punc) {
                if (t.punc == P::LP) depth++;
                else if (t.punc == P::RP) {
                    depth--;
                    if (depth == 0) {
                        p++;
                        return p < toks.size() && toks[p].kind == TokK::Punc && toks[p].punc == P::Arrow;
                    }
                }
            }
            p++;
        }
        return false;
    }

    Node* finishArrow1(std::u16string* pname) {
        Node* f = new Node(NK::Func);
        f->isArrowF = true;
        f->params = new std::vector<std::u16string>();
        f->params->push_back(*pname);
        return finishArrow(f);
    }

    Node* finishArrow(Node* f) {
        if (at(P::LC)) {
            f->isExprBody = false;
            f->body = parseBlock();
        } else {
            f->isExprBody = true;
            f->body = parseAssign();
        }
        return f;
    }

    Node* parseCond() {
        Node* c = parseBinary(1);
        if (at(P::QMark)) {
            advance();
            Node* n = new Node(NK::Cond);
            n->a = c;
            n->b = parseAssign();
            expect(P::Colon);
            n->c = parseAssign();
            return n;
        }
        return c;
    }

    Node* parseBinary(int minPrec) {
        Node* lhs = parseUnary();
        for (;;) {
            int pr = 0; uint8_t opv = 0; bool rightAssoc = false; bool logical = false;
            if (cur().kind == TokK::Punc) {
                pr = precOf(cur().punc, rightAssoc);
                opv = (uint8_t)cur().punc;
                logical = cur().punc == P::AndAnd || cur().punc == P::OrOr || cur().punc == P::Nullish;
            } else if (atKw("in")) { pr = 8; opv = 0xFF; }
            else if (atKw("instanceof")) { pr = 8; opv = 0xFE; }
            if (pr == 0 || pr < minPrec) break;
            advance();
            Node* n = new Node(logical ? NK::Logical : NK::Binary);
            n->op = opv;
            n->a = lhs;
            n->b = rightAssoc ? parseBinary(pr) : parseBinary(pr + 1);
            lhs = n;
        }
        return lhs;
    }

    Node* parseUnary() {
        if (cur().kind == TokK::Punc) {
            P p = cur().punc;
            if (p == P::Not || p == P::Sub || p == P::Add || p == P::BitNot) {
                advance();
                Node* n = new Node(NK::Unary);
                n->op = (uint8_t)p;
                n->a = parseUnary();
                return n;
            }
            if (p == P::Inc || p == P::Dec) {
                advance();
                Node* t = parseUnary();
                if (t->k != NK::Ident && t->k != NK::Member && t->k != NK::Index) fail(u"invalid ++/-- target");
                Node* n = new Node(NK::Unary);
                n->op = (uint8_t)p;
                n->a = t;
                return n;
            }
        }
        if (cur().kind == TokK::Kw) {
            if (atKw("typeof")) {
                advance();
                Node* n = new Node(NK::Unary);
                n->op = (uint8_t)P::QMark; // sentinel: typeof
                n->a = parseUnary();
                return n;
            }
            if (atKw("void")) {
                advance();
                Node* n = new Node(NK::Unary);
                n->op = (uint8_t)P::Colon; // sentinel: void
                n->a = parseUnary();
                return n;
            }
            if (atKw("delete")) {
                advance();
                Node* t = parseUnary();
                if (t->k != NK::Member && t->k != NK::Index) fail(u"invalid delete target");
                Node* n = new Node(NK::Unary);
                n->op = (uint8_t)P::Comma; // sentinel: delete
                n->a = t;
                return n;
            }
        }
        return parsePostfix();
    }

    Node* parsePostfix() {
        Node* n = parseCallMember();
        if (cur().kind == TokK::Punc && (at(P::Inc) || at(P::Dec)) && !newlineBefore()) {
            P p = cur().punc;
            if (n->k == NK::Ident || n->k == NK::Member || n->k == NK::Index) {
                advance();
                Node* post = new Node(NK::Postfix);
                post->op = (uint8_t)p;
                post->a = n;
                return post;
            }
        }
        return n;
    }

    Node* parseMemberOnly() {
        Node* n = parsePrimary();
        for (;;) {
            if (at(P::Dot)) {
                advance();
                if (cur().kind != TokK::Ident && cur().kind != TokK::Kw) fail(u"expected property name");
                Node* m = new Node(NK::Member);
                m->a = n;
                m->str = dupStr(cur().text);
                advance();
                n = m;
            } else if (at(P::LB)) {
                advance();
                Node* m = new Node(NK::Index);
                m->a = n;
                m->b = parseExpression();
                expect(P::RB);
                n = m;
            } else break;
        }
        return n;
    }

    Node* postfixLoop(Node* n) {
        for (;;) {
            if (at(P::Dot)) {
                advance();
                if (cur().kind != TokK::Ident && cur().kind != TokK::Kw) fail(u"expected property name");
                Node* m = new Node(NK::Member);
                m->a = n;
                m->str = dupStr(cur().text);
                advance();
                n = m;
            } else if (at(P::LB)) {
                advance();
                Node* m = new Node(NK::Index);
                m->a = n;
                m->b = parseExpression();
                expect(P::RB);
                n = m;
            } else if (at(P::LP)) {
                advance();
                Node* c = new Node(NK::Call);
                c->a = n;
                c->kids = new std::vector<Node*>();
                if (!at(P::RP)) {
                    for (;;) {
                        c->kids->push_back(parseAssign());
                        if (!eat(P::Comma)) break;
                    }
                }
                expect(P::RP);
                n = c;
            } else break;
        }
        return n;
    }

    Node* parseCallMember() {
        Node* n = parsePrimary();
        return postfixLoop(n);
    }

    Node* parsePrimary() {
        switch (cur().kind) {
            case TokK::Num: {
                Node* n = new Node(NK::Num);
                n->num = advance().num;
                return n;
            }
            case TokK::Str: {
                Node* n = new Node(NK::Str);
                n->str = dupStr(advance().text);
                return n;
            }
            case TokK::TplStart: return parseTemplate();
            case TokK::Ident: {
                Node* n = new Node(NK::Ident);
                n->str = dupStr(advance().text);
                return n;
            }
            case TokK::Kw: {
                if (atKw("true")) { advance(); return new Node(NK::True); }
                if (atKw("false")) { advance(); return new Node(NK::False); }
                if (atKw("null")) { advance(); return new Node(NK::Null); }
                if (atKw("undefined")) { advance(); return new Node(NK::Undef); }
                if (atKw("this")) { advance(); return new Node(NK::This); }
                if (atKw("function")) return parseFunction(false);
                if (atKw("class")) return parseClass();
                if (atKw("new")) {
                    advance();
                    Node* target = parseMemberOnly();
                    Node* n = new Node(NK::New);
                    n->a = target;
                    n->kids = new std::vector<Node*>();
                    if (at(P::LP)) {
                        advance();
                        if (!at(P::RP)) {
                            for (;;) {
                                n->kids->push_back(parseAssign());
                                if (!eat(P::Comma)) break;
                            }
                        }
                        expect(P::RP);
                    }
                    return postfixLoop(n);
                }
                break;
            }
            case TokK::Punc: {
                if (at(P::LP)) {
                    advance();
                    Node* e = parseExpression();
                    expect(P::RP);
                    return e;
                }
                if (at(P::LB)) {
                    advance();
                    Node* n = new Node(NK::Array);
                    n->kids = new std::vector<Node*>();
                    if (!at(P::RB)) {
                        for (;;) {
                            if (at(P::Comma)) { // elision
                                Node* hole = new Node(NK::Undef);
                                n->kids->push_back(hole);
                                advance();
                                continue;
                            }
                            if (at(P::RB)) { n->kids->push_back(new Node(NK::Undef)); break; } // trailing comma
                            n->kids->push_back(parseAssign());
                            if (!eat(P::Comma)) break;
                        }
                    }
                    expect(P::RB);
                    return n;
                }
                if (at(P::LC)) return parseObjectLit();
                break;
            }
            default: break;
        }
        fail(u"unexpected token");
    }

    Node* parseTemplate() {
        Node* n = new Node(NK::Tpl);
        n->kids = new std::vector<Node*>();
        Node* first = new Node(NK::Str);
        first->str = dupStr(advance().text);
        n->kids->push_back(first);
        // now: expr, closing '}', then TplMid/TplEnd
        for (;;) {
            n->kids->push_back(parseExpression());
            expect(P::RC);
            if (cur().kind == TokK::TplMid) {
                Node* piece = new Node(NK::Str);
                piece->str = dupStr(advance().text);
                n->kids->push_back(piece);
                continue;
            }
            if (cur().kind == TokK::TplEnd) {
                Node* piece = new Node(NK::Str);
                piece->str = dupStr(advance().text);
                n->kids->push_back(piece);
                break;
            }
            fail(u"unterminated template");
        }
        return n;
    }

    Node* parseObjectLit() {
        expect(P::LC);
        Node* n = new Node(NK::Object);
        n->kids = new std::vector<Node*>();
        while (!at(P::RC)) {
            Node* prop = new Node(NK::Prop);
            if (cur().kind == TokK::Ident || cur().kind == TokK::Kw) {
                prop->str = dupStr(cur().text);
                advance();
                if (at(P::LP)) { // method shorthand
                    Node* f = new Node(NK::Func);
                    f->params = new std::vector<std::u16string>();
                    expect(P::LP);
                    if (!at(P::RP)) {
                        for (;;) {
                            if (cur().kind != TokK::Ident) fail(u"expected parameter");
                            f->params->push_back(cur().text);
                            advance();
                            if (!eat(P::Comma)) break;
                        }
                    }
                    expect(P::RP);
                    if (!at(P::LC)) fail(u"expected method body");
                    f->body = parseBlock();
                    prop->a = f;
                    prop->b = nullptr;
                    n->kids->push_back(prop);
                    if (!eat(P::Comma)) break;
                    continue;
                }
                if (eat(P::Colon)) {
                    prop->a = parseAssign();
                } else {
                    // shorthand {x}
                    Node* id = new Node(NK::Ident);
                    id->str = dupStr(*prop->str);
                    prop->a = id;
                }
            } else if (cur().kind == TokK::Str) {
                prop->str = dupStr(advance().text);
                expect(P::Colon);
                prop->a = parseAssign();
            } else if (cur().kind == TokK::Num) {
                Node* kn = new Node(NK::Num);
                kn->num = advance().num;
                prop->a = kn;
                prop->op = 1; // marker: numeric key
                expect(P::Colon);
                prop->b = parseAssign();
                n->kids->push_back(prop);
                if (!eat(P::Comma)) break;
                continue;
            } else {
                fail(u"expected property key");
            }
            n->kids->push_back(prop);
            if (!eat(P::Comma)) break;
        }
        expect(P::RC);
        return n;
    }

    Node* parseFunction(bool decl) {
        advance(); // 'function'
        Node* n = new Node(NK::Func);
        n->params = new std::vector<std::u16string>();
        if (cur().kind == TokK::Ident) { n->fname = dupStr(cur().text); advance(); }
        else if (decl) fail(u"function declaration requires a name");
        expect(P::LP);
        if (!at(P::RP)) {
            for (;;) {
                if (cur().kind != TokK::Ident) fail(u"expected parameter");
                n->params->push_back(cur().text);
                advance();
                if (!eat(P::Comma)) break;
            }
        }
        expect(P::RP);
        if (!at(P::LC)) fail(u"expected function body");
        n->body = parseBlock();
        return n;
    }

    // class Name { method(a){} } -> desugars in compiler
    Node* parseClass() {
        advance(); // 'class'
        Node* n = new Node(NK::Func);
        n->params = new std::vector<std::u16string>(); // constructor params
        if (cur().kind != TokK::Ident) fail(u"expected class name");
        n->fname = dupStr(cur().text);
        advance();
        if (eatKw("extends")) fail(u"class extends not supported in Tier 0 MVP scope");
        expect(P::LC);
        n->kids = new std::vector<Node*>(); // methods as Prop-less Func list with names
        while (!at(P::RC)) {
            if (atKw("static")) fail(u"static methods not supported in Tier 0 MVP scope");
            if (atKw("constructor")) advance();
            else if (cur().kind != TokK::Ident) fail(u"expected method name");
            std::u16string mname = atKw("constructor") ? u"constructor" : cur().text;
            if (!atKw("constructor")) advance();
            Node* m = new Node(NK::Func);
            m->fname = new std::u16string(mname);
            m->params = new std::vector<std::u16string>();
            expect(P::LP);
            if (!at(P::RP)) {
                for (;;) {
                    if (cur().kind != TokK::Ident) fail(u"expected parameter");
                    m->params->push_back(cur().text);
                    advance();
                    if (!eat(P::Comma)) break;
                }
            }
            expect(P::RP);
            if (!at(P::LC)) fail(u"expected method body");
            m->body = parseBlock();
            n->kids->push_back(m);
            eat(P::Semi);
        }
        expect(P::RC);
        return n;
    }
};

} // anonymous namespace

Node* Parser::parseProgram(const char* src, size_t len) {
    P2 p;
    try {
        lexAll(src, len, p.toks);
        p.pos = 0;
        return p.parseProgram();
    } catch (const LexError& e) {
        std::u16string m = u"SyntaxError: ";
        m += e.msg;
        throw SyntaxErr{m, e.line};
    }
}

} // namespace ts
