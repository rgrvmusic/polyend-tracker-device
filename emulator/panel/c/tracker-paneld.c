/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* tracker-paneld.c -- the headless Tracker engine and its two doors, in C.
 *
 * One poll() loop owns the sockets, a mutex-protected engine owns the pattern,
 * and two threads run the row clock and the audio block clock.
 *
 * The web face, the Fire client and the text face all speak the panel's
 * vocabulary and do not care what this file is written in.  See ../PROTOCOL.md.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "json.h"

#define VERSION "0.1-c"

#define TRACKS       8
#define STEPS        64
#define INSTRUMENTS  48
#define PADS         48
#define SAMPLE_RATE  44100
#define BLOCK        441
#define WAVE_LEN     2048
#define PEAKS        48
#define MAX_LOG      200
#define MAX_CLIENTS  32
#define IN_MAX       65536
#define OUT_MAX      65536

#define DEFAULT_HTTP 8894
#define DEFAULT_LINE 8895
#define PAGE_FILE    "../web/index.html"

static const char *NOTE_NAMES[12] = {
    "C-", "C#", "D-", "D#", "E-", "F-",
    "F#", "G-", "G#", "A-", "A#", "B-"
};
static const char *FIELDS[4] = { "note", "instr", "vol", "fx" };
static const char *SCREENS[8] = {
    "pattern", "song", "instrument", "sample",
    "master", "performance", "file", "config"
};
static const char *WAVES[8] = {
    "sine", "triangle", "saw", "square", "noise", "click", "bass", "pluck"
};

/* -- small helpers -------------------------------------------------------- */

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double wall_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void note_name(int n, char out[4]) {
    if (n < 0 || n > 119) {
        memcpy(out, "---", 4);
        return;
    }
    out[0] = NOTE_NAMES[n % 12][0];
    out[1] = NOTE_NAMES[n % 12][1];
    out[2] = (char)('0' + n / 12);
    out[3] = 0;
}

/* A growing JSON/text buffer.  Nothing here is a general string library; it
   only has to make one complete message or fail cleanly. */
typedef struct {
    char *p;
    size_t len, cap;
    int err;
} Buf;

static void buf_init(Buf *b) {
    b->p = NULL;
    b->len = b->cap = 0;
    b->err = 0;
}

static void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static int buf_need(Buf *b, size_t extra) {
    if (b->err) return 0;
    if (b->len + extra + 1 <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) {
        b->err = 1;
        return 0;
    }
    b->p = p;
    b->cap = cap;
    return 1;
}

static void buf_putn(Buf *b, const char *s, size_t n) {
    if (!buf_need(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void buf_puts(Buf *b, const char *s) {
    buf_putn(b, s, strlen(s));
}

static void buf_putc(Buf *b, char c) {
    buf_putn(b, &c, 1);
}

static void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0) {
        b->err = 1;
        va_end(ap);
        return;
    }
    if (!buf_need(b, (size_t)n)) {
        va_end(ap);
        return;
    }
    vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

/* A JSON string.  The engine's own strings are ASCII, but a log line can carry
   anything a request put there, so escape rather than trust. */
static void buf_str(Buf *b, const char *s) {
    buf_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
            else buf_putc(b, (char)*p);
        }
    }
    buf_putc(b, '"');
}

static const char *buf_text(Buf *b) {
    return b->p ? b->p : "";
}

/* -- the engine ----------------------------------------------------------- */

typedef struct {
    int has_note, note;
    int instr, vol;
    int has_fx, fx;
} Cell;

typedef struct {
    int slot;
    char name[16];
    const char *wave;
    int volume, tune, filter;
    int length;
    int peaks[PEAKS];
    float *data;
} Instrument;

typedef struct {
    int active;
    int track, note, instr;
    const float *data;
    int len;
    double pos, inc;
    float amp;
    double gate_until;
} Voice;

typedef struct {
    double ts;
    char text[160];
} LogLine;

typedef struct {
    pthread_mutex_t lock;

    int running;
    char screen[24];
    int row, track, field;
    int edit_step;
    int shift;
    int selected_instrument;
    int master;
    int pads_root, pad_page;
    char pad_mode[16];

    Cell pattern[STEPS][TRACKS];
    char pattern_name[32];
    int song[4];
    int song_row;
    int tempo;
    int playing, rec, play_row;
    double next_row_at;

    int track_mute[TRACKS];
    int track_vol[TRACKS];

    Instrument instruments[INSTRUMENTS];
    Voice voices[TRACKS];

    Cell copy_cell;
    int has_copy;

    LogLine log[MAX_LOG];
    int log_n;

    char audio_spec[128];
    char audio_error[160];
    long long audio_frames;
    int sink_kind;              /* 0 none, 1 cmd, 2 wav */
    FILE *sink;
    long long wav_bytes;

    int exit;
} Engine;

static Engine E;
static volatile sig_atomic_t stop_flag;

static int screen_index(const char *name) {
    for (int i = 0; i < 8; i++)
        if (!strcmp(SCREENS[i], name)) return i;
    return -1;
}

static void log_locked(const char *fmt, ...) {
    va_list ap;
    if (E.log_n == MAX_LOG) {
        memmove(&E.log[0], &E.log[1], sizeof E.log[0] * (MAX_LOG - 1));
        E.log_n--;
    }
    LogLine *l = &E.log[E.log_n++];
    l->ts = wall_now();
    va_start(ap, fmt);
    vsnprintf(l->text, sizeof l->text, fmt, ap);
    va_end(ap);
}

static int row_has_note_locked(int row, int note) {
    for (int t = 0; t < TRACKS; t++)
        if (E.pattern[row % STEPS][t].has_note &&
            E.pattern[row % STEPS][t].note == note)
            return 1;
    return 0;
}

