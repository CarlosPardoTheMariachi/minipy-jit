# minipy-jit

A JIT for a tiny Python like language. It emits AArch64 machine code by hand into
executable memory and calls it like a normal function pointer. C++20, no third party
deps. No LLVM, no asmjit. Every instruction word gets built bit by bit in
`src/emitter.cc`.

Apple Silicon / macOS only for now. Getting code to run at runtime means dealing with
W^X: a page can be writable or executable, not both at once.

```python
def fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

def main():
    i = 0
    while i < 30:
        print(fib(i))
        i = i + 1

main()
```

## Three ways to run the same program

Same language, three engines. All three have to agree on stdout, stderr, and exit
status. That match is how I know the machine code is doing the right thing.

| | how it runs |
|---|---|
| `--interp` | tree walking interpreter, also the oracle for the tests |
| `--jit` | compile every function up front, then run |
| `--tier` | interpret, count calls, compile a function once it gets hot |

`--tier` is the one that actually feels like a JIT. Nothing gets compiled until the
program itself shows you which functions are worth it.

### Benchmark (`bench/fib.mp`, fib(0..29), Apple Silicon, `-O2`)

```
  interpreter run :    868.186 ms
  jit compile     :      0.018 ms   (ahead of the run)
  jit run         :     11.536 ms
  tier compile    :      0.013 ms   (during the run, 1 function promoted)
  tier run        :     11.585 ms   (includes the compile above, and the warmup)
  speedup, jit    :       75.3x
  speedup, tier   :       74.9x
```

Tiering hits basically the same speed while only compiling one function. `fib` is
where all the time goes. `main` is called once so it never gets promoted.

Two honest notes on that 75x. The interpreter is a dumb tree walker with
`unordered_map<string, int64_t>` frames, so part of the gap is just "naive baseline
vs machine code," not "great interpreter vs great JIT." And the language is int64
only with a fully static call graph, so there is nothing spicy to specialize on at
runtime the way a real JIT would. The point was to build both strategies and be able
to explain the difference.

## How it works

```
source (.mp)
  lexer        off side rule: indent stack turns whitespace into INDENT/DEDENT
  parser       recursive descent, one function per grammar nonterminal
  AST
  sema         two passes: collect signatures (forward refs / recursion),
               then names, arity, and a frame slot for every local

  then either:
    interpreter   walks the tree
  or:
    codegen       stack machine: result in x0, stack left how you found it
    emitter       labels get backpatched: emit the branch with a hole,
                  finalize() fills in the displacements later
    jit_memory    mmap(MAP_JIT), unprotect, copy, protect, flush i cache,
                  cast to int64_t(*)(), call
```

### Stuff worth reading

- **`src/codegen.cc`**: the AST walk. `x0` is the accumulator. For a binary op you
  eval the left, push it, eval the right, pop, combine. The push is what keeps a
  recursive call in the right operand from trashing the left value.
- **`src/tiering.cc`**: call counting and the handoff. When something goes hot, you
  compile everything it can reach too, because a `BL` only works inside the same
  buffer. Call graph is static so you just read the AST. No patchable stubs.
- **`src/jit_memory.cc`**: the macOS `MAP_JIT` dance, with the failure mode for
  skipping each step. Skipping the i cache invalidate is the worst one. Usually
  works, sometimes fails, always works under a debugger.

## Build and run

```
make                             # -O2
make debug                       # -g -O0

./minipy file.mp                 # JIT (default)
./minipy --interp file.mp        # tree walking interpreter
./minipy --tier   file.mp        # mixed mode; --hot=N sets the threshold
./minipy --disasm file.mp        # emitted AArch64: offset, hex word, mnemonic
./minipy --dump-tokens file.mp   # token stream, including INDENT/DEDENT
./minipy --dump-ast    file.mp   # AST with resolved frame slots
./minipy --time        file.mp   # compile and run ms, separately
```

`--hot` makes the warmup tradeoff obvious:

```
./minipy --tier --hot=1      --time bench/fib.mp     # run_ms ~12
./minipy --tier --hot=100000 --time bench/fib.mp     # run_ms ~28
```

## Testing

```
make test     # 23 tests x 3 engines, plus the encoding smoke test
make bench
```

