#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../registry.h"
#include "../command.h"

/* -------------------------------------------------------------------------
 * Data structures
 * ---------------------------------------------------------------------- */

typedef struct {
    char name[64];
    char version[32];
    char description[256];
    char files[32][256];
    int  file_count;
    char docs[32][256];
    int  doc_count;
} pkg_meta_t;

/* -------------------------------------------------------------------------
 * Path helpers
 * ---------------------------------------------------------------------- */

static void pkg_base_dir(char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out, n, "%s/.mysh", home);
}

static void pkg_bin_dir(char *out, size_t n)
{
    char base[PATH_MAX];
    pkg_base_dir(base, sizeof(base));
    snprintf(out, n, "%s/bin", base);
}

static void pkg_install_dir(char *out, size_t n,
                             const char *name, const char *version)
{
    char base[PATH_MAX];
    pkg_base_dir(base, sizeof(base));
    snprintf(out, n, "%s/pkgs/%s-%s", base, name, version);
}

static void pkg_db_path(char *out, size_t n)
{
    char base[PATH_MAX];
    pkg_base_dir(base, sizeof(base));
    snprintf(out, n, "%s/pkgdb.txt", base);
}

static int pkg_makedirs(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * pkg.json parsing  (strstr-based, no JSON library)
 * ---------------------------------------------------------------------- */

static int extract_json_string(const char *buf, const char *key,
                                char *out, size_t n)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(buf, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < n - 1) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

static int extract_json_str_array(const char *buf, const char *key,
                                   char (*arr)[256], int max)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(buf, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
    if (*p != '[') return 0;
    p++;
    int count = 0;
    while (*p && *p != ']' && count < max) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == ',' || *p == '\t') p++;
        if (*p == '"') {
            p++;
            int i = 0;
            while (*p && *p != '"' && i < 255) arr[count][i++] = *p++;
            arr[count][i] = '\0';
            if (*p == '"') p++;
            count++;
        } else if (*p == ']') {
            break;
        } else {
            p++;
        }
    }
    return count;
}

static int parse_pkg_json(const char *path, pkg_meta_t *m)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "pkg: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz <= 0 || sz > 65536) { fclose(fp); return -1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return -1; }
    fread(buf, 1, (size_t)sz, fp);
    buf[sz] = '\0';
    fclose(fp);

    memset(m, 0, sizeof(*m));
    if (extract_json_string(buf, "name",        m->name,        sizeof(m->name))        != 0 ||
        extract_json_string(buf, "version",     m->version,     sizeof(m->version))     != 0 ||
        extract_json_string(buf, "description", m->description, sizeof(m->description)) != 0) {
        fprintf(stderr, "pkg: pkg.json missing required field (name/version/description)\n");
        free(buf);
        return -1;
    }
    m->file_count = extract_json_str_array(buf, "files", m->files, 32);
    m->doc_count  = extract_json_str_array(buf, "docs",  m->docs,  32);
    free(buf);
    return 0;
}

/* -------------------------------------------------------------------------
 * pkgdb  (~/.mysh/pkgdb.txt, one "name version\n" per line)
 * ---------------------------------------------------------------------- */

static int pkgdb_find(const char *name, char *version_out, size_t n)
{
    char path[PATH_MAX];
    pkg_db_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[256];
    size_t nlen = strlen(name);
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, name, nlen) == 0 && line[nlen] == ' ') {
            char *ver = line + nlen + 1;
            char *end = strchr(ver, '\n');
            if (end) *end = '\0';
            if (version_out) snprintf(version_out, n, "%s", ver);
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -1;
}

static int pkgdb_add(const char *name, const char *version)
{
    char path[PATH_MAX];
    pkg_db_path(path, sizeof(path));
    FILE *fp = fopen(path, "a");
    if (!fp) return -1;
    fprintf(fp, "%s %s\n", name, version);
    fclose(fp);
    return 0;
}

