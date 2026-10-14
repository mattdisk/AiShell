#define _POSIX_C_SOURCE 200809L

#include "ai_config.h"
#include "command.h"   /* json_escape_print() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ai_config_t g_ai_config;

/* -------------------------------------------------------------------------
 * Defaults
 * ---------------------------------------------------------------------- */

static void set_defaults(ai_config_t *cfg)
{
    cfg->claude_path[0]       = '\0';
    cfg->mcp_enabled          = 0;
    snprintf(cfg->mcp_host, sizeof(cfg->mcp_host), "%s", "127.0.0.1");
    cfg->mcp_port             = 0;
    cfg->mcp_token[0]         = '\0';
    cfg->ai_timeout_ms        = 30000;
    cfg->ai_max_retries       = 6;
    cfg->ai_retry_backoff_ms  = 500;
    cfg->mcp_tool_timeout_ms  = 10000;
    snprintf(cfg->ai_backend, sizeof(cfg->ai_backend), "%s", "mock");
}

/* -------------------------------------------------------------------------
 * Integer parsing — returns fallback on any parse error so callers always
 * get a safe value even when the env var contains non-numeric text.
 * ---------------------------------------------------------------------- */

static int parse_int_safe(const char *s, int fallback)
{
    if (!s || *s == '\0') return fallback;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return fallback;
    return (int)v;
}

/* -------------------------------------------------------------------------
 * apply_key_value — write one AISHELL_* key/value pair into *cfg.
 * ---------------------------------------------------------------------- */

static void apply_key_value(ai_config_t *cfg, const char *key, const char *val)
{
    if (strcmp(key, "AISHELL_CLAUDE_PATH") == 0) {
        snprintf(cfg->claude_path, sizeof(cfg->claude_path), "%s", val);
    } else if (strcmp(key, "AISHELL_MCP_ENABLED") == 0) {
        cfg->mcp_enabled = parse_int_safe(val, cfg->mcp_enabled);
    } else if (strcmp(key, "AISHELL_MCP_HOST") == 0) {
        snprintf(cfg->mcp_host, sizeof(cfg->mcp_host), "%s", val);
    } else if (strcmp(key, "AISHELL_MCP_PORT") == 0) {
        cfg->mcp_port = parse_int_safe(val, cfg->mcp_port);
    } else if (strcmp(key, "AISHELL_MCP_TOKEN") == 0) {
        snprintf(cfg->mcp_token, sizeof(cfg->mcp_token), "%s", val);
        /* token is stored but never logged or printed */
    } else if (strcmp(key, "AISHELL_AI_TIMEOUT_MS") == 0) {
        cfg->ai_timeout_ms = parse_int_safe(val, cfg->ai_timeout_ms);
    } else if (strcmp(key, "AISHELL_AI_MAX_RETRIES") == 0) {
        cfg->ai_max_retries = parse_int_safe(val, cfg->ai_max_retries);
    } else if (strcmp(key, "AISHELL_AI_RETRY_BACKOFF_MS") == 0) {
        cfg->ai_retry_backoff_ms = parse_int_safe(val, cfg->ai_retry_backoff_ms);
    } else if (strcmp(key, "AISHELL_MCP_TOOL_TIMEOUT_MS") == 0) {
        cfg->mcp_tool_timeout_ms = parse_int_safe(val, cfg->mcp_tool_timeout_ms);
    } else if (strcmp(key, "AISHELL_AI_BACKEND") == 0) {
        if (strcmp(val, "mock") == 0 || strcmp(val, "claude") == 0) {
            snprintf(cfg->ai_backend, sizeof(cfg->ai_backend), "%s", val);
        } else {
            fprintf(stderr,
                    "aishell: unknown AI backend '%s', using 'mock'\n", val);
        }
    }
    /* Unknown AISHELL_* keys are silently ignored. */
}

/* -------------------------------------------------------------------------
 * load_env_file — parse a .env file, applying only AISHELL_* entries.
 *
 * Format rules:
 *   - Blank lines are skipped.
 *   - Lines beginning with '#' are comments and are skipped.
 *   - Only lines whose key starts with "AISHELL_" are processed.
 *   - KEY=value (the value is everything after the first '=', with leading
 *     and trailing whitespace trimmed).
 *   - A missing file is silently ignored (not an error).
 * ---------------------------------------------------------------------- */

