#ifndef AISHELL_AST_PRINT_H
#define AISHELL_AST_PRINT_H

#include "ast.h"

/* Print a human-readable text tree of the AST to stdout. */
void ast_print_text(const ast_node_t *node);

/*
 * Write a Graphviz DOT representation of the AST to 'path'.
 * All node labels appear before edge declarations, matching the expected
 * demo format.
 */
void ast_write_dot(const ast_node_t *node, const char *path);

#endif /* AISHELL_AST_PRINT_H */