static void make_instruments(void) {
    srand(12345);
    for (int i = 0; i < INSTRUMENTS; i++) {
        Instrument *in = &E.instruments[i];
        memset(in, 0, sizeof *in);
        in->slot = i + 1;
        in->volume = 0;
        if (i >= 8) {
            snprintf(in->name, sizeof in->name, "EMPTY");
            continue;
        }
        in->wave = WAVES[i];
        snprintf(in->name, sizeof in->name, "%s", WAVES[i]);
        for (char *p = in->name; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
        in->volume = 100;
        in->length = WAVE_LEN;
        in->data = malloc(sizeof(float) * WAVE_LEN);
        if (!in->data) continue;

        for (int n = 0; n < WAVE_LEN; n++) {
            double x = (double)n / WAVE_LEN;
            double y;
            if (i == 0) y = sin(2 * M_PI * x);
            else if (i == 1) y = 2.0 / M_PI * asin(sin(2 * M_PI * x));
            else if (i == 2) y = 2.0 * (x - floor(x + 0.5));
            else if (i == 3) y = sin(2 * M_PI * x) >= 0 ? 1.0 : -1.0;
            else if (i == 4) y = (double)rand() / RAND_MAX * 2.0 - 1.0;
            else if (i == 5) {
                if (n < 32) {
                    double w = 0.5 - 0.5 * cos(2 * M_PI * n / 31.0);
                    y = w * 0.9;
                } else y = 0.0;
            } else if (i == 6) {
                y = sin(2 * M_PI * x) + 0.45 * sin(4 * M_PI * x);
            } else {
                y = (sin(2 * M_PI * x) + 0.5 * sin(4 * M_PI * x)) *
                    exp(-3.0 * x);
            }
            in->data[n] = (float)(y * 0.85);
        }
        int step = WAVE_LEN / PEAKS;
        for (int p = 0; p < PEAKS; p++) {
            float peak = 0;
            for (int n = p * step; n < (p + 1) * step && n < WAVE_LEN; n++) {
                float a = fabsf(in->data[n]);
                if (a > peak) peak = a;
            }
            in->peaks[p] = (int)(peak * 99.0f);
        }
    }
}

static float *sample_for_locked(int slot) {
    int idx = slot - 1;
    if (idx < 0 || idx >= INSTRUMENTS) return NULL;
    return E.instruments[idx].wave ? E.instruments[idx].data : NULL;
}

static double row_seconds_locked(void) {
    return 60.0 / (E.tempo < 20 ? 20 : E.tempo) / 4.0;
}

static void trigger_row_locked(int row) {
    double now = mono_now();
    double dur = row_seconds_locked();
    for (int t = 0; t < TRACKS; t++) {
        Cell *c = &E.pattern[row % STEPS][t];
        if (!c->has_note || E.track_mute[t]) continue;
        float *data = sample_for_locked(c->instr);
        if (!data) continue;
        Voice *v = &E.voices[t];
        v->active = 1;
        v->track = t;
        v->note = c->note;
        v->instr = c->instr;
        v->data = data;
        v->len = WAVE_LEN;
        v->pos = 0;
        v->inc = 440.0 * pow(2.0, (c->note - 69) / 12.0) / SAMPLE_RATE *
                 v->len;
        v->amp = (c->vol / 64.0f) * (E.track_vol[t] / 100.0f) *
                 (E.master / 100.0f);
        v->gate_until = now + dur * 0.92;
    }
}

static void preview_locked(int note, int vel) {
    Voice *v = &E.voices[E.track];
    float *data = sample_for_locked(E.selected_instrument);
    if (!data) return;
    v->active = 1;
    v->track = E.track;
    v->note = note;
    v->instr = E.selected_instrument;
    v->data = data;
    v->len = WAVE_LEN;
    v->pos = 0;
    v->inc = 440.0 * pow(2.0, (note - 69) / 12.0) / SAMPLE_RATE * v->len;
    v->amp = (vel / 127.0f) * (E.master / 100.0f);
    v->gate_until = mono_now() + 0.4;
}

static void clear_cell_locked(void) {
    Cell *c = &E.pattern[E.row][E.track];
    c->has_note = 0;
    c->note = -1;
    c->instr = 0;
    c->vol = 64;
    c->has_fx = 0;
    c->fx = 0;
}

static void advance_row_locked(void) {
    E.row = (E.row + E.edit_step) % STEPS;
    if (E.row == 0) log_locked("pattern wrapped to 00");
}

static void pad_note_locked(int pad, int *note) {
    *note = E.pads_root + 12 * E.pad_page + pad;
}

static void pad_press_locked(int pad, int vel) {
    int note;
    pad_note_locked(pad, &note);
    if (!strcmp(E.screen, "pattern") && E.field == 0) {
        if (E.shift) {
            clear_cell_locked();
        } else {
            Cell *c = &E.pattern[E.row][E.track];
            c->has_note = 1;
            c->note = note;
            c->instr = E.selected_instrument;
            c->vol = 64;
            c->has_fx = 0;
            c->fx = 0;
            advance_row_locked();
        }
        return;
    }
    preview_locked(note, vel);
}

static void move_locked(int dr, int dt) {
    E.row = clamp(E.row + dr, 0, STEPS - 1);
    E.track = clamp(E.track + dt, 0, TRACKS - 1);
}

static void insert_row_locked(void) {
    for (int i = STEPS - 1; i > E.row; i--)
        memcpy(E.pattern[i], E.pattern[i - 1], sizeof E.pattern[i]);
    for (int t = 0; t < TRACKS; t++)
        E.pattern[E.row][t] = (Cell){ 0, -1, 0, 64, 0, 0 };
    log_locked("inserted row %02d", E.row);
}

static void delete_row_locked(void) {
    for (int i = E.row; i < STEPS - 1; i++)
        memcpy(E.pattern[i], E.pattern[i + 1], sizeof E.pattern[i]);
    for (int t = 0; t < TRACKS; t++)
        E.pattern[STEPS - 1][t] = (Cell){ 0, -1, 0, 64, 0, 0 };
    log_locked("deleted row %02d", E.row);
}

static void paste_cell_locked(void) {
    if (E.has_copy) E.pattern[E.row][E.track] = E.copy_cell;
}

static void toggle_play_locked(void) {
    E.playing = !E.playing;
    if (E.playing) {
        E.play_row = !strcmp(E.screen, "pattern") ? E.row : 0;
        E.next_row_at = mono_now();
        log_locked("play from row %02d", E.play_row);
    } else {
        log_locked("stop");
    }
}

static void stop_locked(void) {
    E.playing = 0;
    for (int t = 0; t < TRACKS; t++)
        E.voices[t].gate_until = mono_now();
}

static void toggle_rec_locked(void) {
    E.rec = !E.rec;
    log_locked("rec %s", E.rec ? "on" : "off");
}

static int screen_for_button(int bid) {
    switch (bid) {
    case 8: case 19: return 2;      /* instrument */
    case 9: return 5;               /* performance */
    case 10: return 6;              /* file */
    case 11: return 7;              /* config */
    case 12: return 4;              /* master */
    case 13: case 14: case 15: case 16: return 3; /* sample */
    case 17: return 1;              /* song */
    case 22: return 0;              /* pattern */
    default: return -1;
    }
}

static void button_press_locked(int bid) {
    switch (bid) {
    case 29: E.shift = 1; return;
    case 23: toggle_play_locked(); return;
    case 28: toggle_rec_locked(); return;
    case 26: move_locked(-1, 0); return;
    case 31: move_locked(1, 0); return;
    case 30: move_locked(0, -1); return;
    case 32: move_locked(0, 1); return;
    case 25: insert_row_locked(); return;
    case 27:
        if (E.shift) delete_row_locked();
        else clear_cell_locked();
        return;
    case 24:
        if (E.shift) {
            paste_cell_locked();
        } else {
            E.copy_cell = E.pattern[E.row][E.track];
            E.has_copy = 1;
            log_locked("copied %02d:%d", E.row, E.track + 1);
        }
        return;
    case 33:
        if (E.has_copy) paste_cell_locked();
        else clear_cell_locked();
        return;
    case 34:
        snprintf(E.screen, sizeof E.screen, "pattern");
        return;
    case 18: E.field = 0; return;
    case 19: E.field = 1; return;
    case 20: E.field = 2; return;
    case 21: E.field = 3; return;
    default: break;
    }
    int scr = screen_for_button(bid);
    if (scr >= 0) {
        snprintf(E.screen, sizeof E.screen, "%s", SCREENS[scr]);
        if (scr == 3 && bid == 14) preview_locked(E.pads_root + 12 * E.pad_page, 100);
        if (scr == 1) log_locked("song screen");
        return;
    }
    if (bid == 9) snprintf(E.screen, sizeof E.screen, "performance");
    else if (bid == 12) snprintf(E.screen, sizeof E.screen, "master");
    else if (bid == 11) snprintf(E.screen, sizeof E.screen, "config");
    else if (bid == 10) snprintf(E.screen, sizeof E.screen, "file");
    else if (bid == 8) snprintf(E.screen, sizeof E.screen, "instrument");
}

static void button_release_locked(int bid) {
    if (bid == 29) E.shift = 0;
}

static void encoder_locked(int delta) {
    int step = E.shift ? 12 : 1;
    Cell *c = &E.pattern[E.row][E.track];
    if (!strcmp(E.screen, "pattern")) {
        if (E.field == 0) {
            int base = c->has_note ? c->note : 60;
            c->note = clamp(base + delta * step, 0, 119);
            c->has_note = 1;
        } else if (E.field == 1) {
            c->instr = clamp(c->instr + delta, 0, INSTRUMENTS - 1);
            E.selected_instrument = c->instr;
        } else if (E.field == 2) {
            c->vol = clamp(c->vol + delta, 0, 64);
        } else {
            c->fx = clamp(c->fx + delta, 0, 127);
            c->has_fx = c->fx != 0;
        }
    } else if (!strcmp(E.screen, "master")) {
        E.master = clamp(E.master + delta, 0, 100);
    } else if (!strcmp(E.screen, "instrument")) {
        Instrument *in = &E.instruments[E.selected_instrument - 1];
        if (E.field == 2) in->volume = clamp(in->volume + delta, 0, 100);
        else if (E.field == 3) in->filter = clamp(in->filter + delta, -64, 63);
        else in->tune = clamp(in->tune + delta, -24, 24);
    } else if (!strcmp(E.screen, "performance")) {
        E.track_vol[E.track] = clamp(E.track_vol[E.track] + delta, 0, 100);
    } else if (!strcmp(E.screen, "config")) {
        E.tempo = clamp(E.tempo + delta, 20, 300);
    }
}

/* -- requests ------------------------------------------------------------- */

static int jint(const struct json *o, const char *key, int dflt) {
    return json_int(o, key, dflt);
}

static void apply_request_locked(const struct json *o) {
    const char *t = json_str(o, "t", "");
    if (!strcmp(t, "input")) {
        const char *kind = json_str(o, "kind", "");
        if (!strcmp(kind, "pad")) {
            int pad = jint(o, "id", 0);
            if (jint(o, "state", 1)) pad_press_locked(pad, jint(o, "vel", 100));
        } else if (!strcmp(kind, "button")) {
            int bid = jint(o, "id", 0);
            if (jint(o, "state", 1)) button_press_locked(bid);
            else button_release_locked(bid);
        } else if (!strcmp(kind, "encoder")) {
            encoder_locked(jint(o, "delta", 0));
        } else if (!strcmp(kind, "note")) {
            if (jint(o, "on", 1))
                preview_locked(jint(o, "note", 60), jint(o, "vel", 100));
        }
    } else if (!strcmp(t, "transport")) {
        const char *a = json_str(o, "action", "");
        if (!strcmp(a, "play")) {
            if (!E.playing) toggle_play_locked();
        } else if (!strcmp(a, "stop")) stop_locked();
        else if (!strcmp(a, "toggle")) toggle_play_locked();
        else if (!strcmp(a, "rec")) toggle_rec_locked();
    } else if (!strcmp(t, "screen")) {
        const char *n = json_str(o, "name", "");
        int idx = screen_index(n);
        if (idx >= 0) snprintf(E.screen, sizeof E.screen, "%s", n);
    } else if (!strcmp(t, "focus")) {
        E.field = clamp(jint(o, "field", 0), 0, 3);
    } else if (!strcmp(t, "set_cursor")) {
        E.row = clamp(jint(o, "row", E.row), 0, STEPS - 1);
        E.track = clamp(jint(o, "track", E.track), 0, TRACKS - 1);
    } else if (!strcmp(t, "set")) {
        const char *p = json_str(o, "path", "");
        int v = jint(o, "value", 0);
        if (!strcmp(p, "tempo")) E.tempo = clamp(v, 20, 300);
        else if (!strcmp(p, "master")) E.master = clamp(v, 0, 100);
        else if (!strcmp(p, "selected_instrument"))
            E.selected_instrument = clamp(v, 1, INSTRUMENTS);
        else if (!strcmp(p, "edit_step")) E.edit_step = clamp(v, 0, 32);
        else if (!strcmp(p, "pads_root")) E.pads_root = clamp(v, 0, 108);
        else if (!strcmp(p, "track_mute")) {
            int tr = clamp(jint(o, "track", 0), 0, TRACKS - 1);
            E.track_mute[tr] = v != 0;
        } else if (!strcmp(p, "track_vol")) {
            int tr = clamp(jint(o, "track", 0), 0, TRACKS - 1);
            E.track_vol[tr] = clamp(v, 0, 100);
        } else if (!strcmp(p, "song_row")) {
            E.song_row = clamp(v, 0, 3);
        }
    } else if (!strcmp(t, "cell")) {
        int row = clamp(jint(o, "row", 0), 0, STEPS - 1);
        int trk = clamp(jint(o, "track", 0), 0, TRACKS - 1);
        Cell *c = &E.pattern[row][trk];
        const struct json *v;
        if ((v = json_get(o, "note")) != NULL) {
            if (v->type == JSON_NULL) {
                c->has_note = 0;
                c->note = -1;
            } else {
                c->has_note = 1;
                c->note = clamp(jint(o, "note", 60), 0, 119);
            }
        }
        if (json_get(o, "instr"))
            c->instr = clamp(jint(o, "instr", 0), 0, INSTRUMENTS - 1);
        if (json_get(o, "vol"))
            c->vol = clamp(jint(o, "vol", 64), 0, 64);
        if ((v = json_get(o, "fx")) != NULL) {
            if (v->type == JSON_NULL) {
                c->has_fx = 0;
                c->fx = 0;
            } else {
                c->fx = clamp(jint(o, "fx", 0), 0, 127);
                c->has_fx = c->fx != 0;
            }
        }
    } else if (!strcmp(t, "mark")) {
        log_locked("%s", json_str(o, "text", ""));
    }
}

/* -- JSON snapshots ------------------------------------------------------- */

static const char *pad_color_locked(int i, int note) {
    if (E.playing && row_has_note_locked(E.play_row, note)) return "#ffb000";
    if (i == E.row * 12 + E.track) return "#f4f4f4";
    if (note % 12 == 0) return "#2f8f4e";
    if (note % 12 == 3 || note % 12 == 5 || note % 12 == 8 ||
        note % 12 == 10)
        return "#23352a";
    return "#111611";
}

static void build_leds_locked(Buf *b) {
    int base = E.pads_root + 12 * E.pad_page;
    buf_puts(b, "{\"pads\":[");
    for (int i = 0; i < PADS; i++) {
        if (i) buf_putc(b, ',');
        buf_str(b, pad_color_locked(i, base + i));
    }
    buf_puts(b, "],\"buttons\":{");
    int cur = screen_index(E.screen);
    int bid = -1;
    static const int scr_bid[8] = { 22, 17, 8, 13, 12, 9, 10, 11 };
    if (cur >= 0) bid = scr_bid[cur];
    int first = 1;
    if (bid > 0) {
        buf_printf(b, "\"%d\":\"#ffb000\"", bid);
        first = 0;
    }
    if (!first) buf_putc(b, ',');
    buf_puts(b, "\"23\":");
    buf_str(b, E.playing ? "#2f8f4e" : "#111111");
    buf_puts(b, ",\"28\":");
    buf_str(b, E.rec ? "#b23b2e" : "#111111");
    buf_puts(b, "}}");
}

static void cell_json_locked(Buf *b, const Cell *c) {
    buf_putc(b, '[');
    if (c->has_note) buf_printf(b, "%d", c->note);
    else buf_puts(b, "null");
    buf_printf(b, ",%d,%d,", c->instr, c->vol);
    if (c->has_fx) buf_printf(b, "%d", c->fx);
    else buf_puts(b, "null");
    buf_putc(b, ']');
}

/* The whole state.  The lock is held for the walk and released by the caller;
   this is the one place the three faces agree on what the panel is. */
static void build_state_locked(Buf *b) {
    char host[128] = "";
    gethostname(host, sizeof host);

    buf_puts(b, "{\"version\":");
    buf_str(b, VERSION);
    buf_printf(b, ",\"ts\":%.6f,\"host\":", wall_now());
    buf_str(b, host);
    buf_puts(b, ",\"screen\":");
    buf_str(b, E.screen);
    buf_puts(b, ",\"screens\":[");
    for (int i = 0; i < 8; i++) {
        if (i) buf_putc(b, ',');
        buf_str(b, SCREENS[i]);
    }
    buf_printf(b, "],\"cursor\":{\"row\":%d,\"track\":%d,\"field\":%d}",
               E.row, E.track, E.field);
    buf_puts(b, ",\"field\":");
    buf_str(b, FIELDS[E.field]);
    buf_printf(b, ",\"edit_step\":%d,\"shift\":%s,\"tempo\":%d,\"master\":%d",
               E.edit_step, E.shift ? "true" : "false", E.tempo, E.master);
    buf_printf(b, ",\"playing\":%s,\"rec\":%s,\"play_row\":%d",
               E.playing ? "true" : "false", E.rec ? "true" : "false",
               E.play_row);
    buf_printf(b, ",\"song_row\":%d,\"pattern_name\":", E.song_row);
    buf_str(b, E.pattern_name);
    buf_printf(b, ",\"steps\":%d,\"tracks\":%d,\"patterns\":1,\"song\":[%d,%d,%d,%d]",
               STEPS, TRACKS, E.song[0], E.song[1], E.song[2], E.song[3]);

    buf_puts(b, ",\"pattern\":[");
    for (int r = 0; r < STEPS; r++) {
        if (r) buf_putc(b, ',');
        buf_putc(b, '[');
        for (int t = 0; t < TRACKS; t++) {
            if (t) buf_putc(b, ',');
            cell_json_locked(b, &E.pattern[r][t]);
        }
        buf_putc(b, ']');
    }
    buf_putc(b, ']');

    buf_puts(b, ",\"track_mute\":[");
    for (int t = 0; t < TRACKS; t++) {
        if (t) buf_putc(b, ',');
        buf_puts(b, E.track_mute[t] ? "true" : "false");
    }
    buf_puts(b, "],\"track_vol\":[");
    for (int t = 0; t < TRACKS; t++) {
        if (t) buf_putc(b, ',');
        buf_printf(b, "%d", E.track_vol[t]);
    }
    buf_printf(b, "],\"selected_instrument\":%d,\"instruments\":[",
               E.selected_instrument);
    for (int i = 0; i < INSTRUMENTS; i++) {
        Instrument *in = &E.instruments[i];
        if (i) buf_putc(b, ',');
        buf_printf(b, "{\"slot\":%d,\"name\":", in->slot);
        buf_str(b, in->name);
        buf_puts(b, ",\"wave\":");
        if (in->wave) buf_str(b, in->wave);
        else buf_puts(b, "null");
        buf_printf(b, ",\"volume\":%d,\"tune\":%d,\"filter\":%d,\"length\":%d,\"peaks\":[",
                   in->volume, in->tune, in->filter, in->length);
        for (int p = 0; p < PEAKS; p++) {
            if (p) buf_putc(b, ',');
            buf_printf(b, "%d", in->peaks[p]);
        }
        buf_puts(b, "]}");
    }
    buf_printf(b, "],\"pads\":{\"root\":%d,\"page\":%d,\"mode\":",
               E.pads_root, E.pad_page);
    buf_str(b, E.pad_mode);
    buf_printf(b, ",\"count\":%d,\"first_note\":%d},\"audio\":{\"sink\":",
               PADS, E.pads_root + 12 * E.pad_page);
    buf_str(b, E.audio_spec);
    buf_puts(b, ",\"error\":");
    if (E.audio_error[0]) buf_str(b, E.audio_error);
    else buf_puts(b, "null");
    buf_printf(b, ",\"frames\":%lld},\"leds\":", E.audio_frames);
    build_leds_locked(b);
    buf_puts(b, ",\"log\":[");
    int start = E.log_n > 20 ? E.log_n - 20 : 0;
    for (int i = start; i < E.log_n; i++) {
        if (i > start) buf_putc(b, ',');
        buf_printf(b, "{\"ts\":%.6f,\"text\":", E.log[i].ts);
        buf_str(b, E.log[i].text);
        buf_putc(b, '}');
    }
    buf_puts(b, "],\"cursor_cell\":");
    cell_json_locked(b, &E.pattern[E.row][E.track]);
    buf_putc(b, '}');
}

static void build_fire_locked(Buf *b) {
    buf_printf(b, "{\"t\":\"fire\",\"screen\":");
    buf_str(b, E.screen);
    buf_printf(b, ",\"playing\":%d,\"rec\":%d,\"row\":%d,\"play_row\":%d,"
               "\"track\":%d,\"field\":", E.playing, E.rec, E.row, E.play_row,
               E.track);
    buf_str(b, FIELDS[E.field]);
    buf_printf(b, ",\"bpm\":%d,\"steps\":%d,\"pat\":", E.tempo, STEPS);
    buf_str(b, E.pattern_name);
    buf_puts(b, ",\"cells\":[");
    for (int t = 0; t < TRACKS; t++) {
        Cell *c = &E.pattern[E.row][t];
        char nn[4];
        note_name(c->has_note ? c->note : -1, nn);
        char tmp[32];
        if (c->has_fx) snprintf(tmp, sizeof tmp, "%s %02d %02d %02d", nn,
                                c->instr, c->vol, c->fx);
        else snprintf(tmp, sizeof tmp, "%s %02d %02d --", nn, c->instr, c->vol);
        if (t) buf_putc(b, ',');
        buf_str(b, tmp);
    }
    buf_puts(b, "],\"around\":[");
    for (int k = 0; k < 4; k++) {
        int r = (E.row - 1 + k + STEPS) % STEPS;
        if (k) buf_putc(b, ',');
        buf_printf(b, "{\"r\":%d,\"notes\":[", r);
        for (int t = 0; t < TRACKS; t++) {
            char nn[4];
            note_name(E.pattern[r][t].has_note ? E.pattern[r][t].note : -1, nn);
            if (t) buf_putc(b, ',');
            buf_str(b, nn);
        }
        buf_puts(b, "]}");
    }
    buf_puts(b, "],\"mutes\":\"");
    for (int t = 0; t < TRACKS; t++) buf_putc(b, E.track_mute[t] ? '1' : '0');
    buf_puts(b, "\",\"padleds\":[");
    int base = E.pads_root + 12 * E.pad_page;
    for (int i = 0; i < PADS; i++) {
        if (i) buf_putc(b, ',');
        buf_str(b, pad_color_locked(i, base + i));
    }
    buf_puts(b, "],\"btnleds\":{");
    int cur = screen_index(E.screen);
    static const int scr_bid[8] = { 22, 17, 8, 13, 12, 9, 10, 11 };
    int first = 1;
    if (cur >= 0) {
        buf_printf(b, "\"%d\":\"#ffb000\"", scr_bid[cur]);
        first = 0;
    }
    if (!first) buf_putc(b, ',');
    buf_puts(b, "\"23\":");
    buf_str(b, E.playing ? "#2f8f4e" : "#111111");
    buf_puts(b, ",\"28\":");
    buf_str(b, E.rec ? "#b23b2e" : "#111111");
    buf_printf(b, "},\"master\":%d,\"shift\":%d}", E.master, E.shift);
}

/* -- audio ---------------------------------------------------------------- */

static void sink_close_locked(void) {
    if (E.sink) {
        if (E.sink_kind == 1) pclose(E.sink);
        else fclose(E.sink);
        E.sink = NULL;
    }
}

static int sink_open_locked(void) {
    E.sink_kind = 0;
    if (!strcmp(E.audio_spec, "none")) return 1;
    if (!strncmp(E.audio_spec, "cmd:", 4)) {
        E.sink = popen(E.audio_spec + 4, "w");
        if (!E.sink) {
            snprintf(E.audio_error, sizeof E.audio_error, "%s", strerror(errno));
            return 0;
        }
        setvbuf(E.sink, NULL, _IOFBF, 1 << 16);
        E.sink_kind = 1;
        return 1;
    }
    if (!strncmp(E.audio_spec, "wav:", 4)) {
        E.sink = fopen(E.audio_spec + 4, "wb");
        if (!E.sink) {
            snprintf(E.audio_error, sizeof E.audio_error, "%s", strerror(errno));
            return 0;
        }
        unsigned char h[44] = { 0 };
        memcpy(h, "RIFF", 4);
        memcpy(h + 8, "WAVEfmt ", 8);
        h[16] = 16;
        h[20] = 1;
        h[22] = 2;
        h[24] = SAMPLE_RATE & 0xff;
        h[25] = (SAMPLE_RATE >> 8) & 0xff;
        h[26] = (SAMPLE_RATE >> 16) & 0xff;
        h[27] = (SAMPLE_RATE >> 24) & 0xff;
        unsigned rate = SAMPLE_RATE * 4;
        h[28] = rate & 0xff; h[29] = (rate >> 8) & 0xff;
        h[30] = (rate >> 16) & 0xff; h[31] = (rate >> 24) & 0xff;
        h[32] = 4;
        h[34] = 16;
        memcpy(h + 36, "data", 4);
        fwrite(h, 1, 44, E.sink);
        E.wav_bytes = 0;
        E.sink_kind = 2;
        return 1;
    }
    snprintf(E.audio_error, sizeof E.audio_error, "unknown audio sink");
    return 0;
}

static void render_block_locked(int16_t *pcm, int n) {
    float mix[BLOCK];
    for (int i = 0; i < n; i++) mix[i] = 0.0f;
    double now = mono_now();
    float master = E.master / 100.0f;

    for (int t = 0; t < TRACKS; t++) {
        Voice *v = &E.voices[t];
        if (!v->active || !v->data) continue;
        float rel = 1.0f;
        if (now > v->gate_until) {
            rel = 1.0f - (float)((now - v->gate_until) / 0.02);
            if (rel <= 0.0f) {
                v->active = 0;
                v->pos = 0;
                continue;
            }
        }
        for (int i = 0; i < n; i++) {
            int i0 = (int)v->pos;
            double frac = v->pos - i0;
            float s = v->data[i0] * (float)(1.0 - frac) +
                      v->data[(i0 + 1) % v->len] * (float)frac;
            mix[i] += s * v->amp * rel;
            v->pos += v->inc;
            if (v->pos >= v->len) v->pos -= v->len;
        }
    }
    for (int i = 0; i < n; i++) {
        float s = tanhf(mix[i] * master);
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t v = (int16_t)(s * 32767.0f);
        pcm[i * 2] = v;
        pcm[i * 2 + 1] = v;
    }
}

static void *audio_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&E.lock);
    int have_sink = sink_open_locked();
    pthread_mutex_unlock(&E.lock);
    if (!have_sink || E.sink_kind == 0) return NULL;

    double target = mono_now();
    while (E.running) {
        double now = mono_now();
        if (now < target) {
            double d = target - now;
            struct timespec ts = { 0, (long)(d * 1e9) };
            if (ts.tv_nsec > 5000000) ts.tv_nsec = 5000000;
            nanosleep(&ts, NULL);
            continue;
        }
        target += (double)BLOCK / SAMPLE_RATE;

        int16_t pcm[BLOCK * 2];
        pthread_mutex_lock(&E.lock);
        render_block_locked(pcm, BLOCK);
        FILE *f = E.sink;
        int kind = E.sink_kind;
        pthread_mutex_unlock(&E.lock);

        if (!f) break;
        size_t wrote = fwrite(pcm, sizeof(int16_t) * 2, BLOCK, f);
        if (wrote != BLOCK) {
            pthread_mutex_lock(&E.lock);
            snprintf(E.audio_error, sizeof E.audio_error,
                     "audio sink write failed");
            sink_close_locked();
            pthread_mutex_unlock(&E.lock);
            break;
        }
        pthread_mutex_lock(&E.lock);
        E.audio_frames += BLOCK;
        if (kind == 2) E.wav_bytes += (long long)sizeof(int16_t) * 2 * BLOCK;
        pthread_mutex_unlock(&E.lock);
    }
    return NULL;
}

