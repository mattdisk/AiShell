#define _POSIX_C_SOURCE 200809L

#include "claude_client.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Maximum size of the formatted prompt passed to Claude. */
#define PROMPT_MAX 8192

/* Default heartbeat interval: print "still waiting" every N ms during the
 * blocking select loop.  Override with AISHELL_DEBUG_HEARTBEAT_MS for tests. */
#define HEARTBEAT_INTERVAL_MS 8000L

/* Prompt template when no MCP server is available. */
#define PROMPT_NO_MCP \
    "You are a shell command assistant for AiShell, a POSIX-compatible shell.\n\n" \
    "The user wants to run a shell command to accomplish this task:\n\n" \
    "%s\n\n" \
    "Before composing the command, perform a semantic self-check:\n" \
    "  - COUNT: if the instruction says \"count\", \"how many\", or \"total\",\n" \
    "    the command MUST include a counting operation: wc, uniq -c, or awk\n" \
    "    accumulation. A command that only lists files or paths does NOT satisfy\n" \
    "    a count request. Revise the command if it lacks a counting step.\n" \
    "  - PER DIRECTORY: if the instruction says \"per directory\", \"under each\n" \
    "    directory\", or \"for each subdirectory\", the command MUST group results\n" \
    "    by directory (e.g., awk array aggregation). A flat listing of files does\n" \
    "    NOT satisfy a per-directory request. Revise if needed.\n" \
    "  - INTERMEDIATE DATA: if the command would produce only a list of file\n" \
    "    names or raw paths but the instruction requires aggregation or a\n" \
    "    summary result, revise the command to complete the aggregation.\n" \
    "  - WC TOTAL ROW: if the instruction asks for the top-N largest files\n" \
    "    (by byte count or line count), do NOT use:\n" \
    "      find ... -exec wc -c {} +  (batch mode)\n" \
    "    because wc appends a 'total' row when given multiple files, and that\n" \
    "    total will rank first in sort -rn | head output, displacing one real\n" \
    "    file. Prefer:\n" \
    "      find ... -print | xargs -n 1 wc -c | sort -rn | head -n N\n" \
    "    which processes one file at a time and produces no total row.\n\n" \
    "If the user's instruction is not a shell command task — for example, it asks\n" \
    "for a story, an explanation, a joke, or any activity that does not map to a\n" \
    "single shell command — set intent to \"unsupported\" and leave command as an\n" \
    "empty string. Describe why in explanation. AiShell will display the\n" \
    "explanation to the user instead of executing a command.\n\n" \
    "Respond with ONLY a JSON object — no markdown, no code fences, no preamble.\n" \
    "The JSON must have exactly these five fields:\n" \
    "  \"intent\": \"shell_command\" (default) or \"unsupported\" (not a shell task)\n" \
    "  \"command\": one shell command (no chaining: no ; && || backticks or $());\n" \
    "             empty string \"\" when intent is \"unsupported\"\n" \
    "  \"explanation\": brief description of the command, or why it is not a shell task\n" \
    "  \"risk\": one of \"read_only\", \"write\", \"destructive\", \"network\", or \"unknown\"\n" \
    "  \"confidence\": a number between 0.0 and 1.0\n\n" \
    "Example (shell task): {\"intent\":\"shell_command\",\"command\":\"pwd\"," \
    "\"explanation\":\"Prints the current working directory.\"," \
    "\"risk\":\"read_only\",\"confidence\":0.95}\n" \
    "Example (not a shell task): {\"intent\":\"unsupported\",\"command\":\"\"," \
    "\"explanation\":\"This is not a shell command task.\"," \
    "\"risk\":\"unknown\",\"confidence\":1.0}\n\n" \
    "Do not execute commands. Do not suggest commands such as rm -rf, chmod, " \
    "sudo, dd, or similar destructive operations.\n\n" \
    "Return only raw JSON — no markdown, no code fences.\n" \
    "The first character of your response must be '{' and the last must be '}'."

