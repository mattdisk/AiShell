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
    struct arg_str *key  = arg_str1(NULL, NULL, "KEY", "Variable name");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, key, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("unset", "Unset environment variable",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: unset KEY [--json]\nUnset an environment variable.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "unset");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    int status = unsetenv(key->sval[0]) == 0 ? 0 : 1;
    if (json->count) printf("{\"status\":\"%s\",\"key\":\"%s\"}\n", status == 0 ? "ok" : "error", key->sval[0]);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

void builtin_unset_init(void)
{
    registry_add("unset", "Unset environment variable", run);
}
