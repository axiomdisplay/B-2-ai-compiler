// TurboScript Tier 0 — lexer.cpp
// Single-pass tokenizer with template-literal state machine.
#include "parser.h"
#include <cstdlib>
#include <cstring>

namespace ts {

static bool identStartB(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}
static bool identPartB(unsigned char c) { return identStartB(c) || (c >= '0' && c <= '9'); }

static const char* kKeywords[] = {
    "var","let","const","function","return","if","else","while","do","for","in","of",
    "break","continue","new","this","typeof","instanceof","true","false","null",
    "undefined","try","catch","finally","throw","delete","void","class","switch",
    "case","default","extends","static","super","yield","await",
    nullptr
};

static int readEscape(const char* s, size_t len, size_t& i, uint32_t line) {
    size_t j = i + 1;
    if (j >= len) throw LexError{{u"bad escape"}, line};
    char c = s[j];
    switch (c) {
        case 'n': i = j + 1; return '\n';
        case 't': i = j + 1; return '\t';
        case 'r': i = j + 1; return '\r';
        case 'b': i = j + 1; return '\b';
        case 'f': i = j + 1; return '\f';
        case 'v': i = j + 1; return '\v';
        case '0': i = j + 1; return 0;
        case '\n': line++; i = j + 1; return -1;
        case '\r': i = (j + 1 < len && s[j+1] == '\n') ? j + 2 : j + 1; if (i > j + 1) line++; return -1;
        case 'x': {
            if (j + 2 >= len) throw LexError{{u"bad \\x"}, line};
            char b[3] = {s[j+1], s[j+2], 0};
            i = j + 3; return (int)std::strtoul(b, nullptr, 16);
        }
        case 'u': {
            if (j + 1 < len && s[j+1] == '{') {
                size_t e = j + 2; uint32_t v = 0;
                while (e < len && s[e] != '}') {
                    char h = s[e++];
                    int d = (h >= '0' && h <= '9') ? h - '0' : ((h|32) >= 'a' && (h|32) <= 'f') ? (h|32) - 'a' + 10 : -1;
                    if (d < 0) throw LexError{{u"bad \\u{}"}, line};
                    v = v * 16 + (uint32_t)d;
                }
                i = e + 1; return (int)v;
            }
            if (j + 4 >= len) throw LexError{{u"bad \\u"}, line};
            char b[5] = {s[j+1], s[j+2], s[j+3], s[j+4], 0};
            i = j + 5; return (int)std::strtoul(b, nullptr, 16);
        }
        default: i = j + 1; return (unsigned char)c;
    }
}

static void appendCp(std::u16string& out, uint32_t cp) {
    if (cp <= 0xFFFF) { out.push_back((char16_t)cp); return; }
    cp -= 0x10000;
    out.push_back((char16_t)(0xD800 + (cp >> 10)));
    out.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
}

// Scan template text until ` (end) or ${ (expression). i points after ` or }.
static size_t scanTplPiece(const char* s, size_t len, size_t i, uint32_t& line,
                           std::u16string& out, bool& closedByBacktick) {
    while (i < len) {
        char c = s[i];
        if (c == '`') { closedByBacktick = true; return i + 1; }
        if (c == '$' && i + 1 < len && s[i+1] == '{') { closedByBacktick = false; return i + 2; }
        if (c == '\n') line++;
        if (c == '\\') {
            int cp = readEscape(s, len, i, line);
            if (cp >= 0) appendCp(out, (uint32_t)cp);
            continue;
        }
        out.push_back((char16_t)(unsigned char)c);
        i++;
    }
    throw LexError{{u"unterminated template"}, line};
}

static Token* emit(std::vector<Token>& out, TokK k, uint32_t line) {
    out.emplace_back();
    out.back().kind = k;
    out.back().line = line;
    return &out.back();
}

void lexAll(const char* s, size_t len, std::vector<Token>& out) {
    size_t i = 0;
    uint32_t line = 1;
    // template state: brace depth inside current ${...}, or -1 when scanning piece text
    std::vector<int> tplBrace;   // one entry per open template
    bool inTemplateExpr = false; // true => lexing normal tokens until '}' closes expr

    while (i < len) {
        char c = s[i];
        if (c == '\n') { line++; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { i++; continue; }
        if (c == '/' && i + 1 < len && s[i+1] == '/') { while (i < len && s[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < len && s[i+1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i+1] == '/')) { if (s[i] == '\n') line++; i++; }
            if (i + 1 >= len) throw LexError{{u"unterminated comment"}, line};
            i += 2; continue;
        }
        if (c == '"' || c == '\'') {
            char q = c; i++;
            std::u16string txt; bool closed = false;
            while (i < len) {
                char d = s[i];
                if (d == q) { i++; closed = true; break; }
                if (d == '\n') throw LexError{{u"unterminated string"}, line};
                if (d == '\\') { int cp = readEscape(s, len, i, line); if (cp >= 0) appendCp(txt, (uint32_t)cp); continue; }
                txt.push_back((char16_t)(unsigned char)d); i++;
            }
            if (!closed) throw LexError{{u"unterminated string"}, line};
            emit(out, TokK::Str, line)->text = std::move(txt);
            continue;
        }
        if (c == '`') {
            i++;
            std::u16string piece; bool closed;
            i = scanTplPiece(s, len, i, line, piece, closed);
            if (closed) {
                emit(out, TokK::Str, line)->text = std::move(piece); // plain template, no exprs
            } else {
                emit(out, TokK::TplStart, line)->text = std::move(piece);
                tplBrace.push_back(0);
                inTemplateExpr = true;
            }
            continue;
        }
        if (inTemplateExpr && c == '}') {
            if (tplBrace.back() == 0) {
                inTemplateExpr = false;
                emit(out, TokK::Punc, line)->punc = P::RC; // close the expression
                i++;
                std::u16string piece; bool closed;
                i = scanTplPiece(s, len, i, line, piece, closed);
                if (closed) {
                    tplBrace.pop_back();
                    emit(out, TokK::TplEnd, line)->text = std::move(piece);
                } else {
                    emit(out, TokK::TplMid, line)->text = std::move(piece);
                    inTemplateExpr = true;
                }
                continue;
            }
            tplBrace.back()--;
            emit(out, TokK::Punc, line)->punc = P::RC; i++; continue;
        }
        if (inTemplateExpr && c == '{') { tplBrace.back()++; emit(out, TokK::Punc, line)->punc = P::LC; i++; continue; }

        if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < len && s[i+1] >= '0' && s[i+1] <= '9')) {
            const char* start = s + i; char* end = nullptr; double v;
            if (c == '0' && i + 1 < len && (s[i+1] | 32) == 'x') { v = (double)std::strtoull(s+i+2, &end, 16); }
            else if (c == '0' && i + 1 < len && (s[i+1] | 32) == 'o') { v = (double)std::strtoull(s+i+2, &end, 8); }
            else if (c == '0' && i + 1 < len && (s[i+1] | 32) == 'b') { v = (double)std::strtoull(s+i+2, &end, 2); }
            else { v = std::strtod(start, &end); }
            if (end == start) throw LexError{{u"bad number"}, line};
            emit(out, TokK::Num, line)->num = v;
            i += (size_t)(end - start);
            continue;
        }
        if (identStartB((unsigned char)c)) {
            std::u16string txt;
            while (i < len && identPartB((unsigned char)s[i])) { txt.push_back((char16_t)(unsigned char)s[i]); i++; }
            bool kw = false;
            for (int k = 0; kKeywords[k]; k++) {
                size_t wl = std::strlen(kKeywords[k]);
                if (txt.size() == wl) {
                    bool eq = true;
                    for (size_t m2 = 0; m2 < wl; m2++) if (txt[m2] != (char16_t)kKeywords[k][m2]) { eq = false; break; }
                    if (eq) { kw = true; break; }
                }
            }
            emit(out, kw ? TokK::Kw : TokK::Ident, line)->text = std::move(txt);
            continue;
        }
        auto m = [&](const char* t) -> bool {
            size_t n = std::strlen(t);
            if (i + n <= len && std::memcmp(s + i, t, n) == 0) { i += n; return true; }
            return false;
        };
        P p;
        if (m("===")) p = P::StrictEq;
        else if (m("!==")) p = P::StrictNe;
        else if (m(">>>")) p = P::UShr;
        else if (m("==")) p = P::Eq;
        else if (m("!=")) p = P::Ne;
        else if (m("<=")) p = P::Le;
        else if (m(">=")) p = P::Ge;
        else if (m("&&")) p = P::AndAnd;
        else if (m("||")) p = P::OrOr;
        else if (m("??")) p = P::Nullish;
        else if (m("<<")) p = P::Shl;
        else if (m(">>")) p = P::Shr;
        else if (m("**")) p = P::Pow;
        else if (m("++")) p = P::Inc;
        else if (m("--")) p = P::Dec;
        else if (m("+=")) p = P::AddA;
        else if (m("-=")) p = P::SubA;
        else if (m("*=")) p = P::MulA;
        else if (m("/=")) p = P::DivA;
        else if (m("%=")) p = P::ModA;
        else if (m("&=")) p = P::AndA;
        else if (m("|=")) p = P::OrA;
        else if (m("^=")) p = P::XorA;
        else if (m("=>")) p = P::Arrow;
        else if (m("(")) p = P::LP;
        else if (m(")")) p = P::RP;
        else if (m("[")) p = P::LB;
        else if (m("]")) p = P::RB;
        else if (m("{")) p = P::LC;
        else if (m("}")) p = P::RC;
        else if (m(";")) p = P::Semi;
        else if (m(",")) p = P::Comma;
        else if (m(".")) p = P::Dot;
        else if (m("+")) p = P::Add;
        else if (m("-")) p = P::Sub;
        else if (m("*")) p = P::Mul;
        else if (m("/")) p = P::Div;
        else if (m("%")) p = P::Mod;
        else if (m("=")) p = P::Assign;
        else if (m("<")) p = P::Lt;
        else if (m(">")) p = P::Gt;
        else if (m("!")) p = P::Not;
        else if (m("&")) p = P::BitAnd;
        else if (m("|")) p = P::BitOr;
        else if (m("^")) p = P::BitXor;
        else if (m("~")) p = P::BitNot;
        else if (m("?")) p = P::QMark;
        else if (m(":")) p = P::Colon;
        else throw LexError{{u"unexpected character"}, line};
        emit(out, TokK::Punc, line)->punc = p;
    }
    if (inTemplateExpr || !tplBrace.empty()) throw LexError{{u"unterminated template"}, line};
    emit(out, TokK::Eof, line);
}

} // namespace ts
