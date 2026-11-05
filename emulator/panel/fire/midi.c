/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* midi.c -- the transport seam, and the two transports that need no hardware.
 *
 * The file and stdout transports are how the mapping is proved: a transcript
 * line is one message, so the proof is a diff.  The same text is the input
 * fixture format, so a recording from a real Fire can be replayed.
 *
 * The ALSA half is a separate file (midi_alsa.c) because it cannot be compiled
 * everywhere.  See midi.h.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "midi_priv.h"

static char last_error[ERROR_MAX];

const char *midi_error(void) {
    return last_error;
}

void midi_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof (last_error), fmt, ap);
    va_end(ap);
}

/* --------------------------------------------------------------------------
 * The spec
 * ------------------------------------------------------------------------ */

int midi_parse_spec(const char *spec, char *kind, size_t kind_len,
                    char *detail, size_t detail_len) {
    if (!spec || !*spec || !strcmp(spec, "none")) {
        snprintf(kind, kind_len, "none");
        detail[0] = '\0';
        return 1;
    }

    const char *colon = strchr(spec, ':');
    size_t kn = colon ? (size_t)(colon - spec) : strlen(spec);
    if (kn == 0 || kn >= kind_len) {
        midi_set_error("%s: no transport name before the ':'", spec);
        return 0;
    }
    memcpy(kind, spec, kn);
    kind[kn] = '\0';
    snprintf(detail, detail_len, "%s", colon ? colon + 1 : "");

    /* One list, and it is the list build.sh can compile: a kind this build has no
     * code for is refused here rather than at the first send, where the failure
     * would look like the Fire not answering. */
    if (!strcasecmp(kind, "none") || !strcasecmp(kind, "stdout")) {
        return 1;
    }
    if (!strcasecmp(kind, "file")) {
        if (!detail[0]) {
            midi_set_error("file: needs a path, e.g. --out file:/tmp/fire.txt");
            return 0;
        }
        return 1;
    }
    if (!strcasecmp(kind, "alsa")) {
#ifdef HAVE_ALSA
        return 1;
#else
        midi_set_error("alsa: this build has no ALSA (no alsa-lib headers where it "
                       "was compiled).  Use raw:/dev/snd/midiC?D? for a USB MIDI "
                       "endpoint, or --out file:PATH / stdout for a transcript.");
        return 0;
#endif
    }
    if (!strcasecmp(kind, "raw")) {
        return 1;               /* empty detail means: find the first endpoint */
    }
    midi_set_error("%s: unknown transport %s (have alsa, raw, file, stdout, none)",
                   spec, kind);
    return 0;
}

int midi_have_alsa(void) {
#ifdef HAVE_ALSA
    return 1;
#else
    return 0;
#endif
}

int midi_spec_is_replayable(const char *spec) {
    char kind[SPEC_KIND_MAX], detail[SPEC_DETAIL_MAX];
    if (!midi_parse_spec(spec ? spec : "none", kind, sizeof kind,
                         detail, sizeof detail)) {
        return 0;       /* a spec this build cannot open is not a recording */
    }
    /* Over the same list midi_parse_spec walked.  A recording is the one kind
     * that has an end; alsa is a wire, and none and stdout are not read. */
    return !strcasecmp(kind, "file");
}

/* --------------------------------------------------------------------------
 * The transcript text
 * ------------------------------------------------------------------------ */

/* One message per line, hex, upper case, space separated.  A leading '#' is a
 * comment, so a transcript can carry its own provenance. */
static void write_message(struct midi *m, const uint8_t *bytes, int len) {
    if (!m->wrote_anything) {
        fprintf(m->out, "# tracker-fire transcript: one MIDI message per line, hex.  "
                        "Replayable with --in file:THISFILE\n");
        m->wrote_anything = 1;
    }
    for (int i = 0; i < len; i++) {
        fprintf(m->out, i ? " %02X" : "%02X", bytes[i]);
    }
    fputc('\n', m->out);
    /* Flush per message: a killed run leaves a complete transcript. */
    fflush(m->out);
}

/* Parse one transcript line into bytes.  Returns the length, 0 for a line that
 * carries no message, or -1 if it is neither. */
