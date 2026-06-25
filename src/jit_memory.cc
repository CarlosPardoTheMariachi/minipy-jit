// jit_memory.cc — the five-step sequence, in order.  Each step that goes
// missing has its own distinct failure signature, noted at the step, because
// when this breaks the symptom is a crash with no stack trace and knowing
// which symptom means which step is most of the debugging.
#include "jit_memory.h"

#include <libkern/OSCacheControl.h>   // sys_icache_invalidate
#include <pthread.h>                  // pthread_jit_write_protect_np
#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace minipy {

JitMemory::JitMemory(const std::vector<uint8_t>& code) {
    size_ = code.size();

    // 1. Ask the kernel for a fresh region that is executable from birth.
    //
    // MAP_JIT is not optional on Apple Silicon.  The system enforces W^X: a
    // page may be writable or executable, never both at once, so a plain
    // PROT_WRITE|PROT_EXEC mmap comes back EACCES.  MAP_JIT is the sanctioned
    // exception — it hands out a region marked RWX in the page table whose
    // write-vs-execute meaning is then decided per thread by step 2.
    //
    // Failure signature: mmap returns MAP_FAILED with "Permission denied" =
    // the MAP_JIT flag is missing.
    mem_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (mem_ == MAP_FAILED) {
        std::perror("mmap(MAP_JIT)");
        std::abort();
    }

    // 2. Flip this thread's view of JIT regions to writable.
    //
    // This is a CPU register write, not a syscall and not a re-mapping — which
    // is why it's cheap enough to do around every compile.  It affects only
    // the calling thread, hence the pthread_ name, so the memcpy below has to
    // happen on this same thread.  (minipy is single-threaded, so that costs
    // us nothing; it matters only because it changes what a crash means.)
    //
    // Failure signature: crash *while writing*, i.e. inside the memcpy.
    pthread_jit_write_protect_np(0);

    // 3. Copy the finalized instruction bytes in.
    std::memcpy(mem_, code.data(), size_);

    // 4. Flip back to executable, restoring W^X before anything runs.
    //
    // Failure signature: crash on the very first instruction fetch, at an
    // address that looks completely correct in the debugger.
    pthread_jit_write_protect_np(1);

    // 5. Invalidate the instruction cache over the range we just wrote.
    //
    // AArch64 does not keep its instruction and data caches coherent — that's
    // deliberate, since self-modifying code is rare and the coherence hardware
    // would burn power on every core forever.  Our memcpy was a series of
    // stores, so the new bytes are sitting in the *data* cache; an instruction
    // fetch reads the *instruction* cache, which may still hold whatever used
    // to live at these addresses.  This call throws that stale copy away.
    //
    // Failure signature: the worst one on the list — it usually works anyway
    // (a freshly mapped page often has nothing stale cached), then fails
    // intermittently, and it *always* works under lldb, because setting a
    // breakpoint writes into the code and invalidates the i-cache for us.
    // "Works in the debugger, garbage standalone" means this line is missing.
    sys_icache_invalidate(mem_, size_);

    // 6. is the caller's: reinterpret_cast base() to a function pointer and
    // call it.  A function pointer is only an address, and a call only means
    // "set the PC here and start fetching" — the CPU cannot tell our emitted
    // bytes apart from any other function.
}

JitMemory::~JitMemory() {
    if (mem_ != nullptr && mem_ != MAP_FAILED) {
        munmap(mem_, size_);
    }
}

}  // namespace minipy