static void *sequence_thread(void *arg) {
    (void)arg;
    while (E.running) {
        pthread_mutex_lock(&E.lock);
        int playing = E.playing;
        double due = E.next_row_at;
        pthread_mutex_unlock(&E.lock);
        if (!playing) {
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        double now = mono_now();
        if (now < due) {
            double d = due - now;
            if (d > 0.01) d = 0.01;
            struct timespec ts = { 0, (long)(d * 1e9) };
            nanosleep(&ts, NULL);
            continue;
        }
        pthread_mutex_lock(&E.lock);
        int row = E.play_row;
        trigger_row_locked(row);
        E.play_row = (row + 1) % STEPS;
        E.next_row_at = due + row_seconds_locked();
        pthread_mutex_unlock(&E.lock);
    }
    return NULL;
}

/* -- offline render ------------------------------------------------------- */

static int wav_header(FILE *f, unsigned data_bytes) {
    unsigned char h[44] = { 0 };
    unsigned rate = SAMPLE_RATE * 4;
    memcpy(h, "RIFF", 4);
    unsigned riff = data_bytes + 36;
    h[4] = riff & 0xff; h[5] = (riff >> 8) & 0xff;
    h[6] = (riff >> 16) & 0xff; h[7] = (riff >> 24) & 0xff;
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16;
    h[20] = 1;
    h[22] = 2;
    h[24] = SAMPLE_RATE & 0xff;
    h[25] = (SAMPLE_RATE >> 8) & 0xff;
    h[26] = (SAMPLE_RATE >> 16) & 0xff;
    h[27] = (SAMPLE_RATE >> 24) & 0xff;
    h[28] = rate & 0xff; h[29] = (rate >> 8) & 0xff;
    h[30] = (rate >> 16) & 0xff; h[31] = (rate >> 24) & 0xff;
    h[32] = 4;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    h[40] = data_bytes & 0xff; h[41] = (data_bytes >> 8) & 0xff;
    h[42] = (data_bytes >> 16) & 0xff; h[43] = (data_bytes >> 24) & 0xff;
    return fwrite(h, 1, 44, f) == 44;
}

static int render_pattern_wav(const char *path) {
    Cell pattern[STEPS][TRACKS];
    int mute[TRACKS], vol[TRACKS];
    float *data[INSTRUMENTS];
    int tempo, master;

    pthread_mutex_lock(&E.lock);
    memcpy(pattern, E.pattern, sizeof pattern);
    memcpy(mute, E.track_mute, sizeof mute);
    memcpy(vol, E.track_vol, sizeof vol);
    for (int i = 0; i < INSTRUMENTS; i++)
        data[i] = E.instruments[i].wave ? E.instruments[i].data : NULL;
    tempo = E.tempo;
    master = E.master;
    pthread_mutex_unlock(&E.lock);

    double rs = 60.0 / (tempo < 20 ? 20 : tempo) / 4.0;
    int gate = (int)(rs * 0.92 * SAMPLE_RATE);
    if (gate < 1) gate = 1;
    long total = (long)(rs * STEPS * SAMPLE_RATE) + gate;
    float *mix = calloc((size_t)total, sizeof(float));
    if (!mix) return 0;

    for (int row = 0; row < STEPS; row++) {
        long at = (long)(row * rs * SAMPLE_RATE);
        for (int t = 0; t < TRACKS; t++) {
            Cell *c = &pattern[row][t];
            if (!c->has_note || mute[t]) continue;
            if (c->instr < 1 || c->instr > INSTRUMENTS) continue;
            float *d = data[c->instr - 1];
            if (!d) continue;
            int len = WAVE_LEN;
            double inc = 440.0 * pow(2.0, (c->note - 69) / 12.0) /
                         SAMPLE_RATE * len;
            long n = gate < total - at ? gate : total - at;
            if (n <= 0) continue;
            double pos = 0;
            int fade = n / 2 < 220 ? (int)(n / 2) : 220;
            double amp = (c->vol / 64.0) * (vol[t] / 100.0) * (master / 100.0);
            for (long i = 0; i < n; i++) {
                int i0 = (int)pos;
                double frac = pos - i0;
                float s = d[i0] * (float)(1 - frac) +
                          d[(i0 + 1) % len] * (float)frac;
                double env = 1.0;
                if (fade > 0) {
                    if (i < fade) env = (double)i / fade;
                    if (i >= n - fade) env = (double)(n - 1 - i) / fade;
                }
                mix[at + i] += (float)(s * env * amp);
                pos += inc;
                if (pos >= len) pos -= len;
            }
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(mix);
        return 0;
    }
    unsigned bytes = (unsigned)(total * 4);
    if (!wav_header(f, bytes)) {
        fclose(f);
        free(mix);
        return 0;
    }
    for (long i = 0; i < total; i++) {
        float s = tanhf(mix[i]) * 0.9f;
        if (s > 1) s = 1;
        if (s < -1) s = -1;
        int16_t v = (int16_t)(s * 32767);
        int16_t pair[2] = { v, v };
        fwrite(pair, sizeof pair, 1, f);
    }
    fclose(f);
    free(mix);
    return 1;
}

/* -- server --------------------------------------------------------------- */

enum { CL_LINE = 0, CL_HTTP, CL_SSE };

typedef struct {
    int fd;
    int kind;
    int role_fire;
    char in[IN_MAX];
    size_t inlen;
    char out[OUT_MAX];
    size_t outlen, outsent;
    int close_after;
    int file_fd;
    size_t file_left;
    char file_buf[16384];
    size_t file_off, file_len;
    double next_fire;
    long long dropped;
    /* HTTP request state */
    int header_done;
    size_t body_off;
    long body_len;
    char method[8];
    char path[512];
} Client;

static Client clients[MAX_CLIENTS];
static int line_fd = -1, http_fd = -1;
static char page_path[1024];

static int client_push(Client *c, const char *data, size_t n) {
    if (c->outlen + n > sizeof c->out) {
        c->dropped++;
        return 0;
    }
    memcpy(c->out + c->outlen, data, n);
    c->outlen += n;
    return 1;
}

static int client_push_line(Client *c, const char *line, size_t n) {
    if (c->outlen + n + 1 > sizeof c->out) {
        c->dropped++;
        return 0;
    }
    memcpy(c->out + c->outlen, line, n);
    c->outlen += n;
    c->out[c->outlen++] = '\n';
    return 1;
}

static void client_drop(Client *c) {
    if (c->fd >= 0) close(c->fd);
    if (c->file_fd >= 0) close(c->file_fd);
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->file_fd = -1;
}

static void push_state(Client *c) {
    Buf b;
    buf_init(&b);
    pthread_mutex_lock(&E.lock);
    build_state_locked(&b);
    pthread_mutex_unlock(&E.lock);
    if (!b.err) client_push_line(c, buf_text(&b), b.len);
    buf_free(&b);
}

static void push_fire(Client *c) {
    Buf b;
    buf_init(&b);
    pthread_mutex_lock(&E.lock);
    build_fire_locked(&b);
    pthread_mutex_unlock(&E.lock);
    if (!b.err) client_push_line(c, buf_text(&b), b.len);
    buf_free(&b);
}

static void send_hello_state(Client *c) {
    Buf b;
    buf_init(&b);
    char host[128] = "";
    gethostname(host, sizeof host);
    buf_printf(&b, "{\"t\":\"hello\",\"version\":\"%s\",\"role\":\"panel\",\"host\":",
               VERSION);
    buf_str(&b, host);
    buf_putc(&b, '}');
    if (!b.err) client_push_line(c, buf_text(&b), b.len);
    buf_free(&b);
    push_state(c);
}

static void line_request(Client *c, const char *line) {
    struct json *j = json_parse(line);
    if (!j) {
        const char *err = "{\"t\":\"error\",\"error\":\"bad json\"}";
        client_push_line(c, err, strlen(err));
        return;
    }
    const char *t = json_str(j, "t", "");
    if (!strcmp(t, "hello")) {
        const char *role = json_str(j, "role", "state");
        c->role_fire = !strcmp(role, "fire");
    }
    pthread_mutex_lock(&E.lock);
    apply_request_locked(j);
    pthread_mutex_unlock(&E.lock);
    if (!c->role_fire) push_state(c);
    json_free(j);
}

static void line_read(Client *c) {
    for (;;) {
        char *nl = memchr(c->in, '\n', c->inlen);
        if (!nl) break;
        size_t off = (size_t)(nl - c->in);
        size_t n = off;
        if (n && c->in[n - 1] == '\r') n--;
        char *line = malloc(n + 1);
        if (!line) return;
        memcpy(line, c->in, n);
        line[n] = 0;
        line_request(c, line);
        free(line);
        size_t rest = c->inlen - off - 1;
        memmove(c->in, c->in + off + 1, rest);
        c->inlen = rest;
        if (!off) continue;
    }
    if (c->inlen == sizeof c->in - 1) c->inlen = 0; /* oversized line */
}

static int header_value(const char *headers, const char *name, char *out,
                        size_t outlen) {
    size_t n = strlen(name);
    const char *p = headers;
    while (*p) {
        const char *e = strstr(p, "\r\n");
        if (!e) e = p + strlen(p);
        if ((size_t)(e - p) > n && !strncasecmp(p, name, n) && p[n] == ':') {
            const char *v = p + n + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t m = (size_t)(e - v);
            if (m >= outlen) m = outlen - 1;
            memcpy(out, v, m);
            out[m] = 0;
            return 1;
        }
        if (!*e) break;
        p = e + 2;
    }
    return 0;
}

static void http_reply(Client *c, const char *status, const char *ctype,
                       const char *body, size_t len) {
    char head[512];
    snprintf(head, sizeof head,
             "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
             "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
             status, ctype, len);
    client_push(c, head, strlen(head));
    if (body && len) client_push(c, body, len);
    c->close_after = 1;
}

static void http_reply_file(Client *c, const char *path, const char *ctype) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        http_reply(c, "404 Not Found", "text/plain", "missing\n", 8);
        return;
    }
    char head[512];
    snprintf(head, sizeof head,
             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
             "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
             ctype, (long long)st.st_size);
    client_push(c, head, strlen(head));
    c->file_fd = fd;
    c->file_left = (size_t)st.st_size;
    c->file_off = c->file_len = 0;
    c->close_after = 1;
}

