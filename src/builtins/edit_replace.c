#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../registry.h"
#include "../command.h"

static int run(int argc, char **argv)
{
    struct arg_lit  *help        = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *ignore_case = arg_lit0("i", NULL, "Case-insensitive regex");
    struct arg_lit  *fixed       = arg_lit0(NULL, "fixed-strings", "Treat pattern as literal");
    struct arg_lit  *json        = arg_lit0(NULL, "json", "Structured result");
    struct arg_file *file        = arg_file1(NULL, NULL, "FILE", "File path");
    struct arg_str  *pattern     = arg_str1(NULL, NULL, "PATTERN", "Pattern");
    struct arg_str  *replacement = arg_str1(NULL, NULL, "REPLACEMENT", "Replacement");
    struct arg_end  *end         = arg_end(20);
    void *argtable[] = {help, ignore_case, fixed, json, file, pattern, replacement, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("edit-replace", "Global find/replace in a file",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: edit-replace"); arg_print_syntax(stdout, argtable, "\n");
        printf("Global find/replace in a file.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "edit-replace");
        fprintf(stderr, "Usage: edit-replace"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    FILE *fp = fopen(file->filename[0], "r");
    if (!fp) {
        fprintf(stderr, "edit-replace: cannot open file\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 1;
    }
    fseek(fp, 0, SEEK_END); long size = ftell(fp); rewind(fp);
    char *content = calloc((size_t)size + 1, 1);
    fread(content, 1, (size_t)size, fp); fclose(fp);

    char patbuf[1024];
    if (fixed->count) {
        size_t k = 0; patbuf[0] = '\0';
        for (const char *p = pattern->sval[0]; *p && k + 2 < sizeof(patbuf); p++) {
            if (strchr(".^$*+?()[]{}|\\", *p)) patbuf[k++] = '\\';
            patbuf[k++] = *p;
        }
        patbuf[k] = '\0';
    } else {
        snprintf(patbuf, sizeof(patbuf), "%s", pattern->sval[0]);
    }
    regex_t re;
    int flags = REG_EXTENDED | (ignore_case->count ? REG_ICASE : 0);
    if (regcomp(&re, patbuf, flags) != 0) {
        fprintf(stderr, "edit-replace: invalid regex\n");
        free(content); free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    size_t outcap = strlen(content) + 1024;
    char *out = calloc(outcap, 1);
    char *cursor = content;
    regmatch_t m;
    int matches = 0;
    while (regexec(&re, cursor, 1, &m, 0) == 0) {
        matches++;
        size_t prefix = (size_t)m.rm_so;
        while (strlen(out) + prefix + strlen(replacement->sval[0]) + 2 > outcap) {
            outcap *= 2; out = realloc(out, outcap);
        }
        strncat(out, cursor, prefix);
        strcat(out, replacement->sval[0]);
        cursor += m.rm_eo;
        if (m.rm_eo == 0) break;
    }
    strcat(out, cursor);
    fp = fopen(file->filename[0], "w");
    if (!fp) {
        fprintf(stderr, "edit-replace: cannot write file\n");
        regfree(&re); free(content); free(out);
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 1;
    }
    fputs(out, fp); fclose(fp);
    if (json->count)
        printf("{\"path\":\"%s\",\"matches\":%d,\"replacements\":%d}\n",
               file->filename[0], matches, matches);
    regfree(&re); free(content); free(out);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 0;
}

void builtin_edit_replace_init(void)
{
    registry_add("edit-replace", "Global find/replace in a file", run);
}
