#ifndef AISHELL_AI_CONFIG_H
#define AISHELL_AI_CONFIG_H

/*
 * ai_config_t — runtime configuration for the natural-language command mode.
 *
 * Populated by ai_config_load().  Fields are not yet consumed by any code
 * path in Phase 2; they will be wired in during later phases.
 */
typedef struct {
    char claude_path[4096]; /* path to the 'claude' binary; empty = search PATH */
    int  mcp_enabled;       /* non-zero to start the local MCP server */
    char mcp_host[64];      /* MCP server bind address (must be loopback) */
    int  mcp_port;          /* MCP server port; 0 = auto-select */
    char mcp_token[256];    /* MCP auth token; never printed in logs */
    int  ai_timeout_ms;     /* per-attempt Claude invocation timeout */
    int  ai_max_retries;    /* maximum retry attempts for transient failures */
    int  ai_retry_backoff_ms; /* base exponential-backoff delay in ms */
    int  mcp_tool_timeout_ms; /* timeout for individual MCP tool calls */
    char ai_backend[32];     /* "mock" or "claude" */
} ai_config_t;

/* Global config instance populated at startup. */
extern ai_config_t g_ai_config;

/*
 * ai_config_load — populate *cfg with configuration from three sources
 * applied in priority order (lowest to highest):
 *   1. Compiled-in defaults
 *   2. Optional ".env" file in the current working directory
 *   3. Real environment variables (getenv)
 *
 * Silently ignores a missing ".env" file.
 * Never fails or aborts; always produces a fully initialised struct.
 */
void ai_config_load(ai_config_t *cfg);

/*
 * ai_config_dump_json — print *cfg to stdout as a JSON object.
 *
 * Used by the "--dump-config" flag for testing.
 * The mcp_token value is never printed; instead "mcp_token_set" (bool)
 * indicates whether a non-empty token was loaded.
 */
void ai_config_dump_json(const ai_config_t *cfg);

#endif /* AISHELL_AI_CONFIG_H */