/* Prompt template when an AiShell MCP server is available. */
#define PROMPT_WITH_MCP \
    "You are a shell command assistant for AiShell, a POSIX-compatible shell.\n" \
    "You have access to an AiShell MCP server. Use it — do not guess syntax.\n\n" \
    "Required workflow:\n" \
    "  1. Call list_aishell_builtins to discover which commands are built in.\n" \
    "  2. For any builtin you plan to use, call get_command_help to get the\n" \
    "     exact synopsis. IMPORTANT: AiShell builtins do not accept all\n" \
    "     POSIX/GNU flags — always check before using a flag.\n" \
    "     Example: head uses  -n N  not the GNU  -N  form.\n" \
    "  3. Call get_shell_context to learn the current working directory.\n" \
    "  4. Compose your candidate command based on the exact synopsis from step 2.\n" \
    "  4b. semantic self-check before calling validate_command:\n" \
    "     - COUNT: if the instruction says \"count\", \"how many\", or \"total\",\n" \
    "       the command MUST include a counting operation: wc, uniq -c, or awk\n" \
    "       accumulation. A command that only lists files or paths does NOT\n" \
    "       satisfy a count request. Revise the command if it lacks a counting\n" \
    "       step.\n" \
    "     - PER DIRECTORY: if the instruction says \"per directory\", \"under\n" \
    "       each directory\", or \"for each subdirectory\", the command MUST\n" \
    "       group results by directory (e.g., awk array aggregation). A flat\n" \
    "       listing of files does NOT satisfy a per-directory request. Revise\n" \
    "       if needed.\n" \
    "     - INTERMEDIATE DATA: if the command would produce only a list of file\n" \
    "       names or raw paths but the instruction requires aggregation or a\n" \
    "       summary result, revise the command to complete the aggregation.\n" \
    "     - WC TOTAL ROW: if the instruction asks for the top-N largest files,\n" \
    "       do NOT use find ... -exec wc -c {} + (batch mode). wc appends a\n" \
    "       'total' row when given multiple files; that total ranks first in\n" \
    "       sort -rn | head output and displaces one real file. Prefer:\n" \
    "         find ... -print | xargs -n 1 wc -c | sort -rn | head -n N\n" \
    "     If the command fails this check, revise it and return to step 4b\n" \
    "     before proceeding to step 5.\n" \
    "  5. Call validate_command on the candidate command.\n" \
    "     - If level is 'ok': proceed to step 6.\n" \
    "     - If level is 'warn' or 'blocked': read the 'suggestion' field,\n" \
    "       revise the command, and return to step 5.\n" \
    "     - Never return a command that validate_command marks 'blocked'.\n" \
    "  6. Return the final JSON only after validate_command returns level 'ok'.\n\n" \
    "The user wants to run a shell command to accomplish this task:\n\n" \
    "%s\n\n" \
    "If the user's instruction is not a shell command task — for example, it asks\n" \
    "for a story, an explanation, a joke, or any activity that does not map to a\n" \
    "single shell command — set intent to \"unsupported\" and leave command as an\n" \
    "empty string. Describe why in explanation. AiShell will display the\n" \
    "explanation to the user instead of executing a command.\n\n" \
    "Respond with ONLY a JSON object — no markdown, no code fences, no preamble.\n" \
    "The JSON must have exactly these five fields:\n" \
    "  \"intent\": \"shell_command\" (default) or \"unsupported\" (not a shell task)\n" \
    "  \"command\": one shell command (no chaining: no ; && || backticks or $());\n" \
    "             empty string \"\" when intent is \"unsupported\"\n" \
    "  \"explanation\": brief description of the command, or why it is not a shell task\n" \
    "  \"risk\": one of \"read_only\", \"write\", \"destructive\", \"network\", or \"unknown\"\n" \
    "  \"confidence\": a number between 0.0 and 1.0\n\n" \
    "Example (shell task): {\"intent\":\"shell_command\",\"command\":\"pwd\"," \
    "\"explanation\":\"Prints the current working directory.\"," \
    "\"risk\":\"read_only\",\"confidence\":0.95}\n" \
    "Example (not a shell task): {\"intent\":\"unsupported\",\"command\":\"\"," \
    "\"explanation\":\"This is not a shell command task.\"," \
    "\"risk\":\"unknown\",\"confidence\":1.0}\n\n" \
    "Return only raw JSON — no markdown, no code fences.\n" \
    "The first character of your response must be '{' and the last must be '}'."

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Return the current time as milliseconds (monotonic clock). */
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Case-insensitive search for authentication/login-related error patterns. */
static int contains_auth_error(const char *s)
{
    char lower[4096];
    size_t i;
    for (i = 0; s[i] && i < sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)s[i]);
    lower[i] = '\0';

    return (strstr(lower, "not logged in")    != NULL ||
            strstr(lower, "authentication")   != NULL ||
            strstr(lower, "api key")          != NULL ||
            strstr(lower, "please log in")    != NULL ||
            strstr(lower, "please login")     != NULL ||
            strstr(lower, "not authenticated") != NULL);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

