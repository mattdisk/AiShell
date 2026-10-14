/*
 * mcp_server.c — Local HTTP MCP server for AiShell.
 *
 * Runs in a background pthread, binds only to 127.0.0.1, requires Bearer
 * token authentication, and implements the JSON-RPC MCP methods:
 *   initialize, notifications/initialized, tools/list, tools/call.
 *
 * Exposed tools (read-only / validation-only):
 *   get_shell_context     — returns cwd, pid, mcp_port
 *   list_aishell_builtins — returns the registry builtin name list
 *   validate_command      — runs the existing safety validator; never executes
 *   get_command_help      — returns builtin description from the registry
 *
 * No execute_command / run_shell / file-write tools are exposed.
 */

#define _POSIX_C_SOURCE 200809L

#include "ai/mcp_server.h"
#include "ai/ai_validate.h"
#include "registry.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Maximum bytes to read for HTTP headers */
#define MAX_HEADERS   8192
/* Maximum request body size */
#define MAX_BODY      16384
/* Maximum response buffer */
#define MAX_RESP      16384
/* TCP listen backlog */
#define LISTEN_BACKLOG 8

struct mcp_server {
    int          listen_fd;
    int          actual_port;
    pthread_t    thread;
    volatile int running;
    char         token[256];  /* never printed */
};

/* ==========================================================================
 * String / JSON helpers
 * ========================================================================== */

static void json_escape(const char *src, char *dst, size_t dstsz)
{
    size_t di = 0;
    for (const char *p = src; *p && di + 3 < dstsz; p++) {
        switch (*p) {
        case '"':  dst[di++] = '\\'; dst[di++] = '"';  break;
        case '\\': dst[di++] = '\\'; dst[di++] = '\\'; break;
        case '\n': dst[di++] = '\\'; dst[di++] = 'n';  break;
        case '\r': dst[di++] = '\\'; dst[di++] = 'r';  break;
        case '\t': dst[di++] = '\\'; dst[di++] = 't';  break;
        default:   dst[di++] = *p;                     break;
        }
    }
    dst[di] = '\0';
}

/*
 * Case-insensitive HTTP header value extractor.
 * 'name' must be all-lowercase (e.g. "authorization", "content-length").
 * Returns 1 and writes the trimmed value into 'out' if found, 0 otherwise.
 */
static int hdr_get(const char *headers, const char *name,
                   char *out, size_t outsz)
{
    size_t nlen = strlen(name);
    const char *p = headers;

    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) eol = p + strlen(p);

        if ((size_t)(eol - p) > nlen && p[nlen] == ':') {
            int match = 1;
            for (size_t i = 0; i < nlen && match; i++) {
                char c = p[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                if (c != name[i]) match = 0;
            }
            if (match) {
                const char *v = p + nlen + 1;
                while (*v == ' ' || *v == '\t') v++;
                size_t vlen = (size_t)(eol - v);
                while (vlen > 0 && (v[vlen-1] == ' ' || v[vlen-1] == '\t'))
                    vlen--;
                if (vlen >= outsz) vlen = outsz - 1;
                memcpy(out, v, vlen);
                out[vlen] = '\0';
                return 1;
            }
        }
        if (*eol == '\0') break;
        p = eol + 2;
    }
    out[0] = '\0';
    return 0;
}

/*
 * Extract a string value for 'key' from a flat JSON object.
 * Handles simple backslash escapes.  Writes value to 'out'.
 */
static int json_get_str(const char *json, const char *key,
                         char *out, size_t outsz)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) { out[0] = '\0'; return 0; }
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') { out[0] = '\0'; return 0; }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') { out[0] = '\0'; return 0; }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outsz - 1) {
        if (*p == '\\') { p++; if (!*p) break; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

/* Extract the JSON-RPC 'id' as a raw JSON snippet (number or string). */
static void json_get_id(const char *json, char *out, size_t outsz)
{
    const char *p = strstr(json, "\"id\"");
    if (!p) { snprintf(out, outsz, "null"); return; }
    p += 4;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') { snprintf(out, outsz, "null"); return; }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (*p == '"') {
        /* String id — preserve the quotes */
        size_t i = 0;
        if (i < outsz - 1) out[i++] = '"';
        p++;
        while (*p && *p != '"' && i < outsz - 3) {
            if (*p == '\\' && *(p + 1)) { out[i++] = *p++; }
            out[i++] = *p++;
        }
        if (i < outsz - 1) out[i++] = '"';
        out[i] = '\0';
    } else {
        /* Numeric id */
        size_t i = 0;
        while (*p && (*p == '-' || (*p >= '0' && *p <= '9')) && i < outsz - 1)
            out[i++] = *p++;
        out[i] = '\0';
        if (i == 0) snprintf(out, outsz, "null");
    }
}

/*
 * Find a JSON object value by key and return a pointer to the character
 * immediately after the opening '{'.  Returns NULL if the key is absent,
 * the value is not an object, or the input is malformed.
 *
 * The search is textual (not recursive), so pass a sub-string starting at or
 * after the enclosing object to avoid false matches on identically named keys
 * in outer scopes.
 */
static const char *json_find_object(const char *json, const char *key)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '{') return NULL;
    return p + 1;  /* caller searches inside the object */
}

