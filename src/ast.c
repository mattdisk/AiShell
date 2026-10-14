#define _POSIX_C_SOURCE 200809L

#include "ast.h"

#include <stdlib.h>

ast_node_t *ast_make_command(void)
{
    ast_node_t *n = calloc(1, sizeof(ast_node_t));
    if (!n) return NULL;
    n->type = AST_COMMAND;
    return n;
}

ast_node_t *ast_make_pipeline(void)
{
    ast_node_t *n = calloc(1, sizeof(ast_node_t));
    if (!n) return NULL;
    n->type = AST_PIPELINE;
    return n;
}

ast_node_t *ast_make_redirect(redir_type_t type, char *filename, ast_node_t *child)
{
    ast_node_t *n = calloc(1, sizeof(ast_node_t));
    if (!n) return NULL;
    n->type                = AST_REDIRECT;
    n->redirect.redir_type = type;
    n->redirect.filename   = filename;
    n->redirect.child      = child;
    return n;
}

ast_node_t *ast_make_background(ast_node_t *child)
{
    ast_node_t *n = calloc(1, sizeof(ast_node_t));
    if (!n) return NULL;
    n->type               = AST_BACKGROUND;
    n->background.child   = child;
    return n;
}

ast_node_t *ast_make_assignment(char *name, char *value)
{
    ast_node_t *n = calloc(1, sizeof(ast_node_t));
    if (!n) return NULL;
    n->type             = AST_ASSIGNMENT;
    n->assignment.name  = name;
    n->assignment.value = value;
    return n;
}

void ast_free(ast_node_t *node)
{
    if (!node) return;
    switch (node->type) {
    case AST_COMMAND:
        for (int i = 0; i < node->cmd.argc; i++)
            free(node->cmd.argv[i]);
        break;
    case AST_PIPELINE:
        for (int i = 0; i < node->pipeline.count; i++)
            ast_free(node->pipeline.stages[i]);
        break;
    case AST_REDIRECT:
        free(node->redirect.filename);
        ast_free(node->redirect.child);
        break;
    case AST_BACKGROUND:
        ast_free(node->background.child);
        break;
    case AST_ASSIGNMENT:
        free(node->assignment.name);
        free(node->assignment.value);
        break;
    }
    free(node);
}
