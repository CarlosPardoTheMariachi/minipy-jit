#!/usr/bin/env bash
# run.sh — differential test runner.  For every tests/*.mp:
#   1. run under --interp, --jit, and --tier
#   2. assert stdout, stderr, and exit status are byte-identical between them
#   3. assert stdout/stderr/exit match the embedded golden:
#        # expect: <stdout line>
#        # expect-stderr: <stderr line>
#        # expect-exit: <code>   (default 0)
set -u
cd "$(dirname "$0")/.."
BIN=./minipy
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0

# --- encoding smoke test (hand-emitted AArch64) ------------------------------
clang++ -std=c++20 -O2 -o "$TMP/emit_smoke" tests/emit_smoke.cc src/emitter.cc src/jit_memory.cc 2>/dev/null
if "$TMP/emit_smoke" >/dev/null 2>&1; then
    echo "  ok   emit_smoke (encodings)"; pass=$((pass+1))
else
    echo "  FAIL emit_smoke (encodings)"; fail=$((fail+1))
fi

# --- differential + golden tests ---------------------------------------------
for f in tests/*.mp; do
    name=$(basename "$f")

    "$BIN" --interp "$f" >"$TMP/io" 2>"$TMP/ie"; ic=$?
    "$BIN" --jit    "$f" >"$TMP/jo" 2>"$TMP/je"; jc=$?
    # --hot=2 so even a test that calls a function twice crosses into compiled
    # code.  At the default threshold these programs are all far too short to
    # promote anything, and the boundary would never get exercised.
    "$BIN" --tier --hot=2 "$f" >"$TMP/to" 2>"$TMP/te"; tc=$?

    # Golden expectations extracted from the source comments.
    grep '^# expect: '        "$f" | sed 's/^# expect: //'        >"$TMP/exp_out"
    grep '^# expect-stderr: ' "$f" | sed 's/^# expect-stderr: //' >"$TMP/exp_err"
    exp_exit=$(grep '^# expect-exit: ' "$f" | sed 's/^# expect-exit: //')
    exp_exit=${exp_exit:-0}

    ok=1; why=""
    if ! diff -q "$TMP/io" "$TMP/jo" >/dev/null; then ok=0; why="stdout interp!=jit"; fi
    if ! diff -q "$TMP/ie" "$TMP/je" >/dev/null; then ok=0; why="stderr interp!=jit"; fi
    if [ "$ic" != "$jc" ]; then ok=0; why="exit interp($ic)!=jit($jc)"; fi
    if ! diff -q "$TMP/io" "$TMP/to" >/dev/null; then ok=0; why="stdout interp!=tier"; fi
    if ! diff -q "$TMP/ie" "$TMP/te" >/dev/null; then ok=0; why="stderr interp!=tier"; fi
    if [ "$ic" != "$tc" ]; then ok=0; why="exit interp($ic)!=tier($tc)"; fi
    if ! diff -q "$TMP/exp_out" "$TMP/io" >/dev/null; then ok=0; why="stdout!=golden"; fi
    if [ -s "$TMP/exp_err" ] && ! diff -q "$TMP/exp_err" "$TMP/ie" >/dev/null; then ok=0; why="stderr!=golden"; fi
    if [ "$ic" != "$exp_exit" ]; then ok=0; why="exit($ic)!=golden($exp_exit)"; fi

    if [ "$ok" = 1 ]; then
        echo "  ok   $name"; pass=$((pass+1))
    else
        echo "  FAIL $name — $why"; fail=$((fail+1))
        echo "       interp stdout:"; sed 's/^/         /' "$TMP/io"
        echo "       jit stdout:";    sed 's/^/         /' "$TMP/jo"
    fi
done

echo "-----------------------------------------"
echo "  $pass passed, $fail failed"
[ "$fail" = 0 ]