/* Extract a JSON number value for 'key'.  Returns 1 on success, 0 if absent. */
static int json_get_number(const char *json, const char *key, double *out)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '-' && (*p < '0' || *p > '9')) return 0;
    *out = strtod(p, NULL);
    return 1;
}

/* ==========================================================================
 * HTTP send helpers
 * ========================================================================== */

static void send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, buf + sent, len - sent, 0);
#endif
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

static void http_respond(int fd, int code, const char *status,
                          const char *ctype, const char *body)
{
    char hdr[512];
    int  blen = body ? (int)strlen(body) : 0;
    int  hlen = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 %d %s\r\n"
                         "Content-Type: %s\r\n"
                         "Content-Length: %d\r\n"
                         "Connection: close\r\n"
                         "\r\n",
                         code, status,
                         ctype ? ctype : "text/plain",
                         blen);
    send_all(fd, hdr, (size_t)hlen);
    if (blen > 0) send_all(fd, body, (size_t)blen);
}

static void http_respond_202(int fd)
{
    const char *hdr = "HTTP/1.1 202 Accepted\r\n"
                      "Content-Length: 0\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    send_all(fd, hdr, strlen(hdr));
}

static void send_json(int fd, int code, const char *status, const char *json)
{
    http_respond(fd, code, status, "application/json", json);
}

static void send_error(int fd, int code, const char *status, const char *msg)
{
    char body[512];
    char esc[256];
    json_escape(msg, esc, sizeof(esc));
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", esc);
    send_json(fd, code, status, body);
}

/* ==========================================================================
 * MCP JSON-RPC response builders
 * ========================================================================== */

static void jsonrpc_ok(char *buf, size_t sz,
                        const char *id, const char *result_json)
{
    snprintf(buf, sz,
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
             id, result_json);
}

static void jsonrpc_error(char *buf, size_t sz,
                           const char *id, int code, const char *message)
{
    char esc[256];
    json_escape(message, esc, sizeof(esc));
    snprintf(buf, sz,
             "{\"jsonrpc\":\"2.0\",\"id\":%s,"
             "\"error\":{\"code\":%d,\"message\":\"%s\"}}",
             id, code, esc);
}

/* Wraps 'text' in MCP tools/call content format with configurable isError. */
static void tool_result_ex(char *buf, size_t sz,
                            const char *id, const char *text, int is_error)
{
    char esc[MAX_RESP / 2];
    json_escape(text, esc, sizeof(esc));
    snprintf(buf, sz,
             "{\"jsonrpc\":\"2.0\",\"id\":%s,"
             "\"result\":{"
             "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],"
             "\"isError\":%s}}",
             id, esc, is_error ? "true" : "false");
}

static void tool_result(char *buf, size_t sz,
                         const char *id, const char *text)
{
    tool_result_ex(buf, sz, id, text, 0);
}

/* ==========================================================================
 * Tool implementations
 * ========================================================================== */

