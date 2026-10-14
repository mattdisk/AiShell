#!/bin/sh
# Mock Claude that returns malformed JSON (starts with '{' but is syntactically
# invalid).  This tests the ai_json_parse_result() parse-error path.
# Content starting with a non-'{' character would be caught earlier by the
# non-JSON check in at_command_handle() — see mock_claude_non_json.sh.
printf '{"command": "ls", oops this is not valid json}\n'
exit 0
