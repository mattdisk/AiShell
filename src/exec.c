#define _POSIX_C_SOURCE 200809L

#include "exec.h"
#include "registry.h"
#include "token.h"
#include "command.h"
#include "parser.h"
#include "ast_print.h"
#include "demo_trace.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Global: AST debug mode flag (non-zero → print AST before executing)
 * ---------------------------------------------------------------------- */

int g_ast_debug = 0;

/* -------------------------------------------------------------------------
 * Global: shell's own process group ID for job control.
 * Set to getpgrp() in main() when stdin is a terminal.  Zero means the
 * shell is running non-interactively — tcsetpgrp() is skipped but setpgid()
 * still runs so background jobs are always isolated from the shell's group.
 * ---------------------------------------------------------------------- */

pid_t g_shell_pgid  = 0;
pid_t g_last_bg_pid = 0;  /* updated by job_add(); expanded as $! */

/* -------------------------------------------------------------------------
 * Job table
 * ---------------------------------------------------------------------- */

job_t           g_jobs[MAX_JOBS];
int             g_job_count    = 0;
pthread_mutex_t g_jobs_mutex   = PTHREAD_MUTEX_INITIALIZER;

static int g_next_job_id = 1;

static void job_add(pid_t *pids, int count, const char *cmd)
{
    if (g_job_count >= MAX_JOBS) return;
    job_t *j  = &g_jobs[g_job_count++];
    j->id         = g_next_job_id++;
    j->pid_count  = count;
    j->done_count = 0;
    j->done       = 0;
    j->exit_code  = 0;
    for (int i = 0; i < count && i < MAX_PIPE_SEGS; i++) j->pids[i] = pids[i];
    snprintf(j->cmd, sizeof(j->cmd), "%s", cmd ? cmd : "?");
    clock_gettime(CLOCK_MONOTONIC, &j->start_time);
    g_last_bg_pid = pids[0];  /* make $! available immediately */
    printf("[%d] %d\n", j->id, (int)pids[0]);
    fflush(stdout);
}

/*
 * Reap all completed background children.  Called from the main REPL loop
 * when g_got_sigchld is set, and also proactively before each prompt.
 * g_jobs_mutex is held for the duration so the jobs builtin (which may run
 * in a pthread) sees a consistent snapshot.
 */
void reap_jobs(void)
{
    int    status;
    pid_t  pid;

    pthread_mutex_lock(&g_jobs_mutex);

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        for (int i = 0; i < g_job_count; i++) {
            job_t *j = &g_jobs[i];
            if (j->done) continue;
            for (int k = 0; k < j->pid_count; k++) {
                if (j->pids[k] == pid) {
                    j->done_count++;
                    j->exit_code = code;
                    if (j->done_count == j->pid_count) {
                        j->done = 1;
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        double elapsed =
                            (double)(now.tv_sec  - j->start_time.tv_sec) +
                            (double)(now.tv_nsec - j->start_time.tv_nsec) * 1e-9;
                        printf("\n[%d]+ done (%.1fs)\t%s\n",
                               j->id, elapsed, j->cmd);
                        fflush(stdout);
                    }
                    break;
                }
            }
        }
    }

    /* compact: slide out fully-done entries */
    int wi = 0;
    for (int ri = 0; ri < g_job_count; ri++) {
        if (!g_jobs[ri].done) g_jobs[wi++] = g_jobs[ri];
    }
    g_job_count = wi;

    pthread_mutex_unlock(&g_jobs_mutex);
}

/* -------------------------------------------------------------------------
 * Segment type (one stage of a pipeline) — used internally by run_pipeline
 * ---------------------------------------------------------------------- */

typedef struct {
    char *argv[MAX_ARGS];
    int   argc;
    char *infile;
    char *outfile;
    int   append;
} segment_t;

/* -------------------------------------------------------------------------
 * Apply file redirections in the current process
 * ---------------------------------------------------------------------- */

static int apply_redir(segment_t *s)
{
    if (s->infile) {
        int fd = open(s->infile, O_RDONLY);
        if (fd < 0) { perror(s->infile); return -1; }
        if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2"); close(fd); return -1; }
        close(fd);
    }
    if (s->outfile) {
        int flags = O_WRONLY | O_CREAT | (s->append ? O_APPEND : O_TRUNC);
        int fd = open(s->outfile, flags, 0644);
        if (fd < 0) { perror(s->outfile); return -1; }
        if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); close(fd); return -1; }
        close(fd);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Run a builtin in the parent with saved/restored stdio  (constraint 3)
 * ---------------------------------------------------------------------- */

