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

void Sema::run() {
    collect_signatures();
}

}  // namespace minipy
