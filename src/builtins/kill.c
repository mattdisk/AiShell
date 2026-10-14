#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../registry.h"
#include "../command.h"
#include "../exec.h"   /* g_jobs, g_job_count, g_jobs_mutex */

static int run(int argc, char **argv)
{
    struct arg_lit *help   = arg_lit0("h", "help", "Show this help message");
    struct arg_str *sig    = arg_str0("s", NULL, "SIGNAL", "Signal name or number (default: TERM)");
    struct arg_lit *json   = arg_lit0(NULL, "json", "Structured result");
    /* Accept either a numeric PID or a %N job specifier */
    struct arg_str *target = arg_str1(NULL, NULL, "PID|%JOB",
                                      "Process ID, or %%N to target job N by number");
    struct arg_end *end    = arg_end(20);
    void *argtable[] = {help, sig, json, target, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count && json->count) {
        print_help_json("kill", "Send a signal to a process or background job",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: kill"); arg_print_syntax(stdout, argtable, "\n");
        printf("Send a signal to a process or background job.\n\n");
        printf("  kill %%1          send SIGTERM to job 1\n");
        printf("  kill $!          send SIGTERM to the last background job\n");
        printf("  kill -s KILL %%2  send SIGKILL to job 2\n\n");
        arg_print_glossary(stdout, argtable, "  %-22s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "kill");
        fprintf(stderr, "Usage: kill"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }

    /* Resolve signal number */
    int signo = SIGTERM;
    if (sig->count) {
        if      (strcmp(sig->sval[0], "KILL") == 0 || strcmp(sig->sval[0], "9")  == 0) signo = SIGKILL;
        else if (strcmp(sig->sval[0], "TERM") == 0 || strcmp(sig->sval[0], "15") == 0) signo = SIGTERM;
        else if (strcmp(sig->sval[0], "INT")  == 0 || strcmp(sig->sval[0], "2")  == 0) signo = SIGINT;
        else if (strcmp(sig->sval[0], "HUP")  == 0 || strcmp(sig->sval[0], "1")  == 0) signo = SIGHUP;
        else if (strcmp(sig->sval[0], "STOP") == 0 || strcmp(sig->sval[0], "17") == 0) signo = SIGSTOP;
        else if (strcmp(sig->sval[0], "CONT") == 0 || strcmp(sig->sval[0], "19") == 0) signo = SIGCONT;
        else {
            int n;
            if (!parse_int(sig->sval[0], &n)) {
                fprintf(stderr, "kill: unknown signal '%s'\n", sig->sval[0]);
                free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
                return 1;
            }
            signo = n;
        }
    }

    const char *t = target->sval[0];
    int rc;

    if (t[0] == '%') {
        /* ----------------------------------------------------------------
         * Job specifier: %N  — send signal to the process group of job N.
         * Each job's first child is the process group leader so its pid
         * doubles as the pgid; kill(-pgid, sig) reaches the whole group.
         * ---------------------------------------------------------------- */
        int jobid = (int)strtol(t + 1, NULL, 10);
        if (jobid <= 0) {
            fprintf(stderr, "kill: invalid job specifier '%s'\n", t);
            free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
            return 1;
        }

        pid_t pgid = 0;
        pthread_mutex_lock(&g_jobs_mutex);
        for (int i = 0; i < g_job_count; i++) {
            if (g_jobs[i].id == jobid && !g_jobs[i].done) {
                pgid = g_jobs[i].pids[0];
                break;
            }
        }
        pthread_mutex_unlock(&g_jobs_mutex);

        if (pgid == 0) {
            fprintf(stderr, "kill: no such job: %s\n", t);
            free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
            return 1;
        }

        rc = kill(-pgid, signo);  /* negative pgid = signal the whole group */
        if (rc != 0) fprintf(stderr, "kill: %s\n", strerror(errno));
        if (json->count)
            printf("{\"status\":\"%s\",\"job\":%d,\"pgid\":%d}\n",
                   rc == 0 ? "ok" : "error", jobid, (int)pgid);

    } else {
        /* ----------------------------------------------------------------
         * Numeric PID.
         *
         * AiShell builtins like `sleep` are thin wrappers: the registered
         * `sleep` command forks a child (C1) that then forks the real
         * /bin/sleep binary (C2).  C1 is the pid stored in the job table
         * and printed as "[N] PID"; C2 is a grandchild in the same process
         * group.  Sending a signal to C1 alone (positive pid) leaves C2
         * alive as an orphan.
         *
         * When the supplied pid matches a known background job's group
         * leader (i.e. it was produced by `$!` or typed from the "[N] PID"
         * line), send to the process group (-pgid) instead so the whole
         * job is terminated — exactly what `kill %N` does.
         * ---------------------------------------------------------------- */
        int pid;
        if (parse_int(t, &pid) != 0) {
            fprintf(stderr, "kill: invalid PID '%s'\n", t);
            free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
            return 1;
        }

        /* Check if this pid is the leader of a known live background job */
        pid_t target_pid = (pid_t)pid;
        pthread_mutex_lock(&g_jobs_mutex);
        for (int i = 0; i < g_job_count; i++) {
            if (!g_jobs[i].done && g_jobs[i].pids[0] == target_pid) {
                target_pid = -target_pid;  /* negative = whole process group */
                break;
            }
        }
        pthread_mutex_unlock(&g_jobs_mutex);

        rc = kill(target_pid, signo);
        if (rc != 0) fprintf(stderr, "kill: %s\n", strerror(errno));
        if (json->count)
            printf("{\"status\":\"%s\",\"pid\":%d}\n",
                   rc == 0 ? "ok" : "error", pid);
    }

    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return rc == 0 ? 0 : 1;
}

void builtin_kill_init(void)
{
    registry_add("kill", "Send a signal to a process or job", run);
}