claude_status_t claude_invoke(const char *nl_prompt,
                               const ai_config_t *cfg,
                               const char *mcp_config_path,
                               claude_result_t *out)
{
    memset(out, 0, sizeof(*out));

    /* --- Select prompt template and build the formatted prompt --- */
    const char *tmpl = (mcp_config_path && *mcp_config_path)
                       ? PROMPT_WITH_MCP : PROMPT_NO_MCP;
    char prompt_buf[PROMPT_MAX];
    snprintf(prompt_buf, sizeof(prompt_buf), tmpl, nl_prompt);

    /* --- Choose the binary path --- */
    const char *claude_bin = (cfg->claude_path[0] != '\0')
                             ? cfg->claude_path : "claude";

    /* --- Build argv ---
     * Without MCP:  claude -p <prompt>
     * With MCP:     claude -p <prompt>
     *                      --mcp-config <path>
     *                      --strict-mcp-config
     *                      --allowedTools mcp__aishell__*
     *
     * --allowedTools pre-approves the AiShell MCP tools so that Claude Code in
     * headless (-p) mode does not permission-gate them.  Only the wildcard for
     * this server is listed — not Bash, Edit, Write, or any other tool.
     * AiShell still validates every proposed command locally and asks for user
     * confirmation before executing anything. */
    const char *claude_argv[10];
    int ai = 0;
    claude_argv[ai++] = claude_bin;
    claude_argv[ai++] = "-p";
    claude_argv[ai++] = prompt_buf;
    if (mcp_config_path && *mcp_config_path) {
        claude_argv[ai++] = "--mcp-config";
        claude_argv[ai++] = mcp_config_path;
        claude_argv[ai++] = "--strict-mcp-config";
        claude_argv[ai++] = "--allowedTools";
        claude_argv[ai++] = "mcp__aishell__*";
    }
    claude_argv[ai] = NULL;

    /* --- Create stdout and stderr pipes --- */
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) < 0) {
        out->status = CLAUDE_EXEC_FAILED;
        return CLAUDE_EXEC_FAILED;
    }
    if (pipe(err_pipe) < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        out->status = CLAUDE_EXEC_FAILED;
        return CLAUDE_EXEC_FAILED;
    }

    /* --- Fork --- */
    pid_t child = fork();
    if (child < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        out->status = CLAUDE_EXEC_FAILED;
        return CLAUDE_EXEC_FAILED;
    }

    if (child == 0) {
        /* Child: wire up pipes, redirect stdin from /dev/null, exec. */
        close(out_pipe[0]);
        close(err_pipe[0]);

        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(126);
        if (dup2(err_pipe[1], STDERR_FILENO) < 0) _exit(126);
        close(out_pipe[1]);
        close(err_pipe[1]);

        /* Prevent Claude from blocking on an interactive stdin prompt. */
        int dev_null = open("/dev/null", O_RDONLY);
        if (dev_null >= 0) { dup2(dev_null, STDIN_FILENO); close(dev_null); }

        execvp(claude_bin, (char *const *)claude_argv);

        /* execvp failed: write a brief message and exit 127 (not found). */
        const char *msg = (errno == ENOENT) ? "claude: command not found\n"
                                            : "claude: exec failed\n";
        write(STDERR_FILENO, msg, strlen(msg));
        _exit(127);
    }

    /* Parent: close write ends. */
    close(out_pipe[1]);
    close(err_pipe[1]);

    /* --- Select loop: read both pipes with deadline --- */

    /* Heartbeat interval: warn the user every N ms so it doesn't look frozen.
     * Default is HEARTBEAT_INTERVAL_MS; AISHELL_DEBUG_HEARTBEAT_MS overrides
     * to a shorter value in automated tests. */
    long hb_interval_ms = HEARTBEAT_INTERVAL_MS;
    {
        const char *hb_env = getenv("AISHELL_DEBUG_HEARTBEAT_MS");
        if (hb_env) {
            long v = strtol(hb_env, NULL, 10);
            if (v > 0 && v <= 60000L) hb_interval_ms = v;
        }
    }

    long deadline    = now_ms() + (long)cfg->ai_timeout_ms;
    long last_hb_ms  = now_ms();  /* time of the last printed heartbeat */
    size_t out_len = 0, err_len = 0;
    int out_open = 1, err_open = 1;
    int out_truncated = 0;
    char tmp[4096];

    while (out_open || err_open) {
        long remaining = deadline - now_ms();
        if (remaining <= 0) {
            kill(child, SIGKILL);
            out->timed_out = 1;
            break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (out_open) { FD_SET(out_pipe[0], &rfds); if (out_pipe[0] > maxfd) maxfd = out_pipe[0]; }
        if (err_open) { FD_SET(err_pipe[0], &rfds); if (err_pipe[0] > maxfd) maxfd = err_pipe[0]; }

        /* Cap the select timeout to hb_interval_ms so we can emit heartbeats
         * while waiting.  Full-timeout enforcement happens at remaining <= 0. */
        long slice = remaining < hb_interval_ms ? remaining : hb_interval_ms;
        struct timeval tv;
        tv.tv_sec  = slice / 1000;
        tv.tv_usec = (slice % 1000) * 1000;

        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;  /* signal (e.g. SIGCHLD) — retry */
            break;
        }
        if (n == 0) {
            /* Heartbeat slice expired — print if enough real time has elapsed,
             * then re-enter the loop to re-check remaining vs deadline. */
            long now = now_ms();
            if (now - last_hb_ms >= hb_interval_ms) {
                fprintf(stderr,
                        "@ still waiting for Claude Code / MCP tools...\n");
                fflush(stderr);
                last_hb_ms = now;
            }
            continue;  /* full timeout handled by remaining <= 0 at loop top */
        }

        if (out_open && FD_ISSET(out_pipe[0], &rfds)) {
            ssize_t nr = read(out_pipe[0], tmp, sizeof(tmp));
            if (nr > 0) {
                size_t avail = sizeof(out->stdout_buf) - 1 - out_len;
                size_t copy  = ((size_t)nr <= avail) ? (size_t)nr : avail;
                if (copy > 0) {
                    memcpy(out->stdout_buf + out_len, tmp, copy);
                    out_len += copy;
                    out->stdout_buf[out_len] = '\0';
                }
                if ((size_t)nr > avail) out_truncated = 1;
            } else if (nr == 0 || (nr < 0 && errno != EINTR)) {
                close(out_pipe[0]); out_open = 0;
            }
        }

        if (err_open && FD_ISSET(err_pipe[0], &rfds)) {
            ssize_t nr = read(err_pipe[0], tmp, sizeof(tmp));
            if (nr > 0) {
                size_t avail = sizeof(out->stderr_buf) - 1 - err_len;
                size_t copy  = ((size_t)nr <= avail) ? (size_t)nr : avail;
                if (copy > 0) {
                    memcpy(out->stderr_buf + err_len, tmp, copy);
                    err_len += copy;
                    out->stderr_buf[err_len] = '\0';
                }
            } else if (nr == 0 || (nr < 0 && errno != EINTR)) {
                close(err_pipe[0]); err_open = 0;
            }
        }
    }

    /* Close any pipe FDs still open before waiting. */
    if (out_open) { close(out_pipe[0]); }
    if (err_open) { close(err_pipe[0]); }

    /* Wait for child. */
    int wstatus = 0;
    waitpid(child, &wstatus, 0);
    if (WIFEXITED(wstatus))
        out->exit_code = WEXITSTATUS(wstatus);
    else
        out->exit_code = -1;

    /* --- Classify result --- */
    if (out->timed_out) {
        out->status = CLAUDE_TIMEOUT;
        return CLAUDE_TIMEOUT;
    }

    /* Exit 127 = command not found (set by the child's _exit(127) path). */
    if (out->exit_code == 127) {
        out->status = CLAUDE_NOT_FOUND;
        return CLAUDE_NOT_FOUND;
    }

    /* Auth errors take priority over generic nonzero exit. */
    if (contains_auth_error(out->stderr_buf)) {
        out->status = CLAUDE_NOT_LOGGED_IN;
        return CLAUDE_NOT_LOGGED_IN;
    }

    if (out->exit_code != 0) {
        out->status = CLAUDE_NONZERO;
        return CLAUDE_NONZERO;
    }

    if (out->stdout_buf[0] == '\0') {
        out->status = CLAUDE_EMPTY_OUTPUT;
        return CLAUDE_EMPTY_OUTPUT;
    }

    if (out_truncated) {
        out->status = CLAUDE_OUTPUT_TRUNCATED;
        return CLAUDE_OUTPUT_TRUNCATED;
    }

    out->status = CLAUDE_OK;
    return CLAUDE_OK;
}

const char *claude_status_name(claude_status_t status)
{
    switch (status) {
    case CLAUDE_OK:               return "ok";
    case CLAUDE_NOT_FOUND:        return "not_found";
    case CLAUDE_NOT_LOGGED_IN:    return "not_logged_in";
    case CLAUDE_TIMEOUT:          return "timeout";
    case CLAUDE_NONZERO:          return "nonzero";
    case CLAUDE_EMPTY_OUTPUT:     return "empty_output";
    case CLAUDE_OUTPUT_TRUNCATED: return "output_truncated";
    case CLAUDE_EXEC_FAILED:      return "exec_failed";
    default:                      return "unknown";
    }
}

int claude_status_is_retryable(claude_status_t status)
{
    switch (status) {
    case CLAUDE_TIMEOUT:
    case CLAUDE_NONZERO:
    case CLAUDE_EMPTY_OUTPUT:
    case CLAUDE_OUTPUT_TRUNCATED:
        return 1;
    default:
        return 0;
    }
}
