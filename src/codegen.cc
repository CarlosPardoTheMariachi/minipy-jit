// codegen.cc — AST walk that emits AArch64.
//
// Read the header first for the one invariant everything here depends on:
// every expression leaves its value in x0 and leaves the stack as it found it.
//
// This file assumes sema already ran and its results are on the AST.  Slots are
// assigned, calls are known to exist with the right arity, and print is known
// not to have been shadowed — so there is no error handling here at all.  That
// is the payoff for having a separate sema pass: codegen gets to be a
// translation and nothing else.
#include "codegen.h"

#include <chrono>
#include <cstdio>
#include <ostream>
#include <string>
#include <unordered_map>

#include "jit_memory.h"
#include "runtime.h"

namespace minipy {

namespace {

// Slot i lives at x29 - 8*(i+1), which is the layout sema assigned and this
// file just consumes.  The +1 is because slot 0 sits one word BELOW the frame
// pointer, not at it: x29 itself points at the saved x29/x30 pair.
//
// These offsets are negative, which is exactly why every access below uses
// LDUR/STUR.  The scaled LDR/STR immediate form cannot represent a negative
// offset at all, and the failure is silent — -8 written into that unsigned
// field reads as +32760.
int slot_offset(int slot) {
    return -8 * (slot + 1);
}

// The stack pointer must be 16-byte aligned at every access made through it,
// enforced in hardware, so a frame of an odd number of 8-byte slots gets
// rounded up and wastes one.
int frame_size(int nslots) {
    int bytes = nslots * 8;
    return ((bytes + 15) / 16) * 16;
}

// Which condition code each comparison operator turns into.  These are the
// signed conditions; there are unsigned equivalents in the encoding, and
// picking one of those would silently break every negative number.
Cond cond_for(BinOp op) {
    switch (op) {
        case BinOp::Eq: return Cond::EQ;
        case BinOp::Ne: return Cond::NE;
        case BinOp::Lt: return Cond::LT;
        case BinOp::Le: return Cond::LE;
        case BinOp::Gt: return Cond::GT;
        case BinOp::Ge: return Cond::GE;
        default:        return Cond::EQ;   // not a comparison; never reached
    }
}

class Codegen {
public:
    explicit Codegen(const std::vector<const FuncDef*>& funcs) : funcs_(funcs) {}

    CompiledCode compile();

private:
    // --- functions ----------------------------------------------------------
    void gen_function(const FuncDef& fn);
    void gen_prologue(const FuncDef& fn);
    void gen_epilogue();

    // --- statements ---------------------------------------------------------
    void gen_block(const Block& body);
    void gen_stmt(const Stmt* s);
    void gen_if(const IfStmt* s);
    void gen_while(const WhileStmt* s);

    // --- expressions --------------------------------------------------------
    // Each of these leaves its result in x0 and restores the stack.
    void gen_expr(const Expr* e);
    void gen_unary(const UnaryExpr* e);
    void gen_binary(const BinaryExpr* e);
    void gen_short_circuit(const BinaryExpr* e);
    void gen_call(const CallExpr* e);

    // --- small helpers ------------------------------------------------------
    void push_x0();
    void pop_into(int r);
    void call_host(uint64_t address);
    Label div_zero_label();

    Emitter em_;

    // The functions to emit, which may be the whole program or just the part
    // of it that went hot.  Either way the set has to be closed under calling,
    // because a BL can only reach a label in this same buffer.
    const std::vector<const FuncDef*>& funcs_;

    // Every function's label, all created before any body is emitted.  This is
    // the two-pass shape CgenClassTable used for class layouts, and it is what
    // makes forward references and recursion work: a call emits a BL to a label
    // that may not be bound yet, and finalize() fills in the distance once
    // every function has a position.
    std::unordered_map<std::string, Label> func_labels_;

    // The epilogue of the function currently being compiled.  `return` is
    // compiled as a jump here rather than as its own copy of the epilogue, so
    // the frame teardown exists exactly once per function.
    Label return_label_ = -1;