static void tool_get_shell_context(mcp_server_t *srv,
                                    char *out, size_t outsz)
{
    char cwd[4096] = "(unknown)";
    (void)getcwd(cwd, sizeof(cwd));

    char cwd_esc[4096];
    json_escape(cwd, cwd_esc, sizeof(cwd_esc));

    /* Include AiShell project layout hints when the CWD is the project root.
     * Detection: check if src/ai/mcp_server.c exists relative to CWD.
     * This helps Claude generate correct file path arguments and avoids the
     * common mistake of omitting the src/ai/ prefix (e.g. src/mcp_server.c). */
    struct stat _st;
    int in_project = (stat("src/ai/mcp_server.c", &_st) == 0);

    /* Pitfall notes included in every get_shell_context response. */
    static const char *wc_pitfall =
        "For top-N file rankings (largest files by byte or line count), "
        "avoid: find ... -exec wc -c {} + "
        "because wc adds a 'total' row when given multiple files and that "
        "total ranks first in sort -rn | head output. "
        "Prefer: find ... -print | xargs -n 1 wc -c | sort -rn | head -n N";

    /* AiShell builtins (rg, ls, cat, etc.) are only available when AiShell
     * itself dispatches the command.  External programs such as find(1) fork
     * a subprocess and search PATH; they cannot reach the builtin registry.
     * find -exec rg will always fail unless a system rg binary exists. */
    static const char *exec_builtin_pitfall =
        "AiShell builtins (rg, ls, cat, head, tail, etc.) are only available "
        "when AiShell dispatches the top-level command or pipeline stage. "
        "External programs such as find -exec cannot call AiShell builtins "
        "because they search PATH, not the builtin registry. "
        "Do not use find -exec rg unless rg is a system executable. "
        "For recursive source search prefer: grep -R -n PATTERN src/ai "
        "or call rg directly on a known file: rg -n PATTERN src/ai/mcp_server.c";

    if (in_project) {
        snprintf(out, outsz,
                 "{\"cwd\":\"%s\",\"pid\":%d,\"mcp_port\":%d,"
                 "\"aishell_layout\":{"
                 "\"ai_src\":\"src/ai/\","
                 "\"mcp_server\":\"src/ai/mcp_server.c\","
                 "\"mcp_server_h\":\"src/ai/mcp_server.h\","
                 "\"claude_client\":\"src/ai/claude_client.c\","
                 "\"ai_json\":\"src/ai/ai_json.c\","
                 "\"ai_validate\":\"src/ai/ai_validate.c\","
                 "\"main_repl\":\"src/main.c\""
                 "},"
                 "\"common_pitfalls\":{"
                 "\"wc_total_row\":\"%s\","
                 "\"find_exec_builtin\":\"%s\""
                 "}}",
                 cwd_esc, (int)getpid(), srv->actual_port,
                 wc_pitfall, exec_builtin_pitfall);
    } else {
        snprintf(out, outsz,
                 "{\"cwd\":\"%s\",\"pid\":%d,\"mcp_port\":%d,"
                 "\"common_pitfalls\":{"
                 "\"wc_total_row\":\"%s\","
                 "\"find_exec_builtin\":\"%s\""
                 "}}",
                 cwd_esc, (int)getpid(), srv->actual_port,
                 wc_pitfall, exec_builtin_pitfall);
    }
}

/* State for the list_aishell_builtins collector (server is single-threaded). */
static char   g_blst[4096];
static size_t g_blst_len;
static int    g_blst_first;

/* Appends {"name":"<escaped>"} to g_blst for each registered builtin. */
static void collect_builtin_name(const cmd_spec_t *spec)
{
    if (!g_blst_first && g_blst_len + 1 < sizeof(g_blst))
        g_blst[g_blst_len++] = ',';
    g_blst_first = 0;

    char esc[128];
    json_escape(spec->name, esc, sizeof(esc));
    size_t nlen = strlen(esc);

    /* Write {"name":"<name>"} */
    const char *prefix = "{\"name\":\"";
    size_t plen = strlen(prefix);
    if (g_blst_len + plen + nlen + 3 < sizeof(g_blst)) {
        memcpy(g_blst + g_blst_len, prefix, plen);
        g_blst_len += plen;
        memcpy(g_blst + g_blst_len, esc, nlen);
        g_blst_len += nlen;
        g_blst[g_blst_len++] = '"';
        g_blst[g_blst_len++] = '}';
        g_blst[g_blst_len]   = '\0';
    }
}

static void tool_list_aishell_builtins(char *out, size_t outsz)
{
    g_blst[0]    = '[';
    g_blst_len   = 1;
    g_blst_first = 1;
    registry_for_each(collect_builtin_name);
    if (g_blst_len + 2 < sizeof(g_blst)) {
        g_blst[g_blst_len++] = ']';
        g_blst[g_blst_len]   = '\0';
    }
    snprintf(out, outsz, "%s", g_blst);
}

/* ==========================================================================
 * MCP method handlers
 * ========================================================================== */

#define MCP_PROTO "2025-06-18"
#define MCP_NAME  "aishell-mcp"
#define MCP_VER   "0.1.0"

static void handle_initialize(int fd, const char *id)
{
    char result[512];
    snprintf(result, sizeof(result),
             "{"
             "\"protocolVersion\":\"%s\","
             "\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"},"
             "\"capabilities\":{\"tools\":{}}"
             "}",
             MCP_PROTO, MCP_NAME, MCP_VER);

    char resp[MAX_RESP];
    jsonrpc_ok(resp, sizeof(resp), id, result);
    send_json(fd, 200, "OK", resp);
}

