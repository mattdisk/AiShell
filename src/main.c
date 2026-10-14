/*
 * AiShell — entry point
 *
 * Supports three invocation modes:
 *   1. Multicall: invoked as a command name (symlink) -> dispatch directly
 *   2. One-shot:  aishell CMD [ARGS...]               -> dispatch CMD
 *   3. REPL:      aishell                             -> interactive loop
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "registry.h"
#include "token.h"
#include "command.h"
#include "alias.h"
#include "history.h"
#include "exec.h"
#include "demo_trace.h"
#include "ai/ai_generate.h"
#include "ai/ai_config.h"
#include "ai/ai_json.h"
#include "ai/ai_validate.h"
#include "ai/mcp_server.h"

/* Forward declarations — one per builtin init function */
void builtin_astmode_init(void);
void builtin_help_init(void);
void builtin_ls_init(void);
void builtin_stat_init(void);
void builtin_cat_init(void);
void builtin_head_init(void);
void builtin_cp_init(void);
void builtin_mv_init(void);
void builtin_rm_init(void);
void builtin_mkdir_init(void);
void builtin_rmdir_init(void);
void builtin_touch_init(void);
void builtin_rg_init(void);
void builtin_edit_line_init(void);
void builtin_edit_replace_init(void);
void builtin_jobs_init(void);
void builtin_ps_init(void);
void builtin_kill_init(void);
void builtin_wait_init(void);
void builtin_cd_init(void);
void builtin_pwd_init(void);
void builtin_env_init(void);
void builtin_export_init(void);
void builtin_unset_init(void);
void builtin_type_init(void);
void builtin_http_init(void);
void builtin_pkg_init(void);
void builtin_vi_init(void);
void builtin_less_init(void);
void builtin_history_init(void);
void builtin_alias_init(void);
void builtin_echo_init(void);
void builtin_printf_init(void);
void builtin_sleep_init(void);
void builtin_date_init(void);
void builtin_true_init(void);
void builtin_false_init(void);

/* pkg doc generation — defined in src/builtins/pkg.c */
extern void pkg_gen_docs(const char *outdir);

/*
 * --parse-ai-json: read JSON from stdin, run it through ai_json_parse_result(),
 * and print the outcome as a JSON object.  Used only by the test suite to
 * exercise the parser without a real Claude process.
 *
 * Success output:  {"status":"ok","intent":"...","command":"...","explanation":"...","risk":"...","confidence":N}
 * Error output:    {"status":"error","error_code":"...","message":"..."}
 */
static int cmd_parse_ai_json(void)
{
    char buf[65536];
    size_t total = 0;
    int c;
    while (total < sizeof(buf) - 1 && (c = getchar()) != EOF)
        buf[total++] = (char)c;
    buf[total] = '\0';

    ai_result_t result;
    char err[256] = "";
    ai_json_status_t status = ai_json_parse_result(buf, &result, err, sizeof(err));

    if (status == AI_JSON_OK) {
        printf("{\"status\":\"ok\",\"intent\":\"");
        json_escape_print(result.intent);
        printf("\",\"command\":\"");
        json_escape_print(result.command);
        printf("\",\"explanation\":\"");
        json_escape_print(result.explanation);
        printf("\",\"risk\":\"");
        json_escape_print(result.risk);
        printf("\",\"confidence\":%g}\n", result.confidence);
        return 0;
    }

    printf("{\"status\":\"error\",\"error_code\":\"%s\",\"message\":\"",
           ai_json_status_name(status));
    json_escape_print(err);
    printf("\"}\n");
    return 1;
}

/*
 * --validate-command: read one command line from stdin, run it through the
 * safety validator, and print the result as a JSON object.  Used only by the
 * test suite to exercise the validator without a real Claude process.
 *
 * Risk and confidence can be set via AISHELL_TEST_RISK and
 * AISHELL_TEST_CONFIDENCE environment variables (defaults: "read_only" / 1.0).
 *
 * Output: {"level":"ok|warn|blocked","reason":"..."}
 */
