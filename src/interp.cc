// interp.cc — tree-walking interpreter, the differential-testing oracle.
//
// Everything observable here is the contract the JIT must reproduce byte for
// byte later: int64 wraparound arithmetic (computed in uint64_t so we never
// touch C++ signed-overflow UB), // and % truncating toward zero (C/SDIV
// semantics, NOT Python floor division), and the exact division-by-zero
// message + exit(1).
//
// NOTE: this file assumes sema's invariants (callee exists, arity matches,
// print has exactly one arg) — the driver runs sema before either engine, so
// an invalid program never reaches here.  Slots are ignored on purpose: the
// interpreter keeps its string-keyed frame, and the slots are for the JIT.
#include "interp.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "tiering.h"

namespace minipy {

namespace {

// Same observable behavior the JIT runtime's mp_div_zero_abort will have.
// The fflush matters: stdout printed so far must actually come out before
// we die, or the interpreter and JIT could diverge on buffered output.
[[noreturn]] void div_zero_abort() {
    std::fflush(stdout);
    std::fprintf(stderr, "runtime error: division by zero\n");
    std::exit(1);
}

// ---- wraparound arithmetic --------------------------------------------------
// Signed overflow is UB in C++, but UNSIGNED arithmetic is defined to wrap
// mod 2^64.  So: convert to uint64_t, do the op, convert back.  The bit
// pattern is exactly what the AArch64 ADD/SUB/MUL instructions produce.
int64_t wrap_add(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }
int64_t wrap_sub(int64_t a, int64_t b) { return (int64_t)((uint64_t)a - (uint64_t)b); }
int64_t wrap_mul(int64_t a, int64_t b) { return (int64_t)((uint64_t)a * (uint64_t)b); }
int64_t wrap_neg(int64_t a) {
    if (a == INT64_MIN) {
        return INT64_MIN;   // -INT64_MIN doesn't fit; two's-complement wraps
    }
    return -a;
}

// C's / already truncates toward zero, which is exactly SDIV's behavior.
// The two special cases are the divide instructions' edge semantics that
// C++ leaves undefined: they must be checked BEFORE using / and %.
int64_t trunc_div(int64_t a, int64_t b) {
    if (b == 0) {
        div_zero_abort();
    }
    if (a == INT64_MIN && b == -1) {
        return INT64_MIN;   // SDIV wraps here natively; C++ calls it UB
    }
    return a / b;
}
int64_t trunc_mod(int64_t a, int64_t b) {
    if (b == 0) {
        div_zero_abort();
    }
    if (a == INT64_MIN && b == -1) {
        return 0;   // keeps the invariant a == (a//b)*b + a%b
    }
    return a % b;
}

// A function's local variables during one call.  Just name -> value; a name
// that was never assigned simply isn't in the map, and reading it gives 0
// (the zero-init rule).  The JIT does this differently (fixed frame slots),
// which is fine — only observable behavior has to match.
using Frame = std::unordered_map<std::string, int64_t>;

// How a statement finished: fell through normally, or hit a `return` that
// must propagate up out of every enclosing if/while to end the call.
enum class Flow { Normal, Return };

class Interpreter {
public:
    // tiers may be null, which is the pure-interpreter case and the one the
    // differential tests treat as the oracle.  When it isn't null this is still
    // the same interpreter — the only difference is that some calls stop
    // walking the tree and jump into machine code instead.
    Interpreter(const Program& prog, TierManager* tiers) : tiers_(tiers) {
        for (const auto& fn : prog.funcs) {
            funcs_[fn->name] = fn.get();
        }
    }

    int run() {
        auto it = funcs_.find(kMainFunc);
        if (it == funcs_.end()) {
            return 0;   // empty program, nothing to do
        }
        std::vector<int64_t> no_args;
        call(it->second, no_args);
        return 0;
    }

private:
    int64_t call(const FuncDef* fn, const std::vector<int64_t>& args) {
        // Mixed mode: ask whether this function has gone hot.  If it has, the
        // whole call runs as machine code and nothing below this executes —
        // including, crucially, any recursion inside it, which stays entirely
        // in the compiled buffer instead of coming back through here.
        //
        // Note where this sits: a call already in progress cannot be promoted,
        // because by the time control is inside the loop below there is an
        // interpreter frame that would have to be rebuilt as a machine frame.
        // Only the *next* call gets the compiled version.
        if (tiers_ != nullptr) {
            void* entry = tiers_->on_call(fn);
            if (entry != nullptr) {
                return TierManager::invoke(entry, args);
            }
        }

        // Fresh frame per call — this is what makes recursion work: each
        // recursive call gets its own copy of the locals.
        Frame frame;
        for (size_t i = 0; i < fn->params.size(); i++) {
            frame[fn->params[i]] = args[i];
        }
        int64_t ret = 0;   // falling off the end of a function returns 0
        exec_block(fn->body, frame, ret);
        return ret;
    }

    Flow exec_block(const Block& body, Frame& f, int64_t& ret) {
        for (const auto& s : body) {
            if (exec(s.get(), f, ret) == Flow::Return) {
                return Flow::Return;   // stop executing, keep propagating up
            }
        }
        return Flow::Normal;
    }

