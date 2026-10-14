#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help     = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json     = arg_lit0(NULL, "json", "JSON output");
    struct arg_str *cmd_name = arg_str0(NULL, NULL, "COMMAND", "Show help for COMMAND");
    struct arg_end *end      = arg_end(20);
    void *argtable[] = {help, json, cmd_name, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("help", "List built-in commands or show command help",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: help"); arg_print_syntax(stdout, argtable, "\n");
        printf("List built-in commands or show help for a specific command.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "help");
        fprintf(stderr, "Usage: help"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    if (cmd_name->count == 0) {
        registry_print_all();
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    cmd_spec_t *spec = find_command(cmd_name->sval[0]);
    if (!spec) {
        fprintf(stderr, "help: unknown command '%s'\n", cmd_name->sval[0]);
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    char *help_argv[] = {(char *)spec->name, "-h", NULL};
    return spec->run(2, help_argv);
}

void builtin_help_init(void)
{
    registry_add("help", "List built-in commands or show command help", run);
}
