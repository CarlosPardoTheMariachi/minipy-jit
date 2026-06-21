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

}  // namespace minipy

#endif  // MINIPY_INTERP_H