static int run_builtin_redir(cmd_spec_t *spec, int ac, char **av, segment_t *s)
{
    int saved_in  = -1, saved_out = -1;

    if (s->infile) {
        saved_in = dup(STDIN_FILENO);
        if (saved_in < 0) { perror("dup"); return 1; }
    }
    if (s->outfile) {
        saved_out = dup(STDOUT_FILENO);
        if (saved_out < 0) {
            perror("dup");
            if (saved_in >= 0) close(saved_in);
            return 1;
        }
    }

    if (apply_redir(s) < 0) {
        if (saved_in  >= 0) { dup2(saved_in,  STDIN_FILENO);  close(saved_in); }
        if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
        return 1;
    }

    TRACE("builtin '%s' with redirection → main thread (dup2 requires same process), pid=%d",
          spec->name, (int)getpid());
    int rc = spec->run(ac, av);
    /* flush before restoring fds so buffered output lands in the file */
    fflush(NULL);

    if (saved_in  >= 0) { dup2(saved_in,  STDIN_FILENO);  close(saved_in); }
    if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
    return rc;
}

/* -------------------------------------------------------------------------
 * Pthread support for built-in commands
 * ---------------------------------------------------------------------- */

typedef struct {
    cmd_spec_t *spec;
    int         argc;
    char      **argv;
    int         result;
} builtin_thread_arg_t;

static void *builtin_thread_fn(void *arg)
{
    builtin_thread_arg_t *a = (builtin_thread_arg_t *)arg;
    TRACE("builtin '%s' → running in pthread=%lu, process pid=%d (internal, threaded)",
          a->spec->name, (unsigned long)pthread_self(), (int)getpid());
    a->result = a->spec->run(a->argc, a->argv);
    return NULL;
}

static const char *g_threaded_builtins[] = {
    "help",    /* reads the registry, prints to stdout */
    "jobs",    /* reads g_jobs under g_jobs_mutex */
    "pwd",     /* getcwd(), prints to stdout */
    "history", /* reads history list, prints to stdout */
    "echo",    /* prints arguments to stdout */
    "printf",  /* formatted output to stdout */
    "type",    /* reads registry + PATH, prints to stdout */
    "true",    /* returns 0 */
    "false",   /* returns 1 */
    "date",    /* reads system clock, prints to stdout */
    "env",     /* reads environ, prints to stdout */
    NULL
};

static int builtin_is_threaded(const char *name)
{
    for (int i = 0; g_threaded_builtins[i]; i++)
        if (strcmp(g_threaded_builtins[i], name) == 0) return 1;
    return 0;
}

static int run_builtin_in_thread(cmd_spec_t *spec, int argc, char **argv)
{
    builtin_thread_arg_t arg = { spec, argc, argv, 0 };
    pthread_t tid;
    if (pthread_create(&tid, NULL, builtin_thread_fn, &arg) != 0) {
        perror("pthread_create");
        return spec->run(argc, argv);
    }
    pthread_join(tid, NULL);
    return arg.result;
}

/* -------------------------------------------------------------------------
 * Run a pipeline (>=1 segments) or a single background command.
 *
 * Job-control design (process-group isolation):
 *   Every forked job gets its own process group via setpgid().  This
 *   separates it from the shell's group so that Ctrl+C (SIGINT sent to the
 *   terminal's *foreground* process group) only reaches the currently running
 *   foreground job — not background jobs and not the shell.
 *
 *   For a multi-stage pipeline, all stages share one group: the first
 *   child's pid serves as the group id for every stage.
 *
 *   setpgid() is called in both the child and the parent to close the race
 *   window between fork() and setpgid(): whichever side runs first, the
 *   child ends up in the right group before any signal is delivered.
 *
 *   tcsetpgrp() (terminal foreground-group transfer) is only used when
 *   g_shell_pgid > 0, meaning stdin is a real terminal.  In non-interactive
 *   mode the setpgid() calls still happen, keeping background jobs isolated.
 * ---------------------------------------------------------------------- */

