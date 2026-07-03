// tiering.cc — call counting, hot-set discovery, and the handoff into
// compiled code.  Read the header first for the policy and why it is cheap
// here specifically.
#include "tiering.h"

#include <chrono>

#include "codegen.h"

namespace minipy {

TierManager::TierManager(const Program& prog, int threshold)
    : threshold_(threshold) {
    for (const auto& fn : prog.funcs) {
        by_name_[fn->name] = fn.get();
    }
}

// ---------------------------------------------------------------------------
// Finding the set to compile
// ---------------------------------------------------------------------------

void TierManager::collect_calls_expr(const Expr* e, std::vector<std::string>& out) {
    switch (e->kind) {
        case ExprKind::Int:
        case ExprKind::Name:
            return;
        case ExprKind::Unary: {
            auto* u = static_cast<const UnaryExpr*>(e);
            collect_calls_expr(u->operand.get(), out);
            return;
        }
        case ExprKind::Binary: {
            auto* b = static_cast<const BinaryExpr*>(e);
            collect_calls_expr(b->lhs.get(), out);
            collect_calls_expr(b->rhs.get(), out);
            return;
        }
        case ExprKind::Call: {
            auto* c = static_cast<const CallExpr*>(e);
            // print is a builtin living in the host binary, reached by absolute
            // address rather than by BL, so it is not part of any hot set.
            if (c->callee != "print") {
                out.push_back(c->callee);
            }
            // The arguments still have to be walked — a call can hide inside
            // one, and missing it would leave the set not closed.
            for (const auto& arg : c->args) {
                collect_calls_expr(arg.get(), out);
            }
            return;
        }
    }
}

void TierManager::collect_calls_stmt(const Stmt* s, std::vector<std::string>& out) {
    switch (s->kind) {
        case StmtKind::Assign: {
            auto* a = static_cast<const AssignStmt*>(s);
            collect_calls_expr(a->value.get(), out);
            return;
        }
        case StmtKind::Return: {
            auto* r = static_cast<const ReturnStmt*>(s);
            if (r->value) {
                collect_calls_expr(r->value.get(), out);
            }
            return;
        }
        case StmtKind::ExprStmt: {
            auto* es = static_cast<const ExprStmt*>(s);
            collect_calls_expr(es->expr.get(), out);
            return;
        }
        case StmtKind::If: {
            auto* f = static_cast<const IfStmt*>(s);
            for (const auto& branch : f->branches) {
                collect_calls_expr(branch.first.get(), out);
                collect_calls_block(branch.second, out);
            }
            collect_calls_block(f->else_body, out);
            return;
        }
        case StmtKind::While: {
            auto* w = static_cast<const WhileStmt*>(s);
            collect_calls_expr(w->cond.get(), out);
            collect_calls_block(w->body, out);
            return;
        }
    }
}

void TierManager::collect_calls_block(const Block& body, std::vector<std::string>& out) {
    for (const auto& s : body) {
        collect_calls_stmt(s.get(), out);
    }
}

std::vector<const FuncDef*> TierManager::reachable_from(const FuncDef* root) {
    // An ordinary worklist over the call graph.  Every name that turns up is a
    // real function — sema already rejected calls to anything undefined — so
    // there is no missing-callee case to handle.
    std::vector<const FuncDef*> found;
    std::unordered_set<const FuncDef*> seen;
    std::vector<const FuncDef*> work;

    work.push_back(root);
    seen.insert(root);

    while (!work.empty()) {
        const FuncDef* fn = work.back();
        work.pop_back();
        found.push_back(fn);

        std::vector<std::string> callees;
        collect_calls_block(fn->body, callees);

        for (const std::string& name : callees) {
            const FuncDef* callee = by_name_.at(name);
            if (seen.count(callee) == 0) {
                seen.insert(callee);
                work.push_back(callee);
            }
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// Compiling and entering
// ---------------------------------------------------------------------------

void TierManager::compile_hot(const FuncDef* fn) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();

    std::vector<const FuncDef*> set = reachable_from(fn);
    CompiledCode code = compile_functions(set);

    auto region = std::make_unique<JitMemory>(code.bytes);
    uint8_t* base = static_cast<uint8_t*>(region->base());

    // Every function in the region becomes callable, not just the one that
    // tripped the counter.  They are all in the buffer either way — the hot one
    // needs them to exist so its BLs have somewhere to land — so leaving them
    // interpreted would mean compiling code and then declining to use it.
    for (const FuncDef* member : set) {
        compiled_[member] = base + code.offsets.at(member->name);
    }

    // Held for the life of the run; see the comment on regions_.
    regions_.push_back(std::move(region));

    auto end = clock::now();
    compile_ms_ += std::chrono::duration<double, std::milli>(end - start).count();
}

void* TierManager::on_call(const FuncDef* fn) {
    // Already compiled: the common case once a program has warmed up, and it
    // has to stay a single hash lookup because this runs on every call.
    auto it = compiled_.find(fn);
    if (it != compiled_.end()) {
        return it->second;
    }

    counts_[fn]++;
    if (counts_[fn] < threshold_) {
        return nullptr;   // not hot yet, keep interpreting
    }

    compile_hot(fn);
    return compiled_.at(fn);
}

int64_t TierManager::invoke(void* entry, const std::vector<int64_t>& args) {
    // One case per arity, spelled out, rather than one variadic cast.  That
    // shortcut would be wrong on this platform specifically: Apple passes
    // variadic arguments on the stack, while the compiled function reads its
    // arguments from x0..x7, so calling through an (int64_t, ...) pointer would
    // hand it registers it never looks at.  A fixed-arity function pointer is
    // the only cast that produces the calling convention the code was compiled
    // with.
    //
    // Sema caps parameters at 8, so this covers every function that can exist.
    switch (args.size()) {
        case 0:
            return reinterpret_cast<int64_t (*)()>(entry)();
        case 1:
            return reinterpret_cast<int64_t (*)(int64_t)>(entry)(args[0]);
        case 2:
            return reinterpret_cast<int64_t (*)(int64_t, int64_t)>(entry)(
                args[0], args[1]);
        case 3:
            return reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t)>(entry)(
                args[0], args[1], args[2]);
        case 4:
            return reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t,
                                                int64_t)>(entry)(
                args[0], args[1], args[2], args[3]);
        case 5:
            return reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t,
                                                int64_t, int64_t)>(entry)(
                args[0], args[1], args[2], args[3], args[4]);
        case 6:
            return reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t,
                                                int64_t, int64_t, int64_t)>(entry)(
                args[0], args[1], args[2], args[3], args[4], args[5]);
        case 7:
            return reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t,
                                                int64_t, int64_t, int64_t,
                                                int64_t)>(entry)(
                args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
        default:
            return reinterpret_cast<int64_t (*)(int64_t, int64_t, int64_t,
                                                int64_t, int64_t, int64_t,
                                                int64_t, int64_t)>(entry)(
                args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                args[7]);
    }
}

}  // namespace minipy