static int pkgdb_remove(const char *name)
{
    char path[PATH_MAX];
    pkg_db_path(path, sizeof(path));
    char **lines = NULL;
    size_t count = 0;
    if (read_all_lines(path, &lines, &count) != 0) return -1;

    size_t nlen = strlen(name);
    size_t j = 0;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(lines[i], name, nlen) == 0 && lines[i][nlen] == ' ')
            free(lines[i]);
        else
            lines[j++] = lines[i];
    }
    int rc = write_all_lines(path, lines, j);
    /* free remaining (kept) lines */
    for (size_t i = 0; i < j; i++) free(lines[i]);
    free(lines);
    return rc;
}

typedef struct { void (*cb)(const char *, const char *, void *); void *ud; } pkgdb_list_ctx;

static int pkgdb_list(void (*cb)(const char *name, const char *ver, void *ud),
                      void *ud)
{
    char path[PATH_MAX];
    pkg_db_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return 0; /* empty is fine */
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        char *ver = sp + 1;
        char *end = strchr(ver, '\n');
        if (end) *end = '\0';
        cb(line, ver, ud);
    }
    fclose(fp);
    return 0;
}

/* -------------------------------------------------------------------------
 * tar via fork/exec
 * ---------------------------------------------------------------------- */

static int run_tar(const char *const *args)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        execvp("tar", (char *const *)args);
        perror("tar");
        _exit(127);
    }
    /* Retry on EINTR: SIGCHLD for an unrelated child can interrupt waitpid()
     * when SA_RESTART is not set on the SIGCHLD handler. */
    int status = 0; pid_t r;
    do { r = waitpid(pid, &status, 0); } while (r == -1 && errno == EINTR);
    return (r > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : 1;
}

/* -------------------------------------------------------------------------
 * Subcommands
 * ---------------------------------------------------------------------- */

static int cmd_build(int argc, char **argv)
{
    struct arg_lit  *help    = arg_lit0("h", "help",        "Show this help message");
    struct arg_lit  *json    = arg_lit0(NULL, "json",       "JSON output");
    struct arg_file *srcdir  = arg_file1(NULL, NULL, "SRC-DIR",       "Source directory containing pkg.json");
    struct arg_file *outfile = arg_file1(NULL, NULL, "OUTPUT.tar.gz", "Output tarball path");
    struct arg_end  *end     = arg_end(20);
    void *tbl[] = {help, json, srcdir, outfile, end};
    if (arg_nullcheck(tbl)) return 1;
    int nerrors = arg_parse(argc, argv, tbl);
    if (help->count && json->count) {
        print_help_json("pkg build", "Build a package tarball",
                        tbl, sizeof(tbl)/sizeof(tbl[0]));
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: pkg build"); arg_print_syntax(stdout, tbl, "\n");
        printf("Build a package tarball from a source directory.\n\n");
        arg_print_glossary(stdout, tbl, "  %-26s %s\n");
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "pkg build");
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 2;
    }

    const char *src = srcdir->filename[0];
    const char *out = outfile->filename[0];

    /* check pkg.json exists before running tar */
    char pjpath[PATH_MAX];
    snprintf(pjpath, sizeof(pjpath), "%s/pkg.json", src);
    struct stat st;
    if (stat(pjpath, &st) != 0) {
        fprintf(stderr, "pkg: no pkg.json in '%s'\n", src);
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 1;
    }

    const char *args[] = {"tar", "czf", out, "-C", src, ".", NULL};
    int rc = run_tar(args);
    if (rc != 0)
        fprintf(stderr, "pkg: tar failed (exit %d)\n", rc);
    else if (json->count)
        printf("{\"status\":\"ok\",\"output\":\"%s\"}\n", out);

    free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
    return rc;
}

