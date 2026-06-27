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
// This header is only the bookkeeping half; the instruction methods arrive
// with their encodings.  Splitting it that way means the label machinery gets
// tested on its own, so a wrong jump target can't be confused with a wrong
// instruction word.
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

class Emitter {
public:
    Emitter() = default;

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
