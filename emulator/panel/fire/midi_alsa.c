/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* midi_alsa.c -- the Fire transport over the ALSA sequencer.
 *
 * Ports are found by name, never by number: a client number is assigned on
 * plug-in. One poll() covers the sequencer fds and the door socket; input is
 * drained until EAGAIN, without threads.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <alsa/asoundlib.h>

#include "midi_priv.h"

/* The name a Fire answers to on Linux, as USB and ALSA report it.  The match is
 * a case-insensitive substring, so a variant such as "FL STUDIO FIRE Jack 1" is
 * still found. */
#define FIRE_DEFAULT_MATCH "fire"

/* What this program calls itself on the sequencer: client and port name both.
 * find_port must recognise it to skip it, since the name contains
 * FIRE_DEFAULT_MATCH and a client that matched itself would "find a Fire" on a
 * machine with none. */
#define TRACKER_FIRE_CLIENT_NAME "tracker-fire"

struct alsa_state {
    snd_seq_t *seq;
    int        my_port;
    int        dest_client;
    int        dest_port;
    char       port_name[128];      /* what was found and connected to */
    int        out_ok;              /* our writes reach it */
    int        in_ok;               /* its writes reach us */
};

/* --------------------------------------------------------------------------
 * Finding the device
 * ------------------------------------------------------------------------ */

static int match(const char *haystack, const char *needle) {
    return haystack && needle && strcasestr(haystack, needle) != NULL;
}

/* The first port whose client or port name contains `want`, preferring one that can
 * be written to (a synth port and a rawmidi port can share a name, and only one of
 * them lights an LED).  Returns 1 and fills the out-params when found. */
static int find_port(snd_seq_t *seq, const char *want,
                     int *client_out, int *port_out, char *name_out,
                     size_t name_len) {
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t   *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);

    int best_client = -1, best_port = -1, best_writable = 0;
    char best_name[128] = "";

    /* Our own client is not what "fire" means.  Its port name contains the
     * default match and is writable, so it would win the search, light nothing,
     * and read its own SysEx back as input. */
    const int self = snd_seq_client_id(seq);

    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        const int c = snd_seq_client_info_get_client(ci);
        const char *cname = snd_seq_client_info_get_name(ci);
        if (c == 0) continue;                       /* System: Timer/Announce */
        if (c == self) continue;
        if (cname && !strcmp(cname, TRACKER_FIRE_CLIENT_NAME)) continue;

        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            const char *pname = snd_seq_port_info_get_name(pi);
            if (!match(pname, want) && !match(cname, want)) continue;

            const unsigned caps = snd_seq_port_info_get_capability(pi);
            const int writable = (caps & SND_SEQ_PORT_CAP_WRITE) != 0;
            const int p = snd_seq_port_info_get_port(pi);

            /* First writable port wins outright; a readable-only port is kept only
             * as a last resort, because a client that cannot light anything is not
             * the port somebody meant. */
            if (best_client < 0 || (writable && !best_writable)) {
                best_client = c;
                best_port = p;
                best_writable = writable;
                snprintf(best_name, sizeof best_name, "%s / %s", cname, pname);
                if (writable) break;
            }
        }
        if (best_writable) break;
    }

    if (best_client < 0) return 0;
    *client_out = best_client;
    *port_out = best_port;
    snprintf(name_out, name_len, "%s", best_name);
    return 1;
}

/* --------------------------------------------------------------------------
 * Open
 * ------------------------------------------------------------------------ */

