#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit  *help  = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *json  = arg_lit0(NULL, "json", "Structured result");
    struct arg_file *files = arg_filen(NULL, NULL, "FILE", 1, argc, "Files");
    struct arg_end  *end   = arg_end(20);
    void *argtable[] = {help, json, files, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("touch", "Create files or update timestamps",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: touch"); arg_print_syntax(stdout, argtable, "\n");
        printf("Create files or update timestamps.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "touch");
        fprintf(stderr, "Usage: touch"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    int status = 0;
    for (int i = 0; i < files->count; i++) {
        int fd = open(files->filename[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            fprintf(stderr, "touch: cannot touch '%s': %s\n", files->filename[i], strerror(errno));
            status = 1;
        } else {
            close(fd);
        }
    }
    if (json->count) printf("{\"status\":\"%s\"}\n", status == 0 ? "ok" : "error");
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

void builtin_touch_init(void)
{
    registry_add("touch", "Create files or update timestamps", run);
}