static int cmd_validate_command(void)
{
    char buf[512];
    if (!fgets(buf, sizeof(buf), stdin)) {
        printf("{\"level\":\"blocked\",\"reason\":\"no input\"}\n");
        return 1;
    }
    buf[strcspn(buf, "\n")] = '\0';

    const char *risk = getenv("AISHELL_TEST_RISK");
    if (!risk) risk = "read_only";

    double confidence = 1.0;
    const char *conf_s = getenv("AISHELL_TEST_CONFIDENCE");
    if (conf_s) confidence = atof(conf_s);

    ai_validation_t v;
    ai_valid_level_t level = ai_validate_command(buf, risk, confidence, &v);

    printf("{\"level\":\"%s\",\"reason\":\"", ai_valid_level_name(level));
    json_escape_print(v.reason);
    if (v.suggestion[0] != '\0') {
        printf("\",\"suggestion\":\"");
        json_escape_print(v.suggestion);
    }
    printf("\"}\n");
    return (level == AI_VALID_BLOCKED) ? 1 : 0;
}

/*
 * --mcp-test: start the MCP server, print its actual port as JSON, then block
 * waiting for one line on stdin (the test suite sends "stop").  Used only by
 * the test suite to exercise the MCP server without the full REPL.
 *
 * Output (success): {"status":"ok","port":N}
 * Output (disabled): {"status":"disabled"}
 * Output (error):   {"status":"error","reason":"..."}
 */
static int cmd_mcp_test(void)
{
    mcp_server_t *srv = NULL;
    mcp_server_status_t status = mcp_server_start(&g_ai_config, &srv);

    if (status == MCP_SERVER_DISABLED) {
        printf("{\"status\":\"disabled\"}\n");
        fflush(stdout);
        return 0;
    }
    if (status != MCP_SERVER_OK) {
        printf("{\"status\":\"error\",\"reason\":\"%s\"}\n",
               mcp_server_status_name(status));
        fflush(stdout);
        return 1;
    }

    printf("{\"status\":\"ok\",\"port\":%d}\n",
           mcp_server_actual_port(srv));
    fflush(stdout);

    /* Block until the test sends any line or stdin closes. */
    char buf[64];
    (void)fgets(buf, sizeof(buf), stdin);

    mcp_server_stop(srv);
    return 0;
}

static volatile sig_atomic_t g_got_sigchld = 0;
static void handle_sigchld(int sig) { (void)sig; g_got_sigchld = 1; }

/* --commands-json emitter */
static int g_cmd_json_first;

static void emit_one_cmd_json(const cmd_spec_t *spec)
{
    if (!g_cmd_json_first) putchar(',');
    g_cmd_json_first = 0;
    printf("{\"name\":\"");    json_escape_print(spec->name);
    printf("\",\"summary\":\""); json_escape_print(spec->description);
    printf("\"}");
}

static int emit_commands_json(void)
{
    g_cmd_json_first = 1;
    putchar('[');
    registry_for_each(emit_one_cmd_json);
    puts("]");
    return 0;
}

static void register_all_builtins(void)
{
    builtin_astmode_init();
    builtin_help_init();
    builtin_ls_init();
    builtin_stat_init();
    builtin_cat_init();
    builtin_head_init();
    builtin_cp_init();
    builtin_mv_init();
    builtin_rm_init();
    builtin_mkdir_init();
    builtin_rmdir_init();
    builtin_touch_init();
    builtin_rg_init();
    builtin_edit_line_init();
    builtin_edit_replace_init();
    builtin_jobs_init();
    builtin_ps_init();
    builtin_kill_init();
    builtin_wait_init();
    builtin_cd_init();
    builtin_pwd_init();
    builtin_env_init();
    builtin_export_init();
    builtin_unset_init();
    builtin_type_init();
    builtin_http_init();
    builtin_pkg_init();
    builtin_vi_init();
    builtin_less_init();
    builtin_history_init();
    builtin_alias_init();
    builtin_echo_init();
    builtin_printf_init();
    builtin_sleep_init();
    builtin_date_init();
    builtin_true_init();
    builtin_false_init();
}

