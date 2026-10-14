#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>

#include "../registry.h"
#include "../command.h"
#include "../alias.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Structured result");
    struct arg_str *name = arg_str1(NULL, NULL, "NAME", "Command name");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, name, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("type", "Show how a command name is resolved",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: type NAME [--json]\nShow how a name is resolved.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "type");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    const char *n           = name->sval[0];
    const char *alias_value = lookup_alias(n);
    cmd_spec_t  *spec       = find_command(n);
    if (json->count) {
        printf("{\"name\":\"%s\",", n);
        if (alias_value)
            printf("\"kind\":\"alias\",\"path\":\"%s\"}\n", alias_value);
        else if (spec)
            printf("\"kind\":\"builtin\",\"path\":\"\"}\n");
        else
            printf("\"kind\":\"external\",\"path\":\"unknown\"}\n");
    } else {
        if (alias_value)        printf("%s is an alias for %s\n", n, alias_value);
        else if (spec)          printf("%s is a shell builtin\n", n);
        else                    printf("%s is not a shell builtin\n", n);
    }
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_type_init(void)
{
    registry_add("type", "Show how a command name is resolved", run);
}
