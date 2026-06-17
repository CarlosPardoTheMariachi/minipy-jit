// parser.cc — recursive descent, one function per grammar nonterminal.
//
// The core trick of the whole file: the token cursor `pos_` only ever moves
// forward, every parse_* function consumes exactly the tokens of its own
// nonterminal, and precedence comes from which function calls which
// (parse_or calls parse_and calls parse_not ... down to parse_primary).
#include "parser.h"

#include <memory>

#include "error.h"

namespace minipy {

namespace {

bool is_comp_op(Tok k) {
    return k == Tok::EqEq || k == Tok::NotEq || k == Tok::Lt ||
           k == Tok::Le || k == Tok::Gt || k == Tok::Ge;
}

BinOp comp_binop(Tok k) {
    switch (k) {
        case Tok::EqEq:  return BinOp::Eq;
        case Tok::NotEq: return BinOp::Ne;
        case Tok::Lt:    return BinOp::Lt;
        case Tok::Le:    return BinOp::Le;
        case Tok::Gt:    return BinOp::Gt;
        default:         return BinOp::Ge;   // only Tok::Ge is left
    }
}

class Parser {
public:
    explicit Parser(const std::vector<Token>& toks) : toks_(toks) {}

    // program ::= ( NEWLINE | funcdef | statement )*
    Program parse_program() {
        Program prog;
        Block main_body;
        while (!check(Tok::Eof)) {
            // A NEWLINE by itself here is an empty logical line, skip it.
            if (check(Tok::Newline)) {
                advance();
                continue;
            }
            if (check(Tok::Def)) {
                prog.funcs.push_back(parse_funcdef());
            } else {
                // Any top-level statement gets collected; at the end they all
                // become the body of the implicit __main__ function.
                main_body.push_back(parse_statement());
            }
        }
        auto main = std::make_unique<FuncDef>(kMainFunc, 1);
        main->body = std::move(main_body);
        prog.funcs.push_back(std::move(main));
        return prog;
    }

private:
    // --- token cursor --------------------------------------------------------
    // These four are the entire interface to the token stream.

    // Look at the current token (or k tokens ahead) WITHOUT consuming it.
    // The lexer guarantees the stream ends with Eof, so if we ever look past
    // the end we just return that final Eof forever instead of crashing.
    const Token& peek(size_t k = 0) const {
        size_t i = pos_ + k;
        if (i < toks_.size()) {
            return toks_[i];
        }
        return toks_.back();   // back() is always Eof
    }

    // Is the current token this kind?  (pure lookahead, consumes nothing)
    bool check(Tok k) const {
        return peek().kind == k;
    }

    // Consume the current token unconditionally and hand it back.
    const Token& advance() {
        return toks_[pos_++];
    }

    // Consume the current token ONLY if it's the expected kind, and throw a
    // readable error if it isn't.  `what` is the human name for the message.
    const Token& expect(Tok k, const char* what) {
        if (!check(k)) {
            throw CompileError(peek().line,
                std::string("expected ") + what + ", found '" +
                to_string(peek().kind) + "'");
        }
        return toks_[pos_++];
    }

    [[noreturn]] void fail(const char* msg) {
        throw CompileError(peek().line, msg);
    }

    // --- funcdef / block -----------------------------------------------------
    // funcdef ::= "def" IDENT "(" params? ")" ":" block
    std::unique_ptr<FuncDef> parse_funcdef() {
        int ln = peek().line;
        expect(Tok::Def, "'def'");
        const Token& name = expect(Tok::Ident, "function name");
        auto fn = std::make_unique<FuncDef>(name.text, ln);
        expect(Tok::LParen, "'('");
        // params ::= IDENT ( "," IDENT )*  — or nothing at all
        if (!check(Tok::RParen)) {
            fn->params.push_back(expect(Tok::Ident, "parameter name").text);
            while (check(Tok::Comma)) {
                advance();
                fn->params.push_back(expect(Tok::Ident, "parameter name").text);
            }
        }
        expect(Tok::RParen, "')'");
        expect(Tok::Colon, "':'");
        fn->body = parse_block();
        return fn;
    }

    // block ::= NEWLINE INDENT statement+ DEDENT
    // This is where the lexer's off-side tokens pay off: a block has explicit
    // open/close brackets (INDENT/DEDENT), so parsing it is totally ordinary.
    Block parse_block() {
        expect(Tok::Newline, "newline before an indented block");
        expect(Tok::Indent, "an indented block");
        Block body;
        // statement+ means at least one, so parse the first unconditionally.
        body.push_back(parse_statement());
        while (!check(Tok::Dedent) && !check(Tok::Eof)) {
            body.push_back(parse_statement());
        }
        expect(Tok::Dedent, "dedent to close the block");
        return body;
    }

