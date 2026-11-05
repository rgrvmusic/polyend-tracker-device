/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* tracker-tui.c -- a text face for the headless Tracker.
 *
 * The web face and this one are clients of the line-JSON door; neither owns the
 * engine.  The grid, cursor, note names and control ids come from the panel;
 * this file draws them in character cells and maps keys to controls.
 *
 * No curses: the face is a frame of ANSI text, so it builds with only a C
 * compiler.  `--dump` prints one frame.  Keys are listed at the bottom.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "door.h"
#include "json.h"

#define MAX_ROWS   64
#define MAX_TRACKS 8
#define CELL_W     12       /* note(3) sp ins(2) sp vol(2) sp fx(2) */
#define TRACK_W    13       /* one space between cells */
#define GUTTER_W   4        /* marker, two row digits, space */

#define RST   "\x1b[0m"
#define BOLD  "\x1b[1m"
#define DIM   "\x1b[2m"
#define UND   "\x1b[4m"
#define REV   "\x1b[7m"
#define RED   "\x1b[31m"
#define GRN   "\x1b[32m"
#define YEL   "\x1b[33m"
#define GREY  "\x1b[90m"

static const char *NOTE[12] = {
    "C-", "C#", "D-", "D#", "E-", "F-",
    "F#", "G-", "G#", "A-", "A#", "B-"
};

static const char *SCREENS[8] = {
    "pattern", "song", "instrument", "sample",
    "master", "performance", "file", "config"
};

struct cell {
    int has_note;
    int note;
    int instr;
    int vol;
    int has_fx;
    int fx;
};

struct snap {
    int valid;
    int connected;

    char screen[24];
    int row, track, field;

    int rows, tracks;
    struct cell cell[MAX_ROWS][MAX_TRACKS];

    int playing, rec, play_row;
    int tempo, master, sel_inst;
    char inst_name[48];

    int mute[MAX_TRACKS];
    int pad_first;

    char audio_sink[96];
    char audio_err[96];
    int audio_frames;

    char log[4][160];
    int nlog;
};

static struct snap S;
static volatile sig_atomic_t g_stop;
static int g_ansi = 1;
static int g_cols = 100, g_rows = 30;

/* -- small helpers -------------------------------------------------------- */

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int node_int(const struct json *j, int dflt) {
    if (!j) return dflt;
    if (j->type == JSON_BOOL) return (int)j->num;
    if (j->type != JSON_NUM) return dflt;
    return (int)lround(j->num);
}

static const char *screen_name(const char *name) {
    if (!name) return "?";
    return name;
}

static void note_name(int n, char out[4]) {
    if (n < 0 || n > 119) {
        memcpy(out, "---", 4);
        return;
    }
    out[0] = NOTE[n % 12][0];
    out[1] = NOTE[n % 12][1];
    out[2] = (char)('0' + n / 12);
    out[3] = 0;
}

static void seg(const char *text, int style) {
    if (!g_ansi) {
        fputs(text, stdout);
        return;
    }
    if (style == 1) fputs(REV BOLD, stdout);
    else if (style == 2) fputs(UND, stdout);
    else if (style == 3) fputs(GREY, stdout);
    fputs(text, stdout);
    if (style) fputs(RST, stdout);
}

/* -- state parsing -------------------------------------------------------- */

