// sema.cc — the semantic analysis passes.
#include "sema.h"

#include <string>

#include "error.h"

namespace minipy {

namespace {

// The MVP keeps every argument in a register, and AAPCS64 gives us x0..x7 for
// that, so eight parameters is the hard ceiling.  Past that the caller would
// have to start pushing args onto the stack, which is a whole extra calling
// convention we deliberately don't implement yet.
constexpr int kMaxParams = 8;

// Slot i will live at x29 - 8*(i+1), and negative frame offsets have to be
// reached with LDUR/STUR, whose immediate is a signed 9-bit byte offset
// (-256..255).  That puts the last reachable slot somewhere around 31; cap at
// 30 so we're comfortably inside the range and the error is a clean compile
// error instead of an encoding failure deep in the emitter.
constexpr int kMaxSlots = 30;

}  // namespace

// ---------------------------------------------------------------------------
// Pass 1 — signatures
// ---------------------------------------------------------------------------
// Nothing here looks inside a body.  That's the point: after this pass every
// function in the file is known, so a call in ANY body can be checked against
// a signature no matter where in the file the callee was defined.
void Sema::collect_signatures() {
    for (const auto& fn : prog_.funcs) {
        // `print` is a builtin the JIT lowers into a call to the C runtime,
        // not a name in any table — so a user function called `print` would
        // have no way to be reached.  Reject it instead of silently losing it.
        if (fn->name == "print") {
            throw CompileError(fn->lineno, "cannot redefine builtin 'print'");
        }

        if (fn->params.size() > (size_t)kMaxParams) {
            throw CompileError(fn->lineno,
                "function '" + fn->name + "' has more than " +
                std::to_string(kMaxParams) + " parameters (MVP limit)");
        }

        // Second definition of a name already in the table.  Python would just
        // let the later def win; we error, because there is no dynamic
        // rebinding here and a duplicate is far more likely to be a mistake.
        if (sigs_.count(fn->name) > 0) {
            throw CompileError(fn->lineno,
                "redefinition of function '" + fn->name + "'");
        }

        sigs_[fn->name] = FuncSig{(int)fn->params.size(), fn.get()};
    }
}

// ---------------------------------------------------------------------------
// Pass 2a — collect assigned names
// ---------------------------------------------------------------------------
// This walks the WHOLE body, including into if branches and while bodies,
// because a name assigned inside a branch is still a local of the function:
// there is no block scoping, and the local is zero-initialized whether or not
// that branch ever runs.  So `x` in
//
//     if c:
//         x = 1
//     print(x)
//
// is legal and prints 0 when c is false.  That is exactly why this has to be
// a separate pre-pass: the read of `x` is checked in 2b, by which point the
// assignment further up (or further DOWN) the body must already be known.
void Sema::collect_assigns_stmt(const Stmt* s) {
    switch (s->kind) {
        case StmtKind::Assign: {
            auto* a = static_cast<const AssignStmt*>(s);
            // `print = 5` would make the builtin unreachable for the rest of
            // the function, and there's no dynamic lookup to make that mean
            // anything, so it's an error rather than a shadow.
            if (a->name == "print") {
                throw CompileError(a->lineno, "cannot shadow builtin 'print'");
            }
            // First assignment wins the slot; later ones reuse it.
            if (slots_.count(a->name) == 0) {
                slots_[a->name] = next_slot_;
                next_slot_++;
            }
            break;
        }
        case StmtKind::If: {
            auto* f = static_cast<const IfStmt*>(s);
            for (const auto& br : f->branches) {
                collect_assigns_block(br.second);
            }
            // Empty else_body means there was no `else` at all — blocks are
            // never empty, the grammar requires at least one statement.
            if (!f->else_body.empty()) {
                collect_assigns_block(f->else_body);
            }
            break;
        }
        case StmtKind::While: {
            auto* w = static_cast<const WhileStmt*>(s);
            collect_assigns_block(w->body);
            break;
        }
        case StmtKind::Return:
        case StmtKind::ExprStmt:
            break;   // neither one can bind a name
    }
}

void Sema::collect_assigns_block(const Block& body) {
    for (const auto& s : body) {
        collect_assigns_stmt(s.get());
    }
}

// ---------------------------------------------------------------------------
// Pass 2b — resolve reads and calls
// ---------------------------------------------------------------------------
void Sema::check_expr(Expr* e) {
    switch (e->kind) {
        case ExprKind::Int:
            break;   // a literal names nothing

        case ExprKind::Name: {
            auto* n = static_cast<NameExpr*>(e);
            auto it = slots_.find(n->name);
            // Not a parameter and never assigned anywhere in this function,
            // so there is no slot to read from — it can't just default to 0
            // the way an assigned-but-not-yet-reached name does.
            if (it == slots_.end()) {
                throw CompileError(n->lineno,
                    "name '" + n->name + "' is not defined in '" +
                    current_fn_ + "'");
            }
            n->slot = it->second;
            break;
        }

        case ExprKind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            if (c->callee == "print") {
                // print isn't in sigs_ (it's a builtin), so its arity check
                // is hand-written here.
                if (c->args.size() != 1) {
                    throw CompileError(c->lineno,
                        "print() takes exactly one argument");
                }
            } else {
                auto sig = sigs_.find(c->callee);
                if (sig == sigs_.end()) {
                    throw CompileError(c->lineno,
                        "call to undefined function '" + c->callee + "'");
                }
                if ((int)c->args.size() != sig->second.nparams) {
                    throw CompileError(c->lineno,
                        "'" + c->callee + "' expects " +
                        std::to_string(sig->second.nparams) +
                        " argument(s), got " +
                        std::to_string(c->args.size()));
                }
            }
            // The argument expressions are evaluated in the CALLER's frame,
            // so they resolve against the scope we're already in.
            for (auto& a : c->args) {
                check_expr(a.get());
            }
            break;
        }

        case ExprKind::Unary: {
            auto* u = static_cast<UnaryExpr*>(e);
            check_expr(u->operand.get());
            break;
        }

        case ExprKind::Binary: {
            auto* b = static_cast<BinaryExpr*>(e);
            check_expr(b->lhs.get());
            check_expr(b->rhs.get());
            break;
        }
    }
}

