#ifndef AISHELL_PARSER_H
#define AISHELL_PARSER_H

#include "ast.h"
#include "token.h"

/*
 * Parse a token stream into an AST.
 *
 * The full grammar is specified in EBNF in grammar.ebnf at the project root.
 * Each production maps to a static function in parser.c:
 *
 *   parse_input()          -> inputline  ::= assignment | command [ '&' ]
 *   parse_pipeline()       -> pipeline   ::= simple_command ( '|' simple_command )*
 *   parse_simple_command() -> simple_cmd ::= WORD WORD*
 *   parse_redirect_list()  -> redir_list ::= ( '>' WORD | '>>' WORD | '<' WORD )*
 *
 * Returns a heap-allocated AST node, or NULL on syntax error (message
 * already printed to stderr).  The caller must free with ast_free().
 */
ast_node_t *parse_input(token_t *tokens, int ntok);

#endif /* AISHELL_PARSER_H */
