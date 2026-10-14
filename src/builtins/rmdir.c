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
    struct arg_lit  *json = arg_lit0(NULL, "json", "Structured result");
    struct arg_file *dirs = arg_filen(NULL, NULL, "DIR", 1, argc, "Directories");
    struct arg_end  *end  = arg_end(20);
    void *argtable[] = {help, json, dirs, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("rmdir", "Remove empty directories",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: rmdir"); arg_print_syntax(stdout, argtable, "\n");
        printf("Remove empty directories.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "rmdir");
        fprintf(stderr, "Usage: rmdir"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    int status = 0;
    for (int i = 0; i < dirs->count; i++) {
        if (rmdir(dirs->filename[i]) != 0) {
            fprintf(stderr, "rmdir: cannot remove '%s': %s\n", dirs->filename[i], strerror(errno));
            status = 1;
        }
    }
    if (json->count) printf("{\"status\":\"%s\"}\n", status == 0 ? "ok" : "error");
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

void builtin_rmdir_init(void)
{
    registry_add("rmdir", "Remove empty directories", run);
}
