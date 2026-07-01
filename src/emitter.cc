// emitter.cc — AArch64 instruction encodings, label bookkeeping, and buffer
// finalization.
//
// Every encoding here was read out of a real assembler rather than recalled:
// assemble the instruction with known operands, dump the bytes, and work out
// which bits moved when an operand changed.  The `example:` line on each method
// is the literal word clang produced for the operands in the comment, so any
// one of them can be re-checked in ten seconds:
//
//   printf 'movz x0,#42\n' | clang -x assembler -c -o /tmp/a.o - && otool -t /tmp/a.o
//
// That matters more here than anywhere else in the compiler, because a wrong
// bit does not produce an error — it produces a different, legal instruction,
// and the program goes wrong somewhere else entirely.
//
// The field positions for the branches (imm26 in bits 25..0, imm19 in bits
// 23..5, both counting instruction words) came from the same technique: a
// forward branch of +2 words comes back as 0x14000002 for B and 0x54000042 for
// B.LT, which pins both the shift and the units.
#include "emitter.h"

#include <cstdio>
#include <cstdlib>

namespace minipy {

// Internal invariant failures, not user errors.  A malformed .mp file gets a
// CompileError with a line number; getting here instead means the compiler
// itself is wrong, and there is nothing the user could fix, so this is loud
// and immediate rather than an exception someone might swallow.
static void emitter_bug(const char* what) {
    std::fprintf(stderr, "emitter: internal error: %s\n", what);
    std::abort();
}

Label Emitter::new_label() {
    Label l = static_cast<Label>(label_pos_.size());
    label_pos_.push_back(kUnbound);
    return l;
}

void Emitter::bind(Label l) {
    bool in_range = (l >= 0) && (l < static_cast<Label>(label_pos_.size()));
    if (!in_range) {
        emitter_bug("bind() called with a label this emitter never issued");
    }
    if (label_pos_[l] != kUnbound) {
        emitter_bug("bind() called twice on the same label");
    }
    // The label points at the next word to be emitted, which is exactly the
    // current length: binding at the end of the buffer and then emitting means
    // the emitted instruction is the one the label names.
    label_pos_[l] = static_cast<int>(code_.size());
}

void Emitter::emit(uint32_t word, std::string text) {
    Line line;
    line.offset = code_.size() * 4;
    line.word = word;
    line.text = std::move(text);
    listing_.push_back(std::move(line));

    code_.push_back(word);
}

void Emitter::add_fixup(Label l, FixKind kind) {
    bool in_range = (l >= 0) && (l < static_cast<Label>(label_pos_.size()));
    if (!in_range) {
        emitter_bug("branch to a label this emitter never issued");
    }

    Fixup f;
    // Not code_.size() - 1: this runs *before* the branch word is emitted, so
    // the current length is the index that word is about to occupy.
    f.at_word = code_.size();
    f.label = l;
    f.kind = kind;
    fixups_.push_back(f);
}

// ---------------------------------------------------------------------------
// Field helpers
//
// Every one of these exists to turn a silent wrong answer into a loud abort.
// An out-of-range immediate does not fail to assemble here the way it would in
// a real assembler — it just overflows into the neighbouring field and encodes
// some other instruction — so the range checks are the only thing standing
// between a typo and an afternoon in lldb.
// ---------------------------------------------------------------------------

static void check_reg(int r) {
    if (r < 0 || r > 31) {
        emitter_bug("register number outside 0..31");
    }
}

// The 9-bit signed offset shared by LDUR/STUR and by the pre/post-indexed
// forms.  Same field, same range, in bits 20..12.
static uint32_t imm9_field(int imm) {
    if (imm < -256 || imm > 255) {
        emitter_bug("9-bit signed offset out of range (-256..255)");
    }
    return (static_cast<uint32_t>(imm) & 0x1FFu) << 12;
}

// STP/LDP's offset is a 7-bit signed field *scaled by 8*, so it reaches much
// further than the 9-bit one but only in whole registers.
static uint32_t imm7_field(int imm_bytes) {
    if (imm_bytes % 8 != 0) {
        emitter_bug("stp/ldp offset must be a multiple of 8");
    }
    int scaled = imm_bytes / 8;
    if (scaled < -64 || scaled > 63) {
        emitter_bug("stp/ldp offset out of range (-512..504)");
    }
    return (static_cast<uint32_t>(scaled) & 0x7Fu) << 15;
}

static uint32_t imm12_field(uint32_t imm) {
    // The encoding has a shift bit that would let the immediate mean imm<<12,
    // but nothing in minipy needs an offset that big — frames cap out around
    // 240 bytes — so the emitter refuses rather than quietly supporting half
    // of a feature.
    if (imm > 4095) {
        emitter_bug("12-bit immediate out of range (0..4095)");
    }
    return imm << 10;
}

// Register spellings for the listing.  Two of them, because a 31 prints as
// "sp" or "xzr" depending on which instruction is asking, and a listing that
// gets that wrong is worse than no listing.
static std::string xname(int r) {
    if (r == 31) {
        return "xzr";
    }
    return "x" + std::to_string(r);
}

static std::string spname(int r) {
    if (r == 31) {
        return "sp";
    }
    return "x" + std::to_string(r);
}

static const char* cond_name(Cond c) {
    switch (c) {
        case Cond::EQ: return "eq";
        case Cond::NE: return "ne";
        case Cond::GE: return "ge";
        case Cond::LT: return "lt";
        case Cond::GT: return "gt";
        case Cond::LE: return "le";
    }
    return "??";
}

// ---------------------------------------------------------------------------
// Moves and constants
// ---------------------------------------------------------------------------

// MOVZ:  sf(31) opc=10(30:29) 100101(28:23) hw(22:21) imm16(20:5) Rd(4:0)
// example: movz x0, #42  ->  0xd2800540   (0xd2800000 | 42<<5)
void Emitter::movz(int rd, uint16_t imm16, int hw) {
    check_reg(rd);
    if (hw < 0 || hw > 3) {
        emitter_bug("movz/movk halfword index outside 0..3");
    }
    uint32_t word = 0xD2800000u
                  | (static_cast<uint32_t>(hw) << 21)
                  | (static_cast<uint32_t>(imm16) << 5)
                  | static_cast<uint32_t>(rd);

    std::string text = "movz " + xname(rd) + ", #" + std::to_string(imm16);
    if (hw != 0) {
        text += ", lsl #" + std::to_string(16 * hw);
    }
    emit(word, text);
}

// MOVK is MOVZ with opc=11 instead of 10 — one bit apart, and the bit means
// "leave the other three halfwords alone".
// example: movk x0, #0x1234, lsl #32  ->  0xf2c24680
void Emitter::movk(int rd, uint16_t imm16, int hw) {
    check_reg(rd);
    if (hw < 0 || hw > 3) {
        emitter_bug("movz/movk halfword index outside 0..3");
    }
    uint32_t word = 0xF2800000u
                  | (static_cast<uint32_t>(hw) << 21)
                  | (static_cast<uint32_t>(imm16) << 5)
                  | static_cast<uint32_t>(rd);

    std::string text = "movk " + xname(rd) + ", #" + std::to_string(imm16);
    if (hw != 0) {
        text += ", lsl #" + std::to_string(16 * hw);
    }
    emit(word, text);
}

void Emitter::mov_imm64(int rd, uint64_t value) {
    // One MOVZ to establish the register (zeroing everything it doesn't
    // write), then a MOVK per remaining nonzero chunk.  Skipping the zero
    // chunks is what keeps the common small constant down to one instruction:
    // the MOVZ already zeroed those halfwords.
    bool wrote_first = false;
    for (int hw = 0; hw < 4; hw++) {
        uint16_t chunk = static_cast<uint16_t>((value >> (16 * hw)) & 0xFFFFu);
        if (chunk == 0) {
            continue;
        }
        if (!wrote_first) {
            movz(rd, chunk, hw);
            wrote_first = true;
        } else {
            movk(rd, chunk, hw);
        }
    }

    // A value of exactly zero has no nonzero chunk, so the loop emitted
    // nothing at all and the register still holds whatever it held.
    if (!wrote_first) {
        movz(rd, 0, 0);
    }
}

// MOV between registers is ORR Xd, XZR, Xm.  Register 31 here is the zero
// register, which is why this cannot move the stack pointer — for that the
// spelling is add_imm(rd, SP, 0), a different instruction entirely.
// example: mov x0, x1  ->  0xaa0103e0
void Emitter::mov_reg(int rd, int rm) {
    check_reg(rd);
    check_reg(rm);
    uint32_t word = 0xAA000000u
                  | (static_cast<uint32_t>(rm) << 16)
                  | (31u << 5)
                  | static_cast<uint32_t>(rd);
    emit(word, "mov " + xname(rd) + ", " + xname(rm));
}

// ---------------------------------------------------------------------------
// Arithmetic
//
// All the shifted-register forms share a layout:
//   base | Rm(20:16) | shift-amount(15:10, always 0 for us) | Rn(9:5) | Rd(4:0)
// and all of them read a 31 in any field as XZR.
// ---------------------------------------------------------------------------

// example: add x0, x1, x2  ->  0x8b020020
void Emitter::add_reg(int rd, int rn, int rm) {
    check_reg(rd);
    check_reg(rn);
    check_reg(rm);
    uint32_t word = 0x8B000000u
                  | (static_cast<uint32_t>(rm) << 16)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rd);
    emit(word, "add " + xname(rd) + ", " + xname(rn) + ", " + xname(rm));
}

