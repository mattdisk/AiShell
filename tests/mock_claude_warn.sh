#!/bin/sh
# Mock Claude that returns a command that triggers a WARN (rm without dangerous flags).
printf '{"command":"rm file.txt","explanation":"Remove a file.","risk":"write","confidence":0.85}\n'
exit 0
