#ifndef AISHELL_AST_H
#define AISHELL_AST_H

/* Maximum arguments per command node and pipeline stages */
#define AST_MAX_ARGS  128
#define AST_MAX_PIPES   8

typedef enum {
    AST_COMMAND,    /* simple command: cmd + args          */
    AST_PIPELINE,   /* cmd1 | cmd2 | ...                   */
    AST_REDIRECT,   /* >, >>, < wrapping a command/pipeline */
    AST_BACKGROUND, /* & — run child without waiting       */
    AST_ASSIGNMENT, /* NAME=VALUE — set environment var    */
} ast_node_type_t;

typedef enum {
    REDIR_OUT,    /* >  truncate stdout  */
    REDIR_APPEND, /* >> append stdout    */
    REDIR_IN,     /* <  read stdin       */
} redir_type_t;

typedef struct ast_node ast_node_t;

struct ast_node {
    ast_node_type_t type;
    union {
        struct {
            char *argv[AST_MAX_ARGS];
            int   argc;
        } cmd;
        struct {
            ast_node_t *stages[AST_MAX_PIPES];
            int         count;
        } pipeline;
        struct {
            ast_node_t  *child;
            redir_type_t redir_type;
            char        *filename; /* owned, must be freed */
        } redirect;
        struct {
            ast_node_t *child;
        } background;
        struct {
            char *name;  /* owned */
            char *value; /* owned */
        } assignment;
    };
};

ast_node_t *ast_make_command(void);
ast_node_t *ast_make_pipeline(void);
ast_node_t *ast_make_redirect(redir_type_t type, char *filename, ast_node_t *child);
ast_node_t *ast_make_background(ast_node_t *child);
ast_node_t *ast_make_assignment(char *name, char *value);
void        ast_free(ast_node_t *node);

#endif /* AISHELL_AST_H */
