#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Print output as JSON");
    struct arg_str *fmt  = arg_str0(NULL, NULL, "FORMAT", "Format string");
    struct arg_str *args = arg_strn(NULL, NULL, "ARG", 0, MAX_ARGS, "Format arguments");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, fmt, args, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("printf", "Formatted output",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: printf"); arg_print_syntax(stdout, argtable, "\n");
        printf("Formatted output.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors || fmt->count == 0) {
        if (nerrors) arg_print_errors(stderr, end, "printf");
        fprintf(stderr, "Usage: printf FORMAT [ARGS...]\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    const char *format = fmt->sval[0];
    int argi = 0;
    for (const char *p = format; *p; p++) {
        if (*p == '%' && p[1]) {
            p++;
            if      (*p == 's') { printf("%s", argi < args->count ? args->sval[argi++] : ""); }
            else if (*p == 'd') { printf("%d", argi < args->count ? atoi(args->sval[argi++]) : 0); }
            else if (*p == '%') { putchar('%'); }
            else                { putchar('%'); putchar(*p); }
        } else if (*p == '\\' && p[1] == 'n') {
            putchar('\n'); p++;
        } else {
            putchar(*p);
        }
    }
    if (json->count) {
        /* JSON mode: the output was already printed; wrap would require buffering — not implemented */
    }
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_printf_init(void)
{
    registry_add("printf", "Formatted output", run);
}
