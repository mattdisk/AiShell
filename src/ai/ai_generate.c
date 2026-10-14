#define _POSIX_C_SOURCE 200809L

#include "ai_generate.h"
#include "ai_json.h"
#include "ai_validate.h"
#include "ai_config.h"
#include "claude_client.h"
#include "ai/mcp_server.h"
#include "exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define AT_CMDBUF 512

/* -------------------------------------------------------------------------
 * Mock backend
 * ---------------------------------------------------------------------- */

/*
 * Returns a hard-coded JSON response.  Set AISHELL_MOCK_AI_JSON in the
 * environment to override the response for testing.  Replace or bypass this
 * function in a later phase once the Claude backend is the default.
 */
static const char *mock_generate(const char *nl_input)
{
    (void)nl_input;
    const char *override = getenv("AISHELL_MOCK_AI_JSON");
    if (override) return override;
    return "{\"command\":\"pwd\","
           "\"explanation\":\"Prints the current working directory.\","
           "\"risk\":\"read_only\","
           "\"confidence\":1.0}";
}

/* -------------------------------------------------------------------------
 * Claude backend with exponential-backoff retry
 * ---------------------------------------------------------------------- */

/*
 * Invoke Claude Code with retry.  On success, copies JSON into out_json and
 * returns a pointer to it.  On permanent or exhausted-retry failure, prints
 * an "@ error:" message to stderr and returns NULL.
 */
static const char *invoke_claude_with_retry(const char *nl_input,
                                             const ai_config_t *cfg,
                                             mcp_server_t *mcp_srv,
                                             char *out_json, size_t out_len)
{
    /* Write a temporary MCP config when the server is running and has a token. */
    char mcp_path[256] = "";
    if (mcp_srv && cfg->mcp_token[0] != '\0') {
        if (mcp_server_write_config(mcp_srv, cfg,
                                    mcp_path, sizeof(mcp_path)) != 0) {
            fprintf(stderr, "@ warning: failed to write MCP config; "
                            "proceeding without MCP\n");
            mcp_path[0] = '\0';
        }
    }

    int max_attempts = cfg->ai_max_retries + 1;  /* retries + initial attempt */
    const char *ret  = NULL;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (attempt > 0) {
            /* Exponential backoff: backoff_ms * 2^(attempt-1), capped at 10 s */
            long delay_ms = (long)cfg->ai_retry_backoff_ms;
            for (int i = 1; i < attempt && delay_ms < 10000; i++)
                delay_ms *= 2;
            if (delay_ms > 10000) delay_ms = 10000;

            struct timespec ts;
            ts.tv_sec  = delay_ms / 1000;
            ts.tv_nsec = (delay_ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }

        claude_result_t claude_res;
        claude_status_t status = claude_invoke(nl_input, cfg,
                                               mcp_path[0] ? mcp_path : NULL,
                                               &claude_res);

        if (status == CLAUDE_OK) {
            snprintf(out_json, out_len, "%s", claude_res.stdout_buf);
            ret = out_json;
            break;
        }

        /* Non-retryable error: report and give up immediately. */
        if (!claude_status_is_retryable(status)) {
            if (claude_res.stderr_buf[0])
                fprintf(stderr, "@ error: Claude (%s): %s\n",
                        claude_status_name(status), claude_res.stderr_buf);
            else
                fprintf(stderr, "@ error: Claude: %s\n",
                        claude_status_name(status));
            break;  /* ret stays NULL; fall through to cleanup */
        }

        /* Retryable error: log and decide whether to continue. */
        if (attempt + 1 < max_attempts) {
            fprintf(stderr,
                    "@ warning: Claude attempt %d/%d failed (%s), retrying...\n",
                    attempt + 1, max_attempts, claude_status_name(status));
        } else {
            fprintf(stderr,
                    "@ error: Claude failed after %d attempt%s (%s)\n",
                    max_attempts, max_attempts == 1 ? "" : "s",
                    claude_status_name(status));
        }
    }

    /* Remove the temporary MCP config in all code paths. */
    if (mcp_path[0] != '\0')
        unlink(mcp_path);

    return ret;
}

/* -------------------------------------------------------------------------
 * @ command handler
 * ---------------------------------------------------------------------- */