static void parse_state(const struct json *j) {
    struct snap n;
    memset(&n, 0, sizeof n);
    n.valid = 1;
    n.connected = S.connected;

    snprintf(n.screen, sizeof n.screen, "%s",
             json_str(j, "screen", "pattern"));

    const struct json *cur = json_get(j, "cursor");
    n.row = clamp(json_int(cur, "row", 0), 0, MAX_ROWS - 1);
    n.track = clamp(json_int(cur, "track", 0), 0, MAX_TRACKS - 1);
    n.field = clamp(json_int(cur, "field", 0), 0, 3);

    n.playing = json_int(j, "playing", 0);
    n.rec = json_int(j, "rec", 0);
    n.play_row = clamp(json_int(j, "play_row", 0), 0, MAX_ROWS - 1);
    n.tempo = json_int(j, "tempo", 120);
    n.master = json_int(j, "master", 80);
    n.sel_inst = clamp(json_int(j, "selected_instrument", 1), 1, 48);

    const struct json *ins = json_get(j, "instruments");
    const struct json *sel = json_at(ins, (size_t)(n.sel_inst - 1));
    snprintf(n.inst_name, sizeof n.inst_name, "%s",
             json_str(sel, "name", ""));

    const struct json *pat = json_get(j, "pattern");
    n.rows = clamp((int)json_count(pat), 0, MAX_ROWS);
    n.tracks = 0;
    for (int r = 0; r < n.rows; r++) {
        const struct json *rj = json_at(pat, (size_t)r);
        int tc = (int)json_count(rj);
        if (tc > n.tracks) n.tracks = tc;
        for (int t = 0; t < tc && t < MAX_TRACKS; t++) {
            const struct json *cj = json_at(rj, (size_t)t);
            struct cell *c = &n.cell[r][t];
            const struct json *note = json_at(cj, 0);
            const struct json *fx = json_at(cj, 3);
            c->has_note = note && note->type != JSON_NULL;
            c->note = c->has_note ? clamp(node_int(note, 0), 0, 119) : -1;
            c->instr = clamp(node_int(json_at(cj, 1), 0), 0, 47);
            c->vol = clamp(node_int(json_at(cj, 2), 64), 0, 64);
            c->has_fx = fx && fx->type != JSON_NULL;
            c->fx = c->has_fx ? clamp(node_int(fx, 0), 0, 127) : 0;
        }
    }
    if (n.tracks == 0) n.tracks = MAX_TRACKS;

    const struct json *mute = json_get(j, "track_mute");
    for (int t = 0; t < MAX_TRACKS; t++)
        n.mute[t] = node_int(json_at(mute, (size_t)t), 0);

    const struct json *pads = json_get(j, "pads");
    n.pad_first = json_int(pads, "first_note", 48);

    const struct json *audio = json_get(j, "audio");
    snprintf(n.audio_sink, sizeof n.audio_sink, "%s",
             json_str(audio, "sink", "none"));
    snprintf(n.audio_err, sizeof n.audio_err, "%s",
             json_str(audio, "error", ""));
    n.audio_frames = json_int(audio, "frames", n.audio_frames);

    const struct json *log = json_get(j, "log");
    int lc = (int)json_count(log);
    n.nlog = 0;
    for (int i = lc > 4 ? lc - 4 : 0; i < lc; i++) {
        const struct json *e = json_at(log, (size_t)i);
        if (e && e->type == JSON_STR)
            snprintf(n.log[n.nlog++], sizeof n.log[0], "%s", e->str);
        else if (e && e->type == JSON_OBJ)
            snprintf(n.log[n.nlog++], sizeof n.log[0], "%s",
                     json_str(e, "text", ""));
    }

    S = n;
}

static void on_line(const char *line, void *user) {
    (void)user;
    struct json *j = json_parse(line);
    if (!j) return;

    const struct json *t = json_get(j, "t");
    if (t && t->type == JSON_STR) {
        if (!strcmp(t->str, "hello")) {
            S.connected = 1;
            json_free(j);
            return;
        }
        if (!strcmp(t->str, "error")) {
            fprintf(stderr, "panel: %s\n", json_str(j, "error", "?"));
            json_free(j);
            return;
        }
        if (!strcmp(t->str, "fire")) {
            json_free(j);
            return;
        }
    }

    parse_state(j);
    json_free(j);
}

/* -- drawing -------------------------------------------------------------- */

