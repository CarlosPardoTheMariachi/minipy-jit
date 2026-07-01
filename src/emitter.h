// emitter.h — builds a buffer of AArch64 instruction words.
//
// This is the Cool compiler's emit_* layer with the payload swapped: same idea
// of one small method per instruction, but instead of printing a line of MIPS
// assembly text it appends a 32-bit machine word to a vector.  Nothing above
// this file ever thinks about bits, and nothing in this file ever thinks about
// the language being compiled.
//
// The part that isn't just "append a word" is labels.  When a branch is
// emitted, the place it jumps to usually hasn't been emitted yet — the body of
// an if is written before the instruction after the if exists — so the branch
// goes into the buffer with a hole where its displacement belongs, and the
// hole is filled in later.  That is what new_label / bind / finalize do:
//
//   new_label()  reserves an id for a spot we can't name yet
//   bind(l)      says "l is right here", recording the current word index
//   a branch     emits its word and records a fixup {which word, which label}
//   finalize()   walks the fixups and patches each displacement in
//
// The displacement patched in is measured in *instruction words*, not bytes —
// forgetting to divide the byte delta by 4 is the classic backpatch bug and it
// lands you four times too far away.
//
#ifndef MINIPY_EMITTER_H
#define MINIPY_EMITTER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace minipy {

// A label is just an index into the emitter's table of positions.  It's an int
// rather than a pointer or an iterator because the table grows while labels are
// outstanding, and anything that could dangle is a worse trade than a name that
// only means something to the emitter that issued it.
using Label = int;

// Register numbers, which is all a register is once you are writing bits: a
// five-bit field.  The names exist so callers never write a bare 29.
//
// The one that bites: number 31 is NOT one register.  Each instruction decides,
// per field, whether a 31 there means the stack pointer or the zero register.
// The immediate-form ADD/SUB read it as SP; the shifted-register forms read it
// as XZR, in every field.  So `add x0, x1, #0` with Rd=31 adjusts the stack,
// while `add x0, x1, x2` with Rd=31 throws its result away.  Both spellings are
// below so the call site says which one it means, but they are the same number
// and the instruction is what disambiguates them.
namespace reg {
constexpr int X0  = 0;
constexpr int X1  = 1;
constexpr int X2  = 2;
constexpr int X3  = 3;
constexpr int X4  = 4;
constexpr int X5  = 5;
constexpr int X6  = 6;
constexpr int X7  = 7;
constexpr int X9  = 9;
constexpr int X16 = 16;   // scratch for absolute-address calls (BLR)
constexpr int FP  = 29;   // x29, frame pointer
constexpr int LR  = 30;   // x30, link register — where BL leaves the return address
constexpr int SP  = 31;   // stack pointer, in the instructions that read 31 that way
constexpr int XZR = 31;   // zero register, in the instructions that read 31 that way
}  // namespace reg

// Condition codes, as they appear in the four-bit `cond` field.  Only the six
// the language can produce are listed; the encoding has fifteen.
//
// The numbering is not arbitrary: conditions come in pairs that differ only in
// the low bit, and flipping that bit inverts the condition (EQ 0000 / NE 0001,
// GE 1010 / LT 1011, GT 1100 / LE 1101).  CSET exploits this — see cset().
enum class Cond : uint32_t {
    EQ = 0,
    NE = 1,
    GE = 10,
    LT = 11,
    GT = 12,
    LE = 13,
};

class Emitter {
public:
    Emitter() = default;

    // -----------------------------------------------------------------------
    // Moves and constants
    // -----------------------------------------------------------------------

    // MOVZ Xd, #imm16, LSL #(16*hw) — write one 16-bit chunk, zero the rest.
    // hw picks which quarter of the register the chunk lands in, 0 through 3.
    void movz(int rd, uint16_t imm16, int hw);

    // MOVK Xd, #imm16, LSL #(16*hw) — write one chunk, KEEP the other three.
    // The k is "keep", and it is the only difference from MOVZ that matters:
    // building a full 64-bit constant means one MOVZ followed by MOVKs.
    void movk(int rd, uint16_t imm16, int hw);

    // Materialize an arbitrary 64-bit value, one to four instructions.  There
    // is no load-a-64-bit-literal instruction — a 32-bit word cannot carry a
    // 64-bit payload — so every constant is assembled from chunks.
    void mov_imm64(int rd, uint64_t value);

    // MOV Xd, Xm between general-purpose registers (an alias of ORR with XZR).
    // Not usable for the stack pointer: 31 in this encoding means XZR.
    void mov_reg(int rd, int rm);

    // -----------------------------------------------------------------------
    // Arithmetic
    // -----------------------------------------------------------------------
    void add_reg(int rd, int rn, int rm);   // Xd = Xn + Xm
    void sub_reg(int rd, int rn, int rm);   // Xd = Xn - Xm
    void mul(int rd, int rn, int rm);       // Xd = Xn * Xm
    void sdiv(int rd, int rn, int rm);      // Xd = Xn / Xm, truncating toward zero
    void msub(int rd, int rn, int rm, int ra);  // Xd = Xa - Xn*Xm  (the % half)
    void neg(int rd, int rm);               // Xd = -Xm

