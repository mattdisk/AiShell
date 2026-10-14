#!/bin/sh
# Mock Claude Code binary that sleeps before responding.
# Used by smoke_test.py RB001 to verify that the heartbeat fires during a
# long-running Claude invocation.  Sleep duration must exceed
# AISHELL_DEBUG_HEARTBEAT_MS to guarantee at least one heartbeat line.
sleep 3
printf '{"command":"pwd","explanation":"Prints the current working directory.","risk":"read_only","confidence":1.0}\n'
