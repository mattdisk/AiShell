#define _POSIX_C_SOURCE 200809L

#include "socket_client.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int set_nonblocking(int fd, int nonblock)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags);
}

/*
 * Try one candidate address from getaddrinfo with a connect timeout.
 * Returns a connected fd on success, -1 if this address should be skipped.
 * last_err receives the SO_ERROR value so the caller can print a useful message
 * after all candidates fail.
 */
static int try_connect_one(const struct addrinfo *ai, int timeout_ms, int *last_err)
{
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { *last_err = errno; return -1; }

    if (set_nonblocking(fd, 1) < 0) {
        *last_err = errno;
        close(fd);
        return -1;
    }

    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
        /* Immediate success (common on loopback). */
        set_nonblocking(fd, 0);
        return fd;
    }

    if (errno != EINPROGRESS) {
        *last_err = errno;
        close(fd);
        return -1;
    }

    /* Wait for the handshake to complete. */
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000L;

    rc = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (rc <= 0) {
        *last_err = (rc == 0) ? ETIMEDOUT : errno;
        close(fd);
        return -1;
    }

    int so_err = 0;
    socklen_t so_len = sizeof(so_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len) < 0 || so_err != 0) {
        *last_err = so_err ? so_err : errno;
        close(fd);
        return -1;
    }

    set_nonblocking(fd, 0);
    return fd;
}

int sc_connect(const char *host, int port, int timeout_ms)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "http-get: cannot resolve '%s': %s\n",
                host, gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    int last_err = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = try_connect_one(ai, timeout_ms, &last_err);
        if (fd >= 0) break; /* connected — stop trying */
    }
    freeaddrinfo(res);

    if (fd < 0) {
        if (last_err == ETIMEDOUT)
            fprintf(stderr, "http-get: connection timed out after %d ms\n", timeout_ms);
        else
            fprintf(stderr, "http-get: connect to '%s' port %d: %s\n",
                    host, port, strerror(last_err));
    }
    return fd;
}

int sc_send_all(int fd, const char *buf, size_t len, int timeout_ms)
{
    size_t sent = 0;
    while (sent < len) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000L;

        int rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc < 0) {
            fprintf(stderr, "http-get: send select: %s\n", strerror(errno));
            return -1;
        }
        if (rc == 0) {
            fprintf(stderr, "http-get: send timed out after %d ms\n", timeout_ms);
            return -1;
        }

        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            fprintf(stderr, "http-get: send: %s\n", strerror(errno));
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

ssize_t sc_recv_bounded(int fd, char *buf, size_t maxlen, int timeout_ms,
                         int *truncated)
{
    *truncated = 0;
    size_t total = 0;

    while (total < maxlen - 1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000L;

        int rc = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            fprintf(stderr, "http-get: recv select: %s\n", strerror(errno));
            return -1;
        }
        if (rc == 0) {
            fprintf(stderr, "http-get: recv timed out after %d ms\n", timeout_ms);
            return -1;
        }

        ssize_t n = recv(fd, buf + total, maxlen - 1 - total, 0);
        if (n < 0) {
            fprintf(stderr, "http-get: recv: %s\n", strerror(errno));
            return -1;
        }
        if (n == 0) break; /* EOF — server closed connection */

        total += (size_t)n;

        if (total >= maxlen - 1) {
            *truncated = 1; /* caller decides how to report this */
            break;
        }
    }

    buf[total] = '\0';
    return (ssize_t)total;
}

void sc_close(int fd)
{
    if (fd >= 0) close(fd);
}
