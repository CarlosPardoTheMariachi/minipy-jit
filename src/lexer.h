// lexer.h — token definitions and the off-side-rule scanner.
//
// Batch model: lex() tokenizes the whole source into a vector up front, rather
// than the flex/yylex pull model.  Reason: one line boundary can produce
// several tokens at once (NEWLINE followed by multiple DEDENTs), which is
// awkward to express as return-one-token-per-call.
#ifndef MINIPY_LEXER_H
#define MINIPY_LEXER_H

#include <cstdint>
#include <string>
#include <vector>

namespace minipy {

enum class Tok {
    // keywords
    Def, Return, If, Elif, Else, While, And, Or, Not, True, False,
    // literals / identifiers
    Ident, Integer,
    // operators & punctuation
    Assign, EqEq, NotEq, Lt, Le, Gt, Ge,
    Plus, Minus, Star, SlashSlash, Percent,
    LParen, RParen, Comma, Colon,
    // structural (the off-side rule's output)
    Newline, Indent, Dedent, Eof
};

struct Token {
    Tok kind;
    std::string text;     // lexeme for Ident/keywords/Integer; empty otherwise
    int64_t int_val = 0;  // value for Integer
    int line;             // 1-based, for error messages
};

const char* to_string(Tok kind);

// Tokenize the whole source.  Throws CompileError on a lexical error (bad
// character, tab in indentation, inconsistent dedent).  The stream always
// ends with any outstanding Dedents followed by exactly one Eof.
std::vector<Token> lex(const std::string& src);

}  // namespace minipy

#endif  // MINIPY_LEXER_H