static void draw_cell(const struct cell *c, int is_cursor, int field) {
    char note[4], ins[3], vol[3], fx[3];
    note_name(c->has_note ? c->note : -1, note);

    if (c->instr) snprintf(ins, sizeof ins, "%02d", c->instr);
    else memcpy(ins, "..", 3);
    snprintf(vol, sizeof vol, "%02d", c->vol);
    if (c->has_fx) snprintf(fx, sizeof fx, "%02d", c->fx);
    else memcpy(fx, "..", 3);

    const char *text[4] = { note, ins, vol, fx };
    const int empty[4] = { !c->has_note, !c->instr, c->vol == 64,
                           !c->has_fx };

    for (int f = 0; f < 4; f++) {
        int style = 0;
        if (empty[f]) style = 3;
        if (is_cursor && f == field) style = 1;
        else if (is_cursor) style = 2;
        seg(text[f], style);
        if (f < 3) fputc(' ', stdout);
    }
}

static void track_head(int t, int muted) {
    if (!g_ansi) {
        printf("T%d%s          ", t + 1, muted ? "*" : " ");
        return;
    }
    if (muted) printf(DIM);
    printf("T%d%s", t + 1, muted ? "*" : " ");
    if (muted) printf(RST);
    printf("          ");
}

static void render(void) {
    if (g_ansi) {
        fputs("\x1b[H", stdout);
    }

    int nvis = (g_cols - GUTTER_W) / TRACK_W;
    if (nvis < 1) nvis = 1;
    if (nvis > MAX_TRACKS) nvis = MAX_TRACKS;
    if (nvis > S.tracks) nvis = S.tracks;
    int first_t = clamp(S.track - nvis / 2, 0, MAX_TRACKS - nvis);

    int avail = g_rows - 8;
    if (avail < 1) avail = 1;
    if (avail > S.rows) avail = S.rows;
    int top = clamp(S.row - avail / 2, 0, S.rows - avail);

    char line[256];

    /* title and transport */
    if (g_ansi) {
        printf(BOLD RED "POLYEND TRACKER" RST "  %-11.11s %s%s",
               screen_name(S.screen),
               S.playing ? GRN "PLAY " RST : "stop ",
               S.rec ? RED "REC" RST : "   ");
    } else {
        printf("POLYEND TRACKER  %-11.11s %s%s", screen_name(S.screen),
               S.playing ? "PLAY " : "stop ", S.rec ? "REC" : "   ");
    }
    if (g_cols >= 80)
        printf("  BPM %3d  MASTER %3d  INST %02d %-10.10s\n",
               S.tempo, S.master, S.sel_inst, S.inst_name);
    else
        printf("  %d BPM  M%d  I%02d\n", S.tempo, S.master, S.sel_inst);

    /* cursor and audio */
    char audio[80];
    if (S.audio_err[0])
        snprintf(audio, sizeof audio, "audio err: %.48s", S.audio_err);
    else if (!strcmp(S.audio_sink, "none"))
        snprintf(audio, sizeof audio, "audio off");
    else
        snprintf(audio, sizeof audio, "audio %ld frames",
                 (long)S.audio_frames);

    const char *fields[4] = { "note", "instr", "vol", "fx" };
    snprintf(line, sizeof line,
             "row %02d  track %d  field %-5s  pads %s..%s  %.48s",
             S.row, S.track + 1, fields[S.field],
             "---", "---", audio);
    char pa[4], pb[4];
    note_name(S.pad_first, pa);
    note_name(S.pad_first + 47, pb);
    snprintf(line, sizeof line,
             "row %02d  track %d  field %-5s  pads %s..%s  %.48s",
             S.row, S.track + 1, fields[S.field], pa, pb, audio);
    printf("%.*s\n", g_cols, line);

    /* column heads */
    printf("%*s", GUTTER_W, "");
    for (int t = first_t; t < first_t + nvis; t++)
        track_head(t, S.mute[t]);
    if (g_ansi) printf("\x1b[K");
    fputc('\n', stdout);

    /* rows */
    for (int r = top; r < top + avail; r++) {
        int is_play = S.playing && r == S.play_row;
        int is_cur = r == S.row;
        if (g_ansi && is_play) fputs(GRN, stdout);
        else if (g_ansi && is_cur) fputs(YEL, stdout);
        printf("%c%02d ", is_play ? '>' : is_cur ? ':' : ' ', r);
        if (g_ansi) fputs(RST, stdout);

        for (int t = first_t; t < first_t + nvis; t++) {
            draw_cell(&S.cell[r][t], is_cur && t == S.track, S.field);
            fputc(' ', stdout);
            if (t + 1 < first_t + nvis) {
                /* one column of the 13-wide block already used above */
            }
        }
        if (g_ansi) printf("\x1b[K");
        fputc('\n', stdout);
    }

    /* log and keys */
    for (int i = 0; i < 2; i++) {
        const char *l = "";
        int li = S.nlog - 2 + i;
        if (li >= 0 && li < S.nlog) l = S.log[li];
        snprintf(line, sizeof line, "log %s", l);
        printf("%.*s", g_cols, line);
        if (g_ansi) printf("\x1b[K");
        fputc('\n', stdout);
    }

    const char *help1 =
        "keys  arrows move | tab field | -/+ value | [ ] octave | space play | s stop | r rec";
    const char *help2 =
        "      x clear | c/v copy/paste | n/d ins/del | 1-8 mute | i/I instr | b/B bpm | g/G screen | q quit";
    printf("%.*s", g_cols, help1);
    if (g_ansi) printf("\x1b[K");
    fputc('\n', stdout);
    printf("%.*s", g_cols, help2);
    if (g_ansi) printf("\x1b[K\n\x1b[J");
    else fputc('\n', stdout);
    fflush(stdout);
}

