#!/bin/sh
# Mock Claude that simulates a not-logged-in authentication error.
printf 'Error: not logged in. Please authenticate with your Anthropic account.\n' >&2
exit 1
