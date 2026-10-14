#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Print output as JSON");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("date", "Show system date/time",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: date"); arg_print_syntax(stdout, argtable, "\n");
        printf("Show the current system date and time.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "date");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    time_t now = time(NULL);
    if (json->count) {
        printf("{\"timestamp\":%lld,\"date\":\"", (long long)now);
        char buf[64];
        struct tm *tm = localtime(&now);
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", tm);
        printf("%s\"}\n", buf);
    } else {
        printf("%s", ctime(&now));
    }
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_date_init(void)
{
    registry_add("date", "Show system date/time", run);
}
