#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help   = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json   = arg_lit0(NULL, "json", "Structured result");
    struct arg_int *pidarg = arg_int1(NULL, NULL, "PID", "Process ID");
    struct arg_end *end    = arg_end(20);
    void *argtable[] = {help, json, pidarg, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("wait", "Wait for a process to finish",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: wait"); arg_print_syntax(stdout, argtable, "\n");
        printf("Wait for a process to finish.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "wait");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    int status_word = 0;
    pid_t rc = waitpid((pid_t)pidarg->ival[0], &status_word, 0);
    int code = rc < 0 ? 1 : (WIFEXITED(status_word) ? WEXITSTATUS(status_word) : 0);
    if (rc < 0) fprintf(stderr, "wait: %s\n", strerror(errno));
    if (json->count) printf("{\"job\":%d,\"status\":\"exited\",\"code\":%d}\n", pidarg->ival[0], code);
    else printf("%d\n", code);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return rc < 0 ? 1 : 0;
}

void builtin_wait_init(void)
{
    registry_add("wait", "Wait for a process to finish", run);
}