static int cmd_install(int argc, char **argv)
{
    struct arg_lit  *help    = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *json    = arg_lit0(NULL, "json", "JSON output");
    struct arg_file *tarball = arg_file1(NULL, NULL, "TARBALL", "Package tarball");
    struct arg_end  *end     = arg_end(20);
    void *tbl[] = {help, json, tarball, end};
    if (arg_nullcheck(tbl)) return 1;
    int nerrors = arg_parse(argc, argv, tbl);
    if (help->count && json->count) {
        print_help_json("pkg install", "Install a package",
                        tbl, sizeof(tbl)/sizeof(tbl[0]));
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: pkg install"); arg_print_syntax(stdout, tbl, "\n");
        printf("Install a package from a tarball.\n\n");
        arg_print_glossary(stdout, tbl, "  %-26s %s\n");
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "pkg install");
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 2;
    }

    const char *tb = tarball->filename[0];

    /* extract to tmpdir first to read pkg.json */
    char tmpdir[PATH_MAX];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/aishell_pkg_%d", (int)getpid());
    if (mkdir(tmpdir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "pkg: cannot create tmpdir: %s\n", strerror(errno));
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 1;
    }
    const char *xargs[] = {"tar", "xzf", tb, "-C", tmpdir, NULL};
    if (run_tar(xargs) != 0) {
        fprintf(stderr, "pkg: failed to extract '%s'\n", tb);
        remove_recursive(tmpdir);
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 1;
    }

    /* parse pkg.json */
    char pjpath[PATH_MAX];
    snprintf(pjpath, sizeof(pjpath), "%s/pkg.json", tmpdir);
    pkg_meta_t meta;
    if (parse_pkg_json(pjpath, &meta) != 0) {
        remove_recursive(tmpdir);
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 1;
    }

    /* check already installed */
    char existing_ver[32];
    if (pkgdb_find(meta.name, existing_ver, sizeof(existing_ver)) == 0) {
        fprintf(stderr, "pkg: '%s' is already installed (%s)\n",
                meta.name, existing_ver);
        remove_recursive(tmpdir);
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 1;
    }

    /* create install dir and copy */
    char install_dir[PATH_MAX], bin_dir[PATH_MAX];
    pkg_install_dir(install_dir, sizeof(install_dir), meta.name, meta.version);
    pkg_bin_dir(bin_dir, sizeof(bin_dir));
    if (pkg_makedirs(install_dir) != 0 || pkg_makedirs(bin_dir) != 0) {
        fprintf(stderr, "pkg: cannot create directories: %s\n", strerror(errno));
        remove_recursive(tmpdir);
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 1;
    }
    if (copy_recursive(tmpdir, install_dir, 1) != 0) {
        remove_recursive(tmpdir);
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 1;
    }
    remove_recursive(tmpdir);

    /* ensure installed files are executable (copy_file uses 0644) */
    for (int i = 0; i < meta.file_count; i++) {
        char fp[PATH_MAX];
        snprintf(fp, sizeof(fp), "%s/%s", install_dir, meta.files[i]);
        chmod(fp, 0755);
    }

    /* symlink each file into bin_dir */
    for (int i = 0; i < meta.file_count; i++) {
        char src_path[PATH_MAX], link_path[PATH_MAX];
        snprintf(src_path,  sizeof(src_path),  "%s/%s", install_dir, meta.files[i]);
        /* basename may modify its arg; use a copy */
        char base_copy[256];
        snprintf(base_copy, sizeof(base_copy), "%s", meta.files[i]);
        snprintf(link_path, sizeof(link_path), "%s/%s", bin_dir, basename(base_copy));
        /* remove stale symlink if present */
        unlink(link_path);
        if (symlink(src_path, link_path) != 0)
            fprintf(stderr, "pkg: symlink %s -> %s: %s\n",
                    link_path, src_path, strerror(errno));
    }

    pkgdb_add(meta.name, meta.version);

    if (json->count)
        printf("{\"name\":\"%s\",\"version\":\"%s\",\"status\":\"installed\"}\n",
               meta.name, meta.version);
    else
        printf("Installed %s %s\n", meta.name, meta.version);

    free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
    return 0;
}

/* callback context for pkg list */
typedef struct { int json; int first; int count; } list_ctx;

static void list_cb(const char *name, const char *ver, void *ud)
{
    list_ctx *c = (list_ctx *)ud;
    if (c->json) {
        if (!c->first) putchar(',');
        c->first = 0;

        /* gather install_dir and description from pkg.json */
        char install_dir[PATH_MAX], pjpath[PATH_MAX], desc[256];
        pkg_install_dir(install_dir, sizeof(install_dir), name, ver);
        snprintf(pjpath, sizeof(pjpath), "%s/pkg.json", install_dir);
        pkg_meta_t m;
        desc[0] = '\0';
        if (parse_pkg_json(pjpath, &m) == 0)
            snprintf(desc, sizeof(desc), "%s", m.description);

        printf("{\"name\":\"");  json_escape_print(name);
        printf("\",\"version\":\""); json_escape_print(ver);
        printf("\",\"description\":\""); json_escape_print(desc);
        printf("\",\"install_dir\":\""); json_escape_print(install_dir);
        printf("\"}");
    } else {
        printf("%-24s %s\n", name, ver);
    }
    c->count++;
}

