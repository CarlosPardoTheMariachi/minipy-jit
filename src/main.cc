// main.cc — driver.  Phase 2: --dump-tokens and --dump-ast exist so far.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "error.h"
#include "lexer.h"
#include "parser.h"

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << path << "\n";
        std::exit(1);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void dump_tokens(const std::vector<minipy::Token>& toks) {
    for (const auto& t : toks) {
        std::cout << t.line << ": " << minipy::to_string(t.kind);
        if (t.kind == minipy::Tok::Ident) std::cout << " '" << t.text << "'";
        if (t.kind == minipy::Tok::Integer) std::cout << " " << t.int_val;
        std::cout << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    bool opt_dump_tokens = false;
    bool opt_dump_ast = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dump-tokens") opt_dump_tokens = true;
        else if (a == "--dump-ast") opt_dump_ast = true;
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "error: unknown flag " << a << "\n";
            return 1;
        } else {
            path = a;
        }
    }
    if (path.empty()) {
        std::cerr << "usage: minipy [--dump-tokens|--dump-ast] file.mp\n";
        return 1;
    }

    std::string src = read_file(path);
    try {
        std::vector<minipy::Token> toks = minipy::lex(src);
        if (opt_dump_tokens) { dump_tokens(toks); return 0; }

        minipy::Program prog = minipy::parse(toks);
        if (opt_dump_ast) { minipy::dump_ast(prog, std::cout); return 0; }

        std::cerr << "error: only --dump-tokens and --dump-ast are implemented so far\n";
        return 1;
    } catch (const minipy::CompileError& e) {
        std::cerr << path << ":" << e.line << ": error: " << e.what() << "\n";
        return 1;
    }
}