    // ADD/SUB with a 12-bit unsigned immediate.  This is the form that reads
    // register 31 as SP, so it is also how the stack pointer gets adjusted and
    // how `MOV Xd, SP` is spelled (ADD Xd, SP, #0).
    void add_imm(int rd, int rn, uint32_t imm12);
    void sub_imm(int rd, int rn, uint32_t imm12);

    // -----------------------------------------------------------------------
    // Comparison
    // -----------------------------------------------------------------------

    // CMP Xn, Xm — subtract and throw the result away, keeping only the flags.
    void cmp(int rn, int rm);

    // CSET Xd, cond — 1 if the flags satisfy cond, else 0.  Turns the flags,
    // which nothing else in this compiler can read, back into a value.
    void cset(int rd, Cond cond);

    // -----------------------------------------------------------------------
    // Memory
    // -----------------------------------------------------------------------

    // Pre-indexed store / post-indexed load: the stack-machine push and pop.
    // The base register is updated as part of the instruction, which is what
    // makes each of these a single-instruction push or pop.
    void str_pre(int rt, int rn, int imm9);    // STR Xt, [Xn, #imm]!
    void ldr_post(int rt, int rn, int imm9);   // LDR Xt, [Xn], #imm

    // Unscaled signed-offset load/store, for locals at negative offsets from
    // the frame pointer.  The plain LDR/STR immediate form scales an *unsigned*
    // offset, so it cannot reach below the base at all; -8 written into that
    // field silently becomes +32760 and reads memory 32 KB away without
    // faulting.  Every frame slot access therefore goes through these.
    void stur(int rt, int rn, int imm9);       // STUR Xt, [Xn, #imm]
    void ldur(int rt, int rn, int imm9);       // LDUR Xt, [Xn, #imm]

    // Pair store/load, for the frame link.  The immediate is in bytes here and
    // must be a multiple of 8, since the field itself is scaled.
    void stp_pre(int rt, int rt2, int rn, int imm);    // STP Xt,Xt2,[Xn,#imm]!
    void ldp_post(int rt, int rt2, int rn, int imm);   // LDP Xt,Xt2,[Xn],#imm

    // -----------------------------------------------------------------------
    // Branches and calls
    // -----------------------------------------------------------------------
    void b(Label l);                    // unconditional
    void b_cond(Cond cond, Label l);    // branch if the flags satisfy cond
    void cbz(int rt, Label l);          // branch if Xt == 0
    void cbnz(int rt, Label l);         // branch if Xt != 0

    // BL — call something in this same buffer.  The instruction carries a
    // distance, not an address, so the target has to be a label we patch.
    void bl(Label l);

    // BLR — call the address held in a register.  There is no call-an-absolute
    // -address instruction at all, so this plus mov_imm64 is the only way to
    // reach a host function like mp_print, whose address is nowhere near us.
    void blr(int rn);

    void ret();

    // Reserve an id for a position we don't know yet.  Every label handed out
    // must be bound before finalize(), which is checked rather than trusted.
    Label new_label();

    // Define where a label points: the next word to be emitted.  Binding twice
    // is a bug in the caller, not a legal "move the label", so it aborts.
    void bind(Label l);

    // Byte offset of the next instruction.  Codegen needs this to record where
    // each function starts, which is how a call finds its callee.
    size_t here() const { return code_.size() * 4; }

    // Resolve every fixup and hand back the little-endian byte image, ready to
    // be copied into a JitMemory region.  This is where the cheap end-checks
    // live: every label bound, every displacement in range.  They catch whole
    // classes of bug while the failure is still a readable message instead of
    // a jump into the middle of nowhere.
    std::vector<uint8_t> finalize();

    // Human-readable record of what was emitted, kept as we go so --disasm can
    // print it later.  This is deliberately rebuilding the readable .s file
    // that SPIM gave the Cool compiler for free, and it is the only place in
    // the pipeline where assembly text exists at all: bits go to the CPU, text
    // goes to a person, and the text is never read back in.
    struct Line {
        size_t offset;        // byte offset in the buffer
        uint32_t word;        // the instruction word itself
        std::string text;     // mnemonic, for eyes only
    };
    const std::vector<Line>& listing() const { return listing_; }

private:
    // Which field of the instruction the displacement gets patched into.  The
    // two branch families disagree about this, and they disagree about how far
    // they can reach as a result.
    enum class FixKind {
        Imm26,   // B, BL          — bits 25..0,  reach +/- 32 MB
        Imm19,   // B.cond, CBZ/CBNZ — bits 23..5, reach +/- 1 MB
    };

    // Append one instruction word, with its mnemonic for the listing.
    void emit(uint32_t word, std::string text);

    // Record that the *next* word to be emitted branches to l.  Call this
    // immediately before emit() so the recorded index is that instruction's.
    void add_fixup(Label l, FixKind kind);

    std::vector<uint32_t> code_;
    std::vector<Line> listing_;

    // Word index each label was bound at; kUnbound until bind() is called.
    static constexpr int kUnbound = -1;
    std::vector<int> label_pos_;

    struct Fixup {
        size_t at_word;   // index of the branch instruction in code_
        Label label;      // where it wants to go
        FixKind kind;     // which field to patch
    };
    std::vector<Fixup> fixups_;
};

}  // namespace minipy

#endif  // MINIPY_EMITTER_H
