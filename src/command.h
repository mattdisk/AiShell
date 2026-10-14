#ifndef AISHELL_COMMAND_H
#define AISHELL_COMMAND_H

#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>

#define BUF_SIZE 2048
#define MAX_ARGS 128

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void        free_argtable(void **argtable, size_t count);
void        json_escape_print(const char *s);
void        print_error_json(const char *cmd, const char *message);
/*
 * Print the full help for a command as a JSON object to stdout.
 * Derives usage and options automatically from the argtable — every builtin
 * should call this in its (help->count && json->count) branch instead of
 * hand-rolling its own JSON, so new commands get consistent help for free.
 */
void        print_help_json(const char *cmd, const char *desc,
                            void **argtable, size_t n);
int         parse_int(const char *s, int *out);
char        file_type_char(mode_t mode);
void        mode_to_string(mode_t mode, char out[11]);
const char *type_name(mode_t mode);
int         copy_file(const char *src, const char *dst, int force);
int         copy_recursive(const char *src, const char *dst, int force);
int         remove_recursive(const char *path);
int         read_all_lines(const char *path, char ***lines_out, size_t *count_out);
int         read_all_lines_fp(FILE *fp, char ***lines_out, size_t *count_out);
int         write_all_lines(const char *path, char **lines, size_t count);
void        free_lines(char **lines, size_t count);
int         placeholder_command(int argc, char **argv, const char *cmd, const char *reason);
int         run_external_interactive(const char *prog, int argc, char **argv);

#endif
