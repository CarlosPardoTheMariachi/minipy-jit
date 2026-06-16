// main.cc — driver.  Phase 1: only --dump-tokens exists yet.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "error.h"
#include "lexer.h"

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

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dump-tokens") opt_dump_tokens = true;
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "error: unknown flag " << a << "\n";
            return 1;
        } else {
            path = a;
        }
    }
    if (path.empty()) {
        std::cerr << "usage: minipy --dump-tokens file.mp\n";
        return 1;
    }

    std::string src = read_file(path);
    try {
        std::vector<minipy::Token> toks = minipy::lex(src);
        if (opt_dump_tokens) { dump_tokens(toks); return 0; }
        std::cerr << "error: only --dump-tokens is implemented so far\n";
        return 1;
    } catch (const minipy::CompileError& e) {
        std::cerr << path << ":" << e.line << ": error: " << e.what() << "\n";
        return 1;
    }
}
