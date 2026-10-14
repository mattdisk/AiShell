#!/usr/bin/env bash
# demo_ast.sh — Grammar-Based Parsing and AST Demo (with AST visualization)
#
# Usage:
#   bash scripts/demo_ast.sh           # run all demo steps with AST debug mode
#   AISHELL_AST=0 bash scripts/demo_ast.sh  # skip AST output (just behaviour)

set -euo pipefail

# Resolve relative paths (./aishell, ast.dot) against the repository root.
cd "$(dirname "$0")/.." || exit 1

SHELL_BIN="./aishell"

if [ ! -x "$SHELL_BIN" ]; then
    echo "ERROR: $SHELL_BIN not found. Run 'make' first." >&2
    exit 1
fi

# Create a small demo file used by later steps
cat > /tmp/aishell_demo_ast.txt <<'EOF'
apple
banana
cherry
apricot
avocado
EOF

sep() { printf '\n\e[1;36m=== %s ===\e[0m\n' "$*"; }

run_demo() {
    # Feed the script to aishell with AISHELL_AST=1 so the AST is shown.
    AISHELL_AST=1 "$SHELL_BIN" <<'DEMO'
# -----------------------------------------------------------------------
# Step 1: simple command → AST_COMMAND node
# -----------------------------------------------------------------------
echo hello

# -----------------------------------------------------------------------
# Step 2: pipeline → AST_PIPELINE node
# -----------------------------------------------------------------------
cat /tmp/aishell_demo_ast.txt | grep apple

# -----------------------------------------------------------------------
# Step 3: redirection over pipeline → Redirect wrapping Pipeline
# -----------------------------------------------------------------------
cat /tmp/aishell_demo_ast.txt | grep a > /tmp/aishell_ast_out.txt
echo "--- contents of /tmp/aishell_ast_out.txt ---"
cat /tmp/aishell_ast_out.txt

# -----------------------------------------------------------------------
# Step 4: background job → AST_BACKGROUND node
# -----------------------------------------------------------------------
sleep 1 &
jobs

# -----------------------------------------------------------------------
# Step 5: environment variable assignment → AST_ASSIGNMENT node
# -----------------------------------------------------------------------
GREETING=hello
echo $GREETING

# -----------------------------------------------------------------------
# Step 6: variable expansion in pipeline
# -----------------------------------------------------------------------
PATTERN=apple
cat /tmp/aishell_demo_ast.txt | grep $PATTERN

# -----------------------------------------------------------------------
# Step 7: syntax errors — useful messages instead of crashes
# -----------------------------------------------------------------------
ls |
echo missing >

# -----------------------------------------------------------------------
# Step 8: combined: redirect + background + var expansion
# -----------------------------------------------------------------------
OUTFILE=/tmp/aishell_ast_bg.txt
cat /tmp/aishell_demo_ast.txt > $OUTFILE &
wait
cat $OUTFILE

ast off
exit
DEMO
}

sep "AiShell – Grammar-Based Parsing and AST Demo"
echo "Each command shows:"
echo "  1. A text AST tree (proves grammar-based parsing)"
echo "  2. The ast.dot Graphviz file is updated after each command"
echo "  3. The command executes normally"
echo ""
echo "Press Enter to start..."
read -r _

run_demo

sep "DOT file generated (last command)"
cat ast.dot

if command -v dot > /dev/null 2>&1; then
    dot -Tpng ast.dot -o ast.png
    sep "PNG generated: ast.png (open with your image viewer)"
else
    sep "Graphviz 'dot' not found — install with: brew install graphviz"
    echo "Then run: dot -Tpng ast.dot -o ast.png"
fi

sep "Demo complete"
echo "Key files:"
echo "  ast.dot  — last Graphviz DOT file"
echo "  ast.png  — last AST image (if Graphviz installed)"
echo ""
echo "Enable AST mode in aishell interactively:"
echo "  ./aishell --ast          # start REPL with AST mode on"
echo "  AISHELL_AST=1 ./aishell  # same via environment variable"
echo "  ast on   (inside aishell) # toggle on"
echo "  ast off  (inside aishell) # toggle off"
