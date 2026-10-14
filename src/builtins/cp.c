#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit  *help  = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *rec   = arg_lit0("r", NULL, "Recursive copy");
    struct arg_lit  *force = arg_lit0("f", NULL, "Force overwrite");
    struct arg_lit  *json  = arg_lit0(NULL, "json", "Structured result");
    struct arg_file *files = arg_filen(NULL, NULL, "SRC DST", 2, 2, "Source and destination");
    struct arg_end  *end   = arg_end(20);
    void *argtable[] = {help, rec, force, json, files, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("cp", "Copy files or directories",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: cp"); arg_print_syntax(stdout, argtable, "\n");
        printf("Copy files or directories.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "cp");
        fprintf(stderr, "Usage: cp"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    struct stat st;
    int status;
    if (stat(files->filename[0], &st) == 0 && S_ISDIR(st.st_mode) && !rec->count) {
        fprintf(stderr, "cp: -r required for directory '%s'\n", files->filename[0]);
        status = 1;
    } else {
        status = rec->count
            ? copy_recursive(files->filename[0], files->filename[1], force->count)
            : copy_file(files->filename[0], files->filename[1], force->count);
    }
    if (json->count)
        printf("{\"status\":\"%s\",\"src\":\"%s\",\"dst\":\"%s\"}\n",
               status == 0 ? "ok" : "error", files->filename[0], files->filename[1]);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

void builtin_cp_init(void)
{
    registry_add("cp", "Copy files or directories", run);
}