static int cmd_list(int argc, char **argv)
{
    struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit *json = arg_lit0(NULL, "json", "JSON output");
    struct arg_end *end  = arg_end(20);
    void *tbl[] = {help, json, end};
    if (arg_nullcheck(tbl)) return 1;
    int nerrors = arg_parse(argc, argv, tbl);
    if (help->count && json->count) {
        print_help_json("pkg list", "List installed packages",
                        tbl, sizeof(tbl)/sizeof(tbl[0]));
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: pkg list"); arg_print_syntax(stdout, tbl, "\n");
        printf("List installed packages.\n\n");
        arg_print_glossary(stdout, tbl, "  %-26s %s\n");
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "pkg list");
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 2;
    }

    list_ctx ctx = {json->count, 1, 0};
    if (json->count) putchar('[');
    pkgdb_list(list_cb, &ctx);
    if (json->count) puts("]");
    else if (ctx.count == 0) puts("(no packages installed)");

    free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    struct arg_lit  *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *json = arg_lit0(NULL, "json", "JSON output");
    struct arg_str  *name = arg_str1(NULL, NULL, "NAME", "Package name");
    struct arg_end  *end  = arg_end(20);
    void *tbl[] = {help, json, name, end};
    if (arg_nullcheck(tbl)) return 1;
    int nerrors = arg_parse(argc, argv, tbl);
    if (help->count && json->count) {
        print_help_json("pkg info", "Show package details",
                        tbl, sizeof(tbl)/sizeof(tbl[0]));
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: pkg info"); arg_print_syntax(stdout, tbl, "\n");
        printf("Show details for an installed package.\n\n");
        arg_print_glossary(stdout, tbl, "  %-26s %s\n");
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "pkg info");
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 2;
    }

    const char *pkgname = name->sval[0];
    char ver[32];
    if (pkgdb_find(pkgname, ver, sizeof(ver)) != 0) {
        fprintf(stderr, "pkg: '%s' is not installed\n", pkgname);
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 1;
    }

    char install_dir[PATH_MAX], pjpath[PATH_MAX];
    pkg_install_dir(install_dir, sizeof(install_dir), pkgname, ver);
    snprintf(pjpath, sizeof(pjpath), "%s/pkg.json", install_dir);
    pkg_meta_t meta;
    if (parse_pkg_json(pjpath, &meta) != 0) {
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 1;
    }

    if (json->count) {
        printf("{\"name\":\"");        json_escape_print(meta.name);
        printf("\",\"version\":\"");   json_escape_print(meta.version);
        printf("\",\"description\":\""); json_escape_print(meta.description);
        printf("\",\"install_dir\":\""); json_escape_print(install_dir);
        printf("\",\"files\":[");
        for (int i = 0; i < meta.file_count; i++) {
            if (i) putchar(',');
            putchar('"'); json_escape_print(meta.files[i]); putchar('"');
        }
        printf("]}\n");
    } else {
        printf("Name:        %s\n", meta.name);
        printf("Version:     %s\n", meta.version);
        printf("Description: %s\n", meta.description);
        printf("Install dir: %s\n", install_dir);
        if (meta.file_count) {
            printf("Files:\n");
            for (int i = 0; i < meta.file_count; i++)
                printf("  %s\n", meta.files[i]);
        }
    }

    free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
    return 0;
}