void at_command_handle(const char *nl_input, struct mcp_server *mcp_srv)
{
    /* Skip leading whitespace after '@' */
    while (*nl_input == ' ' || *nl_input == '\t')
        nl_input++;

    if (*nl_input == '\0') {
        fprintf(stderr, "aishell: @ requires a natural-language instruction.\n");
        return;
    }

    /* Obtain raw JSON — from mock or Claude backend */
    char claude_buf[8192];
    const char *raw_json;

    if (strcmp(g_ai_config.ai_backend, "claude") == 0) {
        /* Progress messages: printed before the blocking Claude call so the
         * user knows the shell is working and has not frozen. */
        fprintf(stderr, "@ contacting Claude Code...\n");
        if (mcp_srv && g_ai_config.mcp_token[0] != '\0')
            fprintf(stderr,
                    "@ local MCP server enabled;"
                    " Claude may inspect AiShell tools...\n");
        fflush(stderr);

        raw_json = invoke_claude_with_retry(nl_input, &g_ai_config, mcp_srv,
                                             claude_buf, sizeof(claude_buf));
        if (!raw_json) return;  /* error already printed to stderr */
    } else {
        raw_json = mock_generate(nl_input);
    }

    /* Reject obviously non-JSON output before handing it to the parser.
     * Accept: raw JSON (first non-whitespace char is '{') or markdown code
     * fences (strip_code_fence inside ai_json_parse_result handles those).
     * Anything else — e.g. a Claude Code status line that starts with 'M',
     * 'E', etc. — is reported as a controlled error with a sanitized preview
     * capped at 120 characters.  This prevents a confusing "expected '{',
     * got 'M'" message when Claude returns plain-text error output. */
    {
        const char *p = raw_json;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != '{' && strncmp(p, "```", 3) != 0) {
            char preview[121];
            size_t i;
            for (i = 0; i < 120 && raw_json[i]; i++) {
                unsigned char c = (unsigned char)raw_json[i];
                /* Replace control characters (except tab) with '?' */
                preview[i] = (c >= 0x20 || c == '\t') ? (char)c : '?';
            }
            preview[i] = '\0';
            fprintf(stderr, "@ error: Claude returned non-JSON output.\n");
            fprintf(stderr, "@ preview: %s\n", preview);
            return;
        }
    }

    /* Parse and validate the JSON response */
    ai_result_t result;
    char parse_err[256];
    ai_json_status_t st = ai_json_parse_result(raw_json, &result, parse_err,
                                                sizeof(parse_err));
    if (st != AI_JSON_OK) {
        fprintf(stderr, "aishell: AI response error (%s): %s\n",
                ai_json_status_name(st), parse_err);
        return;
    }

    /* Out-of-scope request guard: Claude signalled this is not a shell task */
    if (strcmp(result.intent, "unsupported") == 0) {
        printf("@ not a shell task: %s\n", result.explanation);
        return;
    }

    /* Safety validation before showing the command or asking for confirmation */
    ai_validation_t validation;
    ai_valid_level_t vlevel = ai_validate_command(result.command, result.risk,
                                                   result.confidence, &validation);
    if (vlevel == AI_VALID_BLOCKED) {
        fprintf(stderr, "@ blocked: %s\n", validation.reason);
        return;
    }

    printf("Going to execute:\n");
    printf("  %s\n", result.command);

    char confirm[AT_CMDBUF];

    if (vlevel == AI_VALID_WARN) {
        /* Warn path: require explicit "yes" */
        printf("Warning: %s\n", validation.reason);
        printf("Type yes to continue, or press Enter to cancel: ");
        fflush(stdout);
        if (!fgets(confirm, sizeof(confirm), stdin)) {
            printf("\nCancelled.\n");
            return;
        }
        confirm[strcspn(confirm, "\n")] = '\0';
        if (strcmp(confirm, "yes") != 0) {
            printf("Cancelled.\n");
            return;
        }
    } else {
        /* OK path: pressing Enter confirms; any other input cancels */
        printf("OK to continue? Press Enter to run, or type anything else to cancel: ");
        fflush(stdout);
        if (!fgets(confirm, sizeof(confirm), stdin)) {
            printf("\nCancelled.\n");
            return;
        }
        confirm[strcspn(confirm, "\n")] = '\0';
        if (confirm[0] != '\0') {
            printf("Cancelled.\n");
            return;
        }
    }

    /* Execute through the existing shell pipeline */
    char exec_buf[AT_CMDBUF];
    snprintf(exec_buf, sizeof(exec_buf), "%s", result.command);
    int status = execute_line(exec_buf);
    if (status != 0)
        fprintf(stderr, "aishell: exit status %d\n", status);
}
