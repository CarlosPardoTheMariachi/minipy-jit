// runtime.h — the handful of host functions that emitted code calls into.
//
// This is the trap.handler of the Cool compiler, shrunk to two entries.  The
// rule for what belongs here is simple: anything the CPU has no instruction
// for.  Formatting an integer as decimal text and writing it to a file
// descriptor is thousands of instructions of libc, so `print` is a call; adding
// two numbers is one instruction, so `+` is not.
//
// Everything here is extern "C" for a reason that matters at the bit level:
// C++ mangles names and is free to invent its own calling conventions, while
// the emitted code knows exactly one convention (AAPCS64 — arguments in x0..x7,
// result in x0) and has no way to ask.  extern "C" pins both.
//
// The emitted code reaches these by address, not by name.  There is no linker
// involved in a JIT: codegen materializes the function's address as a 64-bit
// constant and calls it with BLR.
#ifndef MINIPY_RUNTIME_H
#define MINIPY_RUNTIME_H

#include <cstdint>

extern "C" {

// print(x).  Returns 0 because a call is an expression in this language and
// every expression has a value; the language spec says print's is 0.
int64_t mp_print(int64_t v);

// Divide or modulo by zero.  AArch64's SDIV does not trap on a zero divisor —
// it quietly returns 0 — so the check is emitted by codegen and lands here.
[[noreturn]] void mp_div_zero_abort(void);

}  // extern "C"

#endif  // MINIPY_RUNTIME_H
