#!/usr/bin/env bash
# run.sh — the headline benchmark.  Verifies interp and JIT produce
# identical output, then reports interpreter-vs-JIT run time and the speedup.
# JIT compile time is reported separately from run time.
set -u
cd "$(dirname "$0")/.."
BIN=./minipy
PROG=${1:-bench/fib.mp}
REPS=${2:-5}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

echo "benchmark: $PROG   (best of $REPS)"

# 1. Correctness gate: all three engines must be byte-identical.
"$BIN" --interp "$PROG" >"$TMP/io" 2>/dev/null
"$BIN" --jit    "$PROG" >"$TMP/jo" 2>/dev/null
"$BIN" --tier   "$PROG" >"$TMP/to" 2>/dev/null
if diff -q "$TMP/io" "$TMP/jo" >/dev/null && diff -q "$TMP/io" "$TMP/to" >/dev/null; then
    echo "  output check: interp == jit == tier  ✓"
else
    echo "  output check: MISMATCH — aborting benchmark"
    diff "$TMP/io" "$TMP/jo" | head; diff "$TMP/io" "$TMP/to" | head; exit 1
fi

# min over REPS of a "key=value" field printed on stderr.
best() { # $1=engine-flag  $2=field
    local best=""
    for _ in $(seq 1 "$REPS"); do
        v=$("$BIN" "$1" --time "$PROG" 2>&1 >/dev/null | grep -oE "$2=[0-9.]+" | cut -d= -f2)
        if [ -z "$best" ] || awk "BEGIN{exit !($v < $best)}"; then best=$v; fi
    done
    echo "$best"
}

interp_run=$(best --interp run_ms)
jit_run=$(best --jit run_ms)
jit_comp=$(best --jit compile_ms)
tier_run=$(best --tier run_ms)
tier_comp=$(best --tier compile_ms)
tier_n=$("$BIN" --tier --time "$PROG" 2>&1 >/dev/null | grep -oE 'compiled_funcs=[0-9]+' | cut -d= -f2)

printf "  interpreter run : %10.3f ms\n" "$interp_run"
printf "  jit compile     : %10.3f ms   (ahead of the run)\n" "$jit_comp"
printf "  jit run         : %10.3f ms\n" "$jit_run"
printf "  tier compile    : %10.3f ms   (during the run, %s function(s) promoted)\n" "$tier_comp" "$tier_n"
printf "  tier run        : %10.3f ms   (includes the compile above, and the warmup)\n" "$tier_run"
awk "BEGIN{printf \"  speedup, jit    : %10.1fx  (%.1fx incl. compile)\n\", $interp_run/$jit_run, $interp_run/($jit_run+$jit_comp)}"
awk "BEGIN{printf \"  speedup, tier   : %10.1fx\n\", $interp_run/$tier_run}"
