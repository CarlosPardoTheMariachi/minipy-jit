// emit_smoke.cc — execute hand-emitted AArch64 to validate encodings.
// Not part of the minipy binary; compiled and run by tests/run.sh.
#include <cstdint>
#include <cstdio>
#include <functional>

#include "../src/emitter.h"
#include "../src/jit_memory.h"

using namespace minipy;
using Fn = int64_t (*)();

static int failures = 0;

static void check(const char* name, Emitter& e, int64_t expect) {
    auto bytes = e.finalize();
    JitMemory mem(bytes);
    Fn fn = reinterpret_cast<Fn>(mem.base());
    int64_t got = fn();
    if (got == expect) {
        std::printf("  ok   %-22s => %lld\n", name, (long long)got);
    } else {
        std::printf("  FAIL %-22s => %lld (want %lld)\n", name, (long long)got, (long long)expect);
        failures++;
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("emit_smoke:\n");

    { // MOVZ x0,#42 ; RET  — proves the whole memory dance.
        Emitter e; e.movz(reg::X0, 42, 0); e.ret();
        check("movz#42", e, 42);
    }
    { // materialize a big 64-bit constant with MOVZ+MOVK chunks.
        Emitter e; e.mov_imm64(reg::X0, 0x1234567890ABCDEFull); e.ret();
        check("mov_imm64", e, (int64_t)0x1234567890ABCDEFull);
    }
    { // 20 + 22*1  via mul/add:  x0=22; x1=1; x0=x0*x1; x2=20; x0=x0+x2
        Emitter e;
        e.movz(reg::X0, 22, 0); e.movz(reg::X1, 1, 0); e.mul(reg::X0, reg::X0, reg::X1);
        e.movz(reg::X2, 20, 0); e.add_reg(reg::X0, reg::X0, reg::X2); e.ret();
        check("mul+add", e, 42);
    }
    { // 100 - 58 via sub_reg
        Emitter e; e.movz(reg::X0,100,0); e.movz(reg::X1,58,0); e.sub_reg(reg::X0,reg::X0,reg::X1); e.ret();
        check("sub_reg", e, 42);
    }
    { // sdiv: 85 / 2 = 42 (truncates)
        Emitter e; e.movz(reg::X0,85,0); e.movz(reg::X1,2,0); e.sdiv(reg::X0,reg::X0,reg::X1); e.ret();
        check("sdiv", e, 42);
    }
    { // msub for %: 85 % 2 = 1.  sdiv x2,x0,x1 ; msub x0,x2,x1,x0
        Emitter e; e.movz(reg::X0,85,0); e.movz(reg::X1,2,0);
        e.sdiv(reg::X2,reg::X0,reg::X1); e.msub(reg::X0,reg::X2,reg::X1,reg::X0); e.ret();
        check("msub(%)", e, 1);
    }
    { // neg: -(-42) via neg twice ... just neg(0-42): x0=42; neg → -42
        Emitter e; e.movz(reg::X0,42,0); e.neg(reg::X0,reg::X0); e.ret();
        check("neg", e, -42);
    }
    { // cmp + cset LT : 3 < 5 -> 1
        Emitter e; e.movz(reg::X0,3,0); e.movz(reg::X1,5,0); e.cmp(reg::X0,reg::X1);
        e.cset(reg::X0, Cond::LT); e.ret();
        check("cmp+cset lt", e, 1);
    }
    { // cmp + cset GE : 3 >= 5 -> 0
        Emitter e; e.movz(reg::X0,3,0); e.movz(reg::X1,5,0); e.cmp(reg::X0,reg::X1);
        e.cset(reg::X0, Cond::GE); e.ret();
        check("cmp+cset ge", e, 0);
    }
    { // push/pop round-trip through sp: push 42, clobber x0, pop into x0
        Emitter e;
        e.movz(reg::X0,42,0); e.str_pre(reg::X0, reg::SP, -16);
        e.movz(reg::X0,7,0);  e.ldr_post(reg::X0, reg::SP, 16); e.ret();
        check("push/pop", e, 42);
    }
    { // add_imm / sub_imm on a GP reg
        Emitter e; e.movz(reg::X0,40,0); e.add_imm(reg::X0,reg::X0,10); e.sub_imm(reg::X0,reg::X0,8); e.ret();
        check("add/sub imm", e, 42);
    }
    { // cbz taken: x0=0; cbz over movz#99; movz#42
        Emitter e; Label skip = e.new_label();
        e.movz(reg::X0,0,0); e.cbz(reg::X0, skip);
        e.movz(reg::X0,99,0);
        e.bind(skip); e.movz(reg::X0,42,0); e.ret();
        check("cbz taken", e, 42);
    }
    { // b.cond + backward branch loop: sum 1..9 = 45? compute count down: x0=0,x1=9; loop add; ret 45
        Emitter e;
        e.movz(reg::X0,0,0);   // acc
        e.movz(reg::X1,9,0);   // counter
        Label top = e.new_label(); Label end = e.new_label();
        e.bind(top);
        e.cbz(reg::X1, end);           // while x1 != 0
        e.add_reg(reg::X0,reg::X0,reg::X1);
        e.sub_imm(reg::X1,reg::X1,1);
        e.b(top);
        e.bind(end); e.ret();
        check("loop sum1..9", e, 45);
    }
    { // stur/ldur to a frame slot below fp
        Emitter e;
        e.add_imm(reg::FP, reg::SP, 0);       // fp = sp  (MOV Xd,SP is ADD #0, not ORR!)
        e.movz(reg::X0,42,0);
        e.stur(reg::X0, reg::FP, -8);
        e.movz(reg::X0,0,0);
        e.ldur(reg::X0, reg::FP, -8);
        e.ret();
        check("stur/ldur", e, 42);
    }

    { // b.cond taken: 3 < 5, B.LT skips the clobber
        Emitter e; Label skip = e.new_label();
        e.movz(reg::X0, 3, 0); e.movz(reg::X1, 5, 0); e.cmp(reg::X0, reg::X1);
        e.movz(reg::X0, 42, 0);
        e.b_cond(Cond::LT, skip);
        e.movz(reg::X0, 99, 0);          // must be skipped
        e.bind(skip); e.ret();
        check("b.cond taken", e, 42);
    }
    { // b.cond not taken: 5 < 3 is false, B.LT falls through to the overwrite
        Emitter e; Label skip = e.new_label();
        e.movz(reg::X0, 5, 0); e.movz(reg::X1, 3, 0); e.cmp(reg::X0, reg::X1);
        e.movz(reg::X0, 99, 0);
        e.b_cond(Cond::LT, skip);
        e.movz(reg::X0, 42, 0);          // must execute
        e.bind(skip); e.ret();
        check("b.cond not taken", e, 42);
    }
    { // stp/ldp round trip like the frame prologue: push x0/x1 pair, restore
        Emitter e;
        e.movz(reg::X0, 42, 0); e.movz(reg::X1, 7, 0);
        e.stp_pre(reg::X0, reg::X1, reg::SP, -16);   // [sp,#-16]!
        e.movz(reg::X0, 0, 0);  e.movz(reg::X1, 0, 0);
        e.ldp_post(reg::X0, reg::X1, reg::SP, 16);   // [sp],#16
        e.add_reg(reg::X0, reg::X0, reg::X1); e.ret();
        check("stp/ldp pair", e, 49);
    }

    std::printf("emit_smoke: %s\n", failures ? "FAILURES" : "all passed");
    return failures ? 1 : 0;
}