static int run_pipeline(segment_t *segs, int nseg, int background,
                        char ***exp_argvs, int *exp_argcs)
{
    int   pfds[MAX_PIPE_SEGS - 1][2];
    pid_t pids[MAX_PIPE_SEGS];
    pid_t pgid = 0;  /* process group for this job; set after the first fork */

    for (int i = 0; i < nseg - 1; i++) {
        if (pipe(pfds[i]) < 0) {
            perror("pipe");
            for (int k = 0; k < i; k++) { close(pfds[k][0]); close(pfds[k][1]); }
            return 1;
        }
    }

    for (int i = 0; i < nseg; i++) {
        segment_t *s  = &segs[i];
        char     **av = exp_argvs[i];
        int        ac = exp_argcs[i];

        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            for (int k = i; k < nseg - 1; k++) {
                close(pfds[k][0]); close(pfds[k][1]);
            }
            for (int k = 0; k < i; k++) waitpid(pids[k], NULL, 0);
            return 1;
        }

        if (pids[i] == 0) {
            /* child: restore signal defaults so Ctrl+C reaches foreground jobs
             * and inherited SIG_IGN dispositions don't carry into exec'd programs */
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);

            /* Separate this job from the shell's process group.
             * First child (i==0): setpgid(0,0) makes it a new group leader.
             * Later children: join the first child's group using its pid.
             * The parent mirrors this call below to close the race window. */
            setpgid(0, (i == 0) ? 0 : pgid);

            /* wire stdin from previous pipe stage */
            if (i > 0) {
                if (dup2(pfds[i - 1][0], STDIN_FILENO) < 0) {
                    perror("dup2"); _exit(1);
                }
            }
            /* wire stdout to next pipe stage */
            if (i < nseg - 1) {
                if (dup2(pfds[i][1], STDOUT_FILENO) < 0) {
                    perror("dup2"); _exit(1);
                }
            }
            /* close all pipe ends in the child */
            for (int k = 0; k < nseg - 1; k++) {
                close(pfds[k][0]); close(pfds[k][1]);
            }
            /* file redirections override the pipe wiring above */
            if (apply_redir(s) < 0) _exit(1);

            /* builtin in child (pipeline constraint) or external */
            cmd_spec_t *spec = find_command(av[0]);
#ifdef DEMO_TRACE
            /* For pipelines every stage is a child process regardless of whether
             * it is a builtin or external.  For a single external foreground
             * command (nseg==1) spec will be NULL and execvp is used. */
            if (nseg > 1)
                TRACE("pipeline stage '%s' -> child pid=%d, shell pid=%d (forked)",
                      av[0], (int)getpid(), (int)getppid());
            else if (!spec)
                TRACE("external '%s' -> child pid=%d, shell pid=%d (fork+execvp)",
                      av[0], (int)getpid(), (int)getppid());
#endif
            if (spec) { int r = spec->run(ac, av); fflush(NULL); _exit(r); }
            execvp(av[0], av);
            fprintf(stderr, "%s: %s\n", av[0], strerror(errno));
            _exit(127);
        }

        /* parent: mirror setpgid to close the race window.
         * If the child has already called setpgid, this is a harmless no-op.
         * If the child has not yet called it, this ensures the child is in
         * the right group before we send it any signals. */
        if (i == 0) pgid = pids[0];
        setpgid(pids[i], pgid);
    }

    /* parent: close all pipe fds */
    for (int i = 0; i < nseg - 1; i++) { close(pfds[i][0]); close(pfds[i][1]); }

    if (background) {
        job_add(pids, nseg, segs[0].argv[0] ? segs[0].argv[0] : "?");
        return 0;
    }

    /* Foreground job: give the terminal to the job's process group so that
     * Ctrl+C (SIGINT to the terminal's foreground group) reaches the job,
     * not the shell.  Background jobs are already in a separate group and
     * are therefore unaffected by Ctrl+C at the shell prompt. */
    if (g_shell_pgid > 0) {
        tcsetpgrp(STDIN_FILENO, pgid);
    }

    /* wait for all foreground children; return exit status of the last stage.
     * Retry on EINTR: a SIGCHLD for a background job can interrupt waitpid()
     * when SA_RESTART is not set on the SIGCHLD handler. */
    int last_status = 0;
    for (int i = 0; i < nseg; i++) {
        int st; pid_t r;
        do { r = waitpid(pids[i], &st, 0); } while (r == -1 && errno == EINTR);
        if (i == nseg - 1)
            last_status = (r > 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : 1;
    }

    /* Restore terminal control to the shell's process group */
    if (g_shell_pgid > 0) {
        tcsetpgrp(STDIN_FILENO, g_shell_pgid);
    }

    return last_status;
}

