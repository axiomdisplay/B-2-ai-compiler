// TurboScript Tier 0 — parser.h
// Lexer + recursive-descent parser -> compact AST.
// Registered MVP scope gaps (documented in docs/SCOPE.md):
//   generators/async, for-of, switch, labels, classes (see below),
//   optional chaining, getters/setters in literals, BigInt literals.
#include "value.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ts {

enum class NK : uint8_t {
    Num, Str, Ident, Null, Undef, True, False, This, Tpl,
    Array, Object, Prop,
    Func,                      // function expr/decl/arrow
    Call, New, Member, Index,  // Member: a=obj, str=name; Index: a=obj, b=key
    Unary, Postfix, Binary, Logical, Assign, Cond,
    Var,                       // declaration statement (decls list)
    ExprStmt, Block, If, While, DoWhile, For, ForIn,
    Return, Break, Continue, Try, Throw, Empty,
};

enum class TokK : uint8_t {
    Eof, Ident, Num, Str, TplStart, TplMid, TplEnd,
    Punc, Kw,
};

enum class P : uint8_t { // punctuators (subset)
    LP=0, RP, LB, RB, LC, RC, Semi, Comma, Dot,
    Add, Sub, Mul, Div, Mod, Pow,
    Assign, AddA, SubA, MulA, DivA, ModA,
    Eq, Ne, StrictEq, StrictNe, Lt, Le, Gt, Ge,
    AndAnd, OrOr, Nullish, Not, BitAnd, BitOr, BitXor, BitNot,
    Shl, Shr, UShr, ShlA, ShrA, UShrA, AndA, OrA, XorA,
    Inc, Dec, QMark, Colon, Arrow,
};

struct Token {
    TokK kind = TokK::Eof;
    P punc = P::Semi;
    std::u16string text;   // ident / keyword / string content
    double num = 0;
    uint32_t line = 1;
};

struct Node;

struct Decl {
    std::u16string name;
    Node* init = nullptr;
    bool isLet = false, isConst = false;
};

struct SyntaxErr {
    std::u16string msg;
    uint32_t line;
};

struct LexError {
    std::u16string msg;
    uint32_t line;
};

void lexAll(const char* s, size_t len, std::vector<Token>& out);

struct Node {
    NK k;
    uint8_t op = 0;            // P value for operators
    uint32_t line = 1;
    double num = 0;
    std::u16string* str = nullptr;   // owned
    Node* a = nullptr; Node* b = nullptr; Node* c = nullptr;
    Node* body = nullptr;            // function bodies
    std::vector<Node*>* kids = nullptr;
    std::vector<Decl>* decls = nullptr;
    // function nodes
    std::vector<std::u16string>* params = nullptr;
    std::u16string* fname = nullptr;
    bool isArrowF = false, isExprBody = false, strictF = true;

    explicit Node(NK kk) : k(kk) {}
};

class Parser {
public:
    // Returns the program block (NK::Block) or throws const char* on syntax error.
    static Node* parseProgram(const char* src, size_t len);
};

} // namespace ts
