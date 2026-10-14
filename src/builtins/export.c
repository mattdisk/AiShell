#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Structured result");
    struct arg_str *kv   = arg_str1(NULL, NULL, "KEY=VALUE", "Variable assignment");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, kv, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("export", "Set environment variable",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: export KEY=VALUE [--json]\nSet an environment variable.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "export");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    char *copy = strdup(kv->sval[0]);
    char *eq   = strchr(copy, '=');
    if (!eq) {
        fprintf(stderr, "export: expected KEY=VALUE\n");
        free(copy); free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    *eq = '\0';
    int status = setenv(copy, eq + 1, 1) == 0 ? 0 : 1;
    if (json->count) printf("{\"status\":\"%s\",\"key\":\"%s\"}\n", status == 0 ? "ok" : "error", copy);
    free(copy);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

void builtin_export_init(void)
{
    registry_add("export", "Set environment variable", run);
}