static void http_sse(Client *c) {
    static const char head[] =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
    client_push(c, head, sizeof head - 1);
    c->kind = CL_SSE;
    c->close_after = 0;

    Buf b;
    buf_init(&b);
    char host[128] = "";
    gethostname(host, sizeof host);
    buf_printf(&b, "data: {\"t\":\"hello\",\"version\":\"%s\",\"host\":", VERSION);
    buf_str(&b, host);
    buf_puts(&b, ",\"backend\":\"c\"}\n\n");
    if (!b.err) client_push(c, buf_text(&b), b.len);
    buf_free(&b);

    buf_init(&b);
    pthread_mutex_lock(&E.lock);
    build_state_locked(&b);
    pthread_mutex_unlock(&E.lock);
    if (!b.err) {
        client_push(c, "data: ", 6);
        client_push(c, buf_text(&b), b.len);
        client_push(c, "\n\n", 2);
    }
    buf_free(&b);
}

static void http_handle(Client *c) {
    if (!strcmp(c->method, "GET") && (!strcmp(c->path, "/") ||
                                       !strcmp(c->path, "/index.html"))) {
        http_reply_file(c, page_path, "text/html; charset=utf-8");
        return;
    }
    if (!strcmp(c->method, "GET") && !strcmp(c->path, "/state")) {
        Buf b;
        buf_init(&b);
        pthread_mutex_lock(&E.lock);
        build_state_locked(&b);
        pthread_mutex_unlock(&E.lock);
        if (!b.err) http_reply(c, "200 OK", "application/json",
                               buf_text(&b), b.len);
        else http_reply(c, "500 Internal Server Error", "text/plain", "no memory\n", 10);
        buf_free(&b);
        return;
    }
    if (!strcmp(c->method, "GET") && !strcmp(c->path, "/health")) {
        http_reply(c, "200 OK", "application/json", "{\"ok\":true}", 11);
        return;
    }
    if (!strcmp(c->method, "GET") && !strcmp(c->path, "/events")) {
        http_sse(c);
        return;
    }
    if (!strcmp(c->method, "GET") && !strcmp(c->path, "/audio.wav")) {
        char tmp[128];
        snprintf(tmp, sizeof tmp, "/tmp/tracker-panel-%d.wav", (int)getpid());
        if (render_pattern_wav(tmp)) {
            http_reply_file(c, tmp, "audio/wav");
            unlink(tmp);
        } else {
            http_reply(c, "500 Internal Server Error", "text/plain", "render failed\n", 14);
        }
        return;
    }
    if (!strcmp(c->method, "POST") && !strcmp(c->path, "/req")) {
        char *body = c->in + c->body_off;
        body[c->body_len] = 0;
        struct json *j = json_parse(body);
        int ok = 0;
        if (j) {
            pthread_mutex_lock(&E.lock);
            apply_request_locked(j);
            pthread_mutex_unlock(&E.lock);
            json_free(j);
            ok = 1;
        }
        http_reply(c, "200 OK", "application/json",
                   ok ? "{\"ok\":true}" : "{\"ok\":false}",
                   ok ? 11 : 12);
        return;
    }
    http_reply(c, "404 Not Found", "application/json",
               "{\"error\":\"not found\"}", 21);
}

