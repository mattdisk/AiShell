#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../registry.h"
#include "../command.h"

/* mode: 0=replace-line, 1=insert-line, 2=delete-line */
static int edit_line_common(int argc, char **argv, int mode)
{
    const char *cmd = mode == 0 ? "edit-replace-line"
                    : mode == 1 ? "edit-insert-line"
                    :             "edit-delete-line";
    struct arg_lit  *help    = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *json    = arg_lit0(NULL, "json", "Structured result");
    struct arg_file *file    = arg_file1(NULL, NULL, "FILE", "File path");
    struct arg_int  *line_no = arg_int1(NULL, NULL, "N", "Line number");
    struct arg_str  *text    = (mode == 2)
        ? arg_str0(NULL, NULL, "TEXT", "Unused")
        : arg_str1(NULL, NULL, "TEXT", "Text");
    struct arg_end  *end     = arg_end(20);
    void *argtable[] = {help, json, file, line_no, text, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json(cmd, "Edit a line in a file",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: %s", cmd); arg_print_syntax(stdout, argtable, "\n");
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
    char **lines = NULL; size_t count = 0;
    if (read_all_lines(file->filename[0], &lines, &count) != 0) {
        fprintf(stderr, "%s: cannot read file\n", cmd);
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 1;
    }
    int n = line_no->ival[0];
    int status = 0;
    if (n < 1 || (mode != 1 && (size_t)n > count) || (mode == 1 && (size_t)n > count + 1)) {
        fprintf(stderr, "%s: invalid line number\n", cmd);
        status = 1;
    } else if (mode == 0) {
        free(lines[n - 1]);
        size_t len = strlen(text->sval[0]);
        lines[n - 1] = malloc(len + 2);
        sprintf(lines[n - 1], "%s\n", text->sval[0]);
        status = write_all_lines(file->filename[0], lines, count) == 0 ? 0 : 1;
    } else if (mode == 1) {
        char **newlines = calloc(count + 1, sizeof(char *));
        for (size_t i = 0, j = 0; i < count + 1; i++) {
            if (i == (size_t)n - 1) {
                size_t len = strlen(text->sval[0]);
                newlines[i] = malloc(len + 2);
                sprintf(newlines[i], "%s\n", text->sval[0]);
            } else {
                newlines[i] = strdup(lines[j++]);
            }
        }
        status = write_all_lines(file->filename[0], newlines, count + 1) == 0 ? 0 : 1;
        for (size_t i = 0; i < count + 1; i++) free(newlines[i]);
        free(newlines);
    } else {
        free(lines[n - 1]);
        for (size_t i = (size_t)n - 1; i + 1 < count; i++) lines[i] = lines[i + 1];
        status = write_all_lines(file->filename[0], lines, count - 1) == 0 ? 0 : 1;
        count--;
    }
    if (json->count)
        printf("{\"path\":\"%s\",\"line\":%d,\"status\":\"%s\"}\n",
               file->filename[0], n, status == 0 ? "ok" : "error");
    free_lines(lines, count);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

static int run_replace_line(int argc, char **argv) { return edit_line_common(argc, argv, 0); }
static int run_insert_line(int argc, char **argv)  { return edit_line_common(argc, argv, 1); }
static int run_delete_line(int argc, char **argv)  { return edit_line_common(argc, argv, 2); }

void builtin_edit_line_init(void)
{
    registry_add("edit-replace-line", "Replace one line in a file",             run_replace_line);
    registry_add("edit-insert-line",  "Insert one line before a line number",   run_insert_line);
    registry_add("edit-delete-line",  "Delete one line from a file",            run_delete_line);
}
