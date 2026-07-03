// main.cc — driver.  Runs the front end (lex -> parse -> sema), then either
// dumps an intermediate form or hands the AST to one of the two engines.
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "codegen.h"
#include "error.h"
#include "interp.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"

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
    // Which engine to run.  Three of them now, and they must all produce
    // byte-identical output — that is what the differential tests check.
    enum class Engine { Jit, Interp, Tier };
    Engine engine = Engine::Jit;

    bool opt_disasm = false;
    bool opt_time = false;

    // How many calls a function may make before tiering compiles it.  Low
    // enough that anything genuinely repetitive trips it early, high enough
    // that a handful of calls during startup doesn't.
    int hot_threshold = 100;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dump-tokens") opt_dump_tokens = true;
        else if (a == "--dump-ast") opt_dump_ast = true;
        else if (a == "--interp") engine = Engine::Interp;
        else if (a == "--jit") engine = Engine::Jit;   // the default anyway
        else if (a == "--tier") engine = Engine::Tier;
        else if (a.rfind("--hot=", 0) == 0) {
            hot_threshold = std::atoi(a.substr(6).c_str());
            if (hot_threshold < 1) {
                std::cerr << "error: --hot= needs a positive number\n";
                return 1;
            }
        }
        else if (a == "--disasm") opt_disasm = true;
        else if (a == "--time") opt_time = true;
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "error: unknown flag " << a << "\n";
            return 1;
        } else {
            path = a;
        }
    }
    if (path.empty()) {
        std::cerr << "usage: minipy [--dump-tokens|--dump-ast|--interp|--jit|--tier|--disasm]"
                     " [--hot=N] [--time] file.mp\n";
        return 1;
    }

    std::string src = read_file(path);
    try {
        std::vector<minipy::Token> toks = minipy::lex(src);
        if (opt_dump_tokens) { dump_tokens(toks); return 0; }

        minipy::Program prog = minipy::parse(toks);

        // Before the dump, not after: sema annotates the AST with frame slots,
        // and --dump-ast prints them, so running it first is what makes the
        // dump show the resolution rather than a pile of -1s.
        minipy::Sema(prog).run();
        if (opt_dump_ast) { minipy::dump_ast(prog, std::cout); return 0; }

        // Compile without running, and print what came out.  Useful on its
        // own: when emitted code misbehaves, the first question is whether the
        // instructions are the ones that were intended.
        if (opt_disasm) {
            minipy::CompiledCode code = minipy::compile_program(prog);
            minipy::disasm(code, std::cout);
            return 0;
        }

        if (engine == Engine::Interp) return minipy::run_interpreter(prog, opt_time);
        if (engine == Engine::Tier) return minipy::run_tiered(prog, hot_threshold, opt_time);
        return minipy::run_jit(prog, opt_time);
    } catch (const minipy::CompileError& e) {
        std::cerr << path << ":" << e.line << ": error: " << e.what() << "\n";
        return 1;
    }
}