static void handle_tools_list(int fd, const char *id)
{
    const char *tools =
        "["
          "{\"name\":\"get_shell_context\","
           "\"description\":\"Returns current shell context: CWD, PID, MCP port.\","
           "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
          "{\"name\":\"list_aishell_builtins\","
           "\"description\":\"Returns the list of all built-in commands in AiShell. "
                            "Call this first to discover which commands are available "
                            "as AiShell builtins before proposing a command.\","
           "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
          "{\"name\":\"validate_command\","
           "\"description\":\"Run the AiShell safety validator on a candidate command. "
                            "ALWAYS call this before returning the final JSON. "
                            "Returns level (ok/warn/blocked), reason, and an optional "
                            "suggestion field. "
                            "If level is warn or blocked, revise the command using the "
                            "suggestion and call validate_command again until level is ok. "
                            "Never return a command that validate_command marks blocked.\","
           "\"inputSchema\":{"
             "\"type\":\"object\","
             "\"properties\":{"
               "\"command\":{\"type\":\"string\",\"description\":\"Command to validate\"},"
               "\"risk\":{\"type\":\"string\",\"description\":\"Risk label: read_only/write/destructive/network/unknown\"},"
               "\"confidence\":{\"type\":\"number\",\"description\":\"Confidence 0.0-1.0\"}"
             "},"
             "\"required\":[\"command\"]"
           "}},"
          "{\"name\":\"get_command_help\","
           "\"description\":\"Returns the synopsis and description for a named AiShell "
                            "builtin. Call this when you are unsure of a builtin's exact "
                            "syntax — especially for head, tail, ls, rg, cp, mv, rm. "
                            "The synopsis shows the exact flag names and order. "
                            "Always prefer the synopsis over assumed POSIX/GNU syntax.\","
           "\"inputSchema\":{"
             "\"type\":\"object\","
             "\"properties\":{"
               "\"name\":{\"type\":\"string\",\"description\":\"Built-in command name\"}"
             "},"
             "\"required\":[\"name\"]"
           "}}"
        "]";

    char result[2048];
    snprintf(result, sizeof(result), "{\"tools\":%s}", tools);

    char resp[MAX_RESP];
    jsonrpc_ok(resp, sizeof(resp), id, result);
    send_json(fd, 200, "OK", resp);
}

/*
 * validate_command — run the existing safety validator on the proposed command.
 * This tool never executes the command, never forks, and never writes files.
 * Delegates entirely to ai_validate_command() which only tokenises and checks
 * patterns in memory.
 */
static void handle_tool_validate_command(int fd, const char *id,
                                          const char *body)
{
    char   command[512] = "";
    char   risk[64]     = "unknown";
    double confidence   = 1.0;

    /* Arguments live in params.arguments.{command,risk,confidence} */
    const char *args = json_find_object(body, "arguments");
    if (args) {
        json_get_str(args, "command", command, sizeof(command));
        if (!json_get_str(args, "risk", risk, sizeof(risk)))
            snprintf(risk, sizeof(risk), "unknown");
        json_get_number(args, "confidence", &confidence);
    }

    if (command[0] == '\0') {
        char resp[512];
        tool_result_ex(resp, sizeof(resp), id,
                       "{\"error\":\"missing required argument: command\"}",
                       1);
        send_json(fd, 200, "OK", resp);
        return;
    }

    ai_validation_t v;
    ai_valid_level_t level = ai_validate_command(command, risk, confidence, &v);

    char cmd_esc[1024], reason_esc[512], suggestion_esc[512];
    json_escape(command,      cmd_esc,        sizeof(cmd_esc));
    json_escape(v.reason,     reason_esc,     sizeof(reason_esc));
    json_escape(v.suggestion, suggestion_esc, sizeof(suggestion_esc));

    char text[2048];
    if (v.suggestion[0] != '\0') {
        snprintf(text, sizeof(text),
                 "{\"level\":\"%s\",\"reason\":\"%s\","
                 "\"suggestion\":\"%s\",\"command\":\"%s\"}",
                 ai_valid_level_name(level), reason_esc,
                 suggestion_esc, cmd_esc);
    } else {
        snprintf(text, sizeof(text),
                 "{\"level\":\"%s\",\"reason\":\"%s\",\"command\":\"%s\"}",
                 ai_valid_level_name(level), reason_esc, cmd_esc);
    }

    char resp[MAX_RESP];
    tool_result(resp, sizeof(resp), id, text);
    send_json(fd, 200, "OK", resp);
}

