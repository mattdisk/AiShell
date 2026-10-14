/*
 * tiny_http_server — minimal C HTTP/1.0 server for local testing.
 *
 * Usage: ./tools/tiny_http_server PORT
 *
 * Listens on PORT, accepts connections in a loop, reads the incoming
 * request into a bounded buffer, and sends a fixed HTTP/1.0 response.
 * Content-Length is computed from the actual body at startup.
 * No threading, no file serving, no HTTPS.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BACKLOG  8
#define REQ_BUF  4096
#define RSP_BUF  512

static const char BODY[] = "Hello from tiny C HTTP server\n";

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: tiny_http_server PORT\n");
        return 1;
    }

    int port = atoi(argv[1]);
    if (port < 1 || port > 65535) {
        fprintf(stderr, "tiny_http_server: invalid port '%s'\n", argv[1]);
        return 1;
    }

    /* Build the response once, deriving Content-Length from the body. */
    int body_len = (int)(sizeof(BODY) - 1); /* exclude NUL terminator */
    char response[RSP_BUF];
    int resp_len = snprintf(response, sizeof(response),
                            "HTTP/1.0 200 OK\r\n"
                            "Content-Type: text/plain\r\n"
                            "Content-Length: %d\r\n"
                            "Connection: close\r\n"
                            "\r\n"
                            "%s",
                            body_len, BODY);
    if (resp_len < 0 || resp_len >= (int)sizeof(response)) {
        fprintf(stderr, "tiny_http_server: response buffer too small\n");
        return 1;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("tiny_http_server: socket");
        return 1;
    }

    /* Allow immediate re-bind after Ctrl+C. */
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((unsigned short)port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("tiny_http_server: bind");
        close(srv);
        return 1;
    }

    if (listen(srv, BACKLOG) < 0) {
        perror("tiny_http_server: listen");
        close(srv);
        return 1;
    }

    printf("tiny_http_server listening on port %d  (Ctrl+C to stop)\n", port);
    fflush(stdout);

    while (1) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            if (errno == EINTR) continue;
            perror("tiny_http_server: accept");
            continue;
        }

        /* Read and discard the request (bounded). */
        char req[REQ_BUF];
        recv(cli, req, sizeof(req) - 1, 0);

        /* Send the full response, looping until all bytes are written. */
        const char *p    = response;
        size_t      left = (size_t)resp_len;
        while (left > 0) {
            ssize_t n = send(cli, p, left, 0);
            if (n <= 0) break;
            p    += n;
            left -= (size_t)n;
        }

        close(cli);
        printf("Served request.\n");
        fflush(stdout);
    }

    close(srv);
    return 0;
}
