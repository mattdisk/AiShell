#!/usr/bin/env bash
# tests/test_ast.sh — Session 7 regression and feature tests
#
# Usage:  bash tests/test_ast.sh
# Exit 0 → all tests passed; non-zero → at least one failure.

set -uo pipefail
SHELL_BIN="./aishell"
PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); printf '[PASS] %s\n' "$*"; }
fail() { FAIL=$((FAIL+1)); printf '[FAIL] %s\n' "$*"; }

# strip_output: remove prompt prefixes, welcome line, and blank lines.
# When a command produces no output (e.g. assignment), two prompts appear on
# one line: "aishell> aishell> nextoutput".  Use 'g' flag to strip all
# occurrences of "aishell> " globally so the output is clean.
strip_output() {
    sed 's/aishell> //g' | grep -v '^Welcome' | grep -v '^$'
}

run() {
    # run_aishell <expected_output_pattern> <input_line>
    local pattern="$1"; shift
    local input="$*"
    local out
    out=$(printf '%s\nexit\n' "$input" | "$SHELL_BIN" 2>&1 | strip_output)
    if echo "$out" | grep -q "$pattern"; then
        pass "$input  →  matches /$pattern/"
    else
        fail "$input  →  expected /$pattern/, got: $out"
    fi
}

run_exact() {
    local expected="$1"; shift
    local input="$*"
    local out
    out=$(printf '%s\nexit\n' "$input" | "$SHELL_BIN" 2>/dev/null | strip_output | head -1)
    if [ "$out" = "$expected" ]; then
        pass "$input  →  '$expected'"
    else
        fail "$input  →  expected '$expected', got '$out'"
    fi
}

if [ ! -x "$SHELL_BIN" ]; then
    echo "ERROR: $SHELL_BIN not found. Run 'make' first." >&2
    exit 1
fi

echo "Running AiShell Session 7 tests..."
echo ""

# -----------------------------------------------------------------------
# 1. Basic command execution still works
# -----------------------------------------------------------------------
echo "--- Basic commands ---"
run_exact "hello" "echo hello"
run_exact "hello world" "echo hello world"
run "^/" "pwd"

# -----------------------------------------------------------------------
# 2. Pipelines
# -----------------------------------------------------------------------
echo ""
echo "--- Pipelines ---"
run "hello" "echo hello | grep hello"
run "hello" "echo hello world | grep hello"
# multi-stage
out=$(printf 'echo hello world | grep hello | grep world\nexit\n' | \
      "$SHELL_BIN" 2>/dev/null | strip_output | head -1)
if [ "$out" = "hello world" ]; then
    pass "echo hello world | grep hello | grep world  →  'hello world'"
else
    fail "echo hello world | grep hello | grep world  →  expected 'hello world', got '$out'"
fi

# -----------------------------------------------------------------------
# 3. Redirections
# -----------------------------------------------------------------------
echo ""
echo "--- Redirections ---"
TMPF=$(mktemp)
printf 'echo hello > %s\nexit\n' "$TMPF" | "$SHELL_BIN" > /dev/null 2>&1
if [ "$(cat "$TMPF")" = "hello" ]; then
    pass "echo hello > file  →  file contains 'hello'"
else
    fail "echo hello > file  →  expected 'hello', got '$(cat "$TMPF")'"
fi

# Append
printf 'echo first > %s\necho second >> %s\nexit\n' "$TMPF" "$TMPF" | "$SHELL_BIN" > /dev/null 2>&1
if grep -q "first" "$TMPF" && grep -q "second" "$TMPF"; then
    pass ">> append  →  both lines present"
else
    fail ">> append  →  missing content: $(cat "$TMPF")"
fi

# Input redirect
printf 'hello\n' > "$TMPF"
out=$(printf 'grep hello < %s\nexit\n' "$TMPF" | "$SHELL_BIN" 2>/dev/null | \
      strip_output | head -1)
if [ "$out" = "hello" ]; then
    pass "grep hello < file  →  'hello'"
else
    fail "grep hello < file  →  expected 'hello', got '$out'"
fi
rm -f "$TMPF"