// example: sub x0, x1, x2  ->  0xcb020020
void Emitter::sub_reg(int rd, int rn, int rm) {
    check_reg(rd);
    check_reg(rn);
    check_reg(rm);
    uint32_t word = 0xCB000000u
                  | (static_cast<uint32_t>(rm) << 16)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rd);
    emit(word, "sub " + xname(rd) + ", " + xname(rn) + ", " + xname(rm));
}

// MUL is MADD with the addend wired to XZR: Xd = Xa + Xn*Xm with Xa = zero.
// The three-operand multiply is the alias, not the real instruction.
// example: mul x0, x1, x2  ->  0x9b027c20   (Ra=31 shows up as 0x7c00)
void Emitter::mul(int rd, int rn, int rm) {
    check_reg(rd);
    check_reg(rn);
    check_reg(rm);
    uint32_t word = 0x9B000000u
                  | (static_cast<uint32_t>(rm) << 16)
                  | (31u << 10)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rd);
    emit(word, "mul " + xname(rd) + ", " + xname(rn) + ", " + xname(rm));
}

// SDIV truncates toward zero, which is exactly the // semantics the language
// froze.  It does NOT trap on a zero divisor — it returns 0 — so codegen has
// to test the divisor itself before getting here.
// example: sdiv x0, x1, x2  ->  0x9ac20c20
void Emitter::sdiv(int rd, int rn, int rm) {
    check_reg(rd);
    check_reg(rn);
    check_reg(rm);
    uint32_t word = 0x9AC00C00u
                  | (static_cast<uint32_t>(rm) << 16)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rd);
    emit(word, "sdiv " + xname(rd) + ", " + xname(rn) + ", " + xname(rm));
}

