#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../registry.h"
#include "../command.h"

static int head_tail_common(int argc, char **argv, int is_tail)
{
    const char *cmd = is_tail ? "tail" : "head";
    struct arg_lit  *help = arg_lit0("h", "help", "Show this help message");
    struct arg_int  *num  = arg_int0("n", NULL, "N", "Number of lines (default 10)");
    struct arg_lit  *json = arg_lit0(NULL, "json", "Print output as JSON");
    struct arg_file *file = arg_file0(NULL, NULL, "FILE", "File (reads stdin if omitted)");
    struct arg_end  *end  = arg_end(20);
    void *argtable[] = {help, num, json, file, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json(cmd, "Print file lines",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: %s", cmd); arg_print_syntax(stdout, argtable, "\n");
        printf("Print file lines.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, cmd);
        fprintf(stderr, "Usage: %s", cmd); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    int nlines = num->count ? num->ival[0] : 10;
    if (nlines < 0) nlines = 0;

    char **lines = NULL; size_t count = 0;
    int from_stdin = (file->count == 0);
    const char *path = from_stdin ? "-" : file->filename[0];

    if (from_stdin) {
        if (read_all_lines_fp(stdin, &lines, &count) != 0) {
            fprintf(stderr, "%s: cannot read stdin: %s\n", cmd, strerror(errno));
            free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
            return 1;
        }
    } else {
        if (read_all_lines(path, &lines, &count) != 0) {
            fprintf(stderr, "%s: cannot read '%s': %s\n", cmd, path, strerror(errno));
            free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
            return 1;
        }
    }

    size_t start = 0, endidx = count;
    if (is_tail && (size_t)nlines < count) start = count - (size_t)nlines;
    if (!is_tail && (size_t)nlines < count) endidx = (size_t)nlines;
    if (json->count) {
        printf("{\"path\":\""); json_escape_print(path);
        printf("\",\"lines\":[");
    }
    for (size_t i = start; i < endidx; i++) {
        if (json->count) {
            if (i > start) printf(",");
            printf("\""); json_escape_print(lines[i]); printf("\"");
        } else {
            fputs(lines[i], stdout);
        }
    }
    if (json->count) printf("]}\n");
    free_lines(lines, count);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

static int run_head(int argc, char **argv) { return head_tail_common(argc, argv, 0); }
static int run_tail(int argc, char **argv) { return head_tail_common(argc, argv, 1); }

void builtin_head_init(void)
{
    registry_add("head", "Print the first lines of a file or stdin", run_head);
    registry_add("tail", "Print the last lines of a file or stdin",  run_tail);
}
