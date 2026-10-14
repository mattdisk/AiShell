#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../registry.h"
#include "../command.h"
#include "../exec.h"

static int run(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Print output as JSON");
    struct arg_str *cmd  = arg_str0(NULL, NULL, "on|off",
                                    "Enable or disable AST debug mode");
    struct arg_end *end  = arg_end(20);
    void *argtable[]     = { help, json, cmd, end };

    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);

    if (help->count && json->count) {
        printf("{\"command\":\"ast\",\"description\":\"Toggle AST debug mode\"}\n");
        free_argtable(argtable, sizeof(argtable) / sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: ast");
        arg_print_syntax(stdout, argtable, "\n");
        printf("Toggle AST debug mode on or off.\n");
        printf("When on, each command prints a text tree and writes ast.dot\n");
        printf("before execution.  Set AISHELL_AST=1 to enable at startup.\n\n");
        arg_print_glossary(stdout, argtable, "  %-20s %s\n");
        free_argtable(argtable, sizeof(argtable) / sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "ast");
        fprintf(stderr, "Usage: ast");
        arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable) / sizeof(argtable[0]));
        return 2;
    }

    if (cmd->count) {
        if (strcmp(cmd->sval[0], "on") == 0) {
            g_ast_debug = 1;
            if (json->count) printf("{\"ast_debug\":true}\n");
            else             printf("AST debug mode: on\n");
        } else if (strcmp(cmd->sval[0], "off") == 0) {
            g_ast_debug = 0;
            if (json->count) printf("{\"ast_debug\":false}\n");
            else             printf("AST debug mode: off\n");
        } else {
            fprintf(stderr, "ast: expected 'on' or 'off', got '%s'\n", cmd->sval[0]);
            free_argtable(argtable, sizeof(argtable) / sizeof(argtable[0]));
            return 1;
        }
    } else {
        /* No argument: show current state */
        if (json->count)
            printf("{\"ast_debug\":%s}\n", g_ast_debug ? "true" : "false");
        else
            printf("AST debug mode: %s\n", g_ast_debug ? "on" : "off");
    }

    free_argtable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return 0;
}

void builtin_astmode_init(void)
{
    registry_add("ast", "Toggle AST debug mode (on/off)", run);
}