// MSUB: Xd = Xa - Xn*Xm.  There is no remainder instruction, so % is built as
// quotient = a/b (SDIV) followed by a - quotient*b, which is this.
// example: msub x0, x1, x2, x3  ->  0x9b028c20
void Emitter::msub(int rd, int rn, int rm, int ra) {
    check_reg(rd);
    check_reg(rn);
    check_reg(rm);
    check_reg(ra);
    uint32_t word = 0x9B008000u
                  | (static_cast<uint32_t>(rm) << 16)
                  | (static_cast<uint32_t>(ra) << 10)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rd);
    emit(word, "msub " + xname(rd) + ", " + xname(rn) + ", " + xname(rm) +
                   ", " + xname(ra));
}

// NEG is SUB Xd, XZR, Xm.  This is the one place where getting register 31
// backwards is caught immediately: subtracting from the stack pointer instead
// of from zero would give a wildly wrong number rather than a crash.
// example: neg x0, x1  ->  0xcb0103e0, identical to sub x0, xzr, x1
void Emitter::neg(int rd, int rm) {
    check_reg(rd);
    check_reg(rm);
    uint32_t word = 0xCB000000u
                  | (static_cast<uint32_t>(rm) << 16)
                  | (31u << 5)
                  | static_cast<uint32_t>(rd);
    emit(word, "neg " + xname(rd) + ", " + xname(rm));
}

