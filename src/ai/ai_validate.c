#define _POSIX_C_SOURCE 200809L

#include "ai_validate.h"
#include "token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define VTOK_MAX 64  /* max tokens accepted from tokenize_line */
#define VARG_MAX 32  /* max argv words collected per pipeline segment */

/* -------------------------------------------------------------------------
 * Token helpers
 * ---------------------------------------------------------------------- */

/* Free TOK_WORD values heap-allocated by tokenize_line via strndup. */
static void free_tokens(token_t *toks, int ntok)
{
    for (int i = 0; i < ntok; i++)
        if (toks[i].type == TOK_WORD && toks[i].val)
            free(toks[i].val);
}

/*
 * Collect the actual argument words from token range [start, end).
 * Skips redirect-filename words (the TOK_WORD immediately after a redirect
 * operator).  Stops early at TOK_PIPE or TOK_BG as a safety measure.
 * Returns argc; argv_out[i] point into toks[].val — do not free separately.
 */
static int collect_argv(token_t *toks, int start, int end,
                        char **argv_out, int max)
{
    int argc = 0, skip = 0;
    for (int i = start; i < end && argc < max; i++) {
        switch (toks[i].type) {
        case TOK_REDIR_OUT: case TOK_REDIR_APPEND: case TOK_REDIR_IN:
            skip = 1;
            break;
        case TOK_PIPE: case TOK_BG:
            goto done;
        case TOK_WORD:
            if (skip) { skip = 0; break; }
            argv_out[argc++] = toks[i].val;
            break;
        }
    }
done:
    return argc;
}

/* -------------------------------------------------------------------------
 * Blocked-command helpers
 * ---------------------------------------------------------------------- */

/*
 * Return 1 if rm's argv shows a dangerous combination: recursive+force flags,
 * or an explicit root-filesystem path argument.
 */
static int rm_is_blocked(char **argv, int argc, char *reason, size_t rlen)
{
    int has_r = 0, has_f = 0;

    for (int i = 1; i < argc; i++) {  /* skip argv[0] = "rm" */
        const char *w = argv[i];

        if (strcmp(w, "/") == 0 || strcmp(w, "/*") == 0) {
            snprintf(reason, rlen, "rm of root filesystem path '%s'", w);
            return 1;
        }

        /* Short flags like -rf, -fr, -r, -f, -rfd … */
        if (w[0] == '-' && w[1] != '-' && w[1] != '\0') {
            for (int j = 1; w[j]; j++) {
                if (w[j] == 'r' || w[j] == 'R') has_r = 1;
                if (w[j] == 'f') has_f = 1;
            }
        } else if (strcmp(w, "--recursive") == 0) {
            has_r = 1;
        } else if (strcmp(w, "--force") == 0) {
            has_f = 1;
        }
    }

    if (has_r && has_f) {
        snprintf(reason, rlen, "rm with recursive and force flags is blocked");
        return 1;
    }
    return 0;
}

/*
 * Return 1 if the git subcommand+flags are dangerous.
 * Covers: push --force/-f, reset --hard, clean -f/--force.
 */
static int git_is_blocked(char **argv, int argc, char *reason, size_t rlen)
{
    if (argc < 2) return 0;
    const char *sub = argv[1];

    if (strcmp(sub, "push") == 0) {
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
                snprintf(reason, rlen, "'git push --force' is blocked");
                return 1;
            }
        }
    } else if (strcmp(sub, "reset") == 0) {
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--hard") == 0) {
                snprintf(reason, rlen, "'git reset --hard' is blocked");
                return 1;
            }
        }
    } else if (strcmp(sub, "clean") == 0) {
        for (int i = 2; i < argc; i++) {
            const char *w = argv[i];
            if (w[0] == '-' && w[1] != '-' && w[1] != '\0') {
                for (int j = 1; w[j]; j++) {
                    if (w[j] == 'f') {
                        snprintf(reason, rlen, "'git clean -f' is blocked");
                        return 1;
                    }
                }
            } else if (strcmp(w, "--force") == 0) {
                snprintf(reason, rlen, "'git clean --force' is blocked");
                return 1;
            }
        }
    }
    return 0;
}

