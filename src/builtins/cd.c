#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit  *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *json = arg_lit0(NULL, "json", "JSON output");
    struct arg_file *dir  = arg_file0(NULL, NULL, "DIR", "Directory");
    struct arg_end  *end  = arg_end(20);
    void *argtable[] = {help, json, dir, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("cd", "Change directory",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: cd [DIR]\nChange directory.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "cd");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    const char *target = dir->count ? dir->filename[0] : getenv("HOME");
    if (!target) target = ".";
    if (chdir(target) != 0) {
        fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 1;
    }
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_cd_init(void)
{
    registry_add("cd", "Change directory", run);
}
