#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../registry.h"
#include "../command.h"
#include "../alias.h"

struct alias_print_ctx { int json; int first; };

static void print_alias_cb(const char *name, const char *value, void *data)
{
    struct alias_print_ctx *ctx = data;
    if (ctx->json) {
        if (!ctx->first) printf(",");
        printf("\""); json_escape_print(name);
        printf("\":\""); json_escape_print(value); printf("\"");
        ctx->first = 0;
    } else {
        printf("alias %s='%s'\n", name, value);
    }
}

static int run_alias(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Print output as JSON");
    struct arg_str *kv   = arg_str0(NULL, NULL, "NAME=VALUE", "Define alias");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, kv, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("alias", "Manage aliases",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: alias"); arg_print_syntax(stdout, argtable, "\n");
        printf("Manage shell aliases.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "alias");
        fprintf(stderr, "Usage: alias"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    if (kv->count == 0) {
        struct alias_print_ctx ctx = {json->count, 1};
        if (json->count) printf("{\"aliases\":{");
        alias_each(print_alias_cb, &ctx);
        if (json->count) printf("}}\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    char *copy = strdup(kv->sval[0]);
    char *eq   = strchr(copy, '=');
    if (!eq) {
        fprintf(stderr, "alias: expected NAME=VALUE\n");
        free(copy); free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    *eq = '\0';
    int status = alias_add(copy, eq + 1) == 0 ? 0 : 1;
    if (status) fprintf(stderr, "alias: too many aliases\n");
    if (json->count) printf("{\"status\":\"%s\"}\n", status == 0 ? "ok" : "error");
    free(copy);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

static int run_unalias(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Structured result");
    struct arg_str *name = arg_str1(NULL, NULL, "NAME", "Alias name to remove");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, name, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        print_help_json("unalias", "Remove aliases",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: unalias NAME [--json]\nRemove a shell alias.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "unalias");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    int status = alias_remove(name->sval[0]) == 0 ? 0 : 1;
    if (status) fprintf(stderr, "unalias: not found: %s\n", name->sval[0]);
    if (json->count) printf("{\"status\":\"%s\"}\n", status == 0 ? "ok" : "error");
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return status;
}

void builtin_alias_init(void)
{
    registry_add("alias",   "Manage aliases", run_alias);
    registry_add("unalias", "Remove aliases", run_unalias);
}