/*
 * Return 1 if the last pipeline segment's first word is a shell interpreter.
 * Catches patterns like: curl ... | sh, wget ... | bash, etc.
 */
static int check_pipe_to_shell(token_t *toks, int ntok, char *reason, size_t rlen)
{
    static const char *const shells[] = {
        "sh", "bash", "zsh", "fish", "ksh", "dash", NULL
    };

    int last_pipe = -1;
    for (int i = 0; i < ntok; i++)
        if (toks[i].type == TOK_PIPE) last_pipe = i;
    if (last_pipe < 0) return 0;

    for (int i = last_pipe + 1; i < ntok; i++) {
        if (toks[i].type != TOK_WORD) continue;
        for (int j = 0; shells[j]; j++) {
            if (strcmp(toks[i].val, shells[j]) == 0) {
                snprintf(reason, rlen, "pipe to shell '%s' is blocked", toks[i].val);
                return 1;
            }
        }
        break;  /* only check the first word of the last pipeline segment */
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * String-level scan (runs before tokenizing)
 * ---------------------------------------------------------------------- */

/*
 * Check the raw command string for patterns the tokenizer absorbs silently.
 * The scan is conservative: it flags occurrences even inside quoted strings
 * rather than risk missing genuine chaining attempts.
 */
static ai_valid_level_t check_string_patterns(const char *cmd, ai_validation_t *out)
{
    if (!cmd || cmd[0] == '\0') {
        snprintf(out->reason, sizeof(out->reason), "empty command");
        return AI_VALID_BLOCKED;
    }
    if (cmd[0] == '@') {
        snprintf(out->reason, sizeof(out->reason), "command begins with '@'");
        return AI_VALID_BLOCKED;
    }
    if (strchr(cmd, ';')) {
        snprintf(out->reason, sizeof(out->reason),
                 "command chaining with ';' is not supported");
        snprintf(out->suggestion, sizeof(out->suggestion),
                 "Run commands one at a time");
        return AI_VALID_BLOCKED;
    }
    if (strstr(cmd, "&&")) {
        snprintf(out->reason, sizeof(out->reason),
                 "command chaining with '&&' is not supported");
        snprintf(out->suggestion, sizeof(out->suggestion),
                 "Run commands separately; check exit status manually");
        return AI_VALID_BLOCKED;
    }
    if (strstr(cmd, "||")) {
        snprintf(out->reason, sizeof(out->reason),
                 "command chaining with '||' is not supported");
        snprintf(out->suggestion, sizeof(out->suggestion),
                 "Run commands separately; check exit status manually");
        return AI_VALID_BLOCKED;
    }
    if (strchr(cmd, '`')) {
        snprintf(out->reason, sizeof(out->reason),
                 "backtick command substitution is not supported");
        return AI_VALID_BLOCKED;
    }
    if (strstr(cmd, "$(")) {
        snprintf(out->reason, sizeof(out->reason),
                 "command substitution '$()' is not supported");
        return AI_VALID_BLOCKED;
    }
    return AI_VALID_OK;
}

/* -------------------------------------------------------------------------
 * Warn accumulation helpers
 * ---------------------------------------------------------------------- */

/* Upgrade *level to WARN (if still OK) and record the reason. */
static void upgrade_warn(ai_valid_level_t *level, ai_validation_t *out,
                          const char *reason)
{
    if (*level < AI_VALID_WARN) {
        *level = AI_VALID_WARN;
        snprintf(out->reason, sizeof(out->reason), "%s", reason);
    }
}

/* -------------------------------------------------------------------------
 * Token-level analysis
 * ---------------------------------------------------------------------- */

/*
 * Walk the token stream to identify blocked or warn-worthy patterns.
 * Each pipeline segment is collected into an argv[] array and analysed in
 * isolation; redirect filenames are excluded from the argv (they are skipped
 * by collect_argv).
 */
static ai_valid_level_t check_token_patterns(token_t *toks, int ntok,
                                              ai_validation_t *out)
{
    static const char *const blocked_names[] = {
        "sudo", "chmod", "chown", "mkfs", "dd", "shred", "eval", NULL
    };

    ai_valid_level_t result = AI_VALID_OK;

    /* Pipe-to-shell check spans the whole token stream */
    if (check_pipe_to_shell(toks, ntok, out->reason, sizeof(out->reason)))
        return AI_VALID_BLOCKED;

    /* Output redirection → WARN */
    for (int i = 0; i < ntok; i++)
        if (toks[i].type == TOK_REDIR_OUT)
            upgrade_warn(&result, out, "output redirection '>' detected");

    /* Walk each pipeline segment */
    int seg_start = 0;
    for (int i = 0; i <= ntok; i++) {
        int at_boundary = (i == ntok) || (toks[i].type == TOK_PIPE);
        if (!at_boundary) continue;

        char *argv[VARG_MAX];
        int   argc = collect_argv(toks, seg_start, i, argv, VARG_MAX);

        if (argc > 0) {
            const char *cmd0 = argv[0];

            /* Unconditionally blocked command names */
            for (int k = 0; blocked_names[k]; k++) {
                if (strcmp(cmd0, blocked_names[k]) == 0) {
                    snprintf(out->reason, sizeof(out->reason),
                             "'%s' is not allowed", cmd0);
                    return AI_VALID_BLOCKED;
                }
            }

            /* rm: block dangerous forms, warn for everything else */
            if (strcmp(cmd0, "rm") == 0) {
                char rm_reason[256] = "";
                if (rm_is_blocked(argv, argc, rm_reason, sizeof(rm_reason))) {
                    snprintf(out->reason, sizeof(out->reason), "%s", rm_reason);
                    return AI_VALID_BLOCKED;
                }
                upgrade_warn(&result, out, "'rm' may permanently delete files");
            }

            /* mv: warn */
            if (strcmp(cmd0, "mv") == 0)
                upgrade_warn(&result, out, "'mv' may overwrite files");

            /* git: block dangerous subcommands, warn for push */
            if (strcmp(cmd0, "git") == 0) {
                char git_reason[256] = "";
                if (git_is_blocked(argv, argc, git_reason, sizeof(git_reason))) {
                    snprintf(out->reason, sizeof(out->reason), "%s", git_reason);
                    return AI_VALID_BLOCKED;
                }
                if (argc >= 2 && strcmp(argv[1], "push") == 0)
                    upgrade_warn(&result, out,
                                 "'git push' may affect remote repositories");
            }
        }

        seg_start = i + 1;
    }

    return result;
}

/* -------------------------------------------------------------------------
 * Metadata-based warnings (Claude risk and confidence)
 * ---------------------------------------------------------------------- */

static void apply_metadata_warnings(const char *risk, double conf,
                                     ai_valid_level_t *level, ai_validation_t *out)
{
    if (*level >= AI_VALID_BLOCKED) return;
    if (strcmp(risk, "destructive") == 0)
        upgrade_warn(level, out, "Claude classified this command as 'destructive'");
    if (conf < 0.7)
        upgrade_warn(level, out, "Claude confidence is low");
}

/* -------------------------------------------------------------------------
 * Syntax-compatibility checks (AiShell/macOS/BSD-specific)
 * ---------------------------------------------------------------------- */

/*
 * find_shorthand_num_flag — scan arguments following a command name for a
 * numeric-only flag like -10 or -11.
 *
 * Starts scanning at scan_start (the character just after the command name).
 * Stops at '|' or end-of-string without crossing into the next pipeline
 * segment.  Returns 1 and writes the digits into num (null-terminated,
 * truncated to numsz-1) when a match is found; returns 0 otherwise.
 *
 * Only matches -N where every character after '-' is a decimal digit and the
 * flag is followed by whitespace, '|', or end-of-string.  This avoids false
 * matches on flags like -n (letter, not digit) or -rf (letters).
 */
static int find_shorthand_num_flag(const char *scan_start,
                                    char *num, size_t numsz)
{
    const char *q = scan_start;
    while (*q && *q != '|') {
        while (*q == ' ' || *q == '\t') q++;
        if (!*q || *q == '|') break;
        if (*q == '-') {
            const char *r = q + 1;
            /* Numeric-only flag: starts with a non-zero digit */
            if (*r >= '1' && *r <= '9') {
                const char *s = r;
                while (*s >= '0' && *s <= '9') s++;
                /* Must be terminated by whitespace, pipe, or end */
                if (*s == ' ' || *s == '\t' || *s == '\0' || *s == '|') {
                    size_t nlen = (size_t)(s - r);
                    if (nlen >= numsz) nlen = numsz - 1;
                    memcpy(num, r, nlen);
                    num[nlen] = '\0';
                    return 1;
                }
            }
        }
        /* Skip to the end of the current token */
        while (*q && *q != ' ' && *q != '\t' && *q != '|') q++;
    }
    return 0;
}

/*
 * scan_head_tail_shorthand — find the first occurrence of `head -N` or
 * `tail -N` anywhere in cmd, including inside pipeline segments.
 *
 * A token is recognised when it appears at the beginning of the string or
 * immediately after a whitespace or '|' character.  This prevents matching
 * substrings like "ahead" or "detail".
 *
 * Returns 1 and populates name_out ("head" or "tail") and num_out (the digit
 * string, e.g. "11") on success; returns 0 when no such pattern is found.
 */
static int scan_head_tail_shorthand(const char *cmd,
                                     char *name_out, size_t name_outsz,
                                     char *num_out,  size_t num_outsz)
{
    const char *p = cmd;
    while (*p) {
        /* Only treat as a token start when at the beginning or after a
         * word separator so we don't match inside longer words. */
        int at_word_start = (p == cmd ||
                             p[-1] == ' ' || p[-1] == '\t' || p[-1] == '|');
        if (!at_word_start) { p++; continue; }

        int is_head = (strncmp(p, "head", 4) == 0 &&
                       (p[4] == ' ' || p[4] == '\t' || p[4] == '\0' || p[4] == '|'));
        int is_tail = (strncmp(p, "tail", 4) == 0 &&
                       (p[4] == ' ' || p[4] == '\t' || p[4] == '\0' || p[4] == '|'));
        if (!is_head && !is_tail) { p++; continue; }

        if (find_shorthand_num_flag(p + 4, num_out, num_outsz)) {
            snprintf(name_out, name_outsz, "%s", is_head ? "head" : "tail");
            return 1;
        }
        p++;
    }
    return 0;
}

/*
 * Detect patterns that are commonly generated incorrectly by LLMs:
 *   - head/tail -N  (numeric-only flag) instead of -n N
 *   - find -printf  (GNU extension; not available on macOS/BSD)
 *   - **            (recursive glob; AiShell parser does not support it)
 *
 * head/tail -N anywhere in the command (including inside pipelines) is set to
 * BLOCKED because the command will always fail in AiShell; the builtin only
 * accepts  -n N.  The other patterns are set to WARN.
 * Only runs when level < BLOCKED (no point refining an already-blocked cmd).
 */
static void check_syntax_compatibility(const char *cmd,
                                        ai_valid_level_t *level,
                                        ai_validation_t *out)
{
    if (*level >= AI_VALID_BLOCKED) return;

    /* head/tail -N shorthand: scan the entire command so the check fires even
     * when head/tail appears after '|' (e.g.  find ... | sort -rn | head -11).
     * AiShell's builtin only supports  -n N; using -N always fails. */
    {
        char found_cmd[8], found_num[32];
        if (scan_head_tail_shorthand(cmd,
                                     found_cmd, sizeof(found_cmd),
                                     found_num, sizeof(found_num))) {
            *level = AI_VALID_BLOCKED;
            snprintf(out->reason, sizeof(out->reason),
                     "%s -%s: numeric-only flag not supported by AiShell "
                     "(use %s -n %s)",
                     found_cmd, found_num, found_cmd, found_num);
            snprintf(out->suggestion, sizeof(out->suggestion),
                     "Use %s -n %s FILE", found_cmd, found_num);
            return;
        }
    }

    /* find -printf: GNU-only extension; not available on macOS/BSD find */
    if (strncmp(cmd, "find", 4) == 0 &&
        (cmd[4] == ' ' || cmd[4] == '\t' || cmd[4] == '\0')) {
        if (strstr(cmd, "-printf")) {
            upgrade_warn(level, out,
                "find -printf is a GNU extension; unsupported on macOS/BSD");
            snprintf(out->suggestion, sizeof(out->suggestion),
                "Replace -printf with -exec printf ... {} + or "
                "use: find ... -print | xargs ...");
        }
    }

    /* Recursive glob **: AiShell parser does not expand ** */
    if (strstr(cmd, "**")) {
        upgrade_warn(level, out,
            "recursive glob '**' is not supported by AiShell");
        snprintf(out->suggestion, sizeof(out->suggestion),
            "Use: find <dir> -type f -name '<pattern>' "
            "instead of **/<pattern>");
    }
}

/* -------------------------------------------------------------------------
 * Over-escaped regex detection for rg/grep
 * ---------------------------------------------------------------------- */

/*
 * Detect backslash-escaped alternation (\|) in rg or grep commands.  rg uses
 * Rust regex, which does not support \| and will emit "regex error: trailing
 * backslash" or similar at runtime.  grep ERE (grep -E) also treats | as
 * alternation without a backslash; in grep BRE, \| is a GNU extension that
 * works in some implementations but not in AiShell's builtin.
 *
 * The check operates on the raw command string because the AiShell tokenizer
 * copies double-quoted content verbatim (without processing backslash escapes),
 * making the string-level search for the two-char sequence backslash+pipe the
 * most direct and reliable approach.
 *
 * Correct alternatives: use plain | inside a quoted pattern ("foo|bar"),
 * or split patterns across multiple -e flags (rg -e foo -e bar FILE).
 */
static void check_overescaped_regex(const char *cmd,
                                     ai_valid_level_t *level,
                                     ai_validation_t *out)
{
    if (*level >= AI_VALID_BLOCKED) return;

    /* Only applies to rg and grep. */
    int is_rg   = (strncmp(cmd, "rg ",   3) == 0);
    int is_grep = (strncmp(cmd, "grep ", 5) == 0);
    if (!is_rg && !is_grep) return;

    /* Look for the two-char sequence backslash + '|' anywhere in the command.
     * A bare '|' is a pipeline operator and never appears inside a word token;
     * \| inside a quoted pattern is what we want to catch. */
    if (strstr(cmd, "\\|") == NULL) return;

    const char *tool = is_rg ? "rg" : "grep";
    if (*level < AI_VALID_WARN) *level = AI_VALID_WARN;
    snprintf(out->reason, sizeof(out->reason),
             "%s: '\\|' in pattern is invalid (rg/grep does not support "
             "backslash alternation; use '|' or -e flags)", tool);
    snprintf(out->suggestion, sizeof(out->suggestion),
             "Use '|' for alternation (\"pat1|pat2\") or multiple -e flags: "
             "%s -n -e pattern1 -e pattern2 FILE", tool);
}

/* -------------------------------------------------------------------------
 * rg -e flag check
 * ---------------------------------------------------------------------- */

/*
 * Detect use of -e in rg commands.  AiShell's rg builtin is a simplified
 * implementation that accepts a single positional PATTERN; it does not
 * implement the -e / --regexp flag.  Using -e causes an "invalid option"
 * error at runtime.
 *
 * For multiple alternatives, the correct form is a single pattern with '|':
 *   rg -n "TODO|FIXME" src
 *
 * Operates on the raw command string so the check fires regardless of quoting.
 * Only applies when the first word is exactly "rg".
 */
static void check_rg_e_flag(const char *cmd,
                             ai_valid_level_t *level,
                             ai_validation_t *out)
{
    if (*level >= AI_VALID_BLOCKED) return;

    /* Only applies to rg (not grep, which may validly use -e). */
    if (strncmp(cmd, "rg ", 3) != 0) return;

    /* Look for " -e " or " -e" at end-of-string, or short-flag bundles
     * containing 'e' after '-'.  The simplest reliable check: scan for
     * the two-char sequence " -e" (space-dash-e) followed by space or end.
     * This avoids false positives on filenames or patterns containing "-e". */
    const char *p = cmd + 2;  /* skip "rg" */
    while (*p) {
        if (*p == ' ' && *(p + 1) == '-') {
            const char *flag = p + 2;
            /* Short flag bundle: -e, -ne, -en, -nie, etc. */
            if (*flag && *flag != '-') {
                for (const char *f = flag; *f && *f != ' ' && *f != '\t'; f++) {
                    if (*f == 'e') {
                        goto found;
                    }
                }
            }
            /* Long flag: --regexp */
            if (strncmp(flag, "-regexp", 7) == 0 &&
                (flag[7] == '\0' || flag[7] == ' ' || flag[7] == '=')) {
                goto found;
            }
        }
        p++;
    }
    return;

found:
    if (*level < AI_VALID_BLOCKED) *level = AI_VALID_BLOCKED;
    snprintf(out->reason, sizeof(out->reason),
             "rg: -e flag is not supported by AiShell rg "
             "(use a single pattern with '|' for alternation)");
    snprintf(out->suggestion, sizeof(out->suggestion),
             "Use a single pattern: rg -n \"TODO|FIXME\" FILE");
}

/* -------------------------------------------------------------------------
 * File-path existence check for read-heavy commands
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * check_find_exec_builtin: AiShell builtins (e.g. rg) are only available when
 * AiShell itself dispatches the top-level command or pipeline stage.  External
 * programs such as find(1) fork a subprocess and search PATH; they cannot reach
 * AiShell's builtin registry.  A command like "find ... -exec rg ..." will
 * always fail with "No such file or directory" unless a system rg binary exists.
 *
 * Detection (string-level):
 *   - command starts with "find "
 *   - AND contains "-exec rg " (the only AiShell-only builtin Claude typically
 *     places inside find -exec; extend the list as needed)
 *
 * Returns BLOCKED because the command is guaranteed to fail at runtime.
 * ---------------------------------------------------------------------- */

static void check_find_exec_builtin(const char *cmd,
                                     ai_valid_level_t *level,
                                     ai_validation_t *out)
{
    if (*level >= AI_VALID_BLOCKED) return;

    /* Only fires when the command starts with "find " */
    if (strncmp(cmd, "find ", 5) != 0) return;

    /* Look for -exec rg as the exec target. */
    if (strstr(cmd, "-exec rg ") == NULL &&
        strstr(cmd, "-exec rg\t") == NULL)
        return;

    *level = AI_VALID_BLOCKED;
    snprintf(out->reason, sizeof(out->reason),
             "find -exec rg: 'rg' is an AiShell builtin and cannot be "
             "executed by the external find program (not on PATH)");
    snprintf(out->suggestion, sizeof(out->suggestion),
             "Use grep -R -n \"PATTERN\" DIR instead, or call rg directly "
             "on a known file: rg -n \"PATTERN\" src/ai/mcp_server.c");
}

/* -------------------------------------------------------------------------
 * check_wc_total_row: when wc is invoked via -exec {} + (batch mode) and the
 * output is piped into sort -r then head, wc appends a "total" row that will
 * rank first numerically and displace one real file from the top-N list.
 * This is a semantic correctness issue, not a safety issue: return WARN.
 *
 * Detection heuristic (string-level):
 *   - command contains "-exec wc" (wc as exec target)
 *   - AND contains "{} +" (batch marker — multiple files per wc invocation)
 *   - AND contains "sort -r" or "sort -nr" or "sort -rn"
 *   - AND contains "head"
 * ---------------------------------------------------------------------- */

static void check_wc_total_row(const char *cmd,
                                ai_valid_level_t *level,
                                ai_validation_t *out)
{
    if (*level >= AI_VALID_BLOCKED) return;

    /* Must be a find ... -exec wc ... {} + pipeline */
    if (strstr(cmd, "-exec wc") == NULL) return;
    if (strstr(cmd, "{} +")     == NULL) return;

    /* Must also pipe into sort -r (any common form) and head */
    int has_sort_r = (strstr(cmd, "sort -r")  != NULL ||
                      strstr(cmd, "sort -nr") != NULL);
    if (!has_sort_r)              return;
    if (strstr(cmd, "head") == NULL) return;

    if (*level < AI_VALID_WARN) *level = AI_VALID_WARN;
    snprintf(out->reason, sizeof(out->reason),
             "wc with -exec {} + adds a 'total' row that ranks first in "
             "sort -rn | head output, displacing one real file from the top-N list");
    snprintf(out->suggestion, sizeof(out->suggestion),
             "Use xargs -n 1 to process one file at a time (no total row): "
             "find ... -print | xargs -n 1 wc -c | sort -rn | head -n N");
}

/* -------------------------------------------------------------------------
 * check_rg_dir_arg: AiShell's built-in rg accepts file paths only — it does
 * NOT recursively search directories the way real ripgrep does.  Passing a
 * directory (e.g. "src") produces empty output with no error, which is
 * misleading.  Warn and suggest using find to enumerate files first.
 * Only fires when the first token is exactly "rg".
 * ---------------------------------------------------------------------- */

static void check_rg_dir_arg(token_t *toks, int ntok,
                              ai_valid_level_t *level, ai_validation_t *out)
{
    if (*level >= AI_VALID_BLOCKED) return;
    if (ntok == 0 || toks[0].type != TOK_WORD) return;
    if (strcmp(toks[0].val, "rg") != 0) return;

    int non_opt = 0;
    for (int i = 1; i < ntok; i++) {
        if (toks[i].type == TOK_PIPE) break;   /* first pipeline stage only */
        if (toks[i].type != TOK_WORD) continue;
        const char *arg = toks[i].val;
        if (arg[0] == '-') continue;            /* option flag — skip */
        non_opt++;
        if (non_opt == 1) continue;             /* first non-option is PATTERN */

        /* Warn if this argument resolves to an existing directory. */
        struct stat st;
        if (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (*level < AI_VALID_WARN) *level = AI_VALID_WARN;
            snprintf(out->reason, sizeof(out->reason),
                     "rg: '%s' is a directory; AiShell rg does not "
                     "recursively search directories", arg);
            snprintf(out->suggestion, sizeof(out->suggestion),
                     "Use find to enumerate files first: "
                     "find %s -type f -name \"*.c\" -exec rg PATTERN {} +",
                     arg);
            break;  /* report first directory argument only */
        }
    }
}

/*
 * For commands that commonly take file-path arguments (rg, grep, cat, head,
 * tail, wc), check that non-option arguments containing '/' actually exist
 * in the filesystem relative to the current working directory.
 *
 * This catches a common LLM mistake: referencing a file at the wrong path,
 * e.g. src/mcp_server.c instead of src/ai/mcp_server.c.  The check is
 * conservative: only flags paths with '/' that don't contain glob metacharacters
 * (*, ?, [), and only upgrades level to WARN (never BLOCKED) since the issue is
 * not a safety concern.  For rg and grep, the first non-option argument is
 * skipped because it is the search pattern, not a file path.  Only the first
 * missing path is reported.  Only the first pipeline segment is examined.
 *
 * Must be called before free_tokens().
 */
static void check_file_args(token_t *toks, int ntok,
                             ai_valid_level_t *level, ai_validation_t *out)
{
    if (*level >= AI_VALID_BLOCKED) return;

    static const char *const file_cmds[]    = {
        "rg", "grep", "cat", "head", "tail", "wc", NULL
    };
    /* For these commands the first non-option arg is the search pattern. */
    static const char *const pattern_first[] = { "rg", "grep", NULL };

    if (ntok == 0 || toks[0].type != TOK_WORD) return;
    const char *cmd0 = toks[0].val;

    int is_file_cmd = 0, skip_first_arg = 0;
    for (int i = 0; file_cmds[i]; i++)
        if (strcmp(cmd0, file_cmds[i]) == 0) { is_file_cmd = 1; break; }
    if (!is_file_cmd) return;
    for (int i = 0; pattern_first[i]; i++)
        if (strcmp(cmd0, pattern_first[i]) == 0) { skip_first_arg = 1; break; }

    int non_opt = 0;
    for (int i = 1; i < ntok; i++) {
        if (toks[i].type == TOK_PIPE) break;   /* first pipeline segment only */
        if (toks[i].type != TOK_WORD) continue;
        const char *arg = toks[i].val;
        if (arg[0] == '-') continue;            /* option flag — skip */
        non_opt++;
        if (skip_first_arg && non_opt == 1) continue;  /* search pattern — skip */
        if (strchr(arg, '/') == NULL) continue; /* no '/' — not a path-like arg */
        /* Skip glob patterns to avoid false positives on unexpanded globs. */
        if (strchr(arg, '*') || strchr(arg, '?') || strchr(arg, '[')) continue;
        struct stat st;
        if (stat(arg, &st) == 0) continue;      /* path exists — fine */
        /* First missing path found: upgrade to WARN and report. */
        if (*level < AI_VALID_WARN) *level = AI_VALID_WARN;
        snprintf(out->reason, sizeof(out->reason),
                 "path does not exist: %s", arg);
        /* Suggest how to find the correct location. */
        const char *base = strrchr(arg, '/');
        base = base ? base + 1 : arg;
        snprintf(out->suggestion, sizeof(out->suggestion),
                 "Search for the file with: find . -name \"%s\"", base);
        break;  /* report first missing path only */
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

ai_valid_level_t ai_validate_command(const char *command,
                                     const char *claude_risk,
                                     double claude_confidence,
                                     ai_validation_t *out)
{
    out->level        = AI_VALID_OK;
    out->reason[0]    = '\0';
    out->suggestion[0]= '\0';

    /* 1. String-level checks before tokenizing */
    ai_valid_level_t sl = check_string_patterns(command, out);
    if (sl == AI_VALID_BLOCKED) {
        out->level = AI_VALID_BLOCKED;
        return AI_VALID_BLOCKED;
    }

    /* 2. Tokenize into a mutable copy */
    char line_copy[512];
    snprintf(line_copy, sizeof(line_copy), "%s", command);
    token_t toks[VTOK_MAX];
    int ntok = tokenize_line(line_copy, toks, VTOK_MAX);

    if (ntok == 0) {
        snprintf(out->reason, sizeof(out->reason), "empty command after parsing");
        out->level = AI_VALID_BLOCKED;
        return AI_VALID_BLOCKED;
    }

    /* 3. Token-level checks */
    ai_valid_level_t tl = check_token_patterns(toks, ntok, out);

    /* 3b. Over-escaped regex check — uses raw command string */
    if (tl < AI_VALID_BLOCKED)
        check_overescaped_regex(command, &tl, out);

    /* 3b1. find -exec builtin check — AiShell builtins are not on PATH */
    if (tl < AI_VALID_BLOCKED)
        check_find_exec_builtin(command, &tl, out);

    /* 3b2. wc batch-mode total-row check — uses raw command string */
    if (tl < AI_VALID_BLOCKED)
        check_wc_total_row(command, &tl, out);

    /* 3c. rg -e flag check — AiShell rg does not support -e */
    if (tl < AI_VALID_BLOCKED)
        check_rg_e_flag(command, &tl, out);

    /* 3d. rg directory-argument check — AiShell rg does not recurse dirs */
    if (tl < AI_VALID_BLOCKED)
        check_rg_dir_arg(toks, ntok, &tl, out);

    /* 3e. File-path existence check — must run before free_tokens() */
    if (tl < AI_VALID_BLOCKED)
        check_file_args(toks, ntok, &tl, out);

    free_tokens(toks, ntok);

    if (tl == AI_VALID_BLOCKED) {
        out->level = AI_VALID_BLOCKED;
        return AI_VALID_BLOCKED;
    }
    out->level = tl;

    /* 4. Metadata-based warnings */
    apply_metadata_warnings(claude_risk, claude_confidence, &out->level, out);

    /* 5. AiShell/macOS-BSD syntax-compatibility checks */
    check_syntax_compatibility(command, &out->level, out);

    return out->level;
}

const char *ai_valid_level_name(ai_valid_level_t level)
{
    switch (level) {
    case AI_VALID_OK:      return "ok";
    case AI_VALID_WARN:    return "warn";
    case AI_VALID_BLOCKED: return "blocked";
    default:               return "unknown";
    }
}
