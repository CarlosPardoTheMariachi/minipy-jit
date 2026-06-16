# minipy-jit

A JIT compiler for a minimal Python-like language (int64-only), targeting
AArch64 (Apple Silicon), written in C++20 with **no third-party dependencies**.
Pipeline: lexer → recursive-descent parser → AST → sema → (tree-walking
interpreter | direct AArch64 machine-code generation into executable memory).

The interpreter is the oracle: every test runs under both engines and their
stdout/stderr/exit status must be byte-identical (differential testing).
The architecture deliberately follows the Stanford CS143 Cool compiler
(stack-machine codegen, two-pass function tables, emit-helper layer).

## Build

```
make            # -O2 (benchmark build)
make debug      # -g -O0
```

## Run

```
./minipy file.mp                 # JIT (default)
./minipy --interp file.mp        # tree-walking interpreter (the oracle)
./minipy --dump-tokens file.mp   # token stream (incl. INDENT/DEDENT)
./minipy --dump-ast   file.mp    # parsed AST with resolved frame slots
./minipy --disasm     file.mp    # emitted AArch64: offset, hex word, mnemonic
./minipy --time       file.mp    # report compile / run milliseconds
```

## Test & benchmark

```
make test       # differential + golden tests (tests/*.mp) and encoding smoke test
make bench      # headline: fib, interp-vs-JIT speedup, compile time separate
```

## Layout

```
src/lexer.*      off-side-rule scanner (INDENT/DEDENT)
src/parser.*     recursive descent, one function per grammar nonterminal
src/ast.*        AST nodes (each Expr carries a `type` field from day one)
src/sema.*       name/arity checks + flat frame-slot assignment (two-pass)
src/interp.*     tree-walking interpreter / differential oracle
src/emitter.*    AArch64 instruction encoder + backpatched labels
src/jit_memory.* macOS MAP_JIT mmap + write-protect toggle + i-cache flush
src/codegen.*    stack-machine walk (accumulator in x0), AAPCS64 frames
src/runtime.*    mp_print / mp_div_zero_abort (called via absolute addr + BLR)
tests/           *.mp with `# expect:` goldens + emit_smoke.cc encoding checks
bench/           fib.mp (canonical benchmark) + timing harness
```

## Status

In progress, built phase by phase:

- [x] Phase 1 — lexer (off-side rule, `--dump-tokens`)
- [ ] Phase 2 — parser + AST (`--dump-ast`)
- [ ] Phase 3 — tree-walking interpreter (the oracle)
- [ ] Phase 4 — sema (names, arity, frame slots)
- [ ] Phase 5+ — emitter, JIT memory, codegen, benchmark harness
