#ifndef AISHELL_EXEC_H
#define AISHELL_EXEC_H

#include <pthread.h>
#include <sys/types.h>
#include <time.h>

#include "ast.h"

#define MAX_JOBS      64
#define MAX_PIPE_SEGS  8

typedef struct {
    int             id;
    pid_t           pids[MAX_PIPE_SEGS];
    int             pid_count;
    int             done_count;
    char            cmd[256];
    int             done;
    int             exit_code;
    struct timespec start_time; /* wall-clock time the job was launched */
} job_t;

extern job_t           g_jobs[];
extern int             g_job_count;
/* Protects g_jobs / g_job_count between the jobs builtin thread and reap_jobs */
extern pthread_mutex_t g_jobs_mutex;

/*
 * When non-zero, execute_line() prints the AST as a text tree and writes
 * ast.dot before executing each command.  Toggle with `ast on/off` or
 * set AISHELL_AST=1 in the environment, or pass --ast on the command line.
 */
extern int g_ast_debug;

/*
 * Shell's own process group ID.  Set at startup when stdin is a terminal.
 * Non-zero means the shell is running interactively with full job control:
 * run_pipeline() gives the terminal to foreground jobs via tcsetpgrp() and
 * restores it here when they finish.  Zero means non-interactive mode —
 * tcsetpgrp() calls are skipped but setpgid() still runs so background jobs
 * are always isolated from the shell's process group.
 */
extern pid_t g_shell_pgid;

/*
 * PID of the most recently launched background job (the value AiShell prints
 * as "[N] PID").  Expands as $! in any command word, matching bash behaviour.
 * Zero before any background job has been started.
 */
extern pid_t g_last_bg_pid;

void        reap_jobs(void);
int         execute_line(char *line);
int         execute_ast(ast_node_t *node);

#endif /* AISHELL_EXEC_H */
