#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "JSON output");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, end};
    if (arg_nullcheck(argtable)) return 1;
    /* Parse only to detect -h/--json; ignore parse errors to allow pass-through of vi options */
    arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("vi", "Open editor using external vi if available",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: vi [FILE]\nOpen vi editor.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return run_external_interactive("vi", argc, argv);
}

void builtin_vi_init(void)
{
    registry_add("vi", "Open editor using external vi if available", run);
}
