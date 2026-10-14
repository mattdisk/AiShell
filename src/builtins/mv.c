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
    struct arg_lit  *help  = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *force = arg_lit0("f", NULL, "Force overwrite");
    struct arg_lit  *json  = arg_lit0(NULL, "json", "Structured result");
    struct arg_file *files = arg_filen(NULL, NULL, "SRC DST", 2, 2, "Source and destination");
    struct arg_end  *end   = arg_end(20);
    void *argtable[] = {help, force, json, files, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("mv", "Move or rename files or directories",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: mv"); arg_print_syntax(stdout, argtable, "\n");
        printf("Move or rename files or directories.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "mv");
        fprintf(stderr, "Usage: mv"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    if (!force->count && access(files->filename[1], F_OK) == 0) {
        fprintf(stderr, "mv: destination exists; use -f\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 1;
    }
    int status = rename(files->filename[0], files->filename[1]) == 0 ? 0 : 1;
    if (status)
        fprintf(stderr, "mv: cannot move '%s' to '%s': %s\n",
                files->filename[0], files->filename[1], strerror(errno));
    if (json->count) printf("{\"status\":\"%s\"}\n", status == 0 ? "ok" : "error");
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

void builtin_mv_init(void)
{
    registry_add("mv", "Move or rename files or directories", run);
}