static void http_read(Client *c) {
    if (!c->header_done) {
        char *end = strstr(c->in, "\r\n\r\n");
        if (!end) return;
        c->header_done = 1;
        c->body_off = (size_t)(end - c->in) + 4;
        /* request line */
        char *sp = memchr(c->in, ' ', c->body_off);
        if (!sp) return;
        size_t ml = (size_t)(sp - c->in);
        if (ml >= sizeof c->method) ml = sizeof c->method - 1;
        memcpy(c->method, c->in, ml);
        c->method[ml] = 0;
        char *p2 = sp + 1;
        char *sp2 = memchr(p2, ' ', c->body_off - (size_t)(p2 - c->in));
        if (!sp2) return;
        size_t pl = (size_t)(sp2 - p2);
        if (pl >= sizeof c->path) pl = sizeof c->path - 1;
        memcpy(c->path, p2, pl);
        c->path[pl] = 0;
        char clen[32];
        c->body_len = 0;
        if (header_value(c->in, "Content-Length", clen, sizeof clen))
            c->body_len = strtol(clen, NULL, 10);
        if (c->body_len < 0) c->body_len = 0;
    }
    if (c->inlen < c->body_off + (size_t)c->body_len) return;
    http_handle(c);
}

static void client_service(Client *c) {
    if (c->kind == CL_LINE) line_read(c);
    else if (c->kind == CL_HTTP) http_read(c);
}