    // --- statements ----------------------------------------------------------
    // statement ::= simple_stmt NEWLINE | if_stmt | while_stmt
    StmtPtr parse_statement() {
        if (check(Tok::If)) {
            return parse_if();
        }
        if (check(Tok::While)) {
            return parse_while();
        }
        // Everything else is a one-line statement that must end with NEWLINE.
        StmtPtr s = parse_simple_stmt();
        expect(Tok::Newline, "newline after statement");
        return s;
    }

    // simple_stmt ::= assignment | return_stmt | expr
    StmtPtr parse_simple_stmt() {
        int ln = peek().line;
        if (check(Tok::Return)) {
            advance();
            // `return` alone (next token is the NEWLINE) means return-nothing;
            // the value pointer stays null and later phases treat it as 0.
            ExprPtr val;
            if (!check(Tok::Newline)) {
                val = parse_expr();
            }
            return std::make_unique<ReturnStmt>(std::move(val), ln);
        }
        // Assignment vs plain expression: both can start with IDENT, so look
        // one token ahead — `x = ...` is an assignment, anything else (like
        // `x + 1` or `f(x)`) is an expression statement.  This one token of
        // lookahead is the only place the parser needs peek(1) at all.
        if (check(Tok::Ident) && peek(1).kind == Tok::Assign) {
            std::string name = advance().text;   // the IDENT
            advance();                           // the '='
            ExprPtr val = parse_expr();
            return std::make_unique<AssignStmt>(std::move(name), std::move(val), ln);
        }
        return std::make_unique<ExprStmt>(parse_expr(), ln);
    }

    // if_stmt ::= "if" expr ":" block ( "elif" expr ":" block )* ( "else" ":" block )?
    StmtPtr parse_if() {
        int ln = peek().line;
        auto f = std::make_unique<IfStmt>(ln);
        expect(Tok::If, "'if'");
        ExprPtr cond = parse_expr();
        expect(Tok::Colon, "':'");
        f->branches.emplace_back(std::move(cond), parse_block());
        // Each elif is just another (condition, body) pair in the same list.
        while (check(Tok::Elif)) {
            advance();
            ExprPtr c = parse_expr();
            expect(Tok::Colon, "':'");
            f->branches.emplace_back(std::move(c), parse_block());
        }
        if (check(Tok::Else)) {
            advance();
            expect(Tok::Colon, "':'");
            f->else_body = parse_block();
        }
        return f;
    }

    // while_stmt ::= "while" expr ":" block
    StmtPtr parse_while() {
        int ln = peek().line;
        auto w = std::make_unique<WhileStmt>(ln);
        expect(Tok::While, "'while'");
        w->cond = parse_expr();
        expect(Tok::Colon, "':'");
        w->body = parse_block();
        return w;
    }

    // --- expressions ---------------------------------------------------------
    // One function per precedence level, lowest binding first.  Each level
    // parses its operands by calling the NEXT level down, so tighter-binding
    // operators end up deeper in the tree automatically.

    ExprPtr parse_expr() {
        return parse_or();
    }

    // or_expr ::= and_expr ( "or" and_expr )*
    // The while-loop shape builds LEFT associativity: `a or b or c` loops
    // twice and wraps the earlier result as the lhs, giving ((a or b) or c).
    ExprPtr parse_or() {
        ExprPtr lhs = parse_and();
        while (check(Tok::Or)) {
            int ln = advance().line;
            ExprPtr rhs = parse_and();
            lhs = std::make_unique<BinaryExpr>(BinOp::Or, std::move(lhs),
                                               std::move(rhs), ln);
        }
        return lhs;
    }

    // and_expr ::= not_expr ( "and" not_expr )*
    ExprPtr parse_and() {
        ExprPtr lhs = parse_not();
        while (check(Tok::And)) {
            int ln = advance().line;
            ExprPtr rhs = parse_not();
            lhs = std::make_unique<BinaryExpr>(BinOp::And, std::move(lhs),
                                               std::move(rhs), ln);
        }
        return lhs;
    }