/* =========================================================================
 * Session 7: AST-based execution layer
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * expand_env — expand $NAME and ${NAME} in a word using getenv().
 * Returns a newly malloc'd string; never returns NULL (returns "" on OOM).
 * ---------------------------------------------------------------------- */

/* Sentinel set by the parser for single-quoted tokens (must match parser.c). */
#define SQ_SENTINEL '\x01'

static char *expand_env(const char *word)
{
    /* \x01 sentinel: word came from single-quote context — return verbatim,
     * no $VAR expansion.  The sentinel itself is not part of the word value. */
    if ((unsigned char)word[0] == (unsigned char)SQ_SENTINEL)
        return strdup(word + 1);

    char   buf[BUF_SIZE * 2];
    size_t out = 0;
    const char *p = word;

    while (*p && out < sizeof(buf) - 1) {
        if (*p == '$') {
            p++;
            char   name[256];
            size_t nlen   = 0;
            int    braced = (*p == '{');
            if (braced) p++;

            /* $! — PID of the most recently started background job */
            if (*p == '!') {
                p++;
                if (braced && *p == '}') p++;
                char pidbuf[32];
                snprintf(pidbuf, sizeof(pidbuf), "%d", (int)g_last_bg_pid);
                size_t vlen = strlen(pidbuf);
                if (out + vlen < sizeof(buf) - 1) {
                    memcpy(buf + out, pidbuf, vlen);
                    out += vlen;
                }
                continue;
            }

            while (*p && (isalnum((unsigned char)*p) || *p == '_') &&
                   nlen < sizeof(name) - 1)
                name[nlen++] = *p++;
            name[nlen] = '\0';

            if (braced && *p == '}') p++;

            const char *val = (nlen > 0) ? getenv(name) : NULL;
            if (!val) val = "";
            size_t vlen = strlen(val);
            if (out + vlen < sizeof(buf) - 1) {
                memcpy(buf + out, val, vlen);
                out += vlen;
            }
        } else {
            buf[out++] = *p++;
        }
    }
    buf[out] = '\0';
    return strdup(buf);
}

/* -------------------------------------------------------------------------
 * ast_to_segments — flatten an AST into a segment array for run_pipeline().
 *
 * Unwraps AST_BACKGROUND and collects AST_REDIRECT nodes, then builds
 * segment_t entries from the inner AST_PIPELINE or AST_COMMAND.
 * Input redirections apply to the first segment; output to the last.
 * ---------------------------------------------------------------------- */

