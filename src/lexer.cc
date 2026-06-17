// lexer.cc — hand-written scanner with the Python off-side rule.
#include "lexer.h"

#include <cctype>
#include <unordered_map>

#include "error.h"

namespace minipy {

const char* to_string(Tok kind) {
    switch (kind) {
        case Tok::Def: return "def";
        case Tok::Return: return "return";
        case Tok::If: return "if";
        case Tok::Elif: return "elif";
        case Tok::Else: return "else";
        case Tok::While: return "while";
        case Tok::And: return "and";
        case Tok::Or: return "or";
        case Tok::Not: return "not";
        case Tok::True: return "True";
        case Tok::False: return "False";
        case Tok::Ident: return "IDENT";
        case Tok::Integer: return "INTEGER";
        case Tok::Assign: return "=";
        case Tok::EqEq: return "==";
        case Tok::NotEq: return "!=";
        case Tok::Lt: return "<";
        case Tok::Le: return "<=";
        case Tok::Gt: return ">";
        case Tok::Ge: return ">=";
        case Tok::Plus: return "+";
        case Tok::Minus: return "-";
        case Tok::Star: return "*";
        case Tok::SlashSlash: return "//";
        case Tok::Percent: return "%";
        case Tok::LParen: return "(";
        case Tok::RParen: return ")";
        case Tok::Comma: return ",";
        case Tok::Colon: return ":";
        case Tok::Newline: return "NEWLINE";
        case Tok::Indent: return "INDENT";
        case Tok::Dedent: return "DEDENT";
        case Tok::Eof: return "EOF";
    }
    return "?";
}

namespace {

const std::unordered_map<std::string, Tok> kKeywords = {
    {"def", Tok::Def},   {"return", Tok::Return}, {"if", Tok::If},
    {"elif", Tok::Elif}, {"else", Tok::Else},     {"while", Tok::While},
    {"and", Tok::And},   {"or", Tok::Or},         {"not", Tok::Not},
    {"True", Tok::True}, {"False", Tok::False},
};

bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool is_ident_part(char c)  { return std::isalnum((unsigned char)c) || c == '_'; }

}  // namespace

std::vector<Token> lex(const std::string& src) {
    std::vector<Token> toks;
    const size_t n = src.size();
    size_t i = 0;
    int line = 1;
    std::vector<int> indents{0};  // indentation stack; 0 = top level
    bool at_line_start = true;

    while (i < n) {

        if (at_line_start) {
            // We're at the beginning of the line so we need to handle any indentation through spaces first
            size_t j = i;
            int width = 0;
            // count all white spaces
            while (j < n && (src[j] == ' ' || src[j] == '\t')) {
                if (src[j] == '\t')
                    throw CompileError(line, "tab character in indentation");
                width++;
                j++;
            }

            bool end_of_file = j >= n ? true: false;
            if (end_of_file || src[j] == '\n' || src[j] == '#') {
                // If we reach this, it means this line is a useless line so just keep goign 
                // through it and update bookeeing
                // Skip comment text to the newline (no-op for blank line / EOF).
                while (j < n && src[j] != '\n') j++;
                // Consume the '\n' and bump line (skipped if we hit EOF with no newline).
                if (j < n) { j++; line++; }
                i = j;
                continue; // finish useless line 
            }
            // Decide which possible indentation token to emit since this 
            // is clearly a line with actual code afte the indentation
            // and not a useless line
            i = j;
            if (width > indents.back()) {
                indents.push_back(width);
                toks.push_back(Token{Tok::Indent, "", 0, line});
            } else {
                while (width < indents.back()) {
                    indents.pop_back();
                    toks.push_back(Token{Tok::Dedent, "", 0, line});
                }
                if (width != indents.back())
                    throw CompileError(line, "inconsistent dedent");
            }
            at_line_start = false;
            continue;
        }
        
        // NOW WE CAN ACTUALLY START THE LEX STUFF FOR CODE

        // This is the case where there WAS some code stuff but 
        // further in the same line theres more of that
        // spaces, tabs or comment stuff in the middle
        char c = src[i];
        // Mid-line whitespace (tabs are only an error in indentation).
        if (c == ' ' || c == '\t') { i++; continue; }
        // Comment, so ignore this whole part and the next iteration
        // of the algo will hit the c == \n case
        if (c == '#') { while (i < n && src[i] != '\n') i++; continue; }
        if (c == '\n') {
            toks.push_back(Token{Tok::Newline, "", 0, line});
            line++;
            i++;
            at_line_start = true;
            continue;
        }

        // Identifiers and keywords: lex the whole word, then look it up.
        // (Maximal munch for free — no flex-style rule-ordering trap.)
        if (is_ident_start(c)) {
            size_t start = i;
            // this while loop right here is implementing the maximal munch part
            // since it's greedily taking the longest string it can
            while (i < n && is_ident_part(src[i])) i++;
            std::string word = src.substr(start, i - start);
            auto it = kKeywords.find(word);
            Tok kind = (it != kKeywords.end()) ? it->second : Tok::Ident;
            toks.push_back(Token{kind, word, 0, line});
            continue;
        }

        // Integer literals.  Accumulate in uint64_t so an oversized literal
        // wraps mod 2^64 instead of hitting signed-overflow UB.
        if (std::isdigit((unsigned char)c)) {
            size_t start = i;
            uint64_t val = 0;
            while (i < n && std::isdigit((unsigned char)src[i])) {
                val = val * 10u + (uint64_t)(src[i] - '0');
                i++;
            }
            Token t{Tok::Integer, src.substr(start, i - start), (int64_t)val, line};
            toks.push_back(t);
            continue;
        }

        // Operators & punctuation.  Two-char forms checked first = maximal
        // munch (`<=` beats `<`, `//` beats `/`, `==` beats `=`).
        switch (c) {
            case '=':
                if (i + 1 < n && src[i + 1] == '=') {
                    toks.push_back(Token{Tok::EqEq, "", 0, line});
                    i += 2;
                } else {
                    toks.push_back(Token{Tok::Assign, "", 0, line});
                    i += 1;
                }
                continue;
            case '!':
                if (i + 1 < n && src[i + 1] == '=') {
                    toks.push_back(Token{Tok::NotEq, "", 0, line});
                    i += 2;
                    continue;
                }
                throw CompileError(line, "expected '=' after '!'");
            case '<':
                if (i + 1 < n && src[i + 1] == '=') {
                    toks.push_back(Token{Tok::Le, "", 0, line});
                    i += 2;
                } else {
                    toks.push_back(Token{Tok::Lt, "", 0, line});
                    i += 1;
                }
                continue;
            case '>':
                if (i + 1 < n && src[i + 1] == '=') {
                    toks.push_back(Token{Tok::Ge, "", 0, line});
                    i += 2;
                } else {
                    toks.push_back(Token{Tok::Gt, "", 0, line});
                    i += 1;
                }
                continue;
            case '/':
                if (i + 1 < n && src[i + 1] == '/') {
                    toks.push_back(Token{Tok::SlashSlash, "", 0, line});
                    i += 2;
                    continue;
                }
                throw CompileError(line, "single '/' is not an operator (use '//')");
            case '+':
                toks.push_back(Token{Tok::Plus, "", 0, line});
                i++;
                continue;
            case '-':
                toks.push_back(Token{Tok::Minus, "", 0, line});
                i++;
                continue;
            case '*':
                toks.push_back(Token{Tok::Star, "", 0, line});
                i++;
                continue;
            case '%':
                toks.push_back(Token{Tok::Percent, "", 0, line});
                i++;
                continue;
            case '(':
                toks.push_back(Token{Tok::LParen, "", 0, line});
                i++;
                continue;
            case ')':
                toks.push_back(Token{Tok::RParen, "", 0, line});
                i++;
                continue;
            case ',':
                toks.push_back(Token{Tok::Comma, "", 0, line});
                i++;
                continue;
            case ':':
                toks.push_back(Token{Tok::Colon, "", 0, line});
                i++;
                continue;
            default:
                throw CompileError(line,
                    std::string("unexpected character '") + c + "'");
        }
    }

    // EOF: close a dangling last line (no trailing '\n'), drain the stack.
    if (!at_line_start) toks.push_back(Token{Tok::Newline, "", 0, line});
    while (indents.back() > 0) {
        indents.pop_back();
        toks.push_back(Token{Tok::Dedent, "", 0, line});
    }
    toks.push_back(Token{Tok::Eof, "", 0, line});
    return toks;
}

}  // namespace minipy
