#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit  *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *json = arg_lit0(NULL, "json", "Print output as JSON");
    struct arg_file *file = arg_filen(NULL, NULL, "FILE", 1, argc, "File paths");
    struct arg_end  *end  = arg_end(20);
    void *argtable[] = {help, json, file, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("stat", "Show file metadata",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: stat"); arg_print_syntax(stdout, argtable, "\n");
        printf("Show file metadata.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "stat");
        fprintf(stderr, "Usage: stat"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    if (json->count) printf("[");
    int status = 0;
    for (int i = 0; i < file->count; i++) {
        struct stat st;
        const char *p = file->filename[i];
        if (stat(p, &st) != 0) {
            fprintf(stderr, "stat: cannot stat '%s': %s\n", p, strerror(errno));
            status = 1; continue;
        }
        if (json->count) {
            if (i > 0) printf(",");
            printf("{\"path\":\""); json_escape_print(p);
            printf("\",\"type\":\"%s\",\"size\":%lld,\"mode\":%o,"
                   "\"mtime\":%lld,\"atime\":%lld,\"ctime\":%lld}",
                   type_name(st.st_mode), (long long)st.st_size, st.st_mode & 07777,
                   (long long)st.st_mtime, (long long)st.st_atime, (long long)st.st_ctime);
        } else {
            char mode[11]; mode_to_string(st.st_mode, mode);
            printf("  File: %s\n  Type: %s\n  Size: %lld\n  Mode: %s (%o)\n  MTime: %lld\n",
                   p, type_name(st.st_mode), (long long)st.st_size,
                   mode, st.st_mode & 07777, (long long)st.st_mtime);
        }
    }
    if (json->count) printf("]\n");
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

void builtin_stat_init(void)
{
    registry_add("stat", "Show file metadata", run);
}
