#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Structured result");
    struct arg_int *secs = arg_int1(NULL, NULL, "SECONDS", "Delay in seconds");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, secs, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("sleep", "Delay for N seconds",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: sleep"); arg_print_syntax(stdout, argtable, "\n");
        printf("Delay for N seconds.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "sleep");
        fprintf(stderr, "Usage: sleep"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    int n = secs->ival[0] > 0 ? secs->ival[0] : 0;
    int do_json = json->count;
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));

    /* Fork an external sleep so Ctrl+C reaches the child (SIG_DFL) without
     * killing the shell (which keeps SIGINT = SIG_IGN). */
    char secs_str[32];
    snprintf(secs_str, sizeof(secs_str), "%d", n);
    char *ext_argv[] = {"sleep", secs_str, NULL};
    int rc = run_external_interactive("sleep", 2, ext_argv);
    if (rc == 0 && do_json)
        printf("{\"status\":\"ok\",\"seconds\":%d}\n", n);
    return rc;
}

void builtin_sleep_init(void)
{
    registry_add("sleep", "Delay for N seconds", run);
}
