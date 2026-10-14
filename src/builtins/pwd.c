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
    struct arg_lit *json = arg_lit0(NULL, "json", "Print output as JSON");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("pwd", "Print current working directory",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: pwd"); arg_print_syntax(stdout, argtable, "\n");
        printf("Print the current working directory.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "pwd");
        fprintf(stderr, "Usage: pwd"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("pwd");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 1;
    }
    if (json->count) {
        printf("{\"cwd\":\""); json_escape_print(cwd); printf("\"}\n");
    } else {
        printf("%s\n", cwd);
    }
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_pwd_init(void)
{
    registry_add("pwd", "Print current working directory", run);
}
