#define _POSIX_C_SOURCE 200809L

#include <argtable3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "../registry.h"
#include "../command.h"
#include "../net/socket_client.h"

/* Maximum response body buffered in memory (1 MB). */
#define HTTP_MAX_RESPONSE (1024 * 1024)

/* Default connect/send/recv timeout when --timeout is not specified. */
#define HTTP_DEFAULT_TIMEOUT_MS 5000

/*
 * Compute milliseconds remaining before the wall-clock deadline expires.
 * Returns at least 1 ms so callers never pass 0 to select() (which would
 * poll rather than wait).
 */
static int remaining_ms(const struct timeval *t0, int total_ms)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    long elapsed = (now.tv_sec  - t0->tv_sec)  * 1000L
                 + (now.tv_usec - t0->tv_usec) / 1000L;
    long rem = total_ms - elapsed;
    return rem > 1 ? (int)rem : 1;
}

static int run_get(int argc, char **argv)
{
    struct arg_lit *help    = arg_lit0("h", "help",    "Show this help message");
    struct arg_lit *json    = arg_lit0(NULL, "json",   "Print result as a JSON object");
    struct arg_int *timeout = arg_int0(NULL, "timeout","MS",
                                       "Total timeout in milliseconds (default 5000)");
    struct arg_str *hdrs    = arg_strn("H", "header", "Name: Value", 0, 64,
                                       "Custom request header (repeatable)");
    struct arg_str *host    = arg_str1(NULL, NULL, "HOST", "Hostname or IP address");
    struct arg_str *port_a  = arg_str1(NULL, NULL, "PORT", "TCP port (1-65535)");
    struct arg_str *path_a  = arg_str1(NULL, NULL, "PATH", "Request path (must start with /)");
    struct arg_end *end     = arg_end(20);
    void *argtable[] = {help, json, timeout, hdrs, host, port_a, path_a, end};
    if (arg_nullcheck(argtable)) return 1;

    int nerrors = arg_parse(argc, argv, argtable);

    /*
     * Help takes precedence over parse errors (so "http-get -h" works even
     * without HOST/PORT/PATH), matching the convention in ls.c and cat.c.
     */
    if (help->count && json->count) {
        print_help_json("http-get", "Fetch a URL via HTTP/1.0",
                        argtable, sizeof(argtable)/sizeof(argtable[0]));
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (help->count) {
        printf("Usage: http-get"); arg_print_syntax(stdout, argtable, "\n");
        printf("Fetch a URL via HTTP/1.0 using POSIX sockets.\n\n");
        arg_print_glossary(stdout, argtable, "  %-28s %s\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 0;
    }
    if (nerrors) {
        arg_print_errors(stderr, end, "http-get");
        fprintf(stderr, "Usage: http-get"); arg_print_syntax(stderr, argtable, "\n");
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }

    /* Validate port — must be numeric and within 1-65535. */
    int port_num;
    if (parse_int(port_a->sval[0], &port_num) != 0 ||
        port_num < 1 || port_num > 65535) {
        fprintf(stderr, "http-get: invalid port '%s': must be 1-65535\n",
                port_a->sval[0]);
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }

    /* Validate path — must begin with '/'. */
    if (path_a->sval[0][0] != '/') {
        fprintf(stderr, "http-get: path '%s' must start with '/'\n",
                path_a->sval[0]);
        free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
        return 2;
    }

    /* Validate --timeout. */
    int timeout_ms = HTTP_DEFAULT_TIMEOUT_MS;
    if (timeout->count) {
        if (timeout->ival[0] <= 0) {
            fprintf(stderr, "http-get: --timeout must be a positive integer\n");
            free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
            return 2;
        }
        timeout_ms = timeout->ival[0];
    }

    /*
     * Copy strings out of the argtable before freeing it.
     * arg_str storage is owned by argtable and released by free_argtable.
     */
    char host_buf[256];
    char path_buf[2048];
    snprintf(host_buf, sizeof(host_buf), "%s", host->sval[0]);
    snprintf(path_buf, sizeof(path_buf), "%s", path_a->sval[0]);
    int use_json = json->count > 0;

    /* Copy custom headers before freeing argtable. */
    int nhdr = hdrs->count;
    char **hdr_buf = NULL;
    if (nhdr > 0) {
        hdr_buf = malloc((size_t)nhdr * sizeof(char *));
        if (!hdr_buf) {
            fprintf(stderr, "http-get: out of memory\n");
            free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
            return 1;
        }
        for (int i = 0; i < nhdr; i++) {
            hdr_buf[i] = strdup(hdrs->sval[i]);
            if (!hdr_buf[i]) {
                fprintf(stderr, "http-get: out of memory\n");
                for (int j = 0; j < i; j++) free(hdr_buf[j]);
                free(hdr_buf);
                free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));
                return 1;
            }
        }
    }

    free_argtable(argtable, sizeof(argtable)/sizeof(argtable[0]));

    /* Record the wall-clock start so we can hand a shrinking deadline to each
     * network phase (connect → send → recv). */
    struct timeval t0;
    gettimeofday(&t0, NULL);

    /* Connect, consuming from the total deadline. */
    int fd = sc_connect(host_buf, port_num, remaining_ms(&t0, timeout_ms));
    if (fd < 0) {
        if (use_json) {
            printf("{\"ok\":false,\"host\":\"");
            json_escape_print(host_buf);
            printf("\",\"port\":%d,\"path\":\"", port_num);
            json_escape_print(path_buf);
            printf("\",\"error\":\"connection failed\"}\n");
        }
        return 1;
    }

    /*
     * Build and send a minimal HTTP/1.0 request.
     * The blank CRLF line after the headers terminates the request.
     */
    size_t req_cap = 4096;
    for (int i = 0; i < nhdr; i++)
        req_cap += strlen(hdr_buf[i]) + 4; /* "header\r\n" */
    char *req = malloc(req_cap);
    if (!req) {
        fprintf(stderr, "http-get: out of memory\n");
        sc_close(fd);
        for (int i = 0; i < nhdr; i++) free(hdr_buf[i]);
        free(hdr_buf);
        return 1;
    }
    int req_len = snprintf(req, req_cap,
                           "GET %s HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "Connection: close\r\n",
                           path_buf, host_buf);
    for (int i = 0; i < nhdr; i++)
        req_len += snprintf(req + req_len, req_cap - (size_t)req_len,
                            "%s\r\n", hdr_buf[i]);
    req_len += snprintf(req + req_len, req_cap - (size_t)req_len, "\r\n");

    for (int i = 0; i < nhdr; i++) free(hdr_buf[i]);
    free(hdr_buf);

    if (sc_send_all(fd, req, (size_t)req_len, remaining_ms(&t0, timeout_ms)) < 0) {
        free(req);
        sc_close(fd);
        if (use_json) {
            printf("{\"ok\":false,\"host\":\"");
            json_escape_print(host_buf);
            printf("\",\"port\":%d,\"path\":\"", port_num);
            json_escape_print(path_buf);
            printf("\",\"error\":\"send failed\"}\n");
        }
        return 1;
    }
    free(req);

    /* Receive response (bounded to HTTP_MAX_RESPONSE bytes). */
    char *buf = malloc(HTTP_MAX_RESPONSE);
    if (!buf) {
        fprintf(stderr, "http-get: out of memory\n");
        sc_close(fd);
        return 1;
    }

    int truncated = 0;
    ssize_t n = sc_recv_bounded(fd, buf, HTTP_MAX_RESPONSE,
                                remaining_ms(&t0, timeout_ms), &truncated);
    sc_close(fd);

    if (n < 0) {
        free(buf);
        if (use_json) {
            printf("{\"ok\":false,\"host\":\"");
            json_escape_print(host_buf);
            printf("\",\"port\":%d,\"path\":\"", port_num);
            json_escape_print(path_buf);
            printf("\",\"error\":\"recv failed\"}\n");
        }
        return 1;
    }

    if (use_json) {
        /* stdout must be valid JSON — truncation goes in the object, not as stray text. */
        printf("{\n  \"ok\": true,\n  \"host\": \"");
        json_escape_print(host_buf);
        printf("\",\n  \"port\": %d,\n  \"path\": \"", port_num);
        json_escape_print(path_buf);
        printf("\",\n  \"bytes_received\": %zd,\n  \"body\": \"", n);
        json_escape_print(buf);
        printf("\"");
        if (truncated)
            printf(",\n  \"truncated\": true");
        printf("\n}\n");
    } else {
        fwrite(buf, 1, (size_t)n, stdout);
        if (truncated)
            fprintf(stderr,
                    "http-get: warning: response truncated at %zu bytes"
                    " (limit %zu)\n",
                    (size_t)n, (size_t)(HTTP_MAX_RESPONSE - 1));
        /* Ensure the prompt appears on a fresh line. */
        if (n > 0 && buf[n - 1] != '\n') putchar('\n');
    }

    free(buf);
    return 0;
}

static int run_post(int argc, char **argv)
{
    return placeholder_command(argc, argv, "http-post",
                               "networking is reserved for a later implementation");
}

void builtin_http_init(void)
{
    registry_add("http-get",  "Fetch a URL via HTTP/1.0",          run_get);
    registry_add("http-post", "POST data to a URL (placeholder)",  run_post);
}
