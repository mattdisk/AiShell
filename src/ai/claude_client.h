#ifndef AISHELL_CLAUDE_CLIENT_H
#define AISHELL_CLAUDE_CLIENT_H

#include <stddef.h>
#include "ai_config.h"

typedef enum {
    CLAUDE_OK = 0,
    CLAUDE_NOT_FOUND,       /* binary not found (ENOENT / exit 127) */
    CLAUDE_NOT_LOGGED_IN,   /* stderr contains authentication error */
    CLAUDE_TIMEOUT,         /* per-attempt wall-clock timeout exceeded */
    CLAUDE_NONZERO,         /* Claude exited non-zero (no auth error) */
    CLAUDE_EMPTY_OUTPUT,    /* Claude exited 0 but stdout was empty */
    CLAUDE_OUTPUT_TRUNCATED,/* stdout exceeded buffer; output was cut */
    CLAUDE_EXEC_FAILED      /* fork/pipe/exec setup error */
} claude_status_t;

typedef struct {
    claude_status_t status;
    int             exit_code;
    int             timed_out;
    char            stdout_buf[8192];
    char            stderr_buf[4096];
} claude_result_t;

/*
 * Fork and exec Claude Code in headless mode with nl_prompt as the user
 * instruction.  Captures stdout and stderr separately.  Enforces
 * cfg->ai_timeout_ms per call.  Never passes API keys on the command line.
 * When mcp_config_path is non-NULL and non-empty, passes --mcp-config and
 * --strict-mcp-config to Claude and uses the MCP-aware prompt template.
 * Fills *out and returns the status code.
 */
claude_status_t claude_invoke(const char *nl_prompt,
                               const ai_config_t *cfg,
                               const char *mcp_config_path,
                               claude_result_t *out);

const char *claude_status_name(claude_status_t status);

/* Return 1 if the caller should retry on this status, 0 if permanent. */
int claude_status_is_retryable(claude_status_t status);

#endif /* AISHELL_CLAUDE_CLIENT_H */