static int cmd_remove(int argc, char **argv)
{
    struct arg_lit  *help = arg_lit0("h", "help", "Show this help message");
    struct arg_lit  *json = arg_lit0(NULL, "json", "JSON output");
    struct arg_str  *name = arg_str1(NULL, NULL, "NAME", "Package name");
    struct arg_end  *end  = arg_end(20);
    void *tbl[] = {help, json, name, end};
    if (arg_nullcheck(tbl)) return 1;
    int nerrors = arg_parse(argc, argv, tbl);
    if (help->count && json->count) {
        print_help_json("pkg remove", "Remove an installed package",
                        tbl, sizeof(tbl)/sizeof(tbl[0]));
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: pkg remove"); arg_print_syntax(stdout, tbl, "\n");
        printf("Remove an installed package.\n\n");
        arg_print_glossary(stdout, tbl, "  %-26s %s\n");
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "pkg remove");
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 2;
    }

    const char *pkgname = name->sval[0];
    char ver[32];
    if (pkgdb_find(pkgname, ver, sizeof(ver)) != 0) {
        fprintf(stderr, "pkg: '%s' is not installed\n", pkgname);
        free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        return 1;
    }

    /* load metadata to know which symlinks to remove */
    char install_dir[PATH_MAX], pjpath[PATH_MAX], bin_dir[PATH_MAX];
    pkg_install_dir(install_dir, sizeof(install_dir), pkgname, ver);
    snprintf(pjpath, sizeof(pjpath), "%s/pkg.json", install_dir);
    pkg_bin_dir(bin_dir, sizeof(bin_dir));

    pkg_meta_t meta;
    if (parse_pkg_json(pjpath, &meta) == 0) {
        for (int i = 0; i < meta.file_count; i++) {
            char base_copy[256], link_path[PATH_MAX];
            snprintf(base_copy, sizeof(base_copy), "%s", meta.files[i]);
            snprintf(link_path, sizeof(link_path), "%s/%s", bin_dir, basename(base_copy));
            unlink(link_path);
        }
    }

    remove_recursive(install_dir);
    pkgdb_remove(pkgname);

    if (json->count) {
        printf("{\"name\":\""); json_escape_print(pkgname);
        printf("\",\"status\":\"removed\"}\n");
    } else {
        printf("Removed %s\n", pkgname);
    }

    free_argtable(tbl, sizeof(tbl)/sizeof(tbl[0]));
    return 0;
}

/* -------------------------------------------------------------------------
 * Top-level run
 * ---------------------------------------------------------------------- */

static void print_pkg_usage(void)
{
    printf("Usage: pkg <subcommand> [options]\n\n");
    printf("Subcommands:\n");
    printf("  %-12s %s\n", "build",   "Build a package tarball from a source directory");
    printf("  %-12s %s\n", "install", "Install a package from a tarball");
    printf("  %-12s %s\n", "list",    "List installed packages");
    printf("  %-12s %s\n", "info",    "Show details for an installed package");
    printf("  %-12s %s\n", "remove",  "Remove an installed package");
    printf("\nRun 'pkg <subcommand> -h' for subcommand help.\n");
}

static int run(int argc, char **argv)
{
    /* top-level -h / --help (with optional --json) */
    int want_help = (argc >= 2 &&
                     (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0));
    int want_json = 0;
    if (want_help && argc >= 3 && strcmp(argv[2], "--json") == 0) want_json = 1;
    if (!want_help && argc >= 2 && strcmp(argv[1], "--json") == 0 &&
        argc >= 3 && (strcmp(argv[2], "-h") == 0 || strcmp(argv[2], "--help") == 0)) {
        want_help = 1; want_json = 1;
    }
    if (want_help) {
        if (want_json) {
            printf("{\"command\":\"pkg\",\"description\":\"Package manager\","
                   "\"subcommands\":[\"build\",\"install\",\"list\",\"info\",\"remove\"]}\n");
        } else {
            print_pkg_usage();
        }
        return 0;
    }

    if (argc < 2) { print_pkg_usage(); return 0; }

    if (strcmp(argv[1], "build")   == 0) return cmd_build(argc - 1,   argv + 1);
    if (strcmp(argv[1], "install") == 0) return cmd_install(argc - 1, argv + 1);
    if (strcmp(argv[1], "list")    == 0) return cmd_list(argc - 1,    argv + 1);
    if (strcmp(argv[1], "info")    == 0) return cmd_info(argc - 1,    argv + 1);
    if (strcmp(argv[1], "remove")  == 0) return cmd_remove(argc - 1,  argv + 1);

    fprintf(stderr, "pkg: unknown subcommand '%s'\n", argv[1]);
    fprintf(stderr, "Run 'pkg -h' for usage.\n");
    return 1;
}