static void client_flush(Client *c) {
    while (c->outlen > c->outsent) {
        ssize_t n = send(c->fd, c->out + c->outsent, c->outlen - c->outsent,
                         MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            client_drop(c);
            return;
        }
        if (n == 0) return;
        c->outsent += (size_t)n;
    }
    c->outlen = c->outsent = 0;

    /* A body too large for the reply buffer is poured here.  The chunk is read
       once and then written out across as many passes as the socket needs; the
       offset is what makes a short write cheap instead of fatal. */
    for (;;) {
        if (c->file_off < c->file_len) {
            ssize_t n = send(c->fd, c->file_buf + c->file_off,
                             c->file_len - c->file_off, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                close(c->file_fd);
                c->file_fd = -1;
                c->file_left = 0;
                client_drop(c);
                return;
            }
            if (n == 0) return;
            c->file_off += (size_t)n;
            continue;
        }
        if (c->file_fd >= 0 && c->file_left > 0) {
            size_t want = c->file_left < sizeof c->file_buf ?
                          c->file_left : sizeof c->file_buf;
            ssize_t r = read(c->file_fd, c->file_buf, want);
            if (r <= 0) {
                close(c->file_fd);
                c->file_fd = -1;
                c->file_left = 0;
                c->file_len = c->file_off = 0;
                break;
            }
            c->file_len = (size_t)r;
            c->file_off = 0;
            c->file_left -= (size_t)r;
            continue;
        }
        break;
    }
    if (c->file_fd >= 0 && c->file_left == 0) {
        close(c->file_fd);
        c->file_fd = -1;
    }
    if (c->close_after && c->file_fd < 0 && c->outlen == 0 && c->kind != CL_SSE)
        client_drop(c);
}

