// interp.h — tree-walking interpreter.
//
// Written before the JIT, on purpose, to be the differential-testing oracle:
// every observable behavior (stdout, stderr, exit status) must match the JIT
// exactly, so the interpreter defines what "correct" means.
#ifndef MINIPY_INTERP_H
#define MINIPY_INTERP_H

#include "ast.h"

namespace minipy {

// Run the program's __main__ under the interpreter.  Returns the process exit
// code (0 normally; a runtime error like division by zero exits 1 directly).
// If time_it, prints "interp run_ms=..." to stderr for the benchmark harness.
int run_interpreter(const Program& prog, bool time_it = false);

// Run it in mixed mode: interpret, but compile any function called at least
// `threshold` times and use the compiled version for every later call.  Same
// observable behavior as the other two engines — that is the whole contract,
// and it is what the differential tests check.
int run_tiered(const Program& prog, int threshold, bool time_it = false);

}  // namespace minipy

#endif  // MINIPY_INTERP_H