    Flow exec(const Stmt* s, Frame& f, int64_t& ret) {
        switch (s->kind) {
            case StmtKind::Assign: {
                auto* a = static_cast<const AssignStmt*>(s);
                f[a->name] = eval(a->value.get(), f);
                return Flow::Normal;
            }
            case StmtKind::Return: {
                auto* r = static_cast<const ReturnStmt*>(s);
                if (r->value) {
                    ret = eval(r->value.get(), f);
                } else {
                    ret = 0;   // bare `return`
                }
                return Flow::Return;
            }
            case StmtKind::ExprStmt: {
                auto* es = static_cast<const ExprStmt*>(s);
                // Evaluate for side effects (print), throw the value away.
                eval(es->expr.get(), f);
                return Flow::Normal;
            }
            case StmtKind::If: {
                auto* fs = static_cast<const IfStmt*>(s);
                // Walk the if/elif conditions in order; first true one wins
                // and NO later condition even gets evaluated.
                for (const auto& br : fs->branches) {
                    if (eval(br.first.get(), f) != 0) {
                        return exec_block(br.second, f, ret);
                    }
                }
                if (!fs->else_body.empty()) {
                    return exec_block(fs->else_body, f, ret);
                }
                return Flow::Normal;
            }
            case StmtKind::While: {
                auto* w = static_cast<const WhileStmt*>(s);
                while (eval(w->cond.get(), f) != 0) {
                    if (exec_block(w->body, f, ret) == Flow::Return) {
                        return Flow::Return;
                    }
                }
                return Flow::Normal;
            }
        }
        return Flow::Normal;
    }

    int64_t eval(const Expr* e, Frame& f) {
        switch (e->kind) {
            case ExprKind::Int:
                return static_cast<const IntLit*>(e)->value;
            case ExprKind::Name: {
                auto* n = static_cast<const NameExpr*>(e);
                auto it = f.find(n->name);
                if (it == f.end()) {
                    return 0;   // never assigned yet -> zero-init rule
                }
                return it->second;
            }
            case ExprKind::Call:
                return eval_call(static_cast<const CallExpr*>(e), f);
            case ExprKind::Unary: {
                auto* u = static_cast<const UnaryExpr*>(e);
                int64_t v = eval(u->operand.get(), f);
                if (u->op == UnOp::Neg) {
                    return wrap_neg(v);
                }
                // not: 1 if zero, 0 if nonzero
                if (v == 0) {
                    return 1;
                }
                return 0;
            }
            case ExprKind::Binary:
                return eval_binary(static_cast<const BinaryExpr*>(e), f);
        }
        return 0;
    }

    int64_t eval_call(const CallExpr* c, Frame& f) {
        // print is a builtin, not a user function: evaluate the one arg,
        // write it out, and the call's value is 0.
        if (c->callee == "print") {
            int64_t v = eval(c->args[0].get(), f);
            std::printf("%lld\n", (long long)v);
            return 0;
        }
        const FuncDef* fn = funcs_.at(c->callee);   // sema guarantees it exists
        // Evaluate ALL the args in the caller's frame first, left to right,
        // THEN make the call — arg expressions must not see callee locals.
        std::vector<int64_t> args;
        args.reserve(c->args.size());
        for (const auto& a : c->args) {
            args.push_back(eval(a.get(), f));
        }
        return call(fn, args);
    }

    int64_t eval_binary(const BinaryExpr* b, Frame& f) {
        // and/or short-circuit, so they CANNOT evaluate both sides up front
        // like the others — the whole point is that the rhs may never run.
        // The result is the deciding operand's value, not a canned 1/0.
        if (b->op == BinOp::Or) {
            int64_t a = eval(b->lhs.get(), f);
            if (a != 0) {
                return a;   // lhs decided it; rhs never evaluated
            }
            return eval(b->rhs.get(), f);
        }
        if (b->op == BinOp::And) {
            int64_t a = eval(b->lhs.get(), f);
            if (a == 0) {
                return a;   // lhs decided it; rhs never evaluated
            }
            return eval(b->rhs.get(), f);
        }
        // Everything else is strict: evaluate both sides, then combine.
        int64_t l = eval(b->lhs.get(), f);
        int64_t r = eval(b->rhs.get(), f);
        switch (b->op) {
            case BinOp::Add: return wrap_add(l, r);
            case BinOp::Sub: return wrap_sub(l, r);
            case BinOp::Mul: return wrap_mul(l, r);
            case BinOp::Div: return trunc_div(l, r);
            case BinOp::Mod: return trunc_mod(l, r);
            case BinOp::Eq:  return l == r;    // comparisons yield 1 or 0
            case BinOp::Ne:  return l != r;
            case BinOp::Lt:  return l <  r;
            case BinOp::Le:  return l <= r;
            case BinOp::Gt:  return l >  r;
            case BinOp::Ge:  return l >= r;
            default:         return 0;   // And/Or handled above
        }
    }

    std::unordered_map<std::string, const FuncDef*> funcs_;
    TierManager* tiers_;
};

}  // namespace

int run_interpreter(const Program& prog, bool time_it) {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    int rc = Interpreter(prog, nullptr).run();
    auto t1 = clock::now();
    if (time_it) {
        std::fprintf(stderr, "interp run_ms=%.3f\n",
                     std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return rc;
}

int run_tiered(const Program& prog, int threshold, bool time_it) {
    using clock = std::chrono::steady_clock;
    TierManager tiers(prog, threshold);

    auto t0 = clock::now();
    int rc = Interpreter(prog, &tiers).run();
    auto t1 = clock::now();

    if (time_it) {
        // compile_ms is reported separately but is NOT disjoint from run_ms
        // the way it is under --jit: compilation happens partway through the
        // run, so its cost is already inside the total.  Printed anyway
        // because the interesting question about tiering is how small it is.
        std::fprintf(stderr, "tier compile_ms=%.3f\n", tiers.compile_ms());
        std::fprintf(stderr, "tier run_ms=%.3f\n",
                     std::chrono::duration<double, std::milli>(t1 - t0).count());
        std::fprintf(stderr, "tier compiled_funcs=%d\n", tiers.compiled_count());
    }
    return rc;
}

}  // namespace minipy
