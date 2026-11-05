/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* door.c -- see door.h. */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "door.h"

/* A `lamps` snapshot is a few kilobytes of JSON, and a `display` event is 1024
 * bytes of base64 plus its fields.  64 kB is far past the largest line and small
 * enough that a runaway line is caught.  A longer line is discarded to the next
 * newline and counted. */
#define DOOR_LINE_MAX 65536
#define DOOR_TX_MAX   8192

struct door {
    char host[256];
    int  port;
    int  fd;

    char rx[DOOR_LINE_MAX];
    size_t rx_len;
    int  rx_over;               /* discarding an oversized line */

    char tx[DOOR_TX_MAX];
    size_t tx_head, tx_len;

    struct door_stats stats;
};

/* --------------------------------------------------------------------------
 * Connecting
 * ------------------------------------------------------------------------ */

static int connect_to(const char *host, int port) {
    char service[16];
    snprintf(service, sizeof service, "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, service, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    /* Non-blocking from here on: one thread polls two descriptors, so a blocking
     * write would stop the Fire's LEDs from updating. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    /* No Nagle: each command is a whole message, and coalescing adds latency. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

struct door *door_open(const char *host, int port) {
    struct door *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    snprintf(d->host, sizeof d->host, "%s", host);
    d->port = port;
    d->fd = connect_to(host, port);
    /* A refused connection is not an error: the client may start before the
     * panel, and the loop reconnects.  Only an unresolvable name is fatal. */
    if (d->fd < 0) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        char service[16];
        snprintf(service, sizeof service, "%d", port);
        if (getaddrinfo(host, service, &hints, &res) != 0) {
            free(d);
            return NULL;
        }
        freeaddrinfo(res);
    }
    return d;
}

void door_close(struct door *d) {
    if (!d) return;
    if (d->fd >= 0) close(d->fd);
    free(d);
}

int door_fd(struct door *d) {
    return d ? d->fd : -1;
}

int door_connected(struct door *d) {
    return d && d->fd >= 0;
}

int door_reconnect(struct door *d) {
    if (!d) return 0;
    if (d->fd >= 0) return 1;
    d->fd = connect_to(d->host, d->port);
    if (d->fd < 0) return 0;

    /* A new socket is a new peer state: half-read data and queued commands for
     * the old peer are dropped rather than delivered out of order. */
    d->rx_len = 0;
    d->rx_over = 0;
    d->tx_head = d->tx_len = 0;
    d->stats.reconnects++;
    return 1;
}

/* --------------------------------------------------------------------------
 * Sending
 * ------------------------------------------------------------------------ */

static void flush_tx(struct door *d) {
    while (d->tx_len > 0) {
        const ssize_t n = write(d->fd, d->tx + d->tx_head, d->tx_len);
        if (n > 0) {
            d->tx_head += (size_t)n;
            d->tx_len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        /* A real error: drop what was queued.  The read side reports the same
         * disconnect, so retrying here would report it twice. */
        d->tx_head = d->tx_len = 0;
        return;
    }
    d->tx_head = d->tx_len = 0;
}

static int door_vsendf(struct door *d, const char *fmt, va_list ap) {
    if (!d || d->fd < 0) return 0;

    char line[512];
    const int n = vsnprintf(line, sizeof line, fmt, ap);
    if (n < 0) return 0;
    if ((size_t)n + 1 > sizeof line) {
        /* A command longer than this is not one of ours; refuse it so the
         * queue's accounting stays honest. */
        d->stats.sent_dropped++;
        return 0;
    }

    if (d->tx_len + (size_t)n + 1 > DOOR_TX_MAX) {
        /* The panel is not draining.  Count and drop rather than queue: a press
         * delivered a second late is worse than one not delivered. */
        d->stats.sent_dropped++;
        return 0;
    }

    memcpy(d->tx + d->tx_head + d->tx_len, line, (size_t)n);
    d->tx_len += (size_t)n;
    d->tx[d->tx_head + d->tx_len] = '\n';
    d->tx_len += 1;

    d->stats.lines_out++;
    flush_tx(d);
    return 1;
}

int door_send(struct door *d, const char *line) {
    return door_sendf(d, "%s", line);
}

int door_sendf(struct door *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int ok = door_vsendf(d, fmt, ap);
    va_end(ap);
    return ok;
}

/* --------------------------------------------------------------------------
 * Receiving
 * ------------------------------------------------------------------------ */

int door_poll(struct door *d, void (*cb)(const char *line, void *user), void *user) {
    if (!d || d->fd < 0) return 0;

    if (d->tx_len > 0) flush_tx(d);

    int delivered = 0;
    for (;;) {
        if (d->rx_len == sizeof d->rx) {
            /* The buffer is full with no newline.  Discard the line so the next
             * newline resynchronises. */
            d->rx_len = 0;
            d->rx_over = 1;
            d->stats.oversized++;
        }

        const ssize_t n = read(d->fd, d->rx + d->rx_len, sizeof d->rx - d->rx_len);
        if (n == 0) {
            close(d->fd);
            d->fd = -1;
            d->rx_len = 0;
            return -1;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            close(d->fd);
            d->fd = -1;
            d->rx_len = 0;
            return -1;
        }
        d->rx_len += (size_t)n;

        /* Every complete line read, oldest first.  A read can hold several
         * events (the panel writes a burst on attach). */
        size_t start = 0;
        for (size_t i = 0; i < d->rx_len; i++) {
            if (d->rx[i] != '\n') continue;
            size_t len = i - start;
            if (len > 0 && d->rx[start + len - 1] == '\r') len--;
            if (!d->rx_over && len > 0) {
                d->rx[start + len] = '\0';
                cb(d->rx + start, user);
                d->stats.lines_in++;
                delivered++;
            }
            d->rx_over = 0;
            start = i + 1;
        }
        if (start > 0) {
            memmove(d->rx, d->rx + start, d->rx_len - start);
            d->rx_len -= start;
        }
    }
    return delivered;
}

void door_get_stats(struct door *d, struct door_stats *out) {
    if (!out) return;
    if (!d) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = d->stats;
}
