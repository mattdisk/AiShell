#define _POSIX_C_SOURCE 200809L

#include "token.h"
#include "command.h"

#include <glob.h>
#include <stdlib.h>
#include <string.h>

/*
 * Operator-aware tokenizer.  Scans 'line' (mutable) in place, null-terminating
 * each word so that out[i].val points directly into 'line'.  Quotes (" and ')
 * are collapsed: their contents are copied over the quote characters using an
 * in-place write (dst <= p always, so no overlap issue).
 */
int tokenize_line(char *line, token_t *out, int max)
{
    int   n = 0;
    char *p = line;

    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '|') {
            out[n].type = TOK_PIPE; out[n].val = NULL;
            out[n].was_single_quoted = 0; n++; p++;
        } else if (*p == '&') {
            out[n].type = TOK_BG;   out[n].val = NULL;
            out[n].was_single_quoted = 0; n++; p++;
        } else if (*p == '>') {
            if (*(p + 1) == '>') {
                out[n].type = TOK_REDIR_APPEND; out[n].val = NULL;
                out[n].was_single_quoted = 0; n++; p += 2;
            } else {
                out[n].type = TOK_REDIR_OUT;    out[n].val = NULL;
                out[n].was_single_quoted = 0; n++; p++;
            }
        } else if (*p == '<') {
            out[n].type = TOK_REDIR_IN; out[n].val = NULL;
            out[n].was_single_quoted = 0; n++; p++;
        } else {
            /* word: scan, collapsing quotes in place.
             *
             * sq tracks whether every character in this word came from a
             * single-quote context.  It starts true only when the word begins
             * with a single-quote; any bare character or double-quote region
             * clears it.  Used to suppress $VAR expansion for 'literal' tokens
             * like awk programs and shell patterns. */
            char *word_start = p;
            char *dst        = p;
            int   sq         = (*p == '\'');  /* false unless word starts with ' */
            while (*p && *p != ' ' && *p != '\t' &&
                   *p != '|' && *p != '&' && *p != '<' && *p != '>') {
                if (*p == '"') {
                    sq = 0;  /* double-quoted region present */
                    p++;
                    while (*p && *p != '"')  *dst++ = *p++;
                    if (*p) p++;
                } else if (*p == '\'') {
                    /* single-quoted region; sq unchanged */
                    p++;
                    while (*p && *p != '\'') *dst++ = *p++;
                    if (*p) p++;
                } else {
                    sq = 0;  /* bare (unquoted) character */
                    *dst++ = *p++;
                }
            }
            /* strndup captures the word without writing '\0' into the buffer
             * at p's position, which would make the outer loop exit early. */
            out[n].type             = TOK_WORD;
            out[n].val              = strndup(word_start, (size_t)(dst - word_start));
            out[n].was_single_quoted = sq;
            n++;
        }
    }
    return n;
}

int split_line(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *token = strtok(line, " \t");
    while (token != NULL && argc < max_args - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }
    argv[argc] = NULL;
    return argc;
}

int expand_globs(int argc, char **argv, int *out_argc, char ***out_argv)
{
    char **newv = calloc((size_t)MAX_ARGS, sizeof(char *));
    int newc = 0;
    if (!newv) return -1;

    for (int i = 0; i < argc && newc < MAX_ARGS - 1; i++) {
        if (strpbrk(argv[i], "*?[") != NULL) {
            glob_t g;
            memset(&g, 0, sizeof(g));
            int rc = glob(argv[i], 0, NULL, &g);
            if (rc == 0) {
                for (size_t j = 0; j < g.gl_pathc && newc < MAX_ARGS - 1; j++) {
                    newv[newc++] = strdup(g.gl_pathv[j]);
                }
                globfree(&g);
                continue;
            }
            globfree(&g);
        }
        newv[newc++] = strdup(argv[i]);
    }
    newv[newc] = NULL;
    *out_argc = newc;
    *out_argv = newv;
    return 0;
}

void free_expanded_args(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
}
