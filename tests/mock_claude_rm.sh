#!/bin/sh
# Mock Claude that returns a dangerous command (blocked by the safety validator).
printf '{"command":"rm -rf /tmp/test","explanation":"Remove test directory.","risk":"destructive","confidence":0.9}\n'
exit 0