/* -------------------------------------------------------------------------
 * Markdown doc generation  (pkg and subcommands only)
 * ---------------------------------------------------------------------- */

static void write_cmd_md(FILE *fp, const char *name, const char *desc,
                          void **tbl, size_t n)
{
    fprintf(fp, "# %s\n\n%s\n\n## Usage\n\n    %s ", name, desc, name);
    for (size_t i = 0; i < n; i++) {
        struct arg_hdr *hdr = (struct arg_hdr *)tbl[i];
        if (hdr->flag & ARG_TERMINATOR) break;
        int required = hdr->mincount > 0;
        int multi    = hdr->maxcount > 1;
        if (!required) fputc('[', fp);
        if (hdr->shortopts && hdr->shortopts[0])
            fprintf(fp, "-%c", hdr->shortopts[0]);
        else if (hdr->longopts && hdr->longopts[0]) {
            char lo[64] = {0};
            int j = 0;
            while (hdr->longopts[j] && hdr->longopts[j] != ',' && j < 63) {
                lo[j] = hdr->longopts[j]; j++;
            }
            fprintf(fp, "--%s", lo);
        }
        if (hdr->datatype) fprintf(fp, " %s", hdr->datatype);
        if (multi)    fputs(" ...", fp);
        if (!required) fputc(']', fp);
        fputc(' ', fp);
    }
    fprintf(fp, "\n\n## Options\n\n");
    fprintf(fp, "| Flag | Long | Datatype | Required | Description |\n");
    fprintf(fp, "|------|------|----------|----------|-------------|\n");
    for (size_t i = 0; i < n; i++) {
        struct arg_hdr *hdr = (struct arg_hdr *)tbl[i];
        if (hdr->flag & ARG_TERMINATOR) break;
        char shortbuf[8]  = "";
        char longbuf[64]  = "";
        if (hdr->shortopts && hdr->shortopts[0])
            snprintf(shortbuf, sizeof(shortbuf), "-%c", hdr->shortopts[0]);
        if (hdr->longopts && hdr->longopts[0]) {
            int j = 0;
            while (hdr->longopts[j] && hdr->longopts[j] != ',' && j < 62) {
                longbuf[j] = hdr->longopts[j]; j++;
            }
            longbuf[j] = '\0';
            char tmp[68];
            snprintf(tmp, sizeof(tmp), "--%s", longbuf);
            snprintf(longbuf, sizeof(longbuf), "%s", tmp);
        }
        fprintf(fp, "| %s | %s | %s | %s | %s |\n",
                shortbuf,
                longbuf,
                hdr->datatype  ? hdr->datatype  : "",
                hdr->mincount > 0 ? "yes" : "no",
                hdr->glossary  ? hdr->glossary  : "");
    }
}

void pkg_gen_docs(const char *outdir)
{
    /* ensure outdir exists */
    if (pkg_makedirs(outdir) != 0) {
        fprintf(stderr, "pkg: cannot create docs dir '%s': %s\n",
                outdir, strerror(errno));
        return;
    }

    char path[PATH_MAX];
    FILE *fp;

    /* pkg.md — top-level */
    snprintf(path, sizeof(path), "%s/pkg.md", outdir);
    fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "# pkg\n\nPackage manager\n\n## Subcommands\n\n");
        fprintf(fp, "| Subcommand | Description |\n");
        fprintf(fp, "|------------|-------------|\n");
        fprintf(fp, "| [pkg build](pkg-build.md)     | Build a package tarball from a source directory |\n");
        fprintf(fp, "| [pkg install](pkg-install.md) | Install a package from a tarball |\n");
        fprintf(fp, "| [pkg list](pkg-list.md)       | List installed packages |\n");
        fprintf(fp, "| [pkg info](pkg-info.md)       | Show details for an installed package |\n");
        fprintf(fp, "| [pkg remove](pkg-remove.md)   | Remove an installed package |\n\n");
        fprintf(fp, "Run `pkg <subcommand> -h` for subcommand help.\n");
        fclose(fp);
    }