/* -------------------------------------------------------------------------
 * Static usage table for get_command_help.
 *
 * Each entry provides a synopsis derived from the builtin's argtable and an
 * optional compatibility note Claude should know.  This table avoids calling
 * the builtin's run() function from a background thread (which would redirect
 * the shared stdout).  The table only needs to cover commands whose syntax is
 * non-obvious or where LLMs commonly generate wrong invocations.
 * ---------------------------------------------------------------------- */
typedef struct {
    const char *name;
    const char *synopsis;  /* usage line, mirroring argtable output */
    const char *note;      /* optional — NULL when nothing special to say */
} builtin_usage_t;

static const builtin_usage_t builtin_usage_table[] = {
    { "head",  "head [-h] [-n N] [--json] [FILE]",
      "Use -n N to specify line count — e.g. head -n 10 file.txt. "
      "FILE is optional; omit it to read from stdin (e.g. in a pipeline). "
      "The GNU-style -N shorthand (e.g. -10) is NOT supported." },
    { "tail",  "tail [-h] [-n N] [--json] [FILE]",
      "Use -n N to specify line count — e.g. tail -n 20 file.txt. "
      "FILE is optional; omit it to read from stdin (e.g. in a pipeline). "
      "The GNU-style -N shorthand (e.g. -20) is NOT supported." },
    { "ls",    "ls [-h] [-a] [-l] [--json] [PATH]", NULL },
    { "cat",   "cat [-h] [--json] FILE", NULL },
    { "cp",    "cp [-h] [-r] [-f] [--json] SRC DST", NULL },
    { "mv",    "mv [-h] [-f] [--json] SRC DST", NULL },
    { "rm",    "rm [-h] [-r] [-f] [--json] FILE [FILE ...]",
      "rm -rf is blocked by the safety validator." },
    { "mkdir", "mkdir [-h] [-p] [--json] DIR [DIR ...]", NULL },
    { "rmdir", "rmdir [-h] [--json] DIR [DIR ...]", NULL },
    { "touch", "touch [-h] [--json] FILE [FILE ...]", NULL },
    { "pwd",   "pwd [-h] [--json]", NULL },
    { "cd",    "cd [-h] [--json] [DIR]", NULL },
    { "echo",  "echo [-h] [--json] [TEXT ...]", NULL },
    { "rg",    "rg [-hniw] [-C N] [--fixed-strings] [--json] PATTERN FILE [FILE ...]",
      "AiShell rg is a built-in regex search tool. "
      "CRITICAL: rg is an AiShell builtin — it is NOT a system executable. "
      "Do NOT use find -exec rg because find searches PATH, not the builtin "
      "registry; this will fail with 'rg: No such file or directory'. "
      "For recursive search use: grep -R -n PATTERN src/ai "
      "or call rg directly on a known file: rg -n PATTERN src/ai/mcp_server.c "
      "IMPORTANT: FILE must be a file path, not a directory. "
      "AiShell rg does NOT recursively search directories. "
      "PATTERN is a single POSIX extended regex. "
      "Use '|' inside the pattern for alternation: "
      "rg -n \"TODO|FIXME\" src/main.c "
      "Do NOT use -e; AiShell rg does not support it. "
      "Do NOT use '\\|' for alternation — use plain '|' instead." },
    { "stat",  "stat [-h] [--json] FILE [FILE ...]", NULL },
    { "env",   "env [-h] [--json]", NULL },
    { "export","export [-h] [--json] KEY=VALUE", NULL },
    { "history","history [-h] [--json]", NULL },
    { "kill",  "kill [-h] [--json] [-s SIGNAL] PID|%JOBID", NULL },
    { "jobs",  "jobs [-h] [--json]", NULL },
    { "wait",  "wait [-h] [--json] PID", NULL },
    { "sleep", "sleep [-h] [--json] N", NULL },
    { "date",  "date [-h] [--json]", NULL },
    { "type",  "type [-h] [--json] NAME", NULL },
    { "help",  "help [-h] [--json] [COMMAND]", NULL },
    { NULL, NULL, NULL }  /* sentinel */
};

/*
 * get_command_help — return synopsis and description for a known AiShell
 * builtin.  For builtins in the usage table the full synopsis and any
 * compatibility notes are returned.  For other registered builtins only the
 * registry description is returned.  Unknown names produce isError:true.
 * No subprocess is spawned; no external program is called.
 */