int main(int argc, char **argv)
{
    ai_config_load(&g_ai_config);
    register_all_builtins();

    /* Prepend ~/.mysh/bin to PATH so that packages installed with `pkg install`
     * are findable as ordinary commands.  We only add it when it is not already
     * present, and we silently skip the update if the directory does not exist
     * yet (execvp just won't find anything there until a package is installed). */
    {
        const char *home = getenv("HOME");
        if (home) {
            char bin_dir[PATH_MAX];
            snprintf(bin_dir, sizeof(bin_dir), "%s/.mysh/bin", home);
            const char *old_path = getenv("PATH");
            if (!old_path || !strstr(old_path, bin_dir)) {
                char new_path[4096];
                snprintf(new_path, sizeof(new_path), "%s:%s",
                         bin_dir, old_path ? old_path : "");
                setenv("PATH", new_path, 1);
            }
        }
    }

    /* Multicall dispatch: if argv[0] is not "aishell", treat it as the command */
    {
        const char *prog = strrchr(argv[0], '/');
        prog = prog ? prog + 1 : argv[0];
        if (strcmp(prog, "aishell") != 0) {
            cmd_spec_t *spec = find_command(prog);
            if (spec) return spec->run(argc, argv);
            fprintf(stderr, "%s: unknown command\n", prog);
            return 127;
        }
    }

    /* aishell-level flags — only reached when invoked as "aishell", not via multicall */
    if (argc == 2 && strcmp(argv[1], "--commands-json") == 0)
        return emit_commands_json();
    if (argc == 2 && strcmp(argv[1], "--dump-config") == 0) {
        ai_config_dump_json(&g_ai_config);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--parse-ai-json") == 0)
        return cmd_parse_ai_json();
    if (argc == 2 && strcmp(argv[1], "--validate-command") == 0)
        return cmd_validate_command();
    if (argc == 2 && strcmp(argv[1], "--mcp-test") == 0)
        return cmd_mcp_test();
    if (argc == 3 && strcmp(argv[1], "--gen-docs") == 0) {
        pkg_gen_docs(argv[2]);
        return 0;
    }

    /* --ast: start REPL with AST debug mode enabled (fall through to REPL below) */
    if (argc == 2 && strcmp(argv[1], "--ast") == 0) {
        g_ast_debug = 1;
        argc = 1;  /* hide the flag so the REPL path is taken */
    }

    /* One-shot mode: aishell CMD [ARGS...] */
    if (argc > 1) {
        cmd_spec_t *spec = find_command(argv[1]);
        if (spec) return spec->run(argc - 1, argv + 1);
        return run_external_interactive(argv[1], argc - 1, argv + 1);
    }

    /* Enable AST debug mode from environment variable */
    if (getenv("AISHELL_AST")) g_ast_debug = 1;

    /* Ignore Ctrl+C in the shell itself; children reset this after fork. */
    signal(SIGINT, SIG_IGN);

    /* Install SIGCHLD *without* SA_RESTART so that fgets() returns EINTR
     * when a background job finishes.  That lets the REPL print the done
     * notification immediately rather than waiting for the next keystroke. */
    {
        struct sigaction sa;
        sa.sa_handler = handle_sigchld;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;          /* no SA_RESTART: slow syscalls get EINTR */
        sigaction(SIGCHLD, &sa, NULL);
    }

    /* Job control: when stdin is a terminal, put the shell in its own process
     * group and take ownership of the terminal.  This ensures that:
     *   - Ctrl+C (SIGINT to the terminal's foreground process group) only
     *     reaches the current foreground job, not the shell or background jobs.
     *   - Background jobs forked into their own process groups are never in the
     *     foreground group, so they are unaffected by Ctrl+C at the prompt.
     *
     * SIGTTOU/SIGTTIN/SIGTSTP are ignored so the shell is not stopped when it
     * calls tcsetpgrp() or performs other terminal operations.
     *
     * In non-interactive mode (g_shell_pgid stays 0) the setpgid() calls in
     * run_pipeline() still isolate jobs, but tcsetpgrp() is skipped. */
    if (isatty(STDIN_FILENO)) {
        signal(SIGTTOU, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);

        /* Become our own process group leader */
        setpgid(0, 0);
        g_shell_pgid = getpgrp();

        /* Take control of the terminal for the shell's process group */
        tcsetpgrp(STDIN_FILENO, g_shell_pgid);
    }

    /* Start MCP server in background thread (no-op when disabled or token absent). */
    mcp_server_t *mcp_srv = NULL;
    {
        mcp_server_status_t ms = mcp_server_start(&g_ai_config, &mcp_srv);
        if (ms != MCP_SERVER_OK && ms != MCP_SERVER_DISABLED)
            fprintf(stderr, "aishell: MCP server failed to start: %s\n",
                    mcp_server_status_name(ms));
    }

    /* Interactive REPL */
    char  buf[BUF_SIZE];
    char *argv_raw[MAX_ARGS];
    TRACE("AiShell started: pid=%d, main thread pthread=%lu",
          (int)getpid(), (unsigned long)pthread_self());
    printf("Welcome to AiShell. Type help, -h, or --help for usage.\n");

    while (1) {
        /* Reap any finished background children before showing the prompt. */
        if (g_got_sigchld) { g_got_sigchld = 0; reap_jobs(); }

        printf("aishell> ");
        fflush(stdout);

        /* Read a line.  fgets() may return NULL for two reasons:
         *   1. EINTR — SIGCHLD fired while we were waiting for input.
         *      Print the job-done notification and re-issue the prompt.
         *   2. Real EOF (Ctrl+D) — exit the REPL.
         * Without SA_RESTART on SIGCHLD, case 1 happens immediately when a
         * background job finishes instead of waiting for the next keystroke. */
        while (!fgets(buf, sizeof(buf), stdin)) {
            if (errno == EINTR && !feof(stdin)) {
                clearerr(stdin);
                if (g_got_sigchld) { g_got_sigchld = 0; reap_jobs(); }
                printf("aishell> ");
                fflush(stdout);
            } else {
                printf("\n");
                goto repl_exit;
            }
        }
        buf[strcspn(buf, "\n")] = '\0';
        if (buf[0] == '\0') continue;

        add_history(buf);

        if (buf[0] == '@') {
            at_command_handle(buf + 1, mcp_srv);
            continue;
        }

        /* Peek at first word for alias / exit / help handling. */
        char line_copy[BUF_SIZE];
        snprintf(line_copy, sizeof(line_copy), "%s", buf);
        int raw_argc = split_line(line_copy, argv_raw, MAX_ARGS);
        if (raw_argc == 0) continue;

        /*
         * Alias expansion: replace argv[0] with its alias value and keep
         * any trailing arguments from the original input intact.
         */
        char exec_line[BUF_SIZE];
        const char *alias_val = lookup_alias(argv_raw[0]);
        if (alias_val) {
            const char *rest = buf;
            while (*rest && *rest != ' ' && *rest != '\t') rest++;
            while (*rest == ' ' || *rest == '\t') rest++;
            if (*rest)
                snprintf(exec_line, sizeof(exec_line), "%s %s", alias_val, rest);
            else
                snprintf(exec_line, sizeof(exec_line), "%s", alias_val);
            /* re-peek first word so exit/help checks work on expanded line */
            char peek2[BUF_SIZE];
            snprintf(peek2, sizeof(peek2), "%s", exec_line);
            if (split_line(peek2, argv_raw, MAX_ARGS) == 0) continue;
        } else {
            snprintf(exec_line, sizeof(exec_line), "%s", buf);
        }

        if (strcmp(argv_raw[0], "quit") == 0 || strcmp(argv_raw[0], "exit") == 0) break;
        if (strcmp(argv_raw[0], "-h") == 0 || strcmp(argv_raw[0], "--help") == 0) {
            registry_print_all();
            continue;
        }

        int status = execute_line(exec_line);
        if (status != 0)
            fprintf(stderr, "aishell: exit status %d\n", status);
    }

repl_exit:
    mcp_server_stop(mcp_srv);
    history_free();
    alias_free();
    return 0;
}
