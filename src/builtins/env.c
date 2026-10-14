#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../registry.h"
#include "../command.h"

extern char **environ;

static int run(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Structured result");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("env", "List environment variables",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: env [--json]\nList environment variables.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "env");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    if (json->count) printf("{\"env\":{");
    for (char **e = environ; *e; e++) {
        if (json->count) {
            if (e != environ) printf(",");
            char *eq = strchr(*e, '=');
            if (eq) {
                *eq = '\0';
                printf("\""); json_escape_print(*e); printf("\":\"");
                json_escape_print(eq + 1); printf("\"");
                *eq = '=';
            }
        } else {
            printf("%s\n", *e);
        }
    }
    if (json->count) printf("}}\n");
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_env_init(void)
{
    registry_add("env", "List environment variables", run);
}
