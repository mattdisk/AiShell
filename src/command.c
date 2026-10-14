#define _POSIX_C_SOURCE 200809L

#include "command.h"

#include <argtable3.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

void free_argtable(void **argtable, size_t count)
{
    arg_freetable(argtable, count);
}

void print_error_json(const char *cmd, const char *message)
{
    printf("{\"command\":\"%s\",\"status\":\"error\",\"message\":\"%s\"}\n", cmd, message);
}

void json_escape_print(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if      (*p == '\\') printf("\\\\");
        else if (*p == '"')  printf("\\\"");
        else if (*p == '\n') printf("\\n");
        else if (*p == '\t') printf("\\t");
        else if (*p == '\r') printf("\\r");
        else if (*p < 32)    printf("\\u%04x", *p);
        else                 putchar(*p);
    }
}

/*
 * print_help_json — emit a command's full help as a JSON object.
 *
 * Derives the usage string and every option entry directly from the same
 * argtable that arg_parse() uses, so the JSON stays in sync with the
 * human-readable help automatically.  Every builtin's
 * (help->count && json->count) branch should reduce to:
 *
 *   print_help_json("cmd", "description", argtable, ARRAY_SIZE(argtable));
 *   free_argtable(argtable, ARRAY_SIZE(argtable));
 *   return 0;
 *
 * This also means any new command — including those installed via `pkg` if
 * they call this function — gets consistent JSON help without extra work.
 */
void print_help_json(const char *cmd, const char *desc,
                     void **argtable, size_t n)
{
    /* Build usage string into a buffer via fmemopen */
    char usage_buf[512] = "";
    FILE *uf = fmemopen(usage_buf, sizeof(usage_buf) - 1, "w");
    if (uf) {
        fprintf(uf, "%s", cmd);
        arg_print_syntax(uf, argtable, "");
        fclose(uf);
        /* trim trailing whitespace left by arg_print_syntax */
        size_t len = strlen(usage_buf);
        while (len > 0 && (usage_buf[len - 1] == ' ' || usage_buf[len - 1] == '\n'))
            usage_buf[--len] = '\0';
    }

    printf("{\n");
    printf("  \"command\": \"");     json_escape_print(cmd);       printf("\",\n");
    printf("  \"description\": \""); json_escape_print(desc);      printf("\",\n");
    printf("  \"usage\": \"");       json_escape_print(usage_buf); printf("\",\n");
    printf("  \"options\": [");

    int first = 1;
    for (size_t i = 0; i < n; i++) {
        struct arg_hdr *hdr = (struct arg_hdr *)argtable[i];
        if (hdr->flag & ARG_TERMINATOR) break;

        if (!first) putchar(',');
        first = 0;
        printf("\n    {");

        int nf = 0;

        /* Short flag: -x */
        if (hdr->shortopts && hdr->shortopts[0]) {
            printf("\"short\": \"-%c\"", hdr->shortopts[0]);
            nf++;
        }

        /* Long flag: --xxx (first entry in the comma-separated longopts string) */
        if (hdr->longopts && hdr->longopts[0]) {
            char lo[64] = {0};
            int  j = 0;
            while (j < 62 && hdr->longopts[j] && hdr->longopts[j] != ',') {
                lo[j] = hdr->longopts[j];
                j++;
            }
            if (nf) printf(", ");
            printf("\"long\": \"--");
            json_escape_print(lo);
            putchar('"');
            nf++;
        }

        /* Value placeholder, e.g. "PATH", "SIGNAL" */
        if (hdr->datatype && hdr->datatype[0]) {
            if (nf) printf(", ");
            printf("\"argument\": \"");
            json_escape_print(hdr->datatype);
            putchar('"');
            nf++;
        }

        /* One-line description */
        if (hdr->glossary && hdr->glossary[0]) {
            if (nf) printf(", ");
            printf("\"description\": \"");
            json_escape_print(hdr->glossary);
            putchar('"');
        }

        putchar('}');
    }

    printf("\n  ]\n}\n");
}

int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (s == end || *end != '\0' || value < INT_MIN || value > INT_MAX) return -1;
    *out = (int)value;
    return 0;
}

char file_type_char(mode_t mode)
{
    if (S_ISDIR(mode))  return 'd';
    if (S_ISLNK(mode))  return 'l';
    if (S_ISCHR(mode))  return 'c';
    if (S_ISBLK(mode))  return 'b';
    if (S_ISFIFO(mode)) return 'p';
    if (S_ISSOCK(mode)) return 's';
    return '-';
}

