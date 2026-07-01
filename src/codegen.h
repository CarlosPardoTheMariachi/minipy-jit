// codegen.h — walks the AST and drives the Emitter.
//
// This is the Cool compiler's cgen.cc with a different target.  The model is
// the same stack machine from Lecture 12, and it rests on one invariant that
// every method below preserves:
//
//     generating code for an expression leaves its value in x0,
//     and leaves the stack exactly as it found it.
//
// That single rule is what makes the compiler compositional.  Nothing here
// tracks which register holds what, because the answer is always x0; nothing
// here worries that a subexpression might contain a function call, because the
// call also obeys the rule.  A binary operator can therefore be written without
// knowing anything about its operands: evaluate the left side, push it out of
// harm's way, evaluate the right side, pop the left side back, combine.  It is
// slower than keeping values in registers — every operand makes a round trip
// through memory — and that inefficiency is the price of not doing register
// allocation, which is a much larger piece of machinery.
//
// The division of labor with the Emitter is strict: this file decides what
// instructions to emit and in what order, and never touches a bit.
#ifndef MINIPY_CODEGEN_H
#define MINIPY_CODEGEN_H

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

#include "ast.h"
#include "emitter.h"

namespace minipy {

// The finished machine code, plus the two things a caller needs to use it.
struct CompiledCode {
    std::vector<uint8_t> bytes;

    // Where __main__ starts, in bytes from the front of the buffer.  Functions
    // are emitted in source order, so this is generally not 0, and the entry
    // point is base() + entry_offset rather than base().
    size_t entry_offset = 0;

    // The mnemonic record kept while emitting, for --disasm.
    std::vector<Emitter::Line> listing;
};

// Compile the whole program in one pass over the AST.  Every function gets a
// label up front, before any body is emitted, which is what lets a call to a
// function defined later in the file — or to the function currently being
// compiled — resolve to a real displacement at finalize time.
CompiledCode compile_program(const Program& prog);

// Compile, map the code as executable, and call __main__.  Returns the process
// exit code.  With time_it, prints compile and run times to stderr as separate
// numbers, because a JIT that is fast to run and slow to compile is a real
// tradeoff and averaging the two would hide it.
int run_jit(const Program& prog, bool time_it = false);

// Print the listing as "offset: hex-word    mnemonic", one line per
// instruction.  This is the readable .s file that SPIM handed the Cool
// compiler for free, rebuilt by hand — when emitted code misbehaves, the first
// question is always whether the instructions are the ones intended.
void disasm(const CompiledCode& code, std::ostream& os);

}  // namespace minipy

#endif  // MINIPY_CODEGEN_H
