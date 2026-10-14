#!/bin/sh
# mock_claude_args.sh — records argv and copies the MCP config for Phase 8 tests.
#
# Saves each argument (one per line) to /tmp/aishell_mock_args.txt so tests can
# verify flags like --mcp-config and --strict-mcp-config were passed.
# If --mcp-config <path> is present, copies the config file to
# /tmp/aishell_mock_mcp_config.json before AiShell unlinks the original.
# Returns a valid JSON command proposal so the rest of the at_command flow runs.

ARGS_FILE="/tmp/aishell_mock_args.txt"
MCP_COPY="/tmp/aishell_mock_mcp_config.json"

# Save arguments one per line
{
    for arg in "$@"; do
        printf '%s\n' "$arg"
    done
} > "$ARGS_FILE"

# Find --mcp-config and copy the config before AiShell unlinks it
prev=""
for arg in "$@"; do
    if [ "$prev" = "--mcp-config" ] && [ -f "$arg" ]; then
        cp "$arg" "$MCP_COPY"
        break
    fi
    prev="$arg"
done

printf '{"command":"pwd","explanation":"Prints the current working directory.","risk":"read_only","confidence":1.0}\n'
exit 0
