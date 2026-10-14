#ifndef AISHELL_SOCKET_CLIENT_H
#define AISHELL_SOCKET_CLIENT_H

#include <sys/types.h>

/*
 * Resolve host/port and return a connected TCP socket fd.
 * Iterates all addresses returned by getaddrinfo until one connects.
 * Uses non-blocking connect() + select() so timeout_ms is the per-address
 * connect deadline.
 * Returns fd >= 0 on success, -1 on error (message written to stderr).
 */
int sc_connect(const char *host, int port, int timeout_ms);

/*
 * Send exactly len bytes from buf with a select()-based timeout per send.
 * Returns 0 on success, -1 on error (message written to stderr).
 */
int sc_send_all(int fd, const char *buf, size_t len, int timeout_ms);

/*
 * Receive up to maxlen-1 bytes into buf with a per-select() timeout.
 * NUL-terminates buf on success.
 * Sets *truncated = 1 if the response was cut at the maxlen-1 boundary;
 * the caller decides how to report this (stderr warning, JSON flag, etc.).
 * Returns total bytes received (>= 0) on success, -1 on error (message to stderr).
 */
ssize_t sc_recv_bounded(int fd, char *buf, size_t maxlen, int timeout_ms,
                         int *truncated);

/* Close fd; safe to call with fd < 0 (no-op). */
void sc_close(int fd);

#endif /* AISHELL_SOCKET_CLIENT_H */
