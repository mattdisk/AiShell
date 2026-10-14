#ifndef AISHELL_TOKEN_H
#define AISHELL_TOKEN_H

typedef enum {
    TOK_WORD,
    TOK_PIPE,
    TOK_REDIR_OUT,
    TOK_REDIR_APPEND,
    TOK_REDIR_IN,
    TOK_BG
} token_type_t;

typedef struct {
    token_type_t type;
    char        *val;              /* strndup'd content for TOK_WORD; NULL for operators */
    int          was_single_quoted; /* 1 when the entire word came from single-quote context */
} token_t;

int  split_line(char *line, char **argv, int max_args);
int  tokenize_line(char *line, token_t *out, int max);
int  expand_globs(int argc, char **argv, int *out_argc, char ***out_argv);
void free_expanded_args(int argc, char **argv);

#endif