static void handle_tool_get_command_help(int fd, const char *id,
                                          const char *body)
{
    char name[128] = "";

    const char *args = json_find_object(body, "arguments");
    if (args) json_get_str(args, "name", name, sizeof(name));

    if (name[0] == '\0') {
        char resp[512];
        tool_result_ex(resp, sizeof(resp), id,
                       "{\"error\":\"missing required argument: name\"}",
                       1);
        send_json(fd, 200, "OK", resp);
        return;
    }

    /* Look up the registry entry (validates that name is a known builtin). */
    cmd_spec_t *spec = find_command(name);
    if (!spec) {
        char name_esc[256], text[512];
        json_escape(name, name_esc, sizeof(name_esc));
        snprintf(text, sizeof(text),
                 "{\"error\":\"unknown command: %s; "
                 "call list_aishell_builtins to see available commands\"}",
                 name_esc);
        char resp[MAX_RESP];
        tool_result_ex(resp, sizeof(resp), id, text, 1);
        send_json(fd, 200, "OK", resp);
        return;
    }

    /* Search the static usage table for a richer entry. */
    const builtin_usage_t *entry = NULL;
    for (int i = 0; builtin_usage_table[i].name; i++) {
        if (strcmp(builtin_usage_table[i].name, name) == 0) {
            entry = &builtin_usage_table[i];
            break;
        }
    }

    char name_esc[256], desc_esc[512], syn_esc[256], note_esc[512];
    json_escape(spec->name,        name_esc, sizeof(name_esc));
    json_escape(spec->description, desc_esc, sizeof(desc_esc));

    char text[2048];
    if (entry) {
        json_escape(entry->synopsis, syn_esc, sizeof(syn_esc));
        if (entry->note) {
            json_escape(entry->note, note_esc, sizeof(note_esc));
            snprintf(text, sizeof(text),
                     "{\"name\":\"%s\",\"description\":\"%s\","
                     "\"synopsis\":\"%s\",\"note\":\"%s\"}",
                     name_esc, desc_esc, syn_esc, note_esc);
        } else {
            snprintf(text, sizeof(text),
                     "{\"name\":\"%s\",\"description\":\"%s\","
                     "\"synopsis\":\"%s\"}",
                     name_esc, desc_esc, syn_esc);
        }
    } else {
        snprintf(text, sizeof(text),
                 "{\"name\":\"%s\",\"description\":\"%s\","
                 "\"note\":\"Run '%s -h' in AiShell for detailed usage.\"}",
                 name_esc, desc_esc, name_esc);
    }

    char resp[MAX_RESP];
    tool_result(resp, sizeof(resp), id, text);
    send_json(fd, 200, "OK", resp);
}

static void handle_tools_call(int fd, const char *id,
                               const char *body, mcp_server_t *srv)
{
    char tool[128] = "";
    const char *params = strstr(body, "\"params\"");
    if (params) json_get_str(params, "name", tool, sizeof(tool));

    char content[MAX_RESP / 2];
    char resp[MAX_RESP];

    if (strcmp(tool, "get_shell_context") == 0) {
        tool_get_shell_context(srv, content, sizeof(content));
        tool_result(resp, sizeof(resp), id, content);
        send_json(fd, 200, "OK", resp);
    } else if (strcmp(tool, "list_aishell_builtins") == 0) {
        tool_list_aishell_builtins(content, sizeof(content));
        tool_result(resp, sizeof(resp), id, content);
        send_json(fd, 200, "OK", resp);
    } else if (strcmp(tool, "validate_command") == 0) {
        handle_tool_validate_command(fd, id, body);
    } else if (strcmp(tool, "get_command_help") == 0) {
        handle_tool_get_command_help(fd, id, body);
    } else {
        char esc[256];
        json_escape(tool, esc, sizeof(esc));
        char errmsg[384];
        snprintf(errmsg, sizeof(errmsg), "unknown tool: %s", esc);
        jsonrpc_error(resp, sizeof(resp), id, -32601, errmsg);
        send_json(fd, 200, "OK", resp);
    }
}

/* ==========================================================================
 * HTTP connection handler
 * ========================================================================== */

static int is_local_origin(const char *origin, int port)
{
    char a1[128], a2[128];
    snprintf(a1, sizeof(a1), "http://127.0.0.1:%d", port);
    snprintf(a2, sizeof(a2), "http://localhost:%d", port);
    return strcmp(origin, a1) == 0 || strcmp(origin, a2) == 0;
}

