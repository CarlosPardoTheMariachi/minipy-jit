// ast.h — abstract syntax tree for minipy-jit.
//
// One node kind per grammar construct (same shape as the Cool compiler's
// cool-tree.h).  Nodes carry an enum `kind` and each pass (interp, sema,
// codegen) switches on it, instead of one virtual method per pass.
//
// Every Expr carries a `type` field even though the MVP is int64-only —
// deliberate habit so the float upgrade later is a diff, not a rewrite.
#ifndef MINIPY_AST_H
#define MINIPY_AST_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace minipy {

// The one and only value type for the MVP.  Roadmap adds Bool/Float here.
enum class Type { Int };

enum class UnOp { Neg, Not };

enum class BinOp {
    Add, Sub, Mul, Div, Mod,          // arithmetic (Div is //, Mod is %)
    Eq, Ne, Lt, Le, Gt, Ge,           // comparison
    And, Or                           // short-circuiting logical
};

const char* to_string(UnOp op);
const char* to_string(BinOp op);

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------
enum class ExprKind { Int, Name, Call, Unary, Binary };

struct Expr {
    ExprKind kind;
    int lineno;
    Type type = Type::Int;   // annotate from day one (see header comment)

    Expr(ExprKind k, int ln) : kind(k), lineno(ln) {}
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLit : Expr {
    int64_t value;
    IntLit(int64_t v, int ln) : Expr(ExprKind::Int, ln), value(v) {}
};

struct NameExpr : Expr {
    std::string name;
    int slot = -1;   // frame slot, filled in later by sema
    NameExpr(std::string n, int ln)
        : Expr(ExprKind::Name, ln), name(std::move(n)) {}
};

struct CallExpr : Expr {
    std::string callee;
    std::vector<ExprPtr> args;
    CallExpr(std::string n, int ln)
        : Expr(ExprKind::Call, ln), callee(std::move(n)) {}
};

struct UnaryExpr : Expr {
    UnOp op;
    ExprPtr operand;
    UnaryExpr(UnOp o, ExprPtr e, int ln)
        : Expr(ExprKind::Unary, ln), op(o), operand(std::move(e)) {}
};

struct BinaryExpr : Expr {
    BinOp op;
    ExprPtr lhs, rhs;
    BinaryExpr(BinOp o, ExprPtr l, ExprPtr r, int ln)
        : Expr(ExprKind::Binary, ln), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------
enum class StmtKind { Assign, Return, ExprStmt, If, While };

struct Stmt {
    StmtKind kind;
    int lineno;
    Stmt(StmtKind k, int ln) : kind(k), lineno(ln) {}
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;
using Block = std::vector<StmtPtr>;

struct AssignStmt : Stmt {
    std::string name;
    int slot = -1;   // filled in later by sema
    ExprPtr value;
    AssignStmt(std::string n, ExprPtr v, int ln)
        : Stmt(StmtKind::Assign, ln), name(std::move(n)), value(std::move(v)) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;   // may be null: `return` with no expression
    ReturnStmt(ExprPtr v, int ln)
        : Stmt(StmtKind::Return, ln), value(std::move(v)) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(ExprPtr e, int ln)
        : Stmt(StmtKind::ExprStmt, ln), expr(std::move(e)) {}
};

// if/elif/elif/else — a list of (condition, body) branches plus an optional
// else body at the end.
struct IfStmt : Stmt {
    std::vector<std::pair<ExprPtr, Block>> branches;
    Block else_body;
    bool has_else = false;
    explicit IfStmt(int ln) : Stmt(StmtKind::If, ln) {}
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    Block body;
    explicit WhileStmt(int ln) : Stmt(StmtKind::While, ln) {}
};

// ---------------------------------------------------------------------------
// Functions and program
// ---------------------------------------------------------------------------
struct FuncDef {
    std::string name;
    std::vector<std::string> params;
    Block body;
    int lineno;
    int nslots = 0;   // total frame slots (params + locals); filled by sema
    FuncDef(std::string n, int ln) : name(std::move(n)), lineno(ln) {}
};

// The synthesized entry point that wraps the top-level statements.
constexpr const char* kMainFunc = "__main__";

struct Program {
    std::vector<std::unique_ptr<FuncDef>> funcs;   // includes __main__ last
};

// Pretty-printer for --dump-ast (defined in ast.cc).
void dump_ast(const Program& prog, std::ostream& os);

}  // namespace minipy

#endif  // MINIPY_AST_H