static int parse_hex_line(const char *line, uint8_t *out, int max) {
    int n = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (*p == '#') return n;             /* comment to end of line */
        int hi, lo;
        char c = *p++;
        if      (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        else return -1;
        c = *p++;
        if      (c >= '0' && c <= '9') lo = c - '0';
        else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
        else return -1;
        if (n >= max) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

/* --------------------------------------------------------------------------
 * Open, close, describe
 * ------------------------------------------------------------------------ */

struct midi *midi_open(const char *spec, int direction) {
    char kind[SPEC_KIND_MAX], detail[SPEC_DETAIL_MAX];
    if (!midi_parse_spec(spec ? spec : "none", kind, sizeof kind,
                         detail, sizeof detail)) {
        return NULL;
    }

    struct midi *m = calloc(1, sizeof *m);
    if (!m) {
        midi_set_error("out of memory");
        return NULL;
    }
    /* calloc leaves fd at 0, a valid descriptor, which would make a file/stdout
     * transport write to stdin.  Only the raw transport sets a real one. */
    m->fd = -1;
    snprintf(m->kind, sizeof m->kind, "%s", kind);
    snprintf(m->detail, sizeof m->detail, "%s", detail);
    m->direction = direction;

    if (!strcasecmp(kind, "alsa")) {
#ifdef HAVE_ALSA
        if (!midi_alsa_open(m)) {
            free(m);
            return NULL;                    /* midi_alsa_open set the reason */
        }
        return m;
#else
        free(m);
        return NULL;                        /* midi_parse_spec set the reason */
#endif
    }

    if (!strcasecmp(kind, "raw")) {
        char path[SPEC_DETAIL_MAX];
        snprintf(path, sizeof path, "%s", detail);
        if (!path[0]) {
            glob_t g;
            if (glob("/dev/snd/midi*", 0, NULL, &g) == 0 && g.gl_pathc > 0) {
                snprintf(path, sizeof path, "%s", g.gl_pathv[0]);
                globfree(&g);
            } else {
                midi_set_error("raw: no /dev/snd/midi* endpoint; plug the Fire in, "
                               "or say raw:/dev/snd/midiC?D?");
                free(m);
                return NULL;
            }
            snprintf(m->detail, sizeof m->detail, "%s", path);
        }
        m->fd = open(path, O_RDWR | O_NONBLOCK);
        if (m->fd < 0) {
            midi_set_error("raw:%s: %s", path, strerror(errno));
            free(m);
            return NULL;
        }
        return m;
    }

    if (!strcasecmp(kind, "file")) {
        /* "w" only for a transport being sent to.  A spec names a path but not a
         * direction, so opening the same path both ways would truncate it. */
        if (direction & MIDI_SEND) {
            m->out = fopen(detail, "w");
            if (!m->out) {
                midi_set_error("file:%s: %s", detail, strerror(errno));
                free(m);
                return NULL;
            }
        }
        if ((direction & MIDI_RECEIVE) && !(direction & MIDI_SEND)) {
            m->in = fopen(detail, "r");
            if (!m->in) {
                midi_set_error("in file:%s: %s", detail, strerror(errno));
                free(m);
                return NULL;
            }
        }
    } else if (!strcasecmp(kind, "stdout")) {
        if (!(direction & MIDI_SEND)) {
            midi_set_error("stdout: is an output; there is nothing to read from "
                           "it.  A fixture is --in file:PATH.");
            free(m);
            return NULL;
        }
        m->out = stdout;
    }
    return m;
}

void midi_close(struct midi *m) {
    if (!m) return;
    if (!strcasecmp(m->kind, "alsa")) {
#ifdef HAVE_ALSA
        midi_alsa_close(m);
#endif
        free(m);
        return;
    }
    if (m->fd >= 0) close(m->fd);
    if (m->out && m->out != stdout) fclose(m->out);
    if (m->in) fclose(m->in);
    free(m);
}

void midi_describe(struct midi *m, struct midi_info *info) {
    memset(info, 0, sizeof *info);
    if (!m) {
        info->kind = "none";
        info->detail = "";
        return;
    }
    if (!strcasecmp(m->kind, "alsa")) {
#ifdef HAVE_ALSA
        midi_alsa_describe(m, info);
#else
        info->kind = "alsa";
        info->detail = m->detail;
#endif
        return;
    }
    info->kind = m->kind;
    info->detail = m->detail;
    info->writable = m->out != NULL || m->fd >= 0;
    info->readable = m->in != NULL || m->fd >= 0;
}

int midi_fd(struct midi *m) {
    if (!m) return -1;
    if (!strcasecmp(m->kind, "alsa")) {
#ifdef HAVE_ALSA
        return midi_alsa_fd(m);
#else
        return -1;
#endif
    }
    if (m->fd >= 0) return m->fd;
    if (m->in) return fileno(m->in);
    return -1;
}

int midi_send(struct midi *m, const uint8_t *bytes, int len) {
    if (!m || len <= 0) return 0;
    if (!strcasecmp(m->kind, "alsa")) {
#ifdef HAVE_ALSA
        return midi_alsa_send(m, bytes, len);
#else
        return 0;
#endif
    }
    if (m->fd >= 0) {
        const ssize_t n = write(m->fd, bytes, (size_t)len);
        return n == len ? 1 : 0;
    }
    if (!m->out) return 0;
    write_message(m, bytes, len);
    return 1;
}

int midi_take_input(struct midi *m,
                    void (*cb)(const uint8_t *, int, void *), void *user) {
    if (!m) return 0;
    if (!strcasecmp(m->kind, "alsa")) {
#ifdef HAVE_ALSA
        return midi_alsa_take_input(m, cb, user);
#else
        (void)cb;
        (void)user;
        return 0;
#endif
    }
    if (m->fd >= 0) {
        uint8_t buf[256];
        const ssize_t n = read(m->fd, buf, sizeof buf);
        if (n <= 0) return 0;
        int delivered = 0;
        for (ssize_t i = 0; i < n; i++) {
            if (m->rxlen < (int)sizeof m->rx) m->rx[m->rxlen++] = buf[i];
            for (;;) {
                if (m->rxlen < 1) break;
                const uint8_t st = m->rx[0];
                int need = 0;
                if (st == 0xF0) {
                    /* SysEx: only F7 ends it; the Fire's input is not SysEx,
                     * so a stuck one is dropped rather than held forever. */
                    int end = -1;
                    for (int k = 1; k < m->rxlen; k++)
                        if (m->rx[k] == 0xF7) { end = k; break; }
                    if (end < 0) break;
                    cb(m->rx, end + 1, user);
                    memmove(m->rx, m->rx + end + 1, (size_t)(m->rxlen - end - 1));
                    m->rxlen -= end + 1;
                    delivered++;
                    continue;
                }
                if ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) need = 2;
                else if ((st & 0x80) == 0x80) need = 3;
                else { /* not a status byte: drop it */ 
                    memmove(m->rx, m->rx + 1, (size_t)(m->rxlen - 1));
                    m->rxlen--;
                    continue;
                }
                if (m->rxlen < need) break;
                cb(m->rx, need, user);
                memmove(m->rx, m->rx + need, (size_t)(m->rxlen - need));
                m->rxlen -= need;
                delivered++;
            }
        }
        return delivered;
    }
    (void)cb;
    (void)user;
    return 0;   /* a file transport replays through midi_replay, not here */
}

int midi_replay(struct midi *m,
                void (*cb)(const uint8_t *, int, void *), void *user) {
    if (!m || m->in_done) return 0;
    if (strcasecmp(m->kind, "file") || !(m->direction & MIDI_RECEIVE)) {
        midi_set_error("--in needs file:PATH: a live transport is read as it "
                       "arrives, not replayed");
        return -1;
    }
    /* midi_open already opened it, so a missing fixture was reported at
     * startup. */
    if (!m->in) {
        midi_set_error("in file:%s: was opened for sending", m->detail);
        return -1;
    }

    char line[4096];
    uint8_t msg[FIRE_MSG_MAX];
    int count = 0;
    while (fgets(line, sizeof line, m->in)) {
        int n = parse_hex_line(line, msg, sizeof msg);
        if (n < 0) {
            midi_set_error("in %s: not hex: %s", m->detail, line);
            return -1;
        }
        if (n > 0) {
            cb(msg, n, user);
            count++;
        }
    }
    m->in_done = 1;
    return count;
}

int midi_list_ports(void) {
#ifdef HAVE_ALSA
    midi_alsa_list_ports();
#else
    fprintf(stderr, "tracker-fire: no ALSA in this build; raw MIDI endpoints and "
                    "transcripts are still available.\n");
#endif
    glob_t g;
    if (glob("/dev/snd/midi*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++)
            fprintf(stderr, "  raw:%s\n", g.gl_pathv[i]);
        globfree(&g);
        return 0;
    }
    fprintf(stderr, "  no /dev/snd/midi* endpoints\n");
    return 1;
}