/* macro to open file, call write_cmd_md, close */
#define GEN_DOC(fname, cmdname, desc, ...)                          \
    do {                                                            \
        snprintf(path, sizeof(path), "%s/" fname, outdir);         \
        fp = fopen(path, "w");                                      \
        if (fp) {                                                   \
            __VA_ARGS__                                             \
            fclose(fp);                                             \
        }                                                           \
    } while (0)

    /* pkg-build.md */
    GEN_DOC("pkg-build.md", "pkg build", "Build a package tarball", {
        struct arg_lit  *help    = arg_lit0("h", "help",        "Show this help message");
        struct arg_lit  *json    = arg_lit0(NULL, "json",       "JSON output");
        struct arg_file *srcdir  = arg_file1(NULL, NULL, "SRC-DIR",       "Source directory containing pkg.json");
        struct arg_file *outfile = arg_file1(NULL, NULL, "OUTPUT.tar.gz", "Output tarball path");
        struct arg_end  *end     = arg_end(20);
        void *tbl[] = {help, json, srcdir, outfile, end};
        if (!arg_nullcheck(tbl)) {
            write_cmd_md(fp, "pkg build", "Build a package tarball from a source directory",
                         tbl, sizeof(tbl)/sizeof(tbl[0]));
            arg_freetable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        }
    });

    /* pkg-install.md */
    GEN_DOC("pkg-install.md", "pkg install", "Install a package", {
        struct arg_lit  *help    = arg_lit0("h", "help", "Show this help message");
        struct arg_lit  *json    = arg_lit0(NULL, "json", "JSON output");
        struct arg_file *tarball = arg_file1(NULL, NULL, "TARBALL", "Package tarball");
        struct arg_end  *end     = arg_end(20);
        void *tbl[] = {help, json, tarball, end};
        if (!arg_nullcheck(tbl)) {
            write_cmd_md(fp, "pkg install", "Install a package from a tarball",
                         tbl, sizeof(tbl)/sizeof(tbl[0]));
            arg_freetable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        }
    });

    /* pkg-list.md */
    GEN_DOC("pkg-list.md", "pkg list", "List installed packages", {
        struct arg_lit *help = arg_lit0("h", "help", "Show this help message");
        struct arg_lit *json = arg_lit0(NULL, "json", "JSON output");
        struct arg_end *end  = arg_end(20);
        void *tbl[] = {help, json, end};
        if (!arg_nullcheck(tbl)) {
            write_cmd_md(fp, "pkg list", "List installed packages",
                         tbl, sizeof(tbl)/sizeof(tbl[0]));
            arg_freetable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        }
    });

    /* pkg-info.md */
    GEN_DOC("pkg-info.md", "pkg info", "Show package details", {
        struct arg_lit  *help = arg_lit0("h", "help", "Show this help message");
        struct arg_lit  *json = arg_lit0(NULL, "json", "JSON output");
        struct arg_str  *name = arg_str1(NULL, NULL, "NAME", "Package name");
        struct arg_end  *end  = arg_end(20);
        void *tbl[] = {help, json, name, end};
        if (!arg_nullcheck(tbl)) {
            write_cmd_md(fp, "pkg info", "Show details for an installed package",
                         tbl, sizeof(tbl)/sizeof(tbl[0]));
            arg_freetable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        }
    });

    /* pkg-remove.md */
    GEN_DOC("pkg-remove.md", "pkg remove", "Remove an installed package", {
        struct arg_lit  *help = arg_lit0("h", "help", "Show this help message");
        struct arg_lit  *json = arg_lit0(NULL, "json", "JSON output");
        struct arg_str  *name = arg_str1(NULL, NULL, "NAME", "Package name");
        struct arg_end  *end  = arg_end(20);
        void *tbl[] = {help, json, name, end};
        if (!arg_nullcheck(tbl)) {
            write_cmd_md(fp, "pkg remove", "Remove an installed package",
                         tbl, sizeof(tbl)/sizeof(tbl[0]));
            arg_freetable(tbl, sizeof(tbl)/sizeof(tbl[0]));
        }
    });

#undef GEN_DOC
}

/* -------------------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------------- */

void builtin_pkg_init(void)
{
    registry_add("pkg", "Package manager (build/install/list/info/remove)", run);
}