/* Read one byte at a time until \r\n\r\n or buffer full. */
static int read_headers(int fd, char *buf, int bufsz)
{
    int n = 0;
    while (n < bufsz - 1) {
        ssize_t nr = recv(fd, buf + n, 1, 0);
        if (nr <= 0) break;
        n++;
        if (n >= 4 &&
            buf[n-4] == '\r' && buf[n-3] == '\n' &&
            buf[n-2] == '\r' && buf[n-1] == '\n') {
            buf[n] = '\0';
            return n;
        }
    }
    if (n < bufsz) buf[n] = '\0';
    return -1;
}

static void handle_connection(mcp_server_t *srv, int fd)
{
    /* Per-connection receive timeout: prevents infinite block */
    {
        struct timeval tv = {10, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    char hdr_buf[MAX_HEADERS];
    if (read_headers(fd, hdr_buf, sizeof(hdr_buf)) < 0) {
        send_error(fd, 400, "Bad Request", "header read failed");
        return;
    }

    /* Parse request line */
    char method[16] = "", path[256] = "";
    sscanf(hdr_buf, "%15s %255s", method, path);

    /* Path check */
    if (strcmp(path, "/mcp") != 0) {
        send_error(fd, 404, "Not Found", "endpoint not found");
        return;
    }

    /* Method check */
    if (strcmp(method, "POST") != 0) {
        send_error(fd, 405, "Method Not Allowed", "use POST");
        return;
    }

    /* Authorization */
    char auth[512];
    char expected[512];
    hdr_get(hdr_buf, "authorization", auth, sizeof(auth));
    snprintf(expected, sizeof(expected), "Bearer %s", srv->token);
    if (strcmp(auth, expected) != 0) {
        send_error(fd, 401, "Unauthorized", "invalid or missing token");
        return;
    }

    /* Origin check — only when the header is present */
    char origin[256];
    if (hdr_get(hdr_buf, "origin", origin, sizeof(origin))) {
        if (!is_local_origin(origin, srv->actual_port)) {
            send_error(fd, 403, "Forbidden", "non-local origin");
            return;
        }
    }

    /* Content-Length */
    char cl_str[32];
    if (!hdr_get(hdr_buf, "content-length", cl_str, sizeof(cl_str))) {
        send_error(fd, 400, "Bad Request", "missing Content-Length");
        return;
    }
    int content_length = atoi(cl_str);
    if (content_length <= 0 || content_length > MAX_BODY) {
        send_error(fd, 400, "Bad Request", "invalid Content-Length");
        return;
    }

    /* Read body */
    char body[MAX_BODY + 1];
    int received = 0;
    while (received < content_length) {
        ssize_t nr = recv(fd, body + received,
                          (size_t)(content_length - received), 0);
        if (nr <= 0) break;
        received += (int)nr;
    }
    body[received] = '\0';

    /* Parse JSON-RPC method and id */
    char rpc_method[256] = "";
    char rpc_id[64]      = "null";
    json_get_str(body, "method", rpc_method, sizeof(rpc_method));
    json_get_id(body, rpc_id, sizeof(rpc_id));

    /* Dispatch */
    if (strcmp(rpc_method, "initialize") == 0) {
        handle_initialize(fd, rpc_id);
    } else if (strcmp(rpc_method, "notifications/initialized") == 0) {
        http_respond_202(fd);
    } else if (strcmp(rpc_method, "tools/list") == 0) {
        handle_tools_list(fd, rpc_id);
    } else if (strcmp(rpc_method, "tools/call") == 0) {
        handle_tools_call(fd, rpc_id, body, srv);
    } else {
        char resp[512];
        jsonrpc_error(resp, sizeof(resp), rpc_id, -32601, "Method not found");
        send_json(fd, 200, "OK", resp);
    }
}

/* ==========================================================================
 * Background accept thread
 * ========================================================================== */

static void *mcp_thread(void *arg)
{
    mcp_server_t *srv = (mcp_server_t *)arg;

    while (srv->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv->listen_fd, &rfds);
        struct timeval tv = {0, 500000};  /* 500 ms polling interval */

        int n = select(srv->listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;  /* fd was closed by mcp_server_stop() */
        }
        if (n == 0) continue;

        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int conn_fd = accept(srv->listen_fd,
                              (struct sockaddr *)&cli, &cli_len);
        if (conn_fd < 0) {
            if (errno == EINTR || errno == EBADF || errno == EINVAL) break;
            continue;
        }

#ifdef SO_NOSIGPIPE
        {
            int flag = 1;
            setsockopt(conn_fd, SOL_SOCKET, SO_NOSIGPIPE,
                       &flag, sizeof(flag));
        }
#endif

        handle_connection(srv, conn_fd);
        /* Drain any unread request data so the kernel sends FIN not RST. */
        {
            char drain[512];
            struct timeval dtv = {0, 50000};   /* 50 ms */
            setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &dtv, sizeof(dtv));
            while (recv(conn_fd, drain, sizeof(drain), 0) > 0) {}
        }
        close(conn_fd);
    }

    return NULL;
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

mcp_server_status_t mcp_server_start(const ai_config_t *cfg,
                                      mcp_server_t **out)
{
    *out = NULL;

    if (!cfg->mcp_enabled) return MCP_SERVER_DISABLED;

    if (cfg->mcp_token[0] == '\0') {
        fprintf(stderr,
                "aishell: MCP enabled but token is empty; server not started\n");
        return MCP_SERVER_INVALID_CONFIG;
    }

    mcp_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) return MCP_SERVER_BIND_FAILED;

    /* Copy token — never print it */
    snprintf(srv->token, sizeof(srv->token), "%s", cfg->mcp_token);

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) { free(srv); return MCP_SERVER_BIND_FAILED; }

    {
        int opt = 1;
        setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = htons((uint16_t)cfg->mcp_port);

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(srv->listen_fd);
        free(srv);
        return MCP_SERVER_BIND_FAILED;
    }

    /* Retrieve the actual bound port (important when cfg->mcp_port == 0) */
    socklen_t addrlen = sizeof(addr);
    getsockname(srv->listen_fd, (struct sockaddr *)&addr, &addrlen);
    srv->actual_port = ntohs(addr.sin_port);

    if (listen(srv->listen_fd, LISTEN_BACKLOG) < 0) {
        close(srv->listen_fd);
        free(srv);
        return MCP_SERVER_LISTEN_FAILED;
    }

    srv->running = 1;
    if (pthread_create(&srv->thread, NULL, mcp_thread, srv) != 0) {
        close(srv->listen_fd);
        free(srv);
        return MCP_SERVER_THREAD_FAILED;
    }

    fprintf(stderr, "aishell: MCP server listening on 127.0.0.1:%d\n",
            srv->actual_port);

    *out = srv;
    return MCP_SERVER_OK;
}