    // Created on first use, so a program with no division doesn't carry a stub
    // it never reaches.  -1 means "not needed so far".
    Label div_zero_ = -1;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Push bumps the stack by 16 rather than 8, wasting half of every slot, purely
// to keep sp 16-byte aligned.  The alternative is pushing values in pairs,
// which means tracking how many are outstanding — real bookkeeping, in exchange
// for memory this program never runs out of.  Register allocation would delete
// these pushes entirely, which is the actual fix and a much bigger project.
void Codegen::push_x0() {
    em_.str_pre(reg::X0, reg::SP, -16);
}

void Codegen::pop_into(int r) {
    em_.ldr_post(r, reg::SP, 16);
}

// Call a function in the host binary.  BL carries a distance, and the distance
// from the JIT buffer to the host is not known when the instruction is emitted
// (the buffer has no address until it is mapped, after finalize), so the only
// available form is: materialize the absolute address, then BLR through it.
// x16 is the register used because AAPCS64 reserves it for exactly this sort
// of scratch use, so nothing else in the emitted code holds a live value there.
void Codegen::call_host(uint64_t address) {
    em_.mov_imm64(reg::X16, address);
    em_.blr(reg::X16);
}

Label Codegen::div_zero_label() {
    if (div_zero_ < 0) {
        div_zero_ = em_.new_label();
    }
    return div_zero_;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

void Codegen::gen_expr(const Expr* e) {
    switch (e->kind) {
        case ExprKind::Int: {
            auto* lit = static_cast<const IntLit*>(e);
            em_.mov_imm64(reg::X0, static_cast<uint64_t>(lit->value));
            return;
        }
        case ExprKind::Name: {
            // A variable reference is always a fresh load from its slot.  The
            // JIT never assumes a value is still in a register from earlier,
            // which is what makes it safe to compile expressions in isolation.
            auto* n = static_cast<const NameExpr*>(e);
            em_.ldur(reg::X0, reg::FP, slot_offset(n->slot));
            return;
        }
        case ExprKind::Unary:
            gen_unary(static_cast<const UnaryExpr*>(e));
            return;
        case ExprKind::Binary:
            gen_binary(static_cast<const BinaryExpr*>(e));
            return;
        case ExprKind::Call:
            gen_call(static_cast<const CallExpr*>(e));
            return;
    }
}

void Codegen::gen_unary(const UnaryExpr* e) {
    gen_expr(e->operand.get());

    if (e->op == UnOp::Neg) {
        // Wraps at INT64_MIN, which is the defined behavior here — the
        // interpreter special-cases that value to get the same answer without
        // stepping on C++'s signed-overflow rules.
        em_.neg(reg::X0, reg::X0);
        return;
    }

    // `not x` is 1 when x is zero and 0 otherwise.  There is no boolean-negate
    // instruction, so this goes through the flags: compare against zero, then
    // turn the EQ flag back into a value.
    em_.cmp(reg::X0, reg::XZR);
    em_.cset(reg::X0, Cond::EQ);
}

void Codegen::gen_binary(const BinaryExpr* e) {
    // and/or are not arithmetic at all — they choose whether to evaluate their
    // right operand — so they never reach the evaluate-both-sides code below.
    if (e->op == BinOp::And || e->op == BinOp::Or) {
        gen_short_circuit(e);
        return;
    }

    // The stack machine, in five steps.  The push is the load-bearing one: the
    // right operand can be arbitrarily complicated, up to and including a
    // recursive call that runs for a million instructions, and x0 will not
    // survive it.  Memory will.
    gen_expr(e->lhs.get());
    push_x0();
    gen_expr(e->rhs.get());
    pop_into(reg::X1);
    // Now: x1 = left operand, x0 = right operand.  Operand order matters for
    // everything except + and *, so keep x1 first in every instruction below.

    switch (e->op) {
        case BinOp::Add:
            em_.add_reg(reg::X0, reg::X1, reg::X0);
            return;
        case BinOp::Sub:
            em_.sub_reg(reg::X0, reg::X1, reg::X0);
            return;
        case BinOp::Mul:
            // Overflow wraps, which is what the language promises and what the
            // instruction does anyway — MUL keeps the low 64 bits of the
            // product and there is nothing extra to suppress.
            em_.mul(reg::X0, reg::X1, reg::X0);
            return;
        case BinOp::Div:
            // SDIV returns 0 for a zero divisor instead of trapping, so the
            // check has to be emitted.  The divisor is the right operand, and
            // it is sitting in x0 right now.
            em_.cbz(reg::X0, div_zero_label());
            em_.sdiv(reg::X0, reg::X1, reg::X0);
            return;
        case BinOp::Mod:
            // No remainder instruction exists: compute the quotient, then
            // a - quotient*b, which is exactly what MSUB does in one step.
            // x2 is scratch, and nothing is live in it between these two lines.
            em_.cbz(reg::X0, div_zero_label());
            em_.sdiv(reg::X2, reg::X1, reg::X0);
            em_.msub(reg::X0, reg::X2, reg::X0, reg::X1);
            return;
        default:
            // The six comparisons, which all share this shape and differ only
            // in the condition code.  CMP sets the flags, CSET reads them back
            // out as a 1 or 0, because a comparison is a value in this language
            // rather than something only an `if` can consume.
            em_.cmp(reg::X1, reg::X0);
            em_.cset(reg::X0, cond_for(e->op));
            return;
    }
}

void Codegen::gen_short_circuit(const BinaryExpr* e) {
    // `a or b` is a if a is nonzero, otherwise b — the deciding operand's
    // value, not a canned 1/0.  That falls out for free from the stack machine:
    // the left operand is already in x0, so branching around the right operand
    // leaves the right answer sitting there with no extra instruction.
    Label done = em_.new_label();

    gen_expr(e->lhs.get());
    if (e->op == BinOp::Or) {
        em_.cbnz(reg::X0, done);   // nonzero decides it; skip the right side
    } else {
        em_.cbz(reg::X0, done);    // zero decides it; skip the right side
    }
    gen_expr(e->rhs.get());

    em_.bind(done);
}

void Codegen::gen_call(const CallExpr* e) {
    // print is a builtin: one argument, and the call itself is a jump into the
    // host binary rather than into emitted code.
    if (e->callee == "print") {
        gen_expr(e->args[0].get());
        call_host(reinterpret_cast<uint64_t>(&mp_print));
        // mp_print returns 0 in x0, which is the value the language says a
        // print expression has, so there is nothing to fix up afterwards.
        return;
    }

    // Arguments go in x0..x7, but they cannot be evaluated straight into those
    // registers: evaluating the second argument would destroy the first, since
    // every expression lands in x0.  So evaluate them left to right onto the
    // stack — which also preserves the evaluation order the interpreter uses,
    // and that is observable whenever an argument calls print.
    for (size_t i = 0; i < e->args.size(); i++) {
        gen_expr(e->args[i].get());
        push_x0();
    }

    // Then unload them in reverse, because the stack gives back the most
    // recently pushed value first.  Argument n-1 is on top and belongs in
    // x(n-1), so counting down matches the two orders up automatically.
    for (size_t i = e->args.size(); i > 0; i--) {
        pop_into(static_cast<int>(i - 1));
    }

    // BL to a label rather than to an offset: the callee may not have been
    // emitted yet.  The return value arrives in x0 and is already where the
    // invariant wants it.
    em_.bl(func_labels_.at(e->callee));
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

void Codegen::gen_block(const Block& body) {
    for (const auto& s : body) {
        gen_stmt(s.get());
    }
}

void Codegen::gen_stmt(const Stmt* s) {
    switch (s->kind) {
        case StmtKind::Assign: {
            auto* a = static_cast<const AssignStmt*>(s);
            gen_expr(a->value.get());
            em_.stur(reg::X0, reg::FP, slot_offset(a->slot));
            return;
        }
        case StmtKind::Return: {
            auto* r = static_cast<const ReturnStmt*>(s);
            if (r->value) {
                gen_expr(r->value.get());
            } else {
                em_.movz(reg::X0, 0, 0);   // bare `return` yields 0
            }
            // Jump to the single epilogue instead of tearing the frame down
            // here.  A `return` can be nested inside any number of ifs and
            // whiles, and this is what lets it escape all of them at once.
            em_.b(return_label_);
            return;
        }
        case StmtKind::ExprStmt: {
            auto* es = static_cast<const ExprStmt*>(s);
            // Evaluated for its side effects; the value in x0 is simply
            // abandoned.  Nothing needs undoing because the stack invariant
            // means the expression already cleaned up after itself.
            gen_expr(es->expr.get());
            return;
        }
        case StmtKind::If:
            gen_if(static_cast<const IfStmt*>(s));
            return;
        case StmtKind::While:
            gen_while(static_cast<const WhileStmt*>(s));
            return;
    }
}

void Codegen::gen_if(const IfStmt* s) {
    // One label for the far end, shared by every branch, plus one per branch
    // for "this test failed, try the next one".  elif needs no special
    // handling: it is just the next test in the chain.
    Label end = em_.new_label();

    for (const auto& branch : s->branches) {
        Label next = em_.new_label();

        gen_expr(branch.first.get());
        em_.cbz(reg::X0, next);      // false (zero) means try the next test

        gen_block(branch.second);
        em_.b(end);                  // a taken branch skips everything after it

        em_.bind(next);
    }

    // Falls here when every test was false.  An absent else is an empty block,
    // so this needs no condition around it.
    gen_block(s->else_body);

    em_.bind(end);
}

void Codegen::gen_while(const WhileStmt* s) {
    // Test at the top, jump back at the bottom.  The condition is re-emitted
    // nowhere — the backward branch returns to it, which is the whole reason
    // the top label is bound before the condition rather than after it.
    Label top = em_.new_label();
    Label end = em_.new_label();

    em_.bind(top);
    gen_expr(s->cond.get());
    em_.cbz(reg::X0, end);

    gen_block(s->body);
    em_.b(top);

    em_.bind(end);
}

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

void Codegen::gen_prologue(const FuncDef& fn) {
    // Save the caller's frame pointer and our return address as a pair, in one
    // instruction that also moves sp down over them.  Order matters beyond
    // correctness here: x29 at the lower address and x30 above it is the
    // convention every debugger walks to produce a backtrace, and getting it
    // backwards produces code that runs fine and cannot be debugged.
    em_.stp_pre(reg::FP, reg::LR, reg::SP, -16);

    // This frame's base.  From here on x29 does not move, which is why every
    // local is addressed relative to it: sp keeps moving as expression temps
    // are pushed and popped, so offsets from sp would be a moving target.
    // Spelled as ADD #0 because MOV Xd,Xm cannot name the stack pointer.
    em_.add_imm(reg::FP, reg::SP, 0);

    int frame = frame_size(fn.nslots);
    if (frame > 0) {
        em_.sub_imm(reg::SP, reg::SP, static_cast<uint32_t>(frame));
    }

    // Parameters arrive in x0..x7 and are immediately written into slots 0..n-1
    // — sema numbered them in declaration order precisely so this loop is a
    // straight run.  Spilling them costs a store each, and buys uniformity:
    // after this, a parameter and a local are the same thing to every other
    // line of this file.
    for (size_t i = 0; i < fn.params.size(); i++) {
        em_.stur(static_cast<int>(i), reg::FP, slot_offset(static_cast<int>(i)));
    }

    // Locals are zero-initialized because the language says reading one before
    // assigning it gives 0.  Stack memory holds whatever the last call left
    // there, so without this a program's output would depend on what ran
    // before it — and the two engines would disagree at random.
    for (int slot = static_cast<int>(fn.params.size()); slot < fn.nslots; slot++) {
        em_.stur(reg::XZR, reg::FP, slot_offset(slot));
    }
}

void Codegen::gen_epilogue() {
    // Restoring sp from x29 rather than adding the frame size back is
    // deliberate: it also discards any expression temps that are still pushed,
    // so an unbalanced stack anywhere in the body cannot leak out of the call.
    em_.add_imm(reg::SP, reg::FP, 0);
    em_.ldp_post(reg::FP, reg::LR, reg::SP, 16);
    em_.ret();
}

void Codegen::gen_function(const FuncDef& fn) {
    em_.bind(func_labels_.at(fn.name));

    return_label_ = em_.new_label();

    gen_prologue(fn);
    gen_block(fn.body);

    // Reached only by falling off the end of the body, which the language says
    // returns 0.  A function that ends in `return` jumps over this so this
    // will never run for such function
    em_.movz(reg::X0, 0, 0);

    em_.bind(return_label_);
    gen_epilogue();
}

CompiledCode Codegen::compile() {
    // Pass 1: a label for every function, before a single instruction exists.
    // Nothing is emitted here; this only makes the names referenceable, which
    // is what a call to a not-yet-compiled function needs.
    for (const FuncDef* fn : funcs_) {
        func_labels_[fn->name] = em_.new_label();
    }

    // Pass 2: the bodies, in source order.
    CompiledCode out;
    for (const FuncDef* fn : funcs_) {
        // Recorded for every function, not just __main__: tiering enters the
        // buffer at whichever one went hot.
        out.offsets[fn->name] = em_.here();
        if (fn->name == kMainFunc) {
            // Saved because the caller lives outside the buffer and needs an
            // absolute address, which only exists as base() + this offset.
            out.entry_offset = em_.here();
        }
        gen_function(*fn);
    }

    // The shared divide-by-zero stub, emitted last and only if some division
    // actually branched to it.  One copy for the whole program: the failure
    // path is identical everywhere and it never returns, so there is nothing
    // for it to know about its caller.
    if (div_zero_ >= 0) {
        em_.bind(div_zero_);
        call_host(reinterpret_cast<uint64_t>(&mp_div_zero_abort));
        // mp_div_zero_abort calls exit(), so control never comes back and
        // there is nothing sensible to put after the call.
    }

    out.bytes = em_.finalize();
    out.listing = em_.listing();
    return out;
}

}  // namespace

CompiledCode compile_program(const Program& prog) {
    // The whole program is trivially closed under calling — sema already
    // rejected any call to a function that doesn't exist.
    std::vector<const FuncDef*> all;
    all.reserve(prog.funcs.size());
    for (const auto& fn : prog.funcs) {
        all.push_back(fn.get());
    }
    return Codegen(all).compile();
}

CompiledCode compile_functions(const std::vector<const FuncDef*>& funcs) {
    return Codegen(funcs).compile();
}

int run_jit(const Program& prog, bool time_it) {
    using clock = std::chrono::steady_clock;

    auto compile_start = clock::now();
    CompiledCode code = compile_program(prog);
    JitMemory mem(code.bytes);
    auto compile_end = clock::now();

    // A function pointer is nothing but an address, and calling through one
    // only means "start fetching instructions here" — the CPU has no way to
    // tell code that was compiled an instant ago from code that shipped in the
    // binary.  That is the entire trick this project is built around.
    using JitMain = int64_t (*)();
    uint8_t* entry = static_cast<uint8_t*>(mem.base()) + code.entry_offset;
    JitMain main_fn = reinterpret_cast<JitMain>(entry);

    auto run_start = clock::now();
    main_fn();
    auto run_end = clock::now();

    if (time_it) {
        // Two numbers, never one: compile time is paid once and run time scales
        // with the work, so a single total would flatter or damn the JIT
        // depending only on how long the benchmark happens to run.
        std::fprintf(stderr, "jit compile_ms=%.3f\n",
                     std::chrono::duration<double, std::milli>(
                         compile_end - compile_start).count());
        std::fprintf(stderr, "jit run_ms=%.3f\n",
                     std::chrono::duration<double, std::milli>(
                         run_end - run_start).count());
    }
    return 0;
}

void disasm(const CompiledCode& code, std::ostream& os) {
    char buf[64];
    for (const auto& line : code.listing) {
        std::snprintf(buf, sizeof(buf), "%4zu: %08x    ", line.offset, line.word);
        os << buf << line.text << "\n";
    }
}

}  // namespace minipy
