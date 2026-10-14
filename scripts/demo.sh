#!/bin/sh
# AiShell demo script — run after `make` (e.g. `sh scripts/demo.sh`).
# Each section sends commands to the shell via stdin and shows
# the output inline.

# Resolve relative paths against the repository root.
cd "$(dirname "$0")/.." || exit 1

SHELL=./aishell

run() {
    printf '%s\n' "$@" exit | $SHELL
}

echo '=== 1. Built-ins: pwd, cd, echo ==='
run 'pwd' 'cd /tmp' 'pwd' 'echo hello from aishell'

echo ''
echo '=== 2. External command: uname -s ==='
run 'uname -s'

echo ''
echo '=== 3. Pipeline: cat | sort | uniq ==='
printf 'pear\napple\npear\n' > /tmp/aishell_demo.txt
run 'cat /tmp/aishell_demo.txt | sort | uniq'

echo ''
echo '=== 4. Output redirection: echo hello > out.txt ==='
rm -f /tmp/aishell_out.txt
run 'echo hello > /tmp/aishell_out.txt'
echo "(file contents:)"
cat /tmp/aishell_out.txt

echo ''
echo '=== 5. Input redirection: wc -l < demo.txt ==='
run 'wc -l < /tmp/aishell_demo.txt'

echo ''
echo '=== 6. Append redirection: echo again >> out.txt ==='
run 'echo again >> /tmp/aishell_out.txt'
echo "(file contents:)"
cat /tmp/aishell_out.txt

echo ''
echo '=== 7. Background process + jobs ==='
run 'sleep 3 &' 'jobs'

echo ''
echo '=== Done. To test Ctrl+C interactivity, run: ==='
echo '    ./aishell'
echo '    aishell> sleep 100 &'
echo '    aishell> sleep 100   (then press Ctrl+C)'
