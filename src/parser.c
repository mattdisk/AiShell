#define _POSIX_C_SOURCE 200809L

#include "parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sentinel byte prepended to single-quoted argv words.
 * exec.c's expand_env() checks for this byte and returns the word verbatim,
 * suppressing $VAR expansion.  \x01 (SOH) never appears in real shell tokens. */
#define SQ_SENTINEL '\x01'

/* -------------------------------------------------------------------------
 * Parser state — wraps the token array with a position cursor
 * ---------------------------------------------------------------------- */

typedef struct {
    token_t *tokens;
    int      ntok;
    int      pos;
} parser_t;

static token_t *peek(parser_t *p)
{
    if (p->pos >= p->ntok) return NULL;
    return &p->tokens[p->pos];
}

static token_t *advance(parser_t *p)
{
    if (p->pos >= p->ntok) return NULL;
    return &p->tokens[p->pos++];
}

/* -------------------------------------------------------------------------
 * is_assignment: true if word looks like NAME=VALUE where NAME is a valid
 * shell identifier (letter or underscore, followed by alphanumerics/underscores).
 * ---------------------------------------------------------------------- */

static int is_assignment(const char *word)
{
    const char *eq = strchr(word, '=');
    if (!eq || eq == word) return 0;
    if (!isalpha((unsigned char)word[0]) && word[0] != '_') return 0;
    for (const char *p = word + 1; p < eq; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * parse_simple_command: WORD WORD*
 * Stops when it sees a non-WORD token (|, >, >>, <, &).
 * ---------------------------------------------------------------------- */

static ast_node_t *parse_simple_command(parser_t *p)
{
    token_t *t = peek(p);
    if (!t || t->type != TOK_WORD) {
        fprintf(stderr, "aishell: syntax error: expected command\n");
        return NULL;
    }
    ast_node_t *node = ast_make_command();
    if (!node) return NULL;

    while (peek(p) && peek(p)->type == TOK_WORD) {
        t = advance(p);
        if (node->cmd.argc < AST_MAX_ARGS - 1) {
            char *word;
            if (t->was_single_quoted) {
                /* Prefix with sentinel so expand_env() skips $VAR expansion. */
                size_t vlen = strlen(t->val);
                word = malloc(vlen + 2);
                if (word) {
                    word[0] = SQ_SENTINEL;
                    memcpy(word + 1, t->val, vlen + 1);
                } else {
                    word = strdup(t->val);  /* fallback: no sentinel on OOM */
                }
            } else {
                word = strdup(t->val);
            }
            node->cmd.argv[node->cmd.argc++] = word;
        }
    }
    node->cmd.argv[node->cmd.argc] = NULL;
    return node;
}

/* -------------------------------------------------------------------------
 * parse_pipeline: simple_command ('|' simple_command)*
 * Returns either a single AST_COMMAND node (no pipe) or AST_PIPELINE.
 * ---------------------------------------------------------------------- */

static ast_node_t *parse_pipeline(parser_t *p)
{
    ast_node_t *first = parse_simple_command(p);
    if (!first) return NULL;

    if (!peek(p) || peek(p)->type != TOK_PIPE)
        return first;  /* single command, no pipeline wrapper needed */

    ast_node_t *pl = ast_make_pipeline();
    if (!pl) { ast_free(first); return NULL; }
    pl->pipeline.stages[pl->pipeline.count++] = first;

    while (peek(p) && peek(p)->type == TOK_PIPE) {
        advance(p);  /* consume | */

        if (!peek(p) || peek(p)->type != TOK_WORD) {
            fprintf(stderr, "aishell: syntax error: expected command after |\n");
            ast_free(pl);
            return NULL;
        }
        if (pl->pipeline.count >= AST_MAX_PIPES) {
            fprintf(stderr, "aishell: syntax error: too many pipeline stages\n");
            ast_free(pl);
            return NULL;
        }
        ast_node_t *stage = parse_simple_command(p);
        if (!stage) { ast_free(pl); return NULL; }
        pl->pipeline.stages[pl->pipeline.count++] = stage;
    }
    return pl;
}

/* -------------------------------------------------------------------------
 * parse_redirect_list: redirection*
 * Each redirect wraps the inner node: inner = Redirect(op, file, inner).
 * Multiple redirects produce nested Redirect nodes (outermost = last parsed).
 * ---------------------------------------------------------------------- */

static ast_node_t *parse_redirect_list(parser_t *p, ast_node_t *inner)
{
    while (peek(p)) {
        token_t *t = peek(p);
        redir_type_t rtype;

        if      (t->type == TOK_REDIR_OUT)    rtype = REDIR_OUT;
        else if (t->type == TOK_REDIR_APPEND)  rtype = REDIR_APPEND;
        else if (t->type == TOK_REDIR_IN)      rtype = REDIR_IN;
        else break;

        advance(p);  /* consume the redirect operator */

        if (!peek(p) || peek(p)->type != TOK_WORD) {
            const char *op = (rtype == REDIR_IN) ? "<" :
                             (rtype == REDIR_APPEND) ? ">>" : ">";
            fprintf(stderr, "aishell: syntax error: expected filename after '%s'\n", op);
            ast_free(inner);
            return NULL;
        }
        token_t *fname = advance(p);
        ast_node_t *rnode = ast_make_redirect(rtype, strdup(fname->val), inner);
        if (!rnode) { ast_free(inner); return NULL; }
        inner = rnode;
    }
    return inner;
}

/* -------------------------------------------------------------------------
 * parse_input — top-level entry point
 * inputline ::= assignment | command background_opt
 * ---------------------------------------------------------------------- */

ast_node_t *parse_input(token_t *tokens, int ntok)
{
    if (ntok == 0) return NULL;

    /* Single-token assignment: NAME=VALUE */
    if (ntok == 1 && tokens[0].type == TOK_WORD && is_assignment(tokens[0].val)) {
        char *eq    = strchr(tokens[0].val, '=');
        char *name  = strndup(tokens[0].val, (size_t)(eq - tokens[0].val));
        char *value = strdup(eq + 1);
        return ast_make_assignment(name, value);
    }

    parser_t p = { tokens, ntok, 0 };

    ast_node_t *cmd = parse_pipeline(&p);
    if (!cmd) return NULL;

    cmd = parse_redirect_list(&p, cmd);
    if (!cmd) return NULL;

    /* Optional background operator & */
    if (peek(&p) && peek(&p)->type == TOK_BG) {
        advance(&p);
        /* Anything after & is a syntax error (we don't support command lists) */
        if (peek(&p)) {
            token_t *extra = peek(&p);
            fprintf(stderr, "aishell: syntax error: unexpected token '%s' after &\n",
                    extra->val ? extra->val : "?");
            ast_free(cmd);
            return NULL;
        }
        ast_node_t *bg = ast_make_background(cmd);
        if (!bg) { ast_free(cmd); return NULL; }
        cmd = bg;
    }

    /* Leftover tokens (e.g. stray operator) */
    if (peek(&p)) {
        token_t *extra = peek(&p);
        fprintf(stderr, "aishell: syntax error: unexpected token '%s'\n",
                extra->val ? extra->val : "?");
        ast_free(cmd);
        return NULL;
    }

    return cmd;
}
