// ast.cc — operator name tables and the --dump-ast pretty-printer.
#include "ast.h"

#include <ostream>

namespace minipy {

const char* to_string(UnOp op) {
    switch (op) {
        case UnOp::Neg: return "-";
        case UnOp::Not: return "not";
    }
    return "?";
}

const char* to_string(BinOp op) {
    switch (op) {
        case BinOp::Add: return "+";
        case BinOp::Sub: return "-";
        case BinOp::Mul: return "*";
        case BinOp::Div: return "//";
        case BinOp::Mod: return "%";
        case BinOp::Eq:  return "==";
        case BinOp::Ne:  return "!=";
        case BinOp::Lt:  return "<";
        case BinOp::Le:  return "<=";
        case BinOp::Gt:  return ">";
        case BinOp::Ge:  return ">=";
        case BinOp::And: return "and";
        case BinOp::Or:  return "or";
    }
    return "?";
}

namespace {

// Two spaces per nesting level, like Python source itself.
void pad(std::ostream& os, int indent) {
    for (int i = 0; i < indent; i++) {
        os << "  ";
    }
}

void dump_expr(const Expr* e, std::ostream& os) {
    switch (e->kind) {
        case ExprKind::Int: {
            auto* lit = static_cast<const IntLit*>(e);
            os << lit->value;
            break;
        }
        case ExprKind::Name: {
            auto* n = static_cast<const NameExpr*>(e);
            os << n->name;
            // Once sema has run, show which frame slot this name resolved to.
            if (n->slot >= 0) {
                os << "@" << n->slot;
            }
            break;
        }
        case ExprKind::Call: {
            auto* c = static_cast<const CallExpr*>(e);
            os << c->callee << "(";
            for (size_t i = 0; i < c->args.size(); i++) {
                if (i > 0) {
                    os << ", ";
                }
                dump_expr(c->args[i].get(), os);
            }
            os << ")";
            break;
        }
        case ExprKind::Unary: {
            auto* u = static_cast<const UnaryExpr*>(e);
            // Fully parenthesized so the dump shows exactly how precedence
            // grouped things — that's the whole point of dumping.
            os << "(" << to_string(u->op) << " ";
            dump_expr(u->operand.get(), os);
            os << ")";
            break;
        }
        case ExprKind::Binary: {
            auto* b = static_cast<const BinaryExpr*>(e);
            os << "(";
            dump_expr(b->lhs.get(), os);
            os << " " << to_string(b->op) << " ";
            dump_expr(b->rhs.get(), os);
            os << ")";
            break;
        }
    }
}

void dump_block(const Block& body, std::ostream& os, int indent);

void dump_stmt(const Stmt* s, std::ostream& os, int indent) {
    pad(os, indent);
    switch (s->kind) {
        case StmtKind::Assign: {
            auto* a = static_cast<const AssignStmt*>(s);
            os << a->name;
            if (a->slot >= 0) {
                os << "@" << a->slot;
            }
            os << " = ";
            dump_expr(a->value.get(), os);
            os << "\n";
            break;
        }
        case StmtKind::Return: {
            auto* r = static_cast<const ReturnStmt*>(s);
            os << "return";
            if (r->value) {
                os << " ";
                dump_expr(r->value.get(), os);
            }
            os << "\n";
            break;
        }
        case StmtKind::ExprStmt: {
            auto* es = static_cast<const ExprStmt*>(s);
            dump_expr(es->expr.get(), os);
            os << "\n";
            break;
        }
        case StmtKind::If: {
            auto* f = static_cast<const IfStmt*>(s);
            // First branch prints as "if", every later one as "elif".
            for (size_t i = 0; i < f->branches.size(); i++) {
                if (i > 0) {
                    pad(os, indent);
                }
                os << (i == 0 ? "if " : "elif ");
                dump_expr(f->branches[i].first.get(), os);
                os << ":\n";
                dump_block(f->branches[i].second, os, indent + 1);
            }
            if (!f->else_body.empty()) {
                pad(os, indent);
                os << "else:\n";
                dump_block(f->else_body, os, indent + 1);
            }
            break;
        }
        case StmtKind::While: {
            auto* w = static_cast<const WhileStmt*>(s);
            os << "while ";
            dump_expr(w->cond.get(), os);
            os << ":\n";
            dump_block(w->body, os, indent + 1);
            break;
        }
    }
}

void dump_block(const Block& body, std::ostream& os, int indent) {
    for (const auto& s : body) {
        dump_stmt(s.get(), os, indent);
    }
}

}  // namespace

void dump_ast(const Program& prog, std::ostream& os) {
    for (const auto& fn : prog.funcs) {
        os << "def " << fn->name << "(";
        for (size_t i = 0; i < fn->params.size(); i++) {
            if (i > 0) {
                os << ", ";
            }
            os << fn->params[i];
        }
        os << "):";
        // Once sema has run, show how big this function's frame is.
        if (fn->nslots > 0) {
            os << "   ; " << fn->nslots << " slots";
        }
        os << "\n";
        dump_block(fn->body, os, 1);
        os << "\n";
    }
}

}  // namespace minipy
