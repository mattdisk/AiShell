#!/bin/sh
# Mock Claude that returns a valid JSON response for testing.
printf '{"command":"pwd","explanation":"Prints the current working directory.","risk":"read_only","confidence":0.95}\n'
exit 0
