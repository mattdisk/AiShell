#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>

#include "../registry.h"
#include "../command.h"
#include "../history.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Print output as JSON array");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("history", "Show command history",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: history"); arg_print_syntax(stdout, argtable, "\n");
        printf("Show command history.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "history");
        fprintf(stderr, "Usage: history"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    int n = history_size();
    if (json->count) {
        printf("[");
        for (int i = 0; i < n; i++) {
            if (i > 0) printf(",");
            printf("{\"index\":%d,\"command\":\"", i + 1);
            json_escape_print(history_item(i));
            printf("\"}");
        }
        printf("]\n");
    } else {
        for (int i = 0; i < n; i++)
            printf("%4d  %s\n", i + 1, history_item(i));
    }
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_history_init(void)
{
    registry_add("history", "Show command history", run);
}