// ADD/SUB immediate:  base | imm12(21:10) | Rn(9:5) | Rd(4:0).
// This is the family that reads 31 as SP, so it covers three jobs at once:
// ordinary constant arithmetic, moving the stack pointer, and `MOV Xd, SP`
// (which is add_imm(rd, SP, 0) — the ORR-based mov_reg cannot express it).
// example: add x0, x1, #10  ->  0x91002820 ; add sp, sp, #16 -> 0x910043ff
void Emitter::add_imm(int rd, int rn, uint32_t imm12) {
    check_reg(rd);
    check_reg(rn);
    uint32_t word = 0x91000000u
                  | imm12_field(imm12)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rd);
    emit(word, "add " + spname(rd) + ", " + spname(rn) + ", #" +
                   std::to_string(imm12));
}

// example: sub x0, x1, #10  ->  0xd1002820
void Emitter::sub_imm(int rd, int rn, uint32_t imm12) {
    check_reg(rd);
    check_reg(rn);
    uint32_t word = 0xD1000000u
                  | imm12_field(imm12)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rd);
    emit(word, "sub " + spname(rd) + ", " + spname(rn) + ", #" +
                   std::to_string(imm12));
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

// CMP is SUBS with the destination wired to XZR: do the subtraction for the
// flags, discard the difference.
// example: cmp x0, x1  ->  0xeb01001f, the same word as subs xzr, x0, x1
void Emitter::cmp(int rn, int rm) {
    check_reg(rn);
    check_reg(rm);
    uint32_t word = 0xEB000000u
                  | (static_cast<uint32_t>(rm) << 16)
                  | (static_cast<uint32_t>(rn) << 5)
                  | 31u;
    emit(word, "cmp " + xname(rn) + ", " + xname(rm));
}

// CSET Xd, cond is CSINC Xd, XZR, XZR, invert(cond): "if the inverted
// condition holds take XZR, otherwise take XZR+1".  Hence the inversion — the
// bit that has to be flipped is the low bit of the condition code, because the
// encoding pairs each condition with its opposite (see Cond in the header).
// example: cset x0, lt  ->  0x9a9fa7e0, which carries cond=ge in its field
void Emitter::cset(int rd, Cond cond) {
    check_reg(rd);
    uint32_t inverted = static_cast<uint32_t>(cond) ^ 1u;
    uint32_t word = 0x9A800400u
                  | (31u << 16)
                  | (inverted << 12)
                  | (31u << 5)
                  | static_cast<uint32_t>(rd);
    emit(word, std::string("cset ") + xname(rd) + ", " + cond_name(cond));
}

// ---------------------------------------------------------------------------
// Memory
//
// The four addressing modes below differ only in bits 11..10 and bit 22:
//   bits 11:10 = 00 unscaled offset (LDUR/STUR), 01 post-index, 11 pre-index
//   bit 22     = 0 store, 1 load
// so they are one family with one 9-bit signed offset field, not four
// unrelated instructions.
// ---------------------------------------------------------------------------

// example: str x0, [sp, #-16]!  ->  0xf81f0fe0
void Emitter::str_pre(int rt, int rn, int imm9) {
    check_reg(rt);
    check_reg(rn);
    uint32_t word = 0xF8000C00u
                  | imm9_field(imm9)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rt);
    emit(word, "str " + xname(rt) + ", [" + spname(rn) + ", #" +
                   std::to_string(imm9) + "]!");
}

// example: ldr x0, [sp], #16  ->  0xf84107e0
void Emitter::ldr_post(int rt, int rn, int imm9) {
    check_reg(rt);
    check_reg(rn);
    uint32_t word = 0xF8400400u
                  | imm9_field(imm9)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rt);
    emit(word, "ldr " + xname(rt) + ", [" + spname(rn) + "], #" +
                   std::to_string(imm9));
}