static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

static void accept_client(int lfd, int kind) {
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int fd = accept(lfd, (struct sockaddr *)&a, &al);
    if (fd < 0) return;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd >= 0) continue;
        memset(c, 0, sizeof *c);
        c->fd = fd;
        c->kind = kind;
        c->file_fd = -1;
        if (kind == CL_LINE) send_hello_state(c);
        return;
    }
    close(fd);
}

static void broadcast_sse(void) {
    Buf b;
    buf_init(&b);
    pthread_mutex_lock(&E.lock);
    build_state_locked(&b);
    pthread_mutex_unlock(&E.lock);
    if (b.err) {
        buf_free(&b);
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd < 0 || c->kind != CL_SSE) continue;
        if (!client_push(c, "data: ", 6) ||
            !client_push(c, buf_text(&b), b.len) ||
            !client_push(c, "\n\n", 2))
            client_drop(c);
    }
    buf_free(&b);
}

static void usage(const char *a0) {
    printf("usage: %s [--http PORT] [--line PORT] [--audio SPEC] [--render-wav PATH]\n",
           a0);
}

static void on_signal(int sig) {
    (void)sig;
    stop_flag = 1;
}

int main(int argc, char **argv) {
    int http_port = DEFAULT_HTTP;
    int line_port = DEFAULT_LINE;
    const char *audio = "none";
    const char *render = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--http") && i + 1 < argc) http_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--line") && i + 1 < argc) line_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audio = argv[++i];
        else if (!strcmp(argv[i], "--render-wav") && i + 1 < argc) render = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    memset(&E, 0, sizeof E);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].file_fd = -1;
    }
    pthread_mutex_init(&E.lock, NULL);
    E.running = 1;
    snprintf(E.screen, sizeof E.screen, "pattern");
    snprintf(E.pad_mode, sizeof E.pad_mode, "note");
    E.row = E.track = E.field = 0;
    E.edit_step = 1;
    E.selected_instrument = 1;
    E.master = 81;
    E.pads_root = 48;
    E.pad_page = 0;
    snprintf(E.pattern_name, sizeof E.pattern_name, "Pattern 01");
    E.song[0] = E.song[1] = E.song[2] = E.song[3] = 0;
    E.tempo = 120;
    E.track_vol[0] = E.track_vol[1] = E.track_vol[2] = E.track_vol[3] = 100;
    E.track_vol[4] = E.track_vol[5] = E.track_vol[6] = E.track_vol[7] = 100;
    for (int r = 0; r < STEPS; r++)
        for (int t = 0; t < TRACKS; t++)
            E.pattern[r][t] = (Cell){ 0, -1, 0, 64, 0, 0 };
    make_instruments();
    snprintf(E.audio_spec, sizeof E.audio_spec, "%s", audio);
    log_locked("panel ready: %d tracks, %d steps, %d pads", TRACKS, STEPS, PADS);

    snprintf(page_path, sizeof page_path, "%s",
             getenv("PANEL_PAGE") ? getenv("PANEL_PAGE") : PAGE_FILE);
    if (access(page_path, R_OK) != 0) {
        char alt[1024];
        snprintf(alt, sizeof alt, "../web/index.html");
        if (access(alt, R_OK) == 0) snprintf(page_path, sizeof page_path, "%s", alt);
    }

    if (render) {
        int ok = render_pattern_wav(render);
        if (ok) printf("wrote %s\n", render);
        else fprintf(stderr, "tracker-paneld: could not render %s\n", render);
        pthread_mutex_destroy(&E.lock);
        return ok ? 0 : 1;
    }

    line_fd = listen_on(line_port);
    http_fd = listen_on(http_port);
    if (line_fd < 0 || http_fd < 0) {
        fprintf(stderr, "tracker-paneld: cannot bind ports %d/%d: %s\n",
                http_port, line_port, strerror(errno));
        return 1;
    }

    pthread_t ath, sth;
    pthread_create(&ath, NULL, audio_thread, NULL);
    pthread_create(&sth, NULL, sequence_thread, NULL);

    printf("panel   http://0.0.0.0:%d/\n", http_port);
    printf("door    0.0.0.0:%d\n", line_port);
    printf("audio   %s\n", audio);
    fflush(stdout);

    double next_broadcast = 0;
    while (!stop_flag) {
        struct pollfd pfd[MAX_CLIENTS + 2];
        int n = 0;
        pfd[n].fd = line_fd; pfd[n].events = POLLIN; pfd[n].revents = 0;
        int li = n++;
        pfd[n].fd = http_fd; pfd[n].events = POLLIN; pfd[n].revents = 0;
        int hi = n++;
        int idx[MAX_CLIENTS];
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) continue;
            idx[n - 2] = i;
            pfd[n].fd = clients[i].fd;
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            n++;
        }
        int pr = poll(pfd, (nfds_t)n, 5);
        if (pr > 0) {
            if (pfd[li].revents & POLLIN) accept_client(line_fd, CL_LINE);
            if (pfd[hi].revents & POLLIN) accept_client(http_fd, CL_HTTP);
            for (int k = 2; k < n; k++) {
                Client *c = &clients[idx[k - 2]];
                if (c->fd < 0) continue;
                if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                ssize_t r = recv(c->fd, c->in + c->inlen,
                                 sizeof c->in - 1 - c->inlen, 0);
                if (r <= 0) {
                    client_drop(c);
                    continue;
                }
                c->inlen += (size_t)r;
                c->in[c->inlen] = 0;
                client_service(c);
            }
        }

        double now = mono_now();
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &clients[i];
            if (c->fd < 0) continue;
            if (c->kind == CL_LINE && c->role_fire && now >= c->next_fire) {
                c->next_fire = now + 0.1;
                push_fire(c);
            }
            client_flush(c);
        }

        if (now >= next_broadcast) {
            next_broadcast = now + 0.1;
            broadcast_sse();
        }
    }

    E.running = 0;
    pthread_join(ath, NULL);
    pthread_join(sth, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0) client_drop(&clients[i]);
    close(line_fd);
    close(http_fd);

    pthread_mutex_lock(&E.lock);
    sink_close_locked();
    pthread_mutex_unlock(&E.lock);
    pthread_mutex_destroy(&E.lock);
    for (int i = 0; i < INSTRUMENTS; i++) free(E.instruments[i].data);
    return 0;
}