int mcp_server_actual_port(const mcp_server_t *server)
{
    return server ? server->actual_port : -1;
}

void mcp_server_stop(mcp_server_t *server)
{
    if (!server) return;
    server->running = 0;
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    pthread_join(server->thread, NULL);
    free(server);
}

int mcp_server_write_config(const mcp_server_t *server,
                             const ai_config_t  *cfg,
                             char               *path_out,
                             size_t              path_out_len)
{
    if (!server || server->actual_port <= 0 || server->listen_fd < 0)
        return -1;

    /* Create a private temp file; mkstemp ensures uniqueness. */
    char tmpl[64];
    snprintf(tmpl, sizeof(tmpl), "/tmp/aishell_mcp_%d_XXXXXX", (int)getpid());
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;

    /* Restrict to owner: the file contains the bearer token. */
    fchmod(fd, 0600);

    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
                     "{\n"
                     "  \"mcpServers\": {\n"
                     "    \"aishell\": {\n"
                     "      \"type\": \"http\",\n"
                     "      \"url\": \"http://127.0.0.1:%d/mcp\",\n"
                     "      \"headers\": {\n"
                     "        \"Authorization\": \"Bearer %s\"\n"
                     "      },\n"
                     "      \"timeout\": %d\n"
                     "    }\n"
                     "  }\n"
                     "}\n",
                     server->actual_port,
                     cfg->mcp_token,
                     cfg->mcp_tool_timeout_ms);

    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        close(fd);
        unlink(tmpl);
        return -1;
    }

    ssize_t written = write(fd, buf, (size_t)n);
    close(fd);

    if (written != (ssize_t)n) {
        unlink(tmpl);
        return -1;
    }

    snprintf(path_out, path_out_len, "%s", tmpl);
    return 0;
}

const char *mcp_server_status_name(mcp_server_status_t status)
{
    switch (status) {
    case MCP_SERVER_OK:             return "ok";
    case MCP_SERVER_DISABLED:       return "disabled";
    case MCP_SERVER_INVALID_CONFIG: return "invalid_config";
    case MCP_SERVER_BIND_FAILED:    return "bind_failed";
    case MCP_SERVER_LISTEN_FAILED:  return "listen_failed";
    case MCP_SERVER_THREAD_FAILED:  return "thread_failed";
    }
    return "unknown";
}
