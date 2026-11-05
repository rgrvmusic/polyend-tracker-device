/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* door.h -- the panel's line-JSON door, as a client sees it.
 *
 * One TCP socket carries both directions: a command per line in, and every
 * event pushed back out on the same socket, with a reply to each command
 * interleaved among them.  A reply is `{"t":"ok"}` or `{"t":"error"}` and an
 * event is one of the panel's own kinds, so the `t` field demultiplexes.
 *
 * This is a blocking-connect, non-blocking-io client with no threads: the main
 * loop is one poll() over this fd and the MIDI fd.  A dropped door is
 * reconnected rather than fatal.
 *
 * Lines are handed over as text; this file does not parse them.
 */

#ifndef TRACKER_FIRE_DOOR_H
#define TRACKER_FIRE_DOOR_H

struct door;

/* Connect to host:port.  Returns NULL only for a bad host name.  A refused
 * connection returns a door that is not connected, so the panel need not be
 * running first; see door_reconnect. */
struct door *door_open(const char *host, int port);

void door_close(struct door *d);

/* A fd to poll for input, or -1 while disconnected. */
int door_fd(struct door *d);

int door_connected(struct door *d);

/* Try to connect if not already.  Returns 1 when connected.  Safe to call every
 * loop pass; the caller's poll() paces the retries. */
int door_reconnect(struct door *d);

/* Queue one line (the newline is added).  Returns 0 if it could not be queued:
 * a disconnected door or a full queue. */
int door_send(struct door *d, const char *line);

int door_sendf(struct door *d, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Write what has been queued, read what has arrived, and hand every complete
 * line to `cb`.  Returns the lines delivered, or -1 when the connection was
 * lost; the door is then marked disconnected for door_reconnect. */
int door_poll(struct door *d, void (*cb)(const char *line, void *user), void *user);

/* Counters, for the one status line the client prints. */
struct door_stats {
    long lines_in;
    long lines_out;
    long sent_dropped;      /* commands that did not fit the queue */
    long reconnects;
    long oversized;         /* lines longer than the buffer, and discarded */
};
void door_get_stats(struct door *d, struct door_stats *out);

#endif /* TRACKER_FIRE_DOOR_H */