int midi_alsa_open(struct midi *m) {
    struct alsa_state *a = calloc(1, sizeof *a);
    if (!a) {
        midi_set_error("out of memory");
        return 0;
    }
    m->alsa = a;

    /* "default" and not "hw": both are the kernel sequencer, but "default" gets
     * the name a sound server may have assigned.  --out names a port, not a
     * device, because the port is what changes on replug. */
    int rc = snd_seq_open(&a->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
    if (rc < 0) {
        midi_set_error("alsa: cannot open the sequencer: %s", snd_strerror(rc));
        return 0;
    }
    snd_seq_set_client_name(a->seq, TRACKER_FIRE_CLIENT_NAME);

    /* One duplex, exportable port.  SUBS_READ and SUBS_WRITE let the other end
     * subscribe to it. */
    a->my_port = snd_seq_create_simple_port(
        a->seq, TRACKER_FIRE_CLIENT_NAME,
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (a->my_port < 0) {
        midi_set_error("alsa: cannot create a port: %s", snd_strerror(a->my_port));
        return 0;
    }

    const char *want = m->detail[0] ? m->detail : FIRE_DEFAULT_MATCH;
    if (!find_port(a->seq, want, &a->dest_client, &a->dest_port,
                   a->port_name, sizeof a->port_name)) {
        midi_set_error("alsa: no sequencer port matching %s.  --list shows what is "
                       "there; note that the Fire must be plugged in *before* this "
                       "runs, because a client number is assigned when it appears.",
                       want);
        return 0;
    }

    /* Both directions are reported separately: a port can be readable and not
     * writable, or the reverse.  A one-way connection is not fatal, but it
     * explains a program that runs and does nothing. */
    rc = snd_seq_connect_to(a->seq, a->my_port, a->dest_client, a->dest_port);
    a->out_ok = rc >= 0;
    int out_err = rc;
    rc = snd_seq_connect_from(a->seq, a->my_port, a->dest_client, a->dest_port);
    a->in_ok = rc >= 0;
    int in_err = rc;

    if (!a->out_ok && !a->in_ok) {
        midi_set_error("alsa: found %s (client %d:%d) but could not subscribe to "
                       "it: %s.  `aconnect %d:%d <tracker-fire's client>:0` connects "
                       "them by hand.",
                       a->port_name, a->dest_client, a->dest_port,
                       snd_strerror(out_err), a->dest_client, a->dest_port);
        return 0;
    }
    if (!a->out_ok) {
        fprintf(stderr, "tracker-fire: warning: %s is not writable (%s); nothing will "
                        "light.\n", a->port_name, snd_strerror(out_err));
    }
    if (!a->in_ok) {
        fprintf(stderr, "tracker-fire: warning: %s is not readable (%s); no presses "
                        "will arrive.\n", a->port_name, snd_strerror(in_err));
    }

    (void)in_err;
    return 1;
}

void midi_alsa_close(struct midi *m) {
    struct alsa_state *a = m->alsa;
    if (!a) return;
    if (a->seq) snd_seq_close(a->seq);
    free(a);
    m->alsa = NULL;
}

void midi_alsa_describe(struct midi *m, struct midi_info *info) {
    struct alsa_state *a = m->alsa;
    info->kind = "alsa";
    info->detail = a ? a->port_name : "";
    info->writable = a ? a->out_ok : 0;
    info->readable = a ? a->in_ok : 0;
}

int midi_alsa_fd(struct midi *m) {
    struct alsa_state *a = m->alsa;
    if (!a || !a->seq) return -1;
    /* The sequencer's own poll descriptors.  There is normally one; taking the
     * first is what every ALSA-seq client does, and the count is asserted by the
     * poll() call itself when it is handed however many there are. */
    struct pollfd *pfds = NULL;
    int n = snd_seq_poll_descriptors_count(a->seq, POLLIN);
    if (n <= 0) return -1;
    pfds = alloca((size_t)n * sizeof *pfds);
    snd_seq_poll_descriptors(a->seq, pfds, (unsigned)n, POLLIN);
    return pfds[0].fd;
}

/* --------------------------------------------------------------------------
 * Output
 * ------------------------------------------------------------------------ */

int midi_alsa_send(struct midi *m, const uint8_t *bytes, int len) {
    struct alsa_state *a = m->alsa;
    if (!a || !a->out_ok || len <= 0) return 0;

    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, a->my_port);
    snd_seq_ev_set_dest(&ev, a->dest_client, a->dest_port);
    snd_seq_ev_set_direct(&ev);

    const uint8_t status = bytes[0];
    const int channel = status & 0x0F;

    if (status == 0xF0) {
        /* SysEx.  The sequencer carries it as variable-length event data, and the
         * bytes handed to it are the whole message including F0 and F7 -- which is
         * also what arrives on the input side, so the two ends of this file agree
         * about what a sysex event contains. */
        snd_seq_ev_set_sysex(&ev, len, (void *)(uintptr_t)bytes);
    } else if (len < 3) {
        return 0;                       /* not a channel message this can express */
    } else if ((status & 0xF0) == 0x90) {
        snd_seq_ev_set_noteon(&ev, channel, bytes[1], bytes[2]);
    } else if ((status & 0xF0) == 0x80) {
        snd_seq_ev_set_noteoff(&ev, channel, bytes[1], bytes[2]);
    } else if ((status & 0xF0) == 0xB0) {
        snd_seq_ev_set_controller(&ev, channel, bytes[1], bytes[2]);
    } else if ((status & 0xF0) == 0xE0) {
        snd_seq_ev_set_pitchbend(&ev, channel,
                                 ((bytes[2] << 7) | bytes[1]) - 8192);
    } else if ((status & 0xF0) == 0xC0) {
        snd_seq_ev_set_pgmchange(&ev, channel, bytes[1]);
    } else if ((status & 0xF0) == 0xD0) {
        snd_seq_ev_set_chanpress(&ev, channel, bytes[1]);
    } else if ((status & 0xF0) == 0xA0) {
        snd_seq_ev_set_keypress(&ev, channel, bytes[1], bytes[2]);
    } else {
        /* Anything else goes out as raw bytes rather than being dropped: this
         * client's job is to carry what the mapping produced, and a message it
         * cannot name is still a message the panel meant to send. */
        snd_seq_ev_set_variable(&ev, len, (void *)(uintptr_t)bytes);
    }

    const int rc = snd_seq_event_output_direct(a->seq, &ev);
    if (rc < 0) {
        midi_set_error("alsa: send failed: %s", snd_strerror(rc));
        return 0;
    }
    return 1;
}

/* --------------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------------ */

/* One sequencer event as the raw MIDI bytes it stands for.  Returns the length, or
 * 0 for an event that is not MIDI (a timer tick, a port change, a subscription
 * notice) -- those are not failures, they are just not input. */
static int event_to_bytes(snd_seq_event_t *ev, uint8_t *out, int max) {
    const int ch = ev->data.note.channel & 0x0F;
    switch (ev->type) {
    case SND_SEQ_EVENT_NOTEON:
        if (max < 3) return 0;
        out[0] = (uint8_t)(0x90 | ch);
        out[1] = ev->data.note.note;
        out[2] = ev->data.note.velocity;
        return 3;
    case SND_SEQ_EVENT_NOTEOFF:
        if (max < 3) return 0;
        out[0] = (uint8_t)(0x80 | ch);
        out[1] = ev->data.note.note;
        out[2] = ev->data.note.velocity;
        return 3;
    case SND_SEQ_EVENT_CONTROLLER:
        if (max < 3) return 0;
        out[0] = (uint8_t)(0xB0 | ch);
        out[1] = ev->data.control.param;
        out[2] = ev->data.control.value;
        return 3;
    case SND_SEQ_EVENT_PITCHBEND: {
        if (max < 3) return 0;
        const int v = ev->data.control.value + 8192;
        out[0] = (uint8_t)(0xE0 | ch);
        out[1] = (uint8_t)(v & 0x7F);
        out[2] = (uint8_t)((v >> 7) & 0x7F);
        return 3;
    }
    case SND_SEQ_EVENT_PGMCHANGE:
        if (max < 2) return 0;
        out[0] = (uint8_t)(0xC0 | ch);
        out[1] = ev->data.control.value;
        return 2;
    case SND_SEQ_EVENT_CHANPRESS:
        if (max < 2) return 0;
        out[0] = (uint8_t)(0xD0 | ch);
        out[1] = ev->data.control.value;
        return 2;
    case SND_SEQ_EVENT_KEYPRESS:
        if (max < 3) return 0;
        out[0] = (uint8_t)(0xA0 | ch);
        out[1] = ev->data.note.note;
        out[2] = ev->data.note.velocity;
        return 3;
    case SND_SEQ_EVENT_SYSEX: {
        /* Variable-length: the length is in ext.len and the bytes at ext.ptr, and
         * they include the F0 and F7 the sender passed to snd_seq_ev_set_sysex. */
        const int n = (int)ev->data.ext.len;
        if (n <= 0 || n > max) return 0;
        memcpy(out, ev->data.ext.ptr, (size_t)n);
        return n;
    }
    default:
        return 0;
    }
}

int midi_alsa_take_input(struct midi *m,
                         void (*cb)(const uint8_t *, int, void *), void *user) {
    struct alsa_state *a = m->alsa;
    if (!a || !a->in_ok) return 0;

    int delivered = 0;
    for (;;) {
        snd_seq_event_t *ev = NULL;
        const int rc = snd_seq_event_input(a->seq, &ev);
        if (rc < 0) {
            /* -EAGAIN is the empty queue, which is the normal way this loop ends. */
            if (rc != -EAGAIN) {
                midi_set_error("alsa: read failed: %s", snd_strerror(rc));
                return -1;
            }
            break;
        }
        uint8_t bytes[FIRE_MSG_MAX];
        const int n = event_to_bytes(ev, bytes, sizeof bytes);
        if (n > 0) {
            cb(bytes, n, user);
            delivered++;
        }
    }
    return delivered;
}

/* --------------------------------------------------------------------------
 * --list
 * ------------------------------------------------------------------------ */

int midi_alsa_list_ports(void) {
    snd_seq_t *seq = NULL;
    int rc = snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK);
    if (rc < 0) {
        fprintf(stderr, "tracker-fire: cannot open the sequencer: %s\n",
                snd_strerror(rc));
        return 1;
    }

    snd_seq_client_info_t *ci;
    snd_seq_port_info_t   *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);

    printf("%-8s %-40s %s\n", "PORT", "NAME", "CAPS");
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        const int c = snd_seq_client_info_get_client(ci);
        if (c == 0) continue;
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            const unsigned caps = snd_seq_port_info_get_capability(pi);
            char cap[8];
            int k = 0;
            if (caps & SND_SEQ_PORT_CAP_READ)  cap[k++] = 'R';
            if (caps & SND_SEQ_PORT_CAP_WRITE) cap[k++] = 'W';
            cap[k] = '\0';
            printf("%d:%-5d %-40s %s\n", c, snd_seq_port_info_get_port(pi),
                   snd_seq_port_info_get_name(pi), cap);
        }
    }
    snd_seq_close(seq);
    return 0;
}
