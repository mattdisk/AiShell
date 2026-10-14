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
    struct arg_lit  *json = arg_lit0(NULL, "json", "Print output as JSON object");
    struct arg_file *file = arg_file1(NULL, NULL, "FILE", "File to print");
    struct arg_end  *end  = arg_end(20);
    void *argtable[] = {help, json, file, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("cat", "Print file contents",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: cat"); arg_print_syntax(stdout, argtable, "\n");
        printf("Print file contents.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "cat");
        fprintf(stderr, "Usage: cat"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    const char *p = file->filename[0];
    struct stat st;
    if (stat(p, &st) != 0) {
        fprintf(stderr, "cat: cannot access '%s': %s\n", p, strerror(errno));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        fprintf(stderr, "cat: '%s' is a directory\n", p);
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 1;
    }
    FILE *fp = fopen(p, "r");
    if (!fp) {
        fprintf(stderr, "cat: cannot open '%s': %s\n", p, strerror(errno));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 1;
    }
    if (json->count) {
        printf("{\"path\":\""); json_escape_print(p); printf("\",\"content\":\"");
    }
    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        char tmp[2] = {(char)ch, '\0'};
        if (json->count) json_escape_print(tmp); else putchar(ch);
    }
    if (json->count) printf("\"}\n");
    fclose(fp);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_cat_init(void)
{
    registry_add("cat", "Print file contents", run);
}
