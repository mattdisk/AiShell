#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit  *help    = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *all     = arg_lit0("a", NULL, "Include entries starting with .");
    struct arg_lit  *longfmt = arg_lit0("l", NULL, "Long format");
    struct arg_lit  *json    = arg_lit0(NULL, "json", "Print output as JSON array");
    struct arg_file *path    = arg_file0(NULL, NULL, "PATH", "Directory path");
    struct arg_end  *end     = arg_end(20);
    void *argtable[] = {help, all, longfmt, json, path, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("ls", "List directory contents",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: ls"); arg_print_syntax(stdout, argtable, "\n");
        printf("List directory contents.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "ls");
        fprintf(stderr, "Usage: ls"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    const char *dir_path = path->count ? path->filename[0] : ".";
    DIR *dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "ls: cannot open '%s': %s\n", dir_path, strerror(errno));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 1;
    }
    if (json->count) printf("[");
    int first = 1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!all->count && entry->d_name[0] == '.') continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir_path, entry->d_name);
        struct stat st;
        int ok = stat(full, &st) == 0;
        if (json->count) {
            if (!first) printf(",");
            printf("{\"name\":\""); json_escape_print(entry->d_name); printf("\"");
            if (ok) printf(",\"type\":\"%s\",\"size\":%lld,\"mtime\":%lld",
                           type_name(st.st_mode), (long long)st.st_size, (long long)st.st_mtime);
            printf("}");
            first = 0;
        } else if (longfmt->count && ok) {
            char mode[11]; mode_to_string(st.st_mode, mode);
            printf("%s %8lld %lld %s\n", mode, (long long)st.st_size,
                   (long long)st.st_mtime, entry->d_name);
        } else {
            printf("%s\n", entry->d_name);
        }
    }
    if (json->count) printf("]\n");
    closedir(dir);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_ls_init(void)
{
    registry_add("ls", "List directory contents", run);
}
