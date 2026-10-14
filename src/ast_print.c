#define _POSIX_C_SOURCE 200809L

#include "ast_print.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Text tree printer
 *
 * Produces output like:
 *   Background
 *   └── Redirect(> out.txt)
 *       └── Pipeline
 *           ├── Command: cat file.txt
 *           └── Command: grep abc
 * ========================================================================= */

static void ast_print_node(const ast_node_t *node, const char *prefix, int is_last);

/*
 * ast_print_node — recursive helper.
 * 'prefix' is the indentation string built up from parent levels.
 * 'is_last' indicates whether this node is the last child of its parent
 *   (-1 means root — no connector printed).
 */
static void ast_print_node(const ast_node_t *node, const char *prefix, int is_last)
{
    if (!node) return;

    /* connector before this node's label */
    const char *connector = (is_last < 0) ? "" :
                            (is_last)     ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 " /* └── */
                                          : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 "; /* ├── */

    /* prefix for this node's children */
    char child_prefix[1024];
    if (is_last < 0) {
        snprintf(child_prefix, sizeof(child_prefix), "%s", prefix);
    } else {
        /* last child → spaces; non-last → vertical bar continuation */
        snprintf(child_prefix, sizeof(child_prefix), "%s%s", prefix,
                 is_last ? "    " : "\xe2\x94\x82   "); /* "│   " */
    }

    switch (node->type) {
    case AST_COMMAND:
        printf("%s%sCommand: ", prefix, connector);
        for (int i = 0; i < node->cmd.argc; i++) {
            if (i > 0) putchar(' ');
            fputs(node->cmd.argv[i], stdout);
        }
        putchar('\n');
        break;

    case AST_PIPELINE:
        printf("%s%sPipeline\n", prefix, connector);
        for (int i = 0; i < node->pipeline.count; i++)
            ast_print_node(node->pipeline.stages[i], child_prefix,
                           i == node->pipeline.count - 1);
        break;

    case AST_REDIRECT: {
        const char *op = (node->redirect.redir_type == REDIR_IN)     ? "<"  :
                         (node->redirect.redir_type == REDIR_APPEND)  ? ">>" : ">";
        printf("%s%sRedirect(%s %s)\n", prefix, connector, op, node->redirect.filename);
        ast_print_node(node->redirect.child, child_prefix, 1);
        break;
    }

    case AST_BACKGROUND:
        printf("%s%sBackground\n", prefix, connector);
        ast_print_node(node->background.child, child_prefix, 1);
        break;

    case AST_ASSIGNMENT:
        printf("%s%sAssignment: %s=%s\n", prefix, connector,
               node->assignment.name, node->assignment.value);
        break;
    }
}

void ast_print_text(const ast_node_t *node)
{
    ast_print_node(node, "", -1);  /* -1 → root: no connector */
}

/* =========================================================================
 * Graphviz DOT generator
 *
 * Two-pass approach: first emit all node labels (DFS pre-order), then emit
 * all edges (same DFS order, same ID assignment), so the output matches the
 * expected format with labels before edges.
 * ========================================================================= */

/* Emit a DOT-safe label string (escapes backslash and double-quote). */
static void dot_label_str(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '\\' || *s == '"') fputc('\\', f);
        fputc(*s, f);
    }
}

/* Pass 1: print node label lines.  Returns this node's assigned ID. */
static int dot_labels(FILE *f, const ast_node_t *node, int *counter)
{
    if (!node) return -1;
    int my_id = (*counter)++;

    switch (node->type) {
    case AST_COMMAND:
        fprintf(f, "  node%d [label=\"Command: ", my_id);
        for (int i = 0; i < node->cmd.argc; i++) {
            if (i > 0) fputc(' ', f);
            dot_label_str(f, node->cmd.argv[i]);
        }
        fprintf(f, "\"];\n");
        break;

    case AST_PIPELINE:
        fprintf(f, "  node%d [label=\"Pipeline\"];\n", my_id);
        for (int i = 0; i < node->pipeline.count; i++)
            dot_labels(f, node->pipeline.stages[i], counter);
        break;

    case AST_REDIRECT: {
        const char *op = (node->redirect.redir_type == REDIR_IN)    ? "<"  :
                         (node->redirect.redir_type == REDIR_APPEND) ? ">>" : ">";
        fprintf(f, "  node%d [label=\"Redirect: %s ", my_id, op);
        dot_label_str(f, node->redirect.filename);
        fprintf(f, "\"];\n");
        dot_labels(f, node->redirect.child, counter);
        break;
    }

    case AST_BACKGROUND:
        fprintf(f, "  node%d [label=\"Background\"];\n", my_id);
        dot_labels(f, node->background.child, counter);
        break;

    case AST_ASSIGNMENT:
        fprintf(f, "  node%d [label=\"Assignment: ", my_id);
        dot_label_str(f, node->assignment.name);
        fputc('=', f);
        dot_label_str(f, node->assignment.value);
        fprintf(f, "\"];\n");
        break;
    }
    return my_id;
}

/*
 * Pass 2: print edge lines in top-down (parent before child) order.
 *
 * The key insight: child_id = *counter at the moment we are about to recurse
 * into that child, because IDs are assigned sequentially in the same DFS
 * pre-order as pass 1.  Print the edge BEFORE recursing so the parent's edge
 * appears above its children's edges.
 */
static int dot_edges(FILE *f, const ast_node_t *node, int *counter)
{
    if (!node) return -1;
    int my_id = (*counter)++;

    switch (node->type) {
    case AST_COMMAND:
    case AST_ASSIGNMENT:
        break;  /* leaf nodes: no children */

    case AST_PIPELINE:
        for (int i = 0; i < node->pipeline.count; i++) {
            int child_id = *counter;  /* child will receive this ID */
            fprintf(f, "  node%d -> node%d;\n", my_id, child_id);
            dot_edges(f, node->pipeline.stages[i], counter);
        }
        break;

    case AST_REDIRECT: {
        int child_id = *counter;
        fprintf(f, "  node%d -> node%d;\n", my_id, child_id);
        dot_edges(f, node->redirect.child, counter);
        break;
    }

    case AST_BACKGROUND: {
        int child_id = *counter;
        fprintf(f, "  node%d -> node%d;\n", my_id, child_id);
        dot_edges(f, node->background.child, counter);
        break;
    }
    }
    return my_id;
}

void ast_write_dot(const ast_node_t *node, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }

    fprintf(f, "digraph AST {\n");

    int counter = 0;
    dot_labels(f, node, &counter);

    fprintf(f, "\n");

    counter = 0;
    dot_edges(f, node, &counter);

    fprintf(f, "}\n");
    fclose(f);
}