void mode_to_string(mode_t mode, char out[11])
{
    out[0]  = file_type_char(mode);
    out[1]  = (mode & S_IRUSR) ? 'r' : '-';
    out[2]  = (mode & S_IWUSR) ? 'w' : '-';
    out[3]  = (mode & S_IXUSR) ? 'x' : '-';
    out[4]  = (mode & S_IRGRP) ? 'r' : '-';
    out[5]  = (mode & S_IWGRP) ? 'w' : '-';
    out[6]  = (mode & S_IXGRP) ? 'x' : '-';
    out[7]  = (mode & S_IROTH) ? 'r' : '-';
    out[8]  = (mode & S_IWOTH) ? 'w' : '-';
    out[9]  = (mode & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

const char *type_name(mode_t mode)
{
    if (S_ISDIR(mode))  return "directory";
    if (S_ISREG(mode))  return "file";
    if (S_ISLNK(mode))  return "symlink";
    if (S_ISCHR(mode))  return "char";
    if (S_ISBLK(mode))  return "block";
    if (S_ISFIFO(mode)) return "fifo";
    if (S_ISSOCK(mode)) return "socket";
    return "unknown";
}

int copy_file(const char *src, const char *dst, int force)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: cannot open '%s': %s\n", src, strerror(errno));
        return 1;
    }
    int flags = O_WRONLY | O_CREAT;
    flags |= force ? O_TRUNC : O_EXCL;
    int out = open(dst, flags, 0644);
    if (out < 0) {
        fprintf(stderr, "cp: cannot create '%s': %s\n", dst, strerror(errno));
        close(in);
        return 1;
    }
    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(out, buf + written, (size_t)(n - written));
            if (w < 0) {
                fprintf(stderr, "cp: write error: %s\n", strerror(errno));
                close(in); close(out);
                return 1;
            }
            written += w;
        }
    }
    if (n < 0) {
        fprintf(stderr, "cp: read error: %s\n", strerror(errno));
        close(in); close(out);
        return 1;
    }
    close(in); close(out);
    return 0;
}

int copy_recursive(const char *src, const char *dst, int force)
{
    struct stat st;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "cp: cannot stat '%s': %s\n", src, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst, force);

    if (mkdir(dst, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "cp: cannot create directory '%s': %s\n", dst, strerror(errno));
        return 1;
    }
    DIR *dir = opendir(src);
    if (!dir) {
        fprintf(stderr, "cp: cannot open directory '%s': %s\n", src, strerror(errno));
        return 1;
    }
    struct dirent *entry;
    int status = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char src_path[PATH_MAX], dst_path[PATH_MAX];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);
        if (copy_recursive(src_path, dst_path, force) != 0) status = 1;
    }
    closedir(dir);
    return status;
}

int remove_recursive(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "rm: cannot access '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) != 0) {
            fprintf(stderr, "rm: cannot remove '%s': %s\n", path, strerror(errno));
            return 1;
        }
        return 0;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "rm: cannot open directory '%s': %s\n", path, strerror(errno));
        return 1;
    }
    struct dirent *entry;
    int status = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (remove_recursive(child) != 0) status = 1;
    }
    closedir(dir);
    if (rmdir(path) != 0) {
        fprintf(stderr, "rm: cannot remove directory '%s': %s\n", path, strerror(errno));
        return 1;
    }
    return status;
}

int read_all_lines_fp(FILE *fp, char ***lines_out, size_t *count_out)
{
    char **lines = NULL;
    size_t count = 0, cap = 0;
    char *line = NULL;
    size_t len = 0;
    ssize_t n;
    while ((n = getline(&line, &len, fp)) != -1) {
        (void)n;
        if (count == cap) {
            cap = cap == 0 ? 16 : cap * 2;
            char **tmp = realloc(lines, cap * sizeof(char *));
            if (!tmp) {
                free(line);
                for (size_t i = 0; i < count; i++) free(lines[i]);
                free(lines);
                return -1;
            }
            lines = tmp;
        }
        lines[count++] = strdup(line);
    }
    free(line);
    *lines_out = lines;
    *count_out = count;
    return 0;
}

int read_all_lines(const char *path, char ***lines_out, size_t *count_out)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int rc = read_all_lines_fp(fp, lines_out, count_out);
    fclose(fp);
    return rc;
}

int write_all_lines(const char *path, char **lines, size_t count)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    for (size_t i = 0; i < count; i++) fputs(lines[i], fp);
    fclose(fp);
    return 0;
}

void free_lines(char **lines, size_t count)
{
    for (size_t i = 0; i < count; i++) free(lines[i]);
    free(lines);
}

int placeholder_command(int argc, char **argv, const char *cmd, const char *reason)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "Structured result");
    struct arg_end *end  = arg_end(20);
    void *argtable[] = {help, json, end};
    if (arg_nullcheck(argtable)) return 1;
    int nerrors = arg_parse(argc, argv, argtable);
    if (help->count && json->count) {
        printf("{\"command\":\"%s\",\"description\":\"%s\"}\n", cmd, reason);
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: %s [options]\n%s\n", cmd, reason);
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, cmd);
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }
    if (json->count) print_error_json(cmd, reason);
    else fprintf(stderr, "%s: %s\n", cmd, reason);
    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
    return 1;
}

int run_external_interactive(const char *prog, int argc, char **argv)
{
    (void)argc;
    pid_t pid = fork();
    if (pid < 0) { perror(prog); return 1; }
    if (pid == 0) {
        /* Restore default signal dispositions so the child behaves normally.
         * The shell sets SIG_IGN on several signals (SIGINT, SIGTTOU, SIGTTIN,
         * SIGTSTP) for itself; those dispositions must not carry over into the
         * exec'd program. */
        signal(SIGINT,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        execvp(prog, argv);
        fprintf(stderr, "%s: %s\n", prog, strerror(errno));
        _exit(127);
    }
    /* Retry on EINTR: SIGCHLD for an unrelated child can interrupt waitpid()
     * when SA_RESTART is not set on the SIGCHLD handler. */
    int status = 0;
    pid_t r;
    do { r = waitpid(pid, &status, 0); } while (r == -1 && errno == EINTR);
    return (r > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : 1;
}