Differential testing is the whole QA story. Every `tests/*.mp` runs under all three
engines. stdout, stderr, and exit status have to match each other and the `# expect:`
goldens in the file. Tier runs with `--hot=2` so even short tests actually tip into
compiled code.

The suite hits the ugly corners machine code gets wrong: `INT64_MIN` negation and
`// -1`, wraparound, truncating division in all four sign combos, short circuit order
via `print` side effects, `return x` for each of the eight param slots, zero init
locals, and div by zero from inside compiled code.

## Language

```ebnf
program     ::= ( NEWLINE | funcdef | statement )*
funcdef     ::= "def" IDENT "(" params? ")" ":" block
block       ::= NEWLINE INDENT statement+ DEDENT
statement   ::= ( assignment | return | expr ) NEWLINE | if_stmt | while_stmt
if_stmt     ::= "if" expr ":" block ( "elif" expr ":" block )* ( "else" ":" block )?
while_stmt  ::= "while" expr ":" block

expr        ::= or_expr
or_expr     ::= and_expr ( "or" and_expr )*
and_expr    ::= not_expr ( "and" not_expr )*
not_expr    ::= "not" not_expr | comparison
comparison  ::= additive ( ( "==" | "!=" | "<" | "<=" | ">" | ">=" ) additive )?
additive    ::= mult ( ( "+" | "-" ) mult )*
mult        ::= unary ( ( "*" | "//" | "%" ) unary )*
unary       ::= "-" unary | primary
primary     ::= INTEGER | "True" | "False" | IDENT | IDENT "(" args? ")" | "(" expr ")"
```

No left recursion, so each nonterminal is one parse function. The `( op ... )*` loops
give you left associativity without being clever about it.

Semantics both engines have to match:

- One type: int64. `True` is 1, `False` is 0, nonzero is truthy.
- `+ - *` wrap on overflow. Interpreter does the math in `uint64_t` so C++ signed
  overflow UB does not eat you. JIT gets wrap for free from the instructions.
- `//` and `%` truncate toward zero. That is C / `SDIV`, not Python floor division,
  on purpose. `INT64_MIN // -1` wraps to `INT64_MIN`.
- Div by zero prints `runtime error: division by zero` and exits 1. `SDIV` does not
  trap on zero (it returns 0), so the JIT emits an explicit check into an abort stub.
- `and` / `or` short circuit and return the deciding operand, not a fake 1/0.
- One flat scope per function. Locals start at 0. Reading a name that is never
  assigned anywhere in the function is a compile time error.
- Functions are top level only, up to 8 params. Recursion and forward refs are fine.

## Layout

```
src/lexer.*       off side rule scanner (INDENT/DEDENT)
src/parser.*      recursive descent, one function per nonterminal
src/ast.*         AST nodes
src/sema.*        name/arity checks + flat frame slots (two pass)
src/interp.*      tree walking interpreter / test oracle
src/emitter.*     AArch64 encoding + backpatched labels
src/codegen.*     stack machine AST walk (accumulator in x0), AAPCS64 frames
src/jit_memory.*  MAP_JIT mmap, write protect toggle, i cache invalidate
src/tiering.*     call counting, hot set, handoff into machine code
src/runtime.*     mp_print / mp_div_zero_abort, absolute address + BLR
tests/            *.mp goldens + emit_smoke.cc (runs hand built instructions)
bench/            fib.mp + timing harness
```

About 3500 lines. Shape is basically the Stanford CS143 Cool compiler (stack machine
codegen, two pass function tables, `emit_*` helpers), except instead of printing MIPS
text you emit AArch64 words.

## Roadmap

Constant folding and a peephole pass. `break` / `continue`, bitwise ops,
`for ... in range`. Chained comparisons and globals. Then the painful stuff: on stack
replacement (so a long running `main` could get compiled mid loop), a small SSA like
IR, and register allocation.

I also want to grow past int64 only and add real types: floats first, then strings
(and maybe lists). That is where it stops being "everything is a register" and you
actually need a heap and a runtime. For the heap, start with plain C `malloc` (same
bottom layer Python and JS sit on; they just carve their own allocators out of bigger
chunks later). Strings and lists also mean a garbage collector, so once those land I
get to actually explore how to build one. Mark and sweep first, and see how far that
takes us.

macOS on Apple Silicon for now. Linux/ARM would need a different memory dance. The
encodings and codegen themselves are fine elsewhere.
