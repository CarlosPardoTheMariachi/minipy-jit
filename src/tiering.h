// tiering.h — mixed-mode execution: interpret first, compile what gets hot.
//
// This is the piece that makes the name honest.  Up to now the project had two
// separate engines: --interp walks the tree, --jit compiles everything up front
// and then runs it.  Neither is what people usually mean by a JIT.  What they
// mean is this: start interpreting, watch which functions actually run a lot,
// and compile only those, while the program is running.
//
// The policy is deliberately the simplest one that works.  Every call to a
// function bumps a counter; when the counter crosses a threshold the function
// is compiled, and every later call to it runs as machine code.  Calls already
// in flight are untouched — an interpreted frame that is halfway through its
// body stays interpreted until it returns.
//
// The consequence worth knowing up front: a function whose counter only ever
// reaches 1 can never be promoted, no matter how long it runs.  __main__ and a
// main() containing one long loop are exactly that shape.  Promoting a loop
// that is *already running* means rebuilding a compiled frame from an
// interpreter frame mid-flight — on-stack replacement — which is a much larger
// piece of machinery and deliberately out of scope.
//
// Two properties of this language make the rest of it cheap, and neither is
// true of a real dynamic-language runtime:
//
//   1. The call graph is completely static.  Every call names a function
//      directly — no function pointers, no methods, no first-class functions —
//      so when something goes hot, the full set of functions it can reach is
//      known by reading the AST.  Compiling that whole set in one shot means
//      every call inside the compiled region is a plain BL to a known offset in
//      the same buffer.  There are no patchable call stubs and no route back
//      out to the interpreter, and building those is the genuinely hard part of
//      tiering elsewhere.
//
//   2. There is nothing to convert at the boundary.  Every value is an int64
//      and every function takes at most 8 of them, so the interpreter hands its
//      arguments straight to compiled code in registers.  A runtime with boxed
//      values would need a marshalling layer at every crossing.
#ifndef MINIPY_TIERING_H
#define MINIPY_TIERING_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "jit_memory.h"

namespace minipy {

class TierManager {
public:
    // threshold is how many calls a function may make before it is compiled.
    TierManager(const Program& prog, int threshold);

    // Called by the interpreter on entry to every user function call.  Returns
    // the address to call, or nullptr to keep interpreting.  This is the whole
    // policy: counting, deciding, and compiling all happen behind this call.
    void* on_call(const FuncDef* fn);

    // Call compiled code.  Separate from on_call because the interpreter has to
    // evaluate the arguments in between.
    static int64_t invoke(void* entry, const std::vector<int64_t>& args);

    // For the --time summary: how many functions ended up compiled, and how
    // much wall time went into compiling them.
    int compiled_count() const { return static_cast<int>(compiled_.size()); }
    double compile_ms() const { return compile_ms_; }

private:
    // Every function the hot one can reach, transitively, including itself.
    // This is the set that gets compiled together.
    std::vector<const FuncDef*> reachable_from(const FuncDef* root);

    // Gather the names of every function called anywhere in a body.  Plain
    // recursive walks over the AST, the same shape sema uses.
    void collect_calls_block(const Block& body, std::vector<std::string>& out);
    void collect_calls_stmt(const Stmt* s, std::vector<std::string>& out);
    void collect_calls_expr(const Expr* e, std::vector<std::string>& out);

    // Compile fn together with everything it can reach, and record where each
    // one landed.
    void compile_hot(const FuncDef* fn);

    int threshold_;
    double compile_ms_ = 0.0;

    std::unordered_map<std::string, const FuncDef*> by_name_;
    std::unordered_map<const FuncDef*, int> counts_;
    std::unordered_map<const FuncDef*, void*> compiled_;

    // Every buffer ever produced, kept alive for the life of the run.  A
    // function can be compiled more than once — it may be reachable from two
    // different hot roots — and an older copy could still be executing on the
    // stack when the newer one is made, so no buffer is ever released early.
    std::vector<std::unique_ptr<JitMemory>> regions_;
};

}  // namespace minipy

#endif  // MINIPY_TIERING_H
