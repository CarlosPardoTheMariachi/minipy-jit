// runtime.cc — host functions called from emitted code.
//
// Both of these have an exact counterpart inside interp.cc, and the two must
// stay observably identical: the differential tests compare stdout, stderr and
// exit status between the engines, so a stray space in a message here shows up
// as a test failure rather than as a cosmetic difference.
#include "runtime.h"

#include <cstdio>
#include <cstdlib>

extern "C" {

int64_t mp_print(int64_t v) {
    // A plain wrapper around printf rather than a direct call to printf from
    // emitted code, because printf is variadic and Apple's ABI passes variadic
    // arguments on the stack instead of in registers.  Calling it directly
    // would mean codegen implementing a second calling convention for exactly
    // one function; this way everything the JIT calls is ordinary AAPCS64.
    std::printf("%lld\n", static_cast<long long>(v));
    return 0;
}

void mp_div_zero_abort(void) {
    // The flush has to come first.  stdout is line-buffered to a terminal but
    // block-buffered to a pipe, so without this the output printed before the
    // error would be lost on exit under exactly the conditions the test runner
    // uses — and the interpreter, which flushes, would then disagree with the
    // JIT about a program neither engine got wrong.
    std::fflush(stdout);
    std::fprintf(stderr, "runtime error: division by zero\n");
    std::exit(1);
}

}  // extern "C"
