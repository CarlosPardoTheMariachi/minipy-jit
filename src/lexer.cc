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

    auto push = [&](Tok k, std::string text = "") {
        toks.push_back(Token{k, std::move(text), 0, line});
    };
    auto peek = [&](size_t k) -> char { return i + k < n ? src[i + k] : '\0'; };

    while (i < n) {
        // --- Start of a physical line: run the off-side rule. ---
        if (at_line_start) {
            size_t j = i;
            int width = 0;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) {
                if (src[j] == '\t')
                    throw CompileError(line, "tab character in indentation");
                width++;
                j++;
            }
            // Blank or comment-only line: no tokens at all, no indent logic.
            if (j >= n || src[j] == '\n' || src[j] == '#') {
                while (j < n && src[j] != '\n') j++;
                if (j < n) { j++; line++; }
                i = j;
                continue;
            }
            // Real logical line: reconcile width against the stack top.
            i = j;
            if (width > indents.back()) {
                indents.push_back(width);
                push(Tok::Indent);
            } else {
                while (width < indents.back()) {
                    indents.pop_back();
                    push(Tok::Dedent);
                }
                if (width != indents.back())
                    throw CompileError(line, "inconsistent dedent");
            }
            at_line_start = false;
            continue;
        }

        char c = src[i];
        // Mid-line whitespace (tabs are only an error in indentation).
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '#') { while (i < n && src[i] != '\n') i++; continue; }
        if (c == '\n') {
            push(Tok::Newline);
            line++;
            i++;
            at_line_start = true;
            continue;
        }

        // Identifiers and keywords: lex the whole word, then look it up.
        // (Maximal munch for free — no flex-style rule-ordering trap.)
        if (is_ident_start(c)) {
            size_t start = i;
            while (i < n && is_ident_part(src[i])) i++;
            std::string word = src.substr(start, i - start);
            auto it = kKeywords.find(word);
            push(it != kKeywords.end() ? it->second : Tok::Ident, word);
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
                if (peek(1) == '=') { push(Tok::EqEq);  i += 2; }
                else                { push(Tok::Assign); i += 1; }
                continue;
            case '!':
                if (peek(1) == '=') { push(Tok::NotEq); i += 2; continue; }
                throw CompileError(line, "expected '=' after '!'");
            case '<':
                if (peek(1) == '=') { push(Tok::Le); i += 2; }
                else                { push(Tok::Lt); i += 1; }
                continue;
            case '>':
                if (peek(1) == '=') { push(Tok::Ge); i += 2; }
                else                { push(Tok::Gt); i += 1; }
                continue;
            case '/':
                if (peek(1) == '/') { push(Tok::SlashSlash); i += 2; continue; }
                throw CompileError(line, "single '/' is not an operator (use '//')");
            case '+': push(Tok::Plus);    i++; continue;
            case '-': push(Tok::Minus);   i++; continue;
            case '*': push(Tok::Star);    i++; continue;
            case '%': push(Tok::Percent); i++; continue;
            case '(': push(Tok::LParen);  i++; continue;
            case ')': push(Tok::RParen);  i++; continue;
            case ',': push(Tok::Comma);   i++; continue;
            case ':': push(Tok::Colon);   i++; continue;
            default:
                throw CompileError(line,
                    std::string("unexpected character '") + c + "'");
        }
    }

    // EOF: close a dangling last line (no trailing '\n'), drain the stack.
    if (!at_line_start) push(Tok::Newline);
    while (indents.back() > 0) { indents.pop_back(); push(Tok::Dedent); }
    push(Tok::Eof);
    return toks;
}

}  // namespace minipy
