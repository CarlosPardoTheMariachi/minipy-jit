// jit42.cc — proves the JIT memory dance before any encoding work exists.
//
// The two instruction words below are hardcoded, not emitted: they were read
// straight out of a real assembler
//
//     printf 'movz x0,#42\nret\n' | clang -x assembler -c -o /tmp/a.o - \
//         && otool -t /tmp/a.o
//
// so they are known-good by construction.  That's the whole point of running
// this before the Emitter is written — if it prints 42, mmap/write-protect/
// icache are correct, and every bug after this one belongs to the encodings.
// Standalone; not part of the minipy binary and not run by tests/run.sh.
//
//     clang++ -std=c++20 -Wall -Wextra -O2 -o /tmp/jit42 \
//         tests/jit42.cc src/jit_memory.cc && /tmp/jit42
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../src/jit_memory.h"

using Fn = int64_t (*)();

int main() {
    const uint32_t words[] = {
        0xd2800540,   // movz x0, #42
        0xd65f03c0,   // ret
    };

    // AArch64 is little-endian, so the low byte of each word goes first.  The
    // shifts spell that out rather than memcpy'ing the array, because the byte
    // order of the buffer is exactly the thing being proven here.
    std::vector<uint8_t> code;
    for (uint32_t w : words) {
        code.push_back(static_cast<uint8_t>(w & 0xff));
        code.push_back(static_cast<uint8_t>((w >> 8) & 0xff));
        code.push_back(static_cast<uint8_t>((w >> 16) & 0xff));
        code.push_back(static_cast<uint8_t>((w >> 24) & 0xff));
    }

    minipy::JitMemory mem(code);
    Fn fn = reinterpret_cast<Fn>(mem.base());
    int64_t got = fn();

    std::printf("jit42: returned %lld (want 42)\n", (long long)got);
    return got == 42 ? 0 : 1;
}