    // not_expr ::= "not" not_expr | comparison
    // `not` recurses into ITSELF (not the next level down) so `not not x`
    // works — prefix operators nest by recursion, not by loop.
    ExprPtr parse_not() {
        if (check(Tok::Not)) {
            int ln = advance().line;
            ExprPtr operand = parse_not();
            return std::make_unique<UnaryExpr>(UnOp::Not, std::move(operand), ln);
        }
        return parse_comparison();
    }

    // comparison ::= additive ( comp_op additive )?    — note the ?, not a *
    // At most ONE comparison allowed: `a < b < c` is a parse error by design.
    ExprPtr parse_comparison() {
        ExprPtr lhs = parse_additive();
        if (is_comp_op(peek().kind)) {
            BinOp op = comp_binop(peek().kind);
            int ln = advance().line;
            ExprPtr rhs = parse_additive();
            lhs = std::make_unique<BinaryExpr>(op, std::move(lhs),
                                               std::move(rhs), ln);
            // If ANOTHER comparison operator follows, that's chaining.
            if (is_comp_op(peek().kind)) {
                fail("comparison chaining is not allowed");
            }
        }
        return lhs;
    }

    // additive ::= mult ( ( "+" | "-" ) mult )*
    ExprPtr parse_additive() {
        ExprPtr lhs = parse_mult();
        while (check(Tok::Plus) || check(Tok::Minus)) {
            BinOp op;
            if (check(Tok::Plus)) {
                op = BinOp::Add;
            } else {
                op = BinOp::Sub;
            }
            int ln = advance().line;
            ExprPtr rhs = parse_mult();
            lhs = std::make_unique<BinaryExpr>(op, std::move(lhs),
                                               std::move(rhs), ln);
        }
        return lhs;
    }

    // mult ::= unary ( ( "*" | "//" | "%" ) unary )*
    ExprPtr parse_mult() {
        ExprPtr lhs = parse_unary();
        while (check(Tok::Star) || check(Tok::SlashSlash) || check(Tok::Percent)) {
            BinOp op;
            if (check(Tok::Star)) {
                op = BinOp::Mul;
            } else if (check(Tok::SlashSlash)) {
                op = BinOp::Div;
            } else {
                op = BinOp::Mod;
            }
            int ln = advance().line;
            ExprPtr rhs = parse_unary();
            lhs = std::make_unique<BinaryExpr>(op, std::move(lhs),
                                               std::move(rhs), ln);
        }
        return lhs;
    }

    // unary ::= "-" unary | primary
    // Same self-recursion trick as `not`, so `--x` parses as -(-x).
    ExprPtr parse_unary() {
        if (check(Tok::Minus)) {
            int ln = advance().line;
            ExprPtr operand = parse_unary();
            return std::make_unique<UnaryExpr>(UnOp::Neg, std::move(operand), ln);
        }
        return parse_primary();
    }

    // primary ::= INTEGER | "True" | "False" | IDENT | IDENT "(" args? ")" | "(" expr ")"
    ExprPtr parse_primary() {
        const Token& t = peek();
        switch (t.kind) {
            case Tok::Integer:
                advance();
                return std::make_unique<IntLit>(t.int_val, t.line);
            case Tok::True:
                // True and False are just the literals 1 and 0 — after this
                // point the compiler never knows booleans existed.
                advance();
                return std::make_unique<IntLit>(1, t.line);
            case Tok::False:
                advance();
                return std::make_unique<IntLit>(0, t.line);
            case Tok::LParen: {
                // Parenthesized expression: no AST node needed, the grouping
                // is already captured by the tree structure itself.
                advance();
                ExprPtr e = parse_expr();
                expect(Tok::RParen, "')'");
                return e;
            }
            case Tok::Ident: {
                advance();
                // IDENT followed by '(' is a call, otherwise a variable read.
                if (check(Tok::LParen)) {
                    auto call = std::make_unique<CallExpr>(t.text, t.line);
                    advance();   // the '('
                    // args ::= expr ( "," expr )*  — or empty
                    if (!check(Tok::RParen)) {
                        call->args.push_back(parse_expr());
                        while (check(Tok::Comma)) {
                            advance();
                            call->args.push_back(parse_expr());
                        }
                    }
                    expect(Tok::RParen, "')'");
                    return call;
                }
                return std::make_unique<NameExpr>(t.text, t.line);
            }
            default:
                fail("expected an expression");
        }
    }

    const std::vector<Token>& toks_;
    size_t pos_ = 0;
};

}  // namespace

Program parse(const std::vector<Token>& toks) {
    return Parser(toks).parse_program();
}

}  // namespace minipy
