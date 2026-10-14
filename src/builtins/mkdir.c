#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit  *help    = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *parents = arg_lit0("p", NULL, "Create parent directories as needed");
    struct arg_lit  *json    = arg_lit0(NULL, "json", "Structured result");
    struct arg_file *dirs    = arg_filen(NULL, NULL, "DIR", 1, argc, "Directories");
    struct arg_end  *end     = arg_end(20);
    void *argtable[] = {help, parents, json, dirs, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("mkdir", "Create directories",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: mkdir"); arg_print_syntax(stdout, argtable, "\n");
        printf("Create directories.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "mkdir");
        fprintf(stderr, "Usage: mkdir"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    int status = 0;
    for (int i = 0; i < dirs->count; i++) {
        if (!parents->count) {
            if (mkdir(dirs->filename[i], 0755) != 0) {
                fprintf(stderr, "mkdir: cannot create '%s': %s\n", dirs->filename[i], strerror(errno));
                status = 1;
            }
        } else {
            char tmp[PATH_MAX];
            snprintf(tmp, sizeof(tmp), "%s", dirs->filename[i]);
            for (char *p = tmp + 1; *p; p++) {
                if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
            }
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "mkdir: cannot create '%s': %s\n", tmp, strerror(errno));
                status = 1;
            }
        }
    }
    if (json->count) printf("{\"status\":\"%s\"}\n", status == 0 ? "ok" : "error");
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

void builtin_mkdir_init(void)
{
    registry_add("mkdir", "Create directories", run);
}
