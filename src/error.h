// error.h — compile-time error type shared by lexer, parser, and sema.
//
// Runtime errors (division by zero) are a separate mechanism: both engines
// print "runtime error: division by zero" to stderr and exit(1).
#ifndef MINIPY_ERROR_H
#define MINIPY_ERROR_H

#include <stdexcept>
#include <string>

namespace minipy {

// Thrown by the front end on the first error. The driver catches it, prints
// "<file>:<line>: error: <msg>" to stderr, and exits with status 1.
// (First-error-abort is a deliberate MVP simplification — no PA1-style ERROR
// tokens with recovery.)
struct CompileError : std::runtime_error {
    int line;
    CompileError(int ln, const std::string& msg)
        : std::runtime_error(msg), line(ln) {}
};

}  // namespace minipy

#endif  // MINIPY_ERROR_H
