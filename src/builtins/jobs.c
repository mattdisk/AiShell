#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>

#include "../registry.h"
#include "../command.h"
#include "../exec.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Structured result");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("jobs", "List managed jobs",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: jobs"); arg_print_syntax(stdout, argtable, "\n");
        printf("List managed background jobs.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "jobs");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    /* Lock while reading g_jobs so we don't race with reap_jobs */
    pthread_mutex_lock(&g_jobs_mutex);
    if (json->count) printf("[");
    int first = 1;
    for (int i = 0; i < g_job_count; i++) {
        if (g_jobs[i].done) continue;
        if (json->count) {
            if (!first) printf(",");
            printf("{\"id\":%d,\"pid\":%d,\"state\":\"running\",\"command\":\"%s\"}",
                   g_jobs[i].id, (int)g_jobs[i].pids[0], g_jobs[i].cmd);
            first = 0;
        } else {
            printf("[%d] %d running\t%s\n",
                   g_jobs[i].id, (int)g_jobs[i].pids[0], g_jobs[i].cmd);
        }
    }
    if (json->count) printf("]\n");
    pthread_mutex_unlock(&g_jobs_mutex);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_jobs_init(void)
{
    registry_add("jobs", "List managed jobs", run);
}
