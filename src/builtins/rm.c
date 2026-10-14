#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit  *help  = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *rec   = arg_lit0("r", NULL, "Recursive remove");
    struct arg_lit  *force = arg_lit0("f", NULL, "Force; ignore missing files");
    struct arg_lit  *json  = arg_lit0(NULL, "json", "Structured result");
    struct arg_file *files = arg_filen(NULL, NULL, "FILE", 1, argc, "Files");
    struct arg_end  *end   = arg_end(20);
    void *argtable[] = {help, rec, force, json, files, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("rm", "Remove files or directories",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: rm"); arg_print_syntax(stdout, argtable, "\n");
        printf("Remove files or directories.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "rm");
        fprintf(stderr, "Usage: rm"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    int status = 0;
    for (int i = 0; i < files->count; i++) {
        struct stat st;
        const char *p = files->filename[i];
        if (lstat(p, &st) != 0) {
            if (!force->count) { fprintf(stderr, "rm: cannot access '%s': %s\n", p, strerror(errno)); status = 1; }
            continue;
        }
        if (S_ISDIR(st.st_mode) && !rec->count) {
            fprintf(stderr, "rm: cannot remove '%s': is a directory\n", p);
            status = 1; continue;
        }
        if (rec->count ? remove_recursive(p) != 0 : unlink(p) != 0) {
            if (!rec->count) fprintf(stderr, "rm: cannot remove '%s': %s\n", p, strerror(errno));
            status = 1;
        }
    }
    if (json->count) printf("{\"status\":\"%s\"}\n", status == 0 ? "ok" : "error");
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

void builtin_rm_init(void)
{
    registry_add("rm", "Remove files or directories", run);
}
