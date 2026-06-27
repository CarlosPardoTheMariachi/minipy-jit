// emitter.cc — label bookkeeping and buffer finalization.
//
// The field positions used here (imm26 in bits 25..0, imm19 in bits 23..5,
// both counting instruction words) were read out of a real assembler rather
// than recalled: assembling `b`, `bl`, `b.lt`, `cbz` and `cbnz` with known
// displacements and looking at the bytes.  A forward branch of +2 words comes
// back as 0x14000002 for B and 0x54000042 for B.LT, which pins both the shift
// and the units.
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