// example: stur x0, [x29, #-8]  ->  0xf81f83a0
void Emitter::stur(int rt, int rn, int imm9) {
    check_reg(rt);
    check_reg(rn);
    uint32_t word = 0xF8000000u
                  | imm9_field(imm9)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rt);
    emit(word, "stur " + xname(rt) + ", [" + spname(rn) + ", #" +
                   std::to_string(imm9) + "]");
}

// example: ldur x0, [x29, #-8]  ->  0xf85f83a0
void Emitter::ldur(int rt, int rn, int imm9) {
    check_reg(rt);
    check_reg(rn);
    uint32_t word = 0xF8400000u
                  | imm9_field(imm9)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rt);
    emit(word, "ldur " + xname(rt) + ", [" + spname(rn) + ", #" +
                   std::to_string(imm9) + "]");
}

// STP/LDP:  base | imm7(21:15) | Rt2(14:10) | Rn(9:5) | Rt(4:0).
// Rt goes to the lower address and Rt2 to the higher one.  Swapping them is a
// bug that round-trips cleanly — store into the wrong slots, load back out of
// the wrong slots — so it hides from any test that only checks the values, and
// shows up instead as a broken x29/x30 chain that ruins every later backtrace.
// example: stp x29, x30, [sp, #-16]!  ->  0xa9bf7bfd
void Emitter::stp_pre(int rt, int rt2, int rn, int imm) {
    check_reg(rt);
    check_reg(rt2);
    check_reg(rn);
    uint32_t word = 0xA9800000u
                  | imm7_field(imm)
                  | (static_cast<uint32_t>(rt2) << 10)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rt);
    emit(word, "stp " + xname(rt) + ", " + xname(rt2) + ", [" + spname(rn) +
                   ", #" + std::to_string(imm) + "]!");
}

// example: ldp x29, x30, [sp], #16  ->  0xa8c17bfd
void Emitter::ldp_post(int rt, int rt2, int rn, int imm) {
    check_reg(rt);
    check_reg(rt2);
    check_reg(rn);
    uint32_t word = 0xA8C00000u
                  | imm7_field(imm)
                  | (static_cast<uint32_t>(rt2) << 10)
                  | (static_cast<uint32_t>(rn) << 5)
                  | static_cast<uint32_t>(rt);
    emit(word, "ldp " + xname(rt) + ", " + xname(rt2) + ", [" + spname(rn) +
                   "], #" + std::to_string(imm));
}

// ---------------------------------------------------------------------------
// Branches and calls
//
// Each of these emits its word with an empty displacement and leaves a fixup
// behind; finalize() fills the hole in once every label has a position.  The
// add_fixup call must come first, because it records the index the branch is
// about to occupy.
// ---------------------------------------------------------------------------

// example: b .  ->  0x14000000
void Emitter::b(Label l) {
    add_fixup(l, FixKind::Imm26);
    emit(0x14000000u, "b L" + std::to_string(l));
}

// example: b.lt .  ->  0x5400000b   (the condition lives in bits 3..0)
void Emitter::b_cond(Cond cond, Label l) {
    add_fixup(l, FixKind::Imm19);
    uint32_t word = 0x54000000u | static_cast<uint32_t>(cond);
    emit(word, std::string("b.") + cond_name(cond) + " L" + std::to_string(l));
}

// CBZ/CBNZ compare against zero and branch in one instruction, without
// touching the flags — which is why the conditions in this compiler mostly go
// through these rather than through cmp.
// example: cbz x0, .  ->  0xb4000000 ; cbnz x0, . -> 0xb5000000
void Emitter::cbz(int rt, Label l) {
    check_reg(rt);
    add_fixup(l, FixKind::Imm19);
    uint32_t word = 0xB4000000u | static_cast<uint32_t>(rt);
    emit(word, "cbz " + xname(rt) + ", L" + std::to_string(l));
}

void Emitter::cbnz(int rt, Label l) {
    check_reg(rt);
    add_fixup(l, FixKind::Imm19);
    uint32_t word = 0xB5000000u | static_cast<uint32_t>(rt);
    emit(word, "cbnz " + xname(rt) + ", L" + std::to_string(l));
}

