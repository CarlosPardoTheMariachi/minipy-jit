// jit_memory.h — turns a buffer of instruction bytes into memory the CPU will
// actually execute.
//
// The problem this solves: the emitter builds its code in an ordinary
// std::vector, which lives on the heap, and heap pages are mapped
// read+write+NO-execute.  Permission is a property of the *page*, checked by
// the MMU on every instruction fetch, so jumping into a vector faults (SIGBUS)
// even though the bytes in it are perfectly correct.  There is no way to
// promote an existing heap pointer to executable; the only move is to ask the
// kernel for pages that were created executable and copy into them.
//
// One JitMemory owns one such region for its lifetime: it mmaps, copies the
// code in, makes it executable, and munmaps in the destructor.  base() is the
// address to reinterpret_cast to a function pointer and call.
#ifndef MINIPY_JIT_MEMORY_H
#define MINIPY_JIT_MEMORY_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace minipy {

class JitMemory {
public:
    // Aborts with a diagnostic if the mmap is refused — there is no sensible
    // way to continue, and a silent nullptr would crash much later in a place
    // that tells you nothing.
    explicit JitMemory(const std::vector<uint8_t>& code);
    ~JitMemory();

    // The region is a raw kernel mapping, not a value; copying one would mean
    // two owners racing to munmap the same address.
    JitMemory(const JitMemory&) = delete;
    JitMemory& operator=(const JitMemory&) = delete;

    void* base() const { return mem_; }
    size_t size() const { return size_; }

private:
    void* mem_ = nullptr;
    size_t size_ = 0;
};

}  // namespace minipy

#endif  // MINIPY_JIT_MEMORY_H