static int ast_to_segments(ast_node_t *node, segment_t *segs, int *nseg, int *background)
{
    *nseg       = 0;
    *background = 0;

    ast_node_t *inner = node;

    if (inner->type == AST_BACKGROUND) {
        *background = 1;
        inner = inner->background.child;
    }

    /* Collect redirections (innermost wins for conflicting types) */
    char *infile  = NULL;
    char *outfile = NULL;
    int   append  = 0;

    while (inner && inner->type == AST_REDIRECT) {
        switch (inner->redirect.redir_type) {
        case REDIR_IN:     infile  = inner->redirect.filename; break;
        case REDIR_OUT:    outfile = inner->redirect.filename; append = 0; break;
        case REDIR_APPEND: outfile = inner->redirect.filename; append = 1; break;
        }
        inner = inner->redirect.child;
    }

    if (!inner) {
        fprintf(stderr, "aishell: internal error: empty redirect chain\n");
        return -1;
    }

    if (inner->type == AST_PIPELINE) {
        int n = inner->pipeline.count;
        if (n > MAX_PIPE_SEGS) {
            fprintf(stderr, "aishell: too many pipeline stages\n");
            return -1;
        }
        for (int i = 0; i < n; i++) {
            ast_node_t *stage = inner->pipeline.stages[i];
            if (!stage || stage->type != AST_COMMAND) {
                fprintf(stderr, "aishell: internal error: expected command in pipeline\n");
                return -1;
            }
            segs[i].argc = stage->cmd.argc;
            for (int j = 0; j < stage->cmd.argc; j++)
                segs[i].argv[j] = stage->cmd.argv[j];
            segs[i].argv[stage->cmd.argc] = NULL;
            segs[i].infile  = NULL;
            segs[i].outfile = NULL;
            segs[i].append  = 0;
        }
        *nseg = n;
        if (infile)  segs[0].infile     = infile;
        if (outfile) { segs[n-1].outfile = outfile; segs[n-1].append = append; }

    } else if (inner->type == AST_COMMAND) {
        segs[0].argc = inner->cmd.argc;
        for (int j = 0; j < inner->cmd.argc; j++)
            segs[0].argv[j] = inner->cmd.argv[j];
        segs[0].argv[inner->cmd.argc] = NULL;
        segs[0].infile  = infile;
        segs[0].outfile = outfile;
        segs[0].append  = append;
        *nseg = 1;

    } else {
        fprintf(stderr, "aishell: internal error: unexpected AST node type %d\n",
                (int)inner->type);
        return -1;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Structured JSON help for external commands
 *
 * When an installed (or any external) command is invoked with both a help
 * flag (-h / --help) AND --json, the shell intercepts the call, parses the
 * plain-text help output into the same structured JSON shape that builtins
 * produce via print_help_json(), and additionally enriches it with package
 * metadata (description, version) read from ~/.mysh/pkgs when available.
 * ---------------------------------------------------------------------- */

/* Return 1 if argv contains both a help flag and --json. */
static int has_json_and_help(int argc, char **argv)
{
    int found_help = 0, found_json = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
            found_help = 1;
        if (strcmp(argv[i], "--json") == 0)
            found_json = 1;
    }
    return found_help && found_json;
}

/* Read a single JSON string field from a pkg.json buffer. */
static int pkgjson_get(const char *buf, const char *key, char *out, size_t n)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(buf, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < n - 1) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

/* Try to load pkg metadata for `cmd` from ~/.mysh.  Fills meta_desc and
 * meta_ver (both sized META_BUF) if found; leaves them empty otherwise. */
#define META_BUF 256
static void load_pkg_meta(const char *cmd,
                          char meta_desc[META_BUF], char meta_ver[META_BUF])
{
    meta_desc[0] = meta_ver[0] = '\0';
    const char *home = getenv("HOME");
    if (!home) return;

    /* The binary is symlinked as ~/.mysh/bin/<cmd> → <pkgdir>/bin/<cmd> */
    char bin_path[PATH_MAX];
    snprintf(bin_path, sizeof(bin_path), "%s/.mysh/bin/%s", home, cmd);
    char target[PATH_MAX] = "";
    ssize_t tlen = readlink(bin_path, target, sizeof(target) - 1);
    if (tlen < 0) return;
    target[tlen] = '\0';

    /* Strip "/bin/<cmd>" suffix to get the package directory */
    char *bin_sep = strstr(target, "/bin/");
    if (!bin_sep) return;
    *bin_sep = '\0';   /* target is now the package directory path */

    char pjpath[PATH_MAX];
    snprintf(pjpath, sizeof(pjpath), "%s/pkg.json", target);
    FILE *fp = fopen(pjpath, "r");
    if (!fp) return;

    char buf[4096] = "";
    size_t sz = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[sz] = '\0';
    fclose(fp);

    pkgjson_get(buf, "description", meta_desc, META_BUF);
    pkgjson_get(buf, "version",     meta_ver,  META_BUF);
}

/* One parsed option entry from the help text. */
typedef struct {
    char short_f[8];   /* e.g. "-h"   */
    char long_f[64];   /* e.g. "--help" */
    char argument[32]; /* e.g. "SIGNAL" */
    char desc[128];    /* option description */
} ext_opt_t;

/*
 * Parse a standard help-text option line into an ext_opt_t.
 *
 * Expected formats (same as argtable3 / our hello template):
 *   "  -h, --help   description"
 *   "  -s SIGNAL    description"
 *   "  --json        description"
 *   "  PATH          description"
 *
 * Returns 1 if a valid option was found, 0 otherwise.
 */
static int parse_opt_line(const char *line, ext_opt_t *o)
{
    memset(o, 0, sizeof(*o));
    const char *p = line;

    /* Must be indented */
    if (*p != ' ' && *p != '\t') return 0;
    while (*p == ' ' || *p == '\t') p++;

    /* Must start with '-' or uppercase (positional arg) */
    if (*p != '-' && !(*p >= 'A' && *p <= 'Z')) return 0;

    /* Parse all flag tokens (separated by ", " or " ") until 2+ spaces */
    while (*p && !(*p == ' ' && *(p + 1) == ' ') && *p != '\n') {
        /* Skip comma/space separators between tokens */
        while (*p == ',' || *p == ' ') {
            if (*p == ' ' && *(p + 1) == ' ') goto done_flags; /* desc separator */
            p++;
        }
        if (!*p || *p == '\n') break;

        if (*p == '-' && *(p + 1) != '-') {
            /* Short flag: -x */
            o->short_f[0] = '-';
            o->short_f[1] = *(p + 1);
            o->short_f[2] = '\0';
            p += 2;
        } else if (*p == '-' && *(p + 1) == '-') {
            /* Long flag: --xxx */
            int i = 0;
            while (*p && *p != ' ' && *p != ',' && *p != '\n' && i < 63)
                o->long_f[i++] = *p++;
            o->long_f[i] = '\0';
        } else if (*p >= 'A' && *p <= 'Z') {
            /* Uppercase word = argument placeholder */
            int i = 0;
            while (*p && *p != ' ' && *p != ',' && *p != '\n' && i < 31)
                o->argument[i++] = *p++;
            o->argument[i] = '\0';
        } else {
            p++;
        }
    }

done_flags:
    /* Skip the separator (2+ spaces) to reach the description */
    if (*p == ' ' && *(p + 1) == ' ') {
        while (*p == ' ') p++;
        int i = 0;
        while (*p && *p != '\n' && i < 127) o->desc[i++] = *p++;
        o->desc[i] = '\0';
        /* trim trailing spaces */
        while (i > 0 && o->desc[i - 1] == ' ') o->desc[--i] = '\0';
    }

    return (o->short_f[0] || o->long_f[0] || o->argument[0]);
}

/*
 * Parse the plain-text help captured from `prog -h` and print it as the
 * same structured JSON that print_help_json() produces for builtins.
 * Package metadata (description, version) is merged in when available.
 */
static void print_parsed_help_json(const char *prog, const char *text,
                                   const char *meta_desc, const char *meta_ver)
{
    /* --- Usage line --------------------------------------------------- */
    char usage[256] = "";
    const char *p = strstr(text, "Usage:");
    if (p) {
        p += 6;
        while (*p == ' ') p++;
        const char *eol = strchr(p, '\n');
        size_t ulen = eol ? (size_t)(eol - p) : strlen(p);
        if (ulen >= sizeof(usage)) ulen = sizeof(usage) - 1;
        memcpy(usage, p, ulen);
        usage[ulen] = '\0';
    }

    /* --- Description -------------------------------------------------- */
    /* Prefer package metadata; fall back to the line after Usage: */
    char desc[META_BUF] = "";
    if (meta_desc && meta_desc[0]) {
        snprintf(desc, sizeof(desc), "%s", meta_desc);
    } else if (p) {
        const char *after = strchr(p, '\n');
        if (after) {
            after++;
            while (*after == '\n') after++;          /* skip blank lines */
            if (*after && *after != ' ' && *after != '-') {
                const char *eol = strchr(after, '\n');
                size_t dlen = eol ? (size_t)(eol - after) : strlen(after);
                if (dlen >= sizeof(desc)) dlen = sizeof(desc) - 1;
                memcpy(desc, after, dlen);
                desc[dlen] = '\0';
            }
        }
    }

    /* --- Options ------------------------------------------------------ */
    ext_opt_t opts[32];
    int nopt = 0;

    /* Find the options section: "Options:\n" header or first indented flag */
    const char *opt_start = strstr(text, "\nOptions:");
    if (opt_start) {
        opt_start = strchr(opt_start + 1, '\n'); /* skip "Options:" line */
        if (opt_start) opt_start++;
    } else {
        /* No "Options:" header — scan for the first indented flag line */
        opt_start = text;
        while (*opt_start) {
            if ((*opt_start == ' ' || *opt_start == '\t') && *(opt_start+1) == '-')
                break;
            opt_start = strchr(opt_start, '\n');
            if (!opt_start) { opt_start = NULL; break; }
            opt_start++;
        }
    }

    if (opt_start) {
        const char *line = opt_start;
        while (line && *line && nopt < 32) {
            if (*line == '\n') { line++; continue; }
            /* Stop at an unindented non-blank line (e.g. "Examples:") */
            if (*line != ' ' && *line != '\t') break;

            ext_opt_t o;
            if (parse_opt_line(line, &o))
                opts[nopt++] = o;

            line = strchr(line, '\n');
            if (line) line++;
        }
    }

    /* --- Emit JSON (same shape as print_help_json) -------------------- */
    printf("{\n");
    printf("  \"command\": \"");     json_escape_print(prog);  printf("\"");
    if (desc[0]) {
        printf(",\n  \"description\": \"");
        json_escape_print(desc);
        printf("\"");
    }
    if (meta_ver && meta_ver[0]) {
        printf(",\n  \"version\": \"");
        json_escape_print(meta_ver);
        printf("\"");
    }
    if (usage[0]) {
        printf(",\n  \"usage\": \"");
        json_escape_print(usage);
        printf("\"");
    }
    if (nopt > 0) {
        printf(",\n  \"options\": [");
        for (int i = 0; i < nopt; i++) {
            if (i > 0) putchar(',');
            printf("\n    {");
            int nf = 0;
            if (opts[i].short_f[0]) {
                printf("\"short\": \"");
                json_escape_print(opts[i].short_f);
                putchar('"');
                nf++;
            }
            if (opts[i].long_f[0]) {
                if (nf) printf(", ");
                printf("\"long\": \"");
                json_escape_print(opts[i].long_f);
                putchar('"');
                nf++;
            }
            if (opts[i].argument[0]) {
                if (nf) printf(", ");
                printf("\"argument\": \"");
                json_escape_print(opts[i].argument);
                putchar('"');
                nf++;
            }
            if (opts[i].desc[0]) {
                if (nf) printf(", ");
                printf("\"description\": \"");
                json_escape_print(opts[i].desc);
                putchar('"');
            }
            putchar('}');
        }
        printf("\n  ]");
    }
    printf("\n}\n");
}

/* Run `prog` with --json stripped (keeping -h), capture the plain-text help
 * output, parse it into structured JSON, and enrich with pkg metadata. */
static int json_help_external(const char *prog, int argc, char **argv)
{
    /* Build argv without --json */
    char **nargv = malloc(((size_t)argc + 1) * sizeof(char *));
    if (!nargv) return 1;
    int nargc = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--json") != 0)
            nargv[nargc++] = argv[i];
    }
    nargv[nargc] = NULL;

    /* Pipe to capture output */
    int pfd[2];
    if (pipe(pfd) < 0) { free(nargv); return 1; }

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); free(nargv); return 1; }
    if (pid == 0) {
        signal(SIGINT,  SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execvp(prog, nargv);
        _exit(127);
    }
    close(pfd[1]);
    free(nargv);

    /* Read the help text */
    char buf[4096] = "";
    size_t total = 0;
    ssize_t n;
    while ((n = read(pfd[0], buf + total, sizeof(buf) - total - 1)) > 0) {
        total += (size_t)n;
        if (total >= sizeof(buf) - 1) break;
    }
    buf[total] = '\0';
    close(pfd[0]);

    int st = 0; pid_t r;
    do { r = waitpid(pid, &st, 0); } while (r == -1 && errno == EINTR);

    /* Load package metadata from ~/.mysh if this command was installed */
    char meta_desc[META_BUF] = "", meta_ver[META_BUF] = "";
    load_pkg_meta(prog, meta_desc, meta_ver);

    /* Parse the plain-text help and emit structured JSON */
    print_parsed_help_json(prog, buf, meta_desc, meta_ver);

    return (r > 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : 1;
}

/* -------------------------------------------------------------------------
 * execute_ast — execute a parsed AST node.
 *
 * Handles assignments directly; delegates everything else to run_pipeline()
 * via ast_to_segments(), reusing the existing fork/pipe/dup2 infrastructure.
 * ---------------------------------------------------------------------- */

int execute_ast(ast_node_t *node)
{
    if (!node) return 0;

    /* Environment variable assignment */
    if (node->type == AST_ASSIGNMENT) {
        if (setenv(node->assignment.name, node->assignment.value, 1) != 0) {
            perror("setenv");
            return 1;
        }
        return 0;
    }

    /* Flatten AST → segment array */
    segment_t segs[MAX_PIPE_SEGS];
    int nseg = 0, background = 0;
    if (ast_to_segments(node, segs, &nseg, &background) < 0) return 1;
    if (nseg == 0) return 0;

    /* Expand $VAR in infile/outfile paths */
    char *infile_exp[MAX_PIPE_SEGS];
    char *outfile_exp[MAX_PIPE_SEGS];
    for (int i = 0; i < nseg; i++) {
        infile_exp[i]  = NULL;
        outfile_exp[i] = NULL;
    }
    for (int i = 0; i < nseg; i++) {
        if (segs[i].infile) {
            infile_exp[i]   = expand_env(segs[i].infile);
            segs[i].infile  = infile_exp[i];
        }
        if (segs[i].outfile) {
            outfile_exp[i]  = expand_env(segs[i].outfile);
            segs[i].outfile = outfile_exp[i];
        }
    }

    /* Expand $VAR in argv words, then glob-expand */
    char **exp_argvs[MAX_PIPE_SEGS];
    int    exp_argcs[MAX_PIPE_SEGS];
    int    exp_ok = 1;

    for (int i = 0; i < nseg; i++) { exp_argvs[i] = NULL; exp_argcs[i] = 0; }

    for (int i = 0; i < nseg && exp_ok; i++) {
        /* Variable expansion into a temporary argv */
        char *vexp[MAX_ARGS + 1];
        for (int j = 0; j < segs[i].argc; j++)
            vexp[j] = expand_env(segs[i].argv[j]);
        vexp[segs[i].argc] = NULL;

        if (expand_globs(segs[i].argc, vexp, &exp_argcs[i], &exp_argvs[i]) != 0) {
            fprintf(stderr, "aishell: glob expansion failed\n");
            exp_ok = 0;
        } else if (exp_argcs[i] == 0 || exp_argvs[i][0] == NULL) {
            fprintf(stderr, "aishell: empty command in pipeline\n");
            exp_ok = 0;
        }

        /* expand_globs strdup'd everything; free the var-expanded temps */
        for (int j = 0; j < segs[i].argc; j++) free(vexp[j]);
    }

    int status = 0;
    if (!exp_ok) { status = 1; goto cleanup; }

    /* Single foreground command — apply threading and builtin constraints */
    if (nseg == 1 && !background) {
        char      **av   = exp_argvs[0];
        int         ac   = exp_argcs[0];
        cmd_spec_t *spec = find_command(av[0]);

        if (spec) {
            if (!segs[0].infile && !segs[0].outfile) {
                if (builtin_is_threaded(av[0])) {
                    status = run_builtin_in_thread(spec, ac, av);
                } else {
                    TRACE("direct builtin '%s' → main thread (shell state), pid=%d, pthread=%lu",
                          av[0], (int)getpid(), (unsigned long)pthread_self());
                    status = spec->run(ac, av);
                }
            } else {
                status = run_builtin_redir(spec, ac, av, &segs[0]);
            }
            goto cleanup;
        }
        /* External command: intercept "-h/--help + --json" so installed packages
         * get machine-readable help without needing to implement --json themselves. */
        if (has_json_and_help(ac, av)) {
            status = json_help_external(av[0], ac, av);
            goto cleanup;
        }
        /* All other external single foreground commands fall through to run_pipeline */
    }

    status = run_pipeline(segs, nseg, background, exp_argvs, exp_argcs);

cleanup:
    for (int i = 0; i < nseg; i++) {
        if (exp_argvs[i]) free_expanded_args(exp_argcs[i], exp_argvs[i]);
        free(infile_exp[i]);
        free(outfile_exp[i]);
    }
    return status;
}

/* -------------------------------------------------------------------------
 * execute_line — REPL entry point.
 *
 * New flow (Session 7):
 *   input string → tokenize → parse → AST → [print AST] → execute_ast
 * ---------------------------------------------------------------------- */

int execute_line(char *line)
{
    /* Tokenize the input line */
    token_t tokens[MAX_ARGS * 2];
    int ntok = tokenize_line(line, tokens, MAX_ARGS * 2);
    if (ntok <= 0) return 0;

    /* Grammar-based parse: tokens → AST */
    ast_node_t *ast = parse_input(tokens, ntok);

    /* Token string values are owned by the tokenizer; free them now that
     * the parser has strdup'd what it needs into the AST. */
    for (int i = 0; i < ntok; i++)
        if (tokens[i].type == TOK_WORD) free(tokens[i].val);

    if (!ast) return 1;  /* parse error already reported */

    /* AST debug mode: print text tree + write DOT file */
    if (g_ast_debug) {
        printf("--- AST ---\n");
        ast_print_text(ast);
        printf("-----------\n");
        ast_write_dot(ast, "ast.dot");
        /* Generate PNG if Graphviz dot is available (failure is silent) */
        if (system("which dot > /dev/null 2>&1") == 0)
            (void)system("dot -Tpng ast.dot -o ast.png 2>/dev/null");
    }

    /* Execute the AST, timing the wall-clock duration */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int status = execute_ast(ast);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (double)(t1.tv_sec  - t0.tv_sec) +
                     (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    /* Show elapsed time for foreground commands that took at least 1 second.
     * Background jobs return almost instantly so they never trigger this. */
    if (elapsed >= 1.0) {
        printf("[%.1fs]\n", elapsed);
    }

    ast_free(ast);
    return status;
}
