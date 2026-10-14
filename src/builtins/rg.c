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
    struct arg_lit  *line_num    = arg_lit0("n", NULL, "Show line numbers");
    struct arg_lit  *ignore_case = arg_lit0("i", NULL, "Case-insensitive search");
    struct arg_lit  *word        = arg_lit0("w", NULL, "Match whole words");
    struct arg_int  *context     = arg_int0("C", NULL, "N", "Context lines (accepted, not fully implemented)");
    struct arg_lit  *fixed       = arg_lit0(NULL, "fixed-strings", "Treat pattern as literal");
    struct arg_lit  *json        = arg_lit0(NULL, "json", "Emit JSON objects per match");
    struct arg_str  *pattern     = arg_str1(NULL, NULL, "PATTERN", "Search pattern");
    struct arg_file *files       = arg_filen(NULL, NULL, "FILE", 1, argc, "Files");
    struct arg_end  *end         = arg_end(20);
    void *argtable[] = {help, line_num, ignore_case, word, context, fixed, json, pattern, files, end};
    (void)context;
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("rg", "Search text with regular expressions",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: rg"); arg_print_syntax(stdout, argtable, "\n");
        printf("Search text with regular expressions.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "rg");
        fprintf(stderr, "Usage: rg"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    char patbuf[1024];
    const char *pat = pattern->sval[0];
    if (fixed->count) {
        size_t k = 0;
        for (const char *p = pat; *p && k + 2 < sizeof(patbuf); p++) {
            if (strchr(".^$*+?()[]{}|\\", *p)) patbuf[k++] = '\\';
            patbuf[k++] = *p;
        }
        patbuf[k] = '\0';
    } else if (word->count) {
        snprintf(patbuf, sizeof(patbuf), "(^|[^[:alnum:]_])(%s)([^[:alnum:]_]|$)", pat);
    } else {
        snprintf(patbuf, sizeof(patbuf), "%s", pat);
    }
    regex_t re;
    int flags = REG_EXTENDED | (ignore_case->count ? REG_ICASE : 0);
    int rc = regcomp(&re, patbuf, flags);
    if (rc != 0) {
        char ebuf[256]; regerror(rc, &re, ebuf, sizeof(ebuf));
        fprintf(stderr, "rg: regex error: %s\n", ebuf);
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    int matched = 0;
    for (int f = 0; f < files->count; f++) {
        FILE *fp = fopen(files->filename[f], "r");
        if (!fp) { fprintf(stderr, "rg: cannot open '%s': %s\n", files->filename[f], strerror(errno)); continue; }
        char *line = NULL; size_t len = 0; ssize_t n; int lno = 0;
        while ((n = getline(&line, &len, fp)) != -1) {
            (void)n; lno++;
            regmatch_t m;
            if (regexec(&re, line, 1, &m, 0) == 0) {
                matched = 1;
                if (json->count) {
                    printf("{\"file\":\""); json_escape_print(files->filename[f]);
                    printf("\",\"line\":%d,\"column\":%ld,\"text\":\"", lno, (long)m.rm_so + 1);
                    json_escape_print(line); printf("\"}\n");
                } else {
                    printf("%s:", files->filename[f]);
                    if (line_num->count) printf("%d:", lno);
                    fputs(line, stdout);
                }
            }
        }
        free(line); fclose(fp);
    }
    regfree(&re);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return matched ? 0 : 1;
}

void builtin_rg_init(void)
{
    registry_add("rg", "Search text with regular expressions", run);
}
