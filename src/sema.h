// sema.h — name/arity checking and frame-slot assignment.
//
// Two passes, for the same reason the Cool compiler's CgenClassTable collected
// every class layout before emitting any method body: a call can name a
// function that is defined *later* in the file (and recursion names one that
// isn't finished being defined), so all the signatures must exist before any
// body is checked.  Pass 1 collects signatures, pass 2 checks bodies.
//
// Sema has two jobs, and the second one is why it can't be skipped:
//   A. reject invalid programs (undefined names, bad arity, shadowing print)
//      with a clean "file:line: error: ..." instead of a crash;
//   B. assign every parameter and local a frame slot, which codegen later
//      turns into an x29-relative address.  The interpreter ignores slots
//      entirely — it keeps its string-keyed frame — so B is purely for the
//      JIT, and it's the reason sema has to land before codegen.
#ifndef MINIPY_SEMA_H
#define MINIPY_SEMA_H

#include <string>
#include <unordered_map>

#include "ast.h"

namespace minipy {

// What pass 1 records about each function, and what pass 2 checks every call
// against: how many arguments it takes, and where its body is.
struct FuncSig {
    int nparams;
    const FuncDef* def;
};

class Sema {
public:
    explicit Sema(Program& prog) : prog_(prog) {}

    // Run the passes over the whole program, annotating the AST in place.
    // Throws CompileError on the first violation it finds.
    void run();

    // Codegen will want this later to resolve calls to buffer offsets.
    const std::unordered_map<std::string, FuncSig>& signatures() const {
        return sigs_;
    }

private:
    // Pass 1: every function's name and arity, before any body is looked at.
    void collect_signatures();

    Program& prog_;
    std::unordered_map<std::string, FuncSig> sigs_;
};

}  // namespace minipy

#endif  // MINIPY_SEMA_H
