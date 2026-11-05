/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* midi.h -- the Fire's MIDI transport behind one seam.
 *
 *   alsa:NAME   a real or virtual Fire, found by name, both directions
 *   file:PATH   a transcript, both directions, replayable in tests
 *
 * ALSA is optional, so midi_alsa.c is compiled only where it resolves.
 */

#ifndef TRACKER_FIRE_MIDI_H
#define TRACKER_FIRE_MIDI_H

#include <stddef.h>
#include <stdint.h>

/* One MIDI message, fixed size.  A Fire message is a 3-byte CC, a 12-byte RGB
 * SysEx, or a 159-byte OLED stripe.  FIRE_MSG_MAX is larger than all of them.
 * Nothing here allocates. */
#define FIRE_MSG_MAX 256

struct midi;

/* Which way a transport is opened. A file spec is truncated for output, so
 * the direction cannot be inferred from it. */
enum {
    MIDI_SEND    = 1,       /* midi_send will be used */
    MIDI_RECEIVE = 2        /* input will arrive, or a fixture will be replayed */
};

/* What a transport can say about itself, for the one line this program prints at
 * startup. */
struct midi_info {
    const char *kind;       /* "alsa", "file", "stdout", "none" */
    const char *detail;     /* the port it found, or the path it opened */
    int         writable;   /* can midi_send do anything */
    int         readable;   /* can input arrive */
};

/* Open a transport in one direction.  `spec` is one of the forms above; NULL or
 * "" is "none".  Returns NULL on failure; midi_error() says why. */
struct midi *midi_open(const char *spec, int direction);

const char *midi_error(void);

void midi_close(struct midi *m);

/* Fill `info` with what was opened, for the startup line and for --list. */
void midi_describe(struct midi *m, struct midi_info *info);

/* A file descriptor to poll for input, or -1 when input cannot arrive (a file
 * sink, a stdout sink, or "none").  The main loop polls two fds and uses no
 * threads. */
int midi_fd(struct midi *m);

/* Send one complete message.  Returns 1 if it went, 0 otherwise.  It does not
 * retry: the caller owns the queue and pacing, because the Fire drops what
 * arrives faster than it drains. */
int midi_send(struct midi *m, const uint8_t *bytes, int len);

/* Take every complete message that has arrived, oldest first, and hand each to
 * `cb`.  Returns the number delivered, or -1 on error.  It drains the queue per
 * call so a burst does not wait for the next poll round. */
int midi_take_input(struct midi *m,
                    void (*cb)(const uint8_t *bytes, int len, void *user),
                    void *user);

/* Replay a whole input fixture now, one message per call to `cb`.  The fixture
 * uses the same text the file sink writes.  Only a file: transport opened for
 * receiving can do this. */
int midi_replay(struct midi *m,
                void (*cb)(const uint8_t *bytes, int len, void *user),
                void *user);

/* Parse a "KIND:DETAIL" spec into caller buffers.  Returns 0 if the spec names
 * no transport this build has, and writes the reason to midi_error().  This list
 * matches what build.sh can compile. */
int midi_parse_spec(const char *spec, char *kind, size_t kind_len,
                    char *detail, size_t detail_len);

/* True when this spec names a recording (a `file:`) rather than a wire.  A
 * caller must ask before midi_replay(): a wire is read with midi_take_input() as
 * it arrives, and replaying one is wrong.
 *
 * A spec that names no transport this build can open also answers 0.  To tell
 * those apart, ask midi_parse_spec() first; it sets midi_error(). */
int midi_spec_is_replayable(const char *spec);

/* List the ports this build could open, to stderr: with ALSA, every sequencer
 * client's ports with capabilities and numbers; without it, a line saying ALSA
 * was not compiled in.  The Fire's client number is reassigned on every replug,
 * so the client must be able to say what it sees. */
int midi_list_ports(void);

/* True when this build can open a real Fire at all.  A test uses it to say why a
 * hardware rung was skipped rather than failing. */
int midi_have_alsa(void);

#endif /* TRACKER_FIRE_MIDI_H */