void Sema::check_stmt(Stmt* s) {
    switch (s->kind) {
        case StmtKind::Assign: {
            auto* a = static_cast<AssignStmt*>(s);
            // Pass 2a already gave this name a slot (and already rejected
            // `print`), so the lookup can't fail — .at() says so out loud.
            a->slot = slots_.at(a->name);
            check_expr(a->value.get());
            break;
        }
        case StmtKind::Return: {
            auto* r = static_cast<ReturnStmt*>(s);
            if (r->value) {
                check_expr(r->value.get());   // bare `return` has nothing to check
            }
            break;
        }
        case StmtKind::ExprStmt: {
            auto* es = static_cast<ExprStmt*>(s);
            check_expr(es->expr.get());
            break;
        }
        case StmtKind::If: {
            auto* f = static_cast<IfStmt*>(s);
            for (auto& br : f->branches) {
                check_expr(br.first.get());
                check_block(br.second);
            }
            if (!f->else_body.empty()) {
                check_block(f->else_body);
            }
            break;
        }
        case StmtKind::While: {
            auto* w = static_cast<WhileStmt*>(s);
            check_expr(w->cond.get());
            check_block(w->body);
            break;
        }
    }
}

void Sema::check_block(const Block& body) {
    for (const auto& s : body) {
        check_stmt(s.get());
    }
}

// ---------------------------------------------------------------------------
// Pass 2 — one function
// ---------------------------------------------------------------------------
void Sema::check_function(FuncDef& fn) {
    current_fn_ = fn.name;
    slots_.clear();
    next_slot_ = 0;

    // Parameters take slots 0..nparams-1, in declaration order, because the
    // JIT prologue spills x0..x7 straight into those slots.  After that a
    // parameter is just another local and every lookup goes through the same
    // map — no separate "is this a formal?" case the way PA4's
    // CgenVarLocation needed.
    for (const auto& p : fn.params) {
        if (p == "print") {
            throw CompileError(fn.lineno, "cannot shadow builtin 'print'");
        }
        if (slots_.count(p) > 0) {
            throw CompileError(fn.lineno,
                "duplicate parameter '" + p + "' in '" + fn.name + "'");
        }
        slots_[p] = next_slot_;
        next_slot_++;
    }

    collect_assigns_block(fn.body);

    if (next_slot_ > kMaxSlots) {
        throw CompileError(fn.lineno,
            "function '" + fn.name + "' needs more than " +
            std::to_string(kMaxSlots) + " frame slots (MVP limit)");
    }
    // Codegen reads this to size the frame: nslots * 8, rounded up to 16.
    fn.nslots = next_slot_;

    check_block(fn.body);
}

void Sema::run() {
    collect_signatures();
    for (auto& fn : prog_.funcs) {
        check_function(*fn);
    }
}

}  // namespace minipy