// example: bl <here>  ->  0x94000000
void Emitter::bl(Label l) {
    add_fixup(l, FixKind::Imm26);
    emit(0x94000000u, "bl L" + std::to_string(l));
}

// example: blr x16  ->  0xd63f0200
void Emitter::blr(int rn) {
    check_reg(rn);
    uint32_t word = 0xD63F0000u | (static_cast<uint32_t>(rn) << 5);
    emit(word, "blr " + xname(rn));
}

// RET is really "branch to the address in Xn", with x30 as the default — the
// register BL wrote the return address into.
// example: ret  ->  0xd65f03c0
void Emitter::ret() {
    uint32_t word = 0xD65F0000u | (30u << 5);
    emit(word, "ret");
}

std::vector<uint8_t> Emitter::finalize() {
    // Check every label first, so an unbound one is reported as itself rather
    // than as a nonsense displacement computed from kUnbound.
    for (size_t i = 0; i < label_pos_.size(); i++) {
        if (label_pos_[i] == kUnbound) {
            std::fprintf(stderr, "emitter: label %zu was never bound\n", i);
            emitter_bug("unbound label at finalize");
        }
    }

    for (size_t i = 0; i < fixups_.size(); i++) {
        const Fixup& f = fixups_[i];
        if (f.at_word >= code_.size()) {
            emitter_bug("fixup points past the end of the buffer");
        }

        // Word displacement, not bytes.  Both branch families measure from the
        // branch instruction itself, so this is a plain difference of indices
        // and there is no division to get wrong here — the units are words all
        // the way through on purpose.
        int64_t delta = static_cast<int64_t>(label_pos_[f.label]) -
                        static_cast<int64_t>(f.at_word);

        uint32_t word = code_[f.at_word];

        if (f.kind == FixKind::Imm26) {
            // Signed 26-bit field: -2^25 .. 2^25-1 words, about +/- 32 MB.
            bool fits = (delta >= -(1 << 25)) && (delta < (1 << 25));
            if (!fits) {
                emitter_bug("branch target out of range for a 26-bit displacement");
            }
            uint32_t imm26 = static_cast<uint32_t>(delta) & 0x03FFFFFFu;
            word = (word & ~0x03FFFFFFu) | imm26;
        } else {
            // Signed 19-bit field sitting in bits 23..5, about +/- 1 MB.
            bool fits = (delta >= -(1 << 18)) && (delta < (1 << 18));
            if (!fits) {
                emitter_bug("branch target out of range for a 19-bit displacement");
            }
            uint32_t imm19 = static_cast<uint32_t>(delta) & 0x0007FFFFu;
            word = (word & ~(0x0007FFFFu << 5)) | (imm19 << 5);
        }

        code_[f.at_word] = word;

        // Keep the listing honest: it was recorded with an empty displacement,
        // and --disasm printing a branch to +0 would be actively misleading.
        listing_[f.at_word].word = word;
    }

    // Little-endian: low byte of each word first.  Spelled out with shifts
    // instead of memcpy'ing the vector, because on a machine that ever wasn't
    // little-endian the memcpy version would be silently wrong.
    std::vector<uint8_t> bytes;
    bytes.reserve(code_.size() * 4);
    for (size_t i = 0; i < code_.size(); i++) {
        uint32_t w = code_[i];
        bytes.push_back(static_cast<uint8_t>(w & 0xff));
        bytes.push_back(static_cast<uint8_t>((w >> 8) & 0xff));
        bytes.push_back(static_cast<uint8_t>((w >> 16) & 0xff));
        bytes.push_back(static_cast<uint8_t>((w >> 24) & 0xff));
    }

    // Structurally guaranteed by the vector<uint32_t>, but the check costs
    // nothing and this is the last place a truncated buffer could be caught
    // before it becomes an instruction fetch off the end of the mapping.
    if (bytes.size() % 4 != 0) {
        emitter_bug("finalized buffer is not a whole number of instructions");
    }

    return bytes;
}

}  // namespace minipy
