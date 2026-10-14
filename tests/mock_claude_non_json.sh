#!/bin/sh
# Mock Claude Code binary that returns a plain-text error instead of JSON.
# Used by smoke_test.py RB002-RB005 to verify that AiShell detects non-JSON
# output and reports it with a controlled error + sanitized preview.
printf 'MCP connection error: failed to reach tool server\n'
exit 0