static void load_env_file(ai_config_t *cfg, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return;  /* .env is optional */

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline / carriage return */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip blank lines and comments */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        /* Only process AISHELL_* lines */
        if (strncmp(p, "AISHELL_", 8) != 0) continue;

        /* Split on the first '=' */
        const char *eq = strchr(p, '=');
        if (!eq) continue;

        size_t klen = (size_t)(eq - p);
        char key[256];
        if (klen >= sizeof(key)) continue;
        memcpy(key, p, klen);
        key[klen] = '\0';

        /* Value: skip leading whitespace, then trim trailing whitespace */
        const char *vstart = eq + 1;
        while (*vstart == ' ' || *vstart == '\t') vstart++;

        char val[4096];
        size_t vlen = strlen(vstart);
        if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
        memcpy(val, vstart, vlen);
        val[vlen] = '\0';
        while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = '\0';

        apply_key_value(cfg, key, val);
    }

    fclose(fp);
}

/* -------------------------------------------------------------------------
 * overlay_env_vars — apply real process environment variables on top of
 * any values already set (by defaults or .env).  getenv() takes precedence.
 * ---------------------------------------------------------------------- */

static void overlay_env_vars(ai_config_t *cfg)
{
    const char *v;

    if ((v = getenv("AISHELL_CLAUDE_PATH"))        != NULL) apply_key_value(cfg, "AISHELL_CLAUDE_PATH",        v);
    if ((v = getenv("AISHELL_MCP_ENABLED"))         != NULL) apply_key_value(cfg, "AISHELL_MCP_ENABLED",         v);
    if ((v = getenv("AISHELL_MCP_HOST"))            != NULL) apply_key_value(cfg, "AISHELL_MCP_HOST",            v);
    if ((v = getenv("AISHELL_MCP_PORT"))            != NULL) apply_key_value(cfg, "AISHELL_MCP_PORT",            v);
    if ((v = getenv("AISHELL_MCP_TOKEN"))           != NULL) apply_key_value(cfg, "AISHELL_MCP_TOKEN",           v);
    if ((v = getenv("AISHELL_AI_TIMEOUT_MS"))       != NULL) apply_key_value(cfg, "AISHELL_AI_TIMEOUT_MS",       v);
    if ((v = getenv("AISHELL_AI_MAX_RETRIES"))      != NULL) apply_key_value(cfg, "AISHELL_AI_MAX_RETRIES",      v);
    if ((v = getenv("AISHELL_AI_RETRY_BACKOFF_MS")) != NULL) apply_key_value(cfg, "AISHELL_AI_RETRY_BACKOFF_MS", v);
    if ((v = getenv("AISHELL_MCP_TOOL_TIMEOUT_MS")) != NULL) apply_key_value(cfg, "AISHELL_MCP_TOOL_TIMEOUT_MS", v);
    if ((v = getenv("AISHELL_AI_BACKEND"))          != NULL) apply_key_value(cfg, "AISHELL_AI_BACKEND",          v);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void ai_config_load(ai_config_t *cfg)
{
    set_defaults(cfg);
    load_env_file(cfg, ".env");
    overlay_env_vars(cfg);
}

void ai_config_dump_json(const ai_config_t *cfg)
{
    printf("{\n");
    printf("  \"claude_path\": \"");     json_escape_print(cfg->claude_path); printf("\",\n");
    printf("  \"mcp_enabled\": %d,\n",  cfg->mcp_enabled);
    printf("  \"mcp_host\": \"");        json_escape_print(cfg->mcp_host);    printf("\",\n");
    printf("  \"mcp_port\": %d,\n",     cfg->mcp_port);
    /* Never print the token value — only indicate whether one is set. */
    printf("  \"mcp_token_set\": %s,\n", cfg->mcp_token[0] ? "true" : "false");
    printf("  \"ai_timeout_ms\": %d,\n",       cfg->ai_timeout_ms);
    printf("  \"ai_max_retries\": %d,\n",      cfg->ai_max_retries);
    printf("  \"ai_retry_backoff_ms\": %d,\n", cfg->ai_retry_backoff_ms);
    printf("  \"mcp_tool_timeout_ms\": %d,\n",  cfg->mcp_tool_timeout_ms);
    printf("  \"ai_backend\": \"");             json_escape_print(cfg->ai_backend); printf("\"\n");
    printf("}\n");
}