# -----------------------------------------------------------------------
# 4. Background jobs
# -----------------------------------------------------------------------
echo ""
echo "--- Background jobs ---"
out=$(printf 'sleep 3 &\njobs\nexit\n' | "$SHELL_BIN" 2>/dev/null | strip_output)
if echo "$out" | grep -q "sleep"; then
    pass "sleep & + jobs  →  job listed"
else
    fail "sleep & + jobs  →  expected job in list, got: $out"
fi

# -----------------------------------------------------------------------
# 5. Environment variable assignment and expansion
# -----------------------------------------------------------------------
echo ""
echo "--- Environment variables ---"
out=$(printf 'MYVAR=hello\necho $MYVAR\nexit\n' | "$SHELL_BIN" 2>/dev/null | \
      strip_output | tail -1)
if [ "$out" = "hello" ]; then
    pass "MYVAR=hello; echo \$MYVAR  →  'hello'"
else
    fail "MYVAR=hello; echo \$MYVAR  →  expected 'hello', got '$out'"
fi

# Expansion in pipeline
out=$(printf 'PATTERN=hello\necho hello world | grep $PATTERN\nexit\n' | \
      "$SHELL_BIN" 2>/dev/null | strip_output | tail -1)
if [ "$out" = "hello world" ]; then
    pass "PATTERN=hello; echo hello world | grep \$PATTERN  →  'hello world'"
else
    fail "PATTERN=hello; echo hello world | grep \$PATTERN  →  expected 'hello world', got '$out'"
fi

# Empty variable expands to empty
run "" "EMPTY=\necho \$EMPTY"

# -----------------------------------------------------------------------
# 6. Syntax error detection
# -----------------------------------------------------------------------
echo ""
echo "--- Syntax error detection ---"
run "syntax error" "ls |"
run "syntax error" "echo hi >"
run "syntax error" "echo hi >>"
run "syntax error" "echo hi <"

# -----------------------------------------------------------------------
# 7. AST debug mode — ast on/off builtin
# -----------------------------------------------------------------------
echo ""
echo "--- AST debug mode ---"
out=$(printf 'ast on\necho test\nast off\nexit\n' | "$SHELL_BIN" 2>/dev/null)
if echo "$out" | grep -q "Command: echo test"; then
    pass "ast on → shows AST text tree"
else
    fail "ast on → expected 'Command: echo test' in output, got: $out"
fi
if echo "$out" | grep -q "AST debug mode: on"; then
    pass "ast on → prints confirmation"
else
    fail "ast on → expected confirmation message"
fi

# AST mode via env var
out=$(printf 'echo hello\nexit\n' | AISHELL_AST=1 "$SHELL_BIN" 2>/dev/null)
if echo "$out" | grep -q "Command: echo hello"; then
    pass "AISHELL_AST=1 → shows AST text tree"
else
    fail "AISHELL_AST=1 → expected 'Command: echo hello' in AST output"
fi

# -----------------------------------------------------------------------
# 8. AST DOT file generation
# -----------------------------------------------------------------------
echo ""
echo "--- DOT file generation ---"
rm -f ast.dot
printf 'ast on\necho hello world\nexit\n' | "$SHELL_BIN" > /dev/null 2>&1
if [ -f ast.dot ] && grep -q "digraph AST" ast.dot; then
    pass "ast.dot generated with digraph header"
else
    fail "ast.dot not generated or missing 'digraph AST'"
fi
if grep -q 'node0 \[label=' ast.dot; then
    pass "ast.dot contains node label"
else
    fail "ast.dot missing node labels"
fi

# -----------------------------------------------------------------------
# 9. Existing builtins still work (non-regression)
# -----------------------------------------------------------------------
echo ""
echo "--- Non-regression: existing builtins ---"
run "^/" "pwd"
run "[0-9][0-9]:[0-9][0-9]" "date"
out=$(printf 'export TESTVAR=42\nenv\nexit\n' | "$SHELL_BIN" 2>/dev/null | strip_output | grep TESTVAR || true)
if echo "$out" | grep -q "42"; then
    pass "export TESTVAR=42 → appears in env"
else
    fail "export TESTVAR=42 → not found in env output: $out"
fi

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------
echo ""
echo "========================================"
echo "Results: $PASS passed, $FAIL failed"
echo "========================================"
[ "$FAIL" -eq 0 ]
