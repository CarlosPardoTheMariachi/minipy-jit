# Makefile — minipy-jit.
# Apple clang, C++20, standard library only (no LLVM, no asmjit).
CXX      := clang++
CXXSTD   := -std=c++20
WARN     := -Wall -Wextra
# -O2 for honest benchmark numbers; `make debug` swaps in -g -O0.
OPT      := -O2
CXXFLAGS := $(CXXSTD) $(WARN) $(OPT)

SRC := $(wildcard src/*.cc)
OBJ := $(SRC:.cc=.o)
BIN := minipy

.PHONY: all debug clean test bench
all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ)

src/%.o: src/%.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

debug: OPT := -g -O0
debug: clean $(BIN)

test: $(BIN)
	@tests/run.sh

bench: $(BIN)
	@bench/run.sh

clean:
	rm -f $(OBJ) $(BIN)
