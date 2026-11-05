/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* midi_priv.h -- what midi.c and midi_alsa.c both have to agree on.
 *
 * midi_alsa.c is compiled only where ALSA is installed (see midi.h), so it is a
 * separate translation unit that needs this shared structure.  Everything here
 * is internal; callers outside panel/fire/ include midi.h.
 */

#ifndef TRACKER_FIRE_MIDI_PRIV_H
#define TRACKER_FIRE_MIDI_PRIV_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "midi.h"

#define SPEC_KIND_MAX   16
#define SPEC_DETAIL_MAX 256
#define ERROR_MAX       256

struct midi {
    char kind[SPEC_KIND_MAX];
    char detail[SPEC_DETAIL_MAX];
    char error[ERROR_MAX];

    /* The ALSA sequencer's own state, owned by midi_alsa.c and opaque here.  A
     * void pointer keeps the structure the same shape with and without ALSA. */
    void *alsa;

    /* The direction this transport was opened in.  It decides whether `detail`
     * is a path to write or to read; getting it wrong truncates silently. */
    int direction;

    /* file:/stdout sink, and file: input fixture.  Both NULL when unused. */
    FILE *out;
    FILE *in;
    int   in_done;

    /* raw:/dev/snd/midiC?D? -- a USB MIDI endpoint opened directly, with no
     * alsa-lib.  The bytes are already MIDI, so rx holds a message being
     * assembled across reads. */
    int     fd;
    uint8_t rx[1024];
    int     rxlen;      /* a fixture is replayed once; see midi_replay */

    int wrote_anything; /* whether a transcript header has been written */
};

/* Set the reason the last call failed.  Shared with midi_alsa.c, since the caller
 * asks midi_error() whichever half failed. */
void midi_set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef HAVE_ALSA
/* Fill in `m->alsa` and connect.  Returns 1 on success; on failure it calls
 * midi_set_error() and leaves m->alsa NULL.  `m->detail` is the port name to look
 * for, or empty for the first Fire-looking port. */
int  midi_alsa_open(struct midi *m);
void midi_alsa_close(struct midi *m);
int  midi_alsa_fd(struct midi *m);
int  midi_alsa_send(struct midi *m, const uint8_t *bytes, int len);
int  midi_alsa_take_input(struct midi *m,
                          void (*cb)(const uint8_t *, int, void *), void *user);
void midi_alsa_describe(struct midi *m, struct midi_info *info);
int  midi_alsa_list_ports(void);
#endif

#endif /* TRACKER_FIRE_MIDI_PRIV_H */
