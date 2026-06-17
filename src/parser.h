// parser.h — recursive-descent parser.
//
// The LALR bison grammar from the Cool compiler (PA2) becomes one hand-written
// parse function per nonterminal.  The grammar is LL-friendly by construction:
// no left recursion, and the `( op ... )*` loops give left associativity.
#ifndef MINIPY_PARSER_H
#define MINIPY_PARSER_H

#include <vector>

#include "ast.h"
#include "lexer.h"

namespace minipy {

// Parse a token stream into a Program.  Top-level statements are gathered, in
// order, into a synthesized __main__ function.  Throws CompileError.
Program parse(const std::vector<Token>& toks);

}  // namespace minipy

#endif  // MINIPY_PARSER_H