/* -- terminal ------------------------------------------------------------- */

static struct termios g_orig;
static int g_tty;

static void tty_restore(void) {
    if (!g_tty) return;
    fputs("\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    g_tty = 0;
}

static int tty_raw(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return 0;
    if (tcgetattr(STDIN_FILENO, &g_orig) != 0) return 0;
    struct termios t = g_orig;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return 0;
    g_tty = 1;
    fputs("\x1b[?1049h\x1b[?25l", stdout);
    fflush(stdout);
    return 1;
}

static void tty_size(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col && w.ws_row) {
        g_cols = w.ws_col;
        g_rows = w.ws_row;
    }
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* -- commands ------------------------------------------------------------- */

static struct door *D;

static void send_ping(void) {
    if (D && door_connected(D)) door_send(D, "{\"t\":\"ping\"}");
}

static void send_hello(void) {
    if (D && door_connected(D))
        door_send(D, "{\"t\":\"hello\",\"role\":\"tui\"}");
}

static void move_to(int r, int t) {
    S.row = clamp(r, 0, S.rows ? S.rows - 1 : MAX_ROWS - 1);
    S.track = clamp(t, 0, MAX_TRACKS - 1);
    door_sendf(D, "{\"t\":\"set_cursor\",\"row\":%d,\"track\":%d}",
               S.row, S.track);
}

static void set_field(int f) {
    S.field = clamp(f, 0, 3);
    door_sendf(D, "{\"t\":\"focus\",\"field\":%d}", S.field);
}

static void button(int id, int state) {
    door_sendf(D, "{\"t\":\"input\",\"kind\":\"button\",\"id\":%d,\"state\":%d}",
               id, state);
}

static void tap(int id) {
    button(id, 1);
    button(id, 0);
}

static void with_shift(int id) {
    button(29, 1);
    tap(id);
    button(29, 0);
}

static void encoder(int delta) {
    door_sendf(D, "{\"t\":\"input\",\"kind\":\"encoder\",\"delta\":%d}", delta);
}

static void with_shift_encoder(int delta) {
    button(29, 1);
    encoder(delta);
    button(29, 0);
}

static void set_value(const char *path, int value) {
    door_sendf(D, "{\"t\":\"set\",\"path\":\"%s\",\"value\":%d}", path, value);
}

static void cycle_screen(int dir) {
    int idx = 0;
    for (int i = 0; i < 8; i++)
        if (!strcmp(SCREENS[i], S.screen)) idx = i;
    idx = (idx + dir + 8) % 8;
    snprintf(S.screen, sizeof S.screen, "%s", SCREENS[idx]);
    door_sendf(D, "{\"t\":\"screen\",\"name\":\"%s\"}", SCREENS[idx]);
}

static void toggle_mute(int t) {
    int v = !S.mute[t];
    S.mute[t] = v;
    door_sendf(D,
        "{\"t\":\"set\",\"path\":\"track_mute\",\"track\":%d,\"value\":%s}",
        t, v ? "true" : "false");
}

static void preview(void) {
    const struct cell *c = &S.cell[S.row][S.track];
    int note = c->has_note ? c->note : S.pad_first;
    door_sendf(D, "{\"t\":\"input\",\"kind\":\"note\",\"note\":%d,\"on\":true,"
               "\"vel\":100}", note);
}

static void handle_key(unsigned char ch, int *esc_state) {
    if (*esc_state == 1) {
        *esc_state = (ch == '[') ? 2 : 0;
        return;
    }
    if (*esc_state == 2) {
        *esc_state = 0;
        if (ch == 'A') move_to(S.row - 1, S.track);
        else if (ch == 'B') move_to(S.row + 1, S.track);
        else if (ch == 'C') move_to(S.row, S.track + 1);
        else if (ch == 'D') move_to(S.row, S.track - 1);
        else if (ch == 'Z') set_field((S.field + 3) % 4);
        return;
    }
    switch (ch) {
    case 0x1b: *esc_state = 1; break;
    case 3: g_stop = 1; break;              /* Ctrl-C in raw mode */
    case '\t': set_field((S.field + 1) % 4); break;
    case '\r': case '\n': tap(33); break;
    case 127: case 8: tap(34); break;
    case 'k': move_to(S.row - 1, S.track); break;
    case 'j': move_to(S.row + 1, S.track); break;
    case 'h': move_to(S.row, S.track - 1); break;
    case 'l': move_to(S.row, S.track + 1); break;
    case ' ': door_send(D, "{\"t\":\"transport\",\"action\":\"toggle\"}"); break;
    case 's': door_send(D, "{\"t\":\"transport\",\"action\":\"stop\"}"); break;
    case 'r': door_send(D, "{\"t\":\"transport\",\"action\":\"rec\"}"); break;
    case 'x': tap(27); break;
    case 'c': tap(24); break;
    case 'v': with_shift(24); break;
    case 'n': tap(25); break;
    case 'd': with_shift(27); break;
    case '-': encoder(-1); break;
    case '+': case '=': encoder(1); break;
    case '[': with_shift_encoder(-1); break;
    case ']': with_shift_encoder(1); break;
    case 'i': set_value("selected_instrument", S.sel_inst - 1); break;
    case 'I': set_value("selected_instrument", S.sel_inst + 1); break;
    case 'b': set_value("tempo", S.tempo - 1); break;
    case 'B': set_value("tempo", S.tempo + 1); break;
    case 'm': set_value("master", S.master - 1); break;
    case 'M': set_value("master", S.master + 1); break;
    case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8':
        toggle_mute(ch - '1'); break;
    case 'g': cycle_screen(-1); break;
    case 'G': cycle_screen(1); break;
    case 'p': preview(); break;
    case 'q': g_stop = 1; break;
    default: break;
    }
    send_ping();
}

/* -- main ----------------------------------------------------------------- */

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int split_door(const char *spec, char *host, size_t hsz, int *port) {
    const char *colon = strrchr(spec, ':');
    if (!colon || colon == spec) return 0;
    size_t n = (size_t)(colon - spec);
    if (n >= hsz) return 0;
    memcpy(host, spec, n);
    host[n] = 0;
    *port = atoi(colon + 1);
    return *port > 0;
}

static int dump(const char *host, int port, int cols, int rows) {
    char spec[128];
    snprintf(spec, sizeof spec, "%s:%d", host, port);
    D = door_open(host, port);
    if (!D) {
        fprintf(stderr, "tracker-tui: bad door address %s\n", spec);
        return 1;
    }
    g_ansi = 0;
    g_cols = cols;
    g_rows = rows;

    double deadline = now_s() + 3.0;
    int greeted = 0;
    while (now_s() < deadline && !S.valid) {
        if (!door_connected(D) && !door_reconnect(D)) {
            struct timespec ts = { 0, 50 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        if (!greeted) {
            send_hello();
            greeted = 1;
        }
        send_ping();
        int rc = door_poll(D, on_line, NULL);
        if (rc < 0) greeted = 0;
        struct timespec ts = { 0, 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (!S.valid) {
        fprintf(stderr, "tracker-tui: no state from %s\n", spec);
        door_close(D);
        return 1;
    }
    render();
    door_close(D);
    return 0;
}

static void usage(const char *argv0) {
    printf("usage: %s [--door HOST:PORT] [--dump] [--cols N] [--rows N]\n",
           argv0);
}

int main(int argc, char **argv) {
    const char *door_spec = "127.0.0.1:8895";
    int want_dump = 0;
    int cols = 0, rows = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--door") && i + 1 < argc) {
            door_spec = argv[++i];
        } else if (!strcmp(argv[i], "--dump")) {
            want_dump = 1;
        } else if (!strcmp(argv[i], "--cols") && i + 1 < argc) {
            cols = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--rows") && i + 1 < argc) {
            rows = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    char host[128];
    int port;
    if (!split_door(door_spec, host, sizeof host, &port)) {
        fprintf(stderr, "tracker-tui: bad door address %s\n", door_spec);
        return 2;
    }

    memset(&S, 0, sizeof S);
    S.rows = MAX_ROWS;
    S.tracks = MAX_TRACKS;
    S.tempo = 120;
    S.master = 80;
    S.sel_inst = 1;
    S.pad_first = 48;
    snprintf(S.screen, sizeof S.screen, "pattern");

    if (want_dump)
        return dump(host, port, cols > 0 ? cols : 100, rows > 0 ? rows : 30);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    tty_size();
    if (cols > 0) g_cols = cols;
    if (rows > 0) g_rows = rows;
    g_tty = tty_raw();
    if (!g_tty) {
        /* No terminal: behave like --dump rather than printing a frame every
         * tick into a pipe. */
        return dump(host, port, g_cols, g_rows);
    }
    atexit(tty_restore);

    D = door_open(host, port);
    if (!D) {
        fprintf(stderr, "tracker-tui: bad door address %s\n", door_spec);
        return 1;
    }

    double next_retry = 0, next_ping = 0, last_draw = 0;
    int greeted = 0;
    int esc_state = 0;

    while (!g_stop) {
        double now = now_s();

        if (!door_connected(D) && now >= next_retry) {
            greeted = 0;
            door_reconnect(D);
            next_retry = now + 0.5;
        }
        if (door_connected(D) && !greeted) {
            send_hello();
            greeted = 1;
            next_ping = 0;
        }

        struct pollfd pfd[2];
        int nf = 0;
        int door_i = -1, in_i = -1;
        int dfd = door_fd(D);
        if (dfd >= 0) {
            pfd[nf].fd = dfd;
            pfd[nf].events = POLLIN;
            pfd[nf].revents = 0;
            door_i = nf++;
        }
        pfd[nf].fd = STDIN_FILENO;
        pfd[nf].events = POLLIN;
        pfd[nf].revents = 0;
        in_i = nf++;

        int pr = poll(pfd, (nfds_t)nf, 100);
        if (pr > 0) {
            if (door_i >= 0 && (pfd[door_i].revents & (POLLIN | POLLHUP | POLLERR))) {
                if (door_poll(D, on_line, NULL) < 0) greeted = 0;
            }
            if (in_i >= 0 && (pfd[in_i].revents & POLLIN)) {
                unsigned char buf[64];
                ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
                for (ssize_t i = 0; i < r; i++)
                    handle_key(buf[i], &esc_state);
            }
        }

        now = now_s();
        if (door_connected(D) && now >= next_ping) {
            send_ping();
            next_ping = now + 0.1;
        }
        if (now - last_draw > 0.12) {
            render();
            last_draw = now;
        }
    }

    printf("\x1b[?25h\n");
    fflush(stdout);
    door_close(D);
    return 0;
}
