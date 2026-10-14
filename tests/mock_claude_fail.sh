#!/bin/sh
# Mock Claude that always fails with a generic nonzero exit (retryable).
printf 'Claude error: service temporarily unavailable\n' >&2
exit 2
