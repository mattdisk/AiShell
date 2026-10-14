#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help  = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json  = arg_lit0(NULL, "json", "Print output as JSON");
    struct arg_str *words = arg_strn(NULL, NULL, "TEXT", 0, MAX_ARGS, "Text to print");
    struct arg_end *end   = arg_end(20);
    void *argtable[] = {help, json, words, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("echo", "Print text",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: echo"); arg_print_syntax(stdout, argtable, "\n");
        printf("Print text to stdout.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "echo");
        fprintf(stderr, "Usage: echo"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    if (json->count) {
        printf("{\"text\":\"");
        for (int i = 0; i < words->count; i++) {
            if (i > 0) json_escape_print(" ");
            json_escape_print(words->sval[i]);
        }
        printf("\"}\n");
    } else {
        for (int i = 0; i < words->count; i++) {
            if (i > 0) putchar(' ');
            fputs(words->sval[i], stdout);
        }
        putchar('\n');
    }
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_echo_init(void)
{
    registry_add("echo", "Print text", run);
}
