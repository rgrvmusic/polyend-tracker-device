/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* tracker-fire.c -- an Akai Fire as a face for the panel's Tracker.
 *
 * A line-JSON door client: Fire controls map to the Tracker's ids, and the
 * panel's `fire` event drives the OLED and lamps. door.c, json.c and midi.c
 * carry the transport; libfeuer supplies the wire parser and the 128x64
 * display.
 *
 * With hardware:
 *   ./build/tracker-fire --out alsa:FIRE --list
 *   ./build/tracker-fire --out alsa:FIRE --door 127.0.0.1:8893
 * Without hardware:
 *   ./build/tracker-fire --out stdout --door 127.0.0.1:8893 -v
 */

#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "door.h"
#include "json.h"
#include "midi.h"

#include "feuer_input.h"
#include "feuer_midi_protocol.h"
#include "feuer_oled.h"
#include "feuer_output.h"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8895
#define TRACKS 8
#define PADS 48
#define STEPS 64
#define OLED_REFRESH_MS 3000

struct client {
    struct door *door;
    struct midi *midi;
    struct midi *inmidi;   /* --in; NULL means the --out port is duplex */
    FireOLED *oled;
    int verbose;
    int stopping;

    /* What the panel last told us. */
    char screen[24];
    char field[8];
    int playing, rec, row, play_row, track, bpm, master, shift;
    int steps;
    char around[4][TRACKS][8];
    int around_row[4];
    int mutes[TRACKS];

    /* Fire output, coalesced: one pending value per control, not a queue. */
    int pad_rgb[PADS][3];
    int pad_sent[64][3];
    int pad_dirty[64];
    uint8_t btn_led[64];
    int btn_dirty[64];

    int oled_pending;
    int oled_stripe;
    int oled_sig;
    int64_t last_full_ms;
    int hellod;

    /* The firmware host's door (emulator/firmware) takes the Tracker's button
     * ids directly and sends its text face instead of the panel's project
     * state.  --firmware switches the request vocabulary and puts the text
     * face on the OLED. */
    int firmware;
    char text[24][96];
    int ntext;
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The Fire's pad grid is 4 rows of 16 from the bottom-left; the Tracker's is 4
 * rows of 12.  Rows and columns pass through, so low notes are at the bottom of
 * both and the four spare columns stay dark. */
static int cell_to_pad(int cell) {
    if (cell < 0 || cell >= 64) return -1;
    const int row = cell / 16, col = cell % 16;
    if (col >= 12) return -1;
    return row * 12 + col;
}

static int pad_to_cell(int pad) {
    if (pad < 0 || pad >= PADS) return -1;
    return (pad / 12) * 16 + pad % 12;
}

static int hex_rgb(const char *s, int *r, int *g, int *b) {
    unsigned int rr, gg, bb;
    if (!s || s[0] != '#' || strlen(s) < 7) return 0;
    if (sscanf(s, "#%2x%2x%2x", &rr, &gg, &bb) != 3) return 0;
    *r = (int)rr; *g = (int)gg; *b = (int)bb;
    return 1;
}

static uint8_t led_level(int r, int g, int b) {
    if (r <= 8 && g <= 8 && b <= 8) return FIRE_LED_OFF;
    if (r > g + b) return r > 128 ? FIRE_LED_BRIGHT_RED : FIRE_LED_DIM_RED;
    return FIRE_LED_GREEN;
}

static void tap(struct client *c, int button_id) {
    if (c->firmware) {
        door_sendf(c->door, "{\"t\":\"button\",\"button\":%d,\"down\":1}", button_id);
        door_sendf(c->door, "{\"t\":\"button\",\"button\":%d,\"down\":0}", button_id);
        return;
    }
    door_sendf(c->door, "{\"t\":\"input\",\"kind\":\"button\",\"id\":%d,\"state\":1}",
               button_id);
    door_sendf(c->door, "{\"t\":\"input\",\"kind\":\"button\",\"id\":%d,\"state\":0}",
               button_id);
}

static void hold(struct client *c, int button_id, int down) {
    if (c->firmware) {
        door_sendf(c->door, "{\"t\":\"button\",\"button\":%d,\"down\":%d}", button_id,
                   down ? 1 : 0);
        return;
    }
    door_sendf(c->door,
               "{\"t\":\"input\",\"kind\":\"button\",\"id\":%d,\"state\":%d}",
               button_id, down ? 1 : 0);
}

/* The firmware has no "show screen" request: its screens are the mode row
 * buttons (mtInterfaceButtonsDef 8..22). */
static int screen_button(const char *name) {
    static const struct { const char *name; int id; } map[] = {
        {"pattern", 22}, {"song", 17}, {"instrument", 19}, {"sample", 13},
        {"master", 12}, {"performance", 9}, {"file", 10}, {"config", 11},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(map[i].name, name)) return map[i].id;
    return 22;
}

static void screen(struct client *c, const char *name) {
    if (c->firmware) {
        tap(c, screen_button(name));
        return;
    }
    door_sendf(c->door, "{\"t\":\"screen\",\"name\":\"%s\"}", name);
}

static const char *SCREEN_CYCLE[] = {
    "pattern", "song", "instrument", "sample", "master",
    "performance", "file", "config", NULL
};

static void next_screen(struct client *c) {
    int i = 0;
    while (SCREEN_CYCLE[i] && strcmp(SCREEN_CYCLE[i], c->screen)) i++;
    if (!SCREEN_CYCLE[i]) i = 0;
    else i = SCREEN_CYCLE[i + 1] ? i + 1 : 0;
    screen(c, SCREEN_CYCLE[i]);
}

static void on_pad(int pad, int pressed, int velocity, void *ud) {
    struct client *c = ud;
    const int tp = cell_to_pad(pad);
    if (tp < 0) return;
    if (c->verbose) fprintf(stderr, "fire: pad %d -> %d %s\n", pad, tp,
                            pressed ? "down" : "up");
    if (c->firmware) {
        door_sendf(c->door, "{\"t\":\"pad\",\"pad\":%d,\"down\":%d}", tp,
                   pressed ? 1 : 0);
        /* The firmware exposes no lamp state, so a pressed pad lights itself. */
        c->pad_rgb[tp][0] = pressed ? 0 : 0;
        c->pad_rgb[tp][1] = pressed ? 96 : 0;
        c->pad_rgb[tp][2] = pressed ? 32 : 0;
        c->pad_dirty[tp] = 1;
        return;
    }
    door_sendf(c->door,
               "{\"t\":\"input\",\"kind\":\"pad\",\"id\":%d,\"state\":%d,\"vel\":%d}",
               tp, pressed ? 1 : 0, pressed ? velocity : 0);
}

static void on_button(int button, int pressed, void *ud) {
    struct client *c = ud;
    if (c->verbose)
        fprintf(stderr, "fire: button %d %s (%s)\n", button,
                pressed ? "down" : "up",
                button == FIRE_BTN_PLAY ? "play" :
                button == FIRE_BTN_STOP ? "stop" :
                button == FIRE_BTN_RECORD ? "record" :
                button == FIRE_BTN_SHIFT ? "shift" : "other");

    switch (button) {
    case FIRE_BTN_SHIFT:
        c->shift = pressed;
        hold(c, 29, pressed);
        break;
    case FIRE_BTN_PLAY:
        if (pressed) tap(c, 23);
        break;
    case FIRE_BTN_STOP:
        if (pressed) {
            /* The Tracker's Play button is a toggle; the firmware has no
             * separate stop request. */
            if (c->firmware) tap(c, 23);
            else door_send(c->door, "{\"t\":\"transport\",\"action\":\"stop\"}");
        }
        break;
    case FIRE_BTN_RECORD:
        if (pressed) tap(c, 28);
        break;
    case FIRE_BTN_GRID_LEFT:
        if (pressed) tap(c, 30);
        break;
    case FIRE_BTN_GRID_RIGHT:
        if (pressed) tap(c, 32);
        break;
    case FIRE_BTN_PATTERN_UP:
        if (pressed) tap(c, 26);
        break;
    case FIRE_BTN_PATTERN_DOWN:
        if (pressed) tap(c, 31);
        break;
    case FIRE_BTN_NOTE:
        if (pressed) tap(c, 18);
        break;
    case FIRE_BTN_DRUM:
        if (pressed) tap(c, 19);
        break;
    case FIRE_BTN_STEP:
    case FIRE_BTN_PATTERN:
        if (pressed) screen(c, "pattern");
        break;
    case FIRE_BTN_PERFORM:
        if (pressed) screen(c, "performance");
        break;
    case FIRE_BTN_BROWSER:
        if (pressed) screen(c, "file");
        break;
    case FIRE_BTN_ALT:
        if (pressed) {
            if (c->firmware) tap(c, 23);
            else door_send(c->door, "{\"t\":\"transport\",\"action\":\"stop\"}");
        }
        break;
    case FIRE_BTN_CHANNEL_MODE:
        if (pressed) next_screen(c);
        break;
    case FIRE_BTN_SOLO_1:
    case FIRE_BTN_SOLO_2:
    case FIRE_BTN_SOLO_3:
    case FIRE_BTN_SOLO_4:
        /* The firmware has no cursor-track request; the pattern screen moves
         * between tracks with the arrow buttons. */
        if (pressed && !c->firmware) {
            door_sendf(c->door, "{\"t\":\"set_cursor\",\"track\":%d}",
                       button - FIRE_BTN_SOLO_1);
        }
        break;
    case FIRE_BTN_MUTE_1:
    case FIRE_BTN_MUTE_2:
    case FIRE_BTN_MUTE_3:
    case FIRE_BTN_MUTE_4:
        if (pressed && !c->firmware) {
            const int t = button - FIRE_BTN_MUTE_1;
            door_sendf(c->door,
                "{\"t\":\"set\",\"path\":\"track_mute\",\"track\":%d,\"value\":%s}",
                t, c->mutes[t] ? "false" : "true");
        }
        break;
    default:
        break;
    }
}

static void on_encoder(int cc, int delta, void *ud) {
    struct client *c = ud;
    if (delta == 0) return;
    if (cc == FIRE_CC_SELECT) {
        /* SELECT walks the cursor down or up a row per detent. */
        const int id = delta > 0 ? 31 : 26;
        int n = delta > 0 ? delta : -delta;
        if (n > 4) n = 4;
        for (int i = 0; i < n; i++) tap(c, id);
        return;
    }
    if (cc >= FIRE_CC_VOLUME && cc <= FIRE_CC_RESONANCE) {
        if (c->firmware)
            door_sendf(c->door, "{\"t\":\"encoder\",\"delta\":%d}", delta);
        else
            door_sendf(c->door, "{\"t\":\"input\",\"kind\":\"encoder\",\"delta\":%d}",
                       delta);
    }
}

/* --------------------------------------------------------------------------
 * The panel's `fire` event
 * ------------------------------------------------------------------------ */

static void set_button(struct client *c, int note, int level) {
    if (note < 0 || note > 63) return;
    if (c->btn_led[note] == level) return;
    c->btn_led[note] = (uint8_t)level;
    c->btn_dirty[note] = 1;
}

static void update_leds(struct client *c) {
    set_button(c, FIRE_BTN_PLAY, c->playing ? FIRE_LED_GREEN : FIRE_LED_OFF);
    set_button(c, FIRE_BTN_RECORD, c->rec ? FIRE_LED_BRIGHT_RED : FIRE_LED_OFF);
    set_button(c, FIRE_BTN_SHIFT, c->shift ? FIRE_LED_GREEN : FIRE_LED_OFF);
    set_button(c, FIRE_BTN_STEP, 0);      /* cleared below, per screen */
    set_button(c, FIRE_BTN_PATTERN, 0);
    set_button(c, FIRE_BTN_NOTE, 0);
    set_button(c, FIRE_BTN_DRUM, 0);
    set_button(c, FIRE_BTN_PERFORM, 0);
    set_button(c, FIRE_BTN_BROWSER, 0);
    set_button(c, FIRE_BTN_CHANNEL_MODE, 0);
    set_button(c, FIRE_BTN_ALT, 0);
    if (!strcmp(c->screen, "pattern")) set_button(c, FIRE_BTN_PATTERN, FIRE_LED_GREEN);
    if (!strcmp(c->screen, "song")) set_button(c, FIRE_BTN_BROWSER, FIRE_LED_GREEN);
    if (!strcmp(c->screen, "file")) set_button(c, FIRE_BTN_BROWSER, FIRE_LED_GREEN);
    if (!strcmp(c->screen, "instrument") || !strcmp(c->screen, "sample"))
        set_button(c, FIRE_BTN_DRUM, FIRE_LED_GREEN);
    if (!strcmp(c->screen, "performance"))
        set_button(c, FIRE_BTN_PERFORM, FIRE_LED_GREEN);
    if (!strcmp(c->screen, "master"))
        set_button(c, FIRE_BTN_CHANNEL_MODE, FIRE_LED_GREEN);
    if (!strcmp(c->screen, "config"))
        set_button(c, FIRE_BTN_ALT, FIRE_LED_GREEN);

    if (c->track >= 0 && c->track < 4)
        set_button(c, FIRE_BTN_SOLO_1 + c->track, FIRE_LED_GREEN);
    for (int t = 0; t < 4; t++)
        set_button(c, FIRE_BTN_MUTE_1 + t,
                   c->mutes[t] ? FIRE_LED_DIM_RED : FIRE_LED_OFF);
    /* The four remaining Tracker tracks have no Fire lamp of their own; they
     * are reachable from the web face and the door, and the SOLO row stops at
     * track 4 because that is where the Fire's buttons stop. */
}

static void render_oled(struct client *c) {
    char line[40];
    fire_oled_clear(c->oled);

    snprintf(line, sizeof line, "TRACKER %3d BPM %s",
             c->bpm, c->playing ? "PLAY" : "STOP");
    fire_oled_text(c->oled, 0, 0, line, 0);

    snprintf(line, sizeof line, "%-8s T%d %-4s M%02d",
             c->screen, c->track + 1, c->field, c->master);
    fire_oled_text(c->oled, 0, 9, line, 0);

    for (int i = 0; i < 4; i++) {
        char notes[17];
        int n = 0;
        notes[0] = '\0';
        for (int t = 0; t < TRACKS; t++) {
            const char *s = c->around[i][t];
            char a = '.', b = '.';
            if (s[0] && s[0] != '-') {
                a = s[0];
                b = (s[1] == '#' || s[1] == '-') ? s[1] : '-';
            }
            notes[n++] = a;
            notes[n++] = b;
        }
        notes[n] = '\0';
        snprintf(line, sizeof line, "R%02d %s", c->around_row[i], notes);
        fire_oled_text(c->oled, 0, 18 + i * 9, line, 0);
    }

    snprintf(line, sizeof line, "C%02d %02d %s %s",
             c->row, c->steps, c->rec ? "REC" : "   ", "FIRE");
    fire_oled_text(c->oled, 0, 54, line, 0);

    c->oled_pending = 1;
    c->oled_stripe = 0;
}

/* Firmware mode: the OLED carries the screen's own text face, one line per
 * 9 px row, up to the seven rows that fit. */
static void render_oled_fw(struct client *c) {
    fire_oled_clear(c->oled);
    for (int i = 0; i < c->ntext && i < 7; i++)
        fire_oled_text(c->oled, 0, i * 9, c->text[i], 0);
    c->oled_pending = 1;
    c->oled_stripe = 0;
    c->last_full_ms = now_ms();
}

static int text_signature(struct client *c) {
    int h = 2166136261u;
    for (int i = 0; i < c->ntext; i++)
        for (const char *p = c->text[i]; *p; p++)
            h = (h ^ (unsigned char)*p) * 16777619u;
    return (int)h;
}

static int oled_signature(struct client *c) {
    /* Only what the display shows.  The play row is deliberately not here: it
     * changes sixteen times a bar, and a full OLED refresh is 1272 bytes. */
    int h = 2166136261u;
    const char *parts[] = {c->screen, c->field};
    for (int i = 0; i < 2; i++)
        for (const char *p = parts[i]; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    h = (h ^ c->row) * 16777619u;
    h = (h ^ c->track) * 16777619u;
    h = (h ^ c->bpm) * 16777619u;
    h = (h ^ c->master) * 16777619u;
    h = (h ^ c->playing) * 16777619u;
    h = (h ^ c->rec) * 16777619u;
    for (int i = 0; i < 4; i++) {
        h = (h ^ c->around_row[i]) * 16777619u;
        for (int t = 0; t < TRACKS; t++)
            for (const char *p = c->around[i][t]; *p; p++)
                h = (h ^ (unsigned char)*p) * 16777619u;
    }
    return (int)h;
}

static void on_fire_state(struct client *c, const struct json *j) {
    const char *s = json_str(j, "screen", "pattern");
    snprintf(c->screen, sizeof c->screen, "%s", s);
    snprintf(c->field, sizeof c->field, "%s", json_str(j, "field", "note"));
    c->playing = json_int(j, "playing", 0);
    c->rec = json_int(j, "rec", 0);
    c->row = json_int(j, "row", 0);
    c->play_row = json_int(j, "play_row", 0);
    c->track = json_int(j, "track", 0);
    c->bpm = json_int(j, "bpm", 120);
    c->master = json_int(j, "master", 81);
    c->shift = json_int(j, "shift", 0);
    c->steps = json_int(j, "steps", STEPS);

    const struct json *around = json_get(j, "around");
    for (int i = 0; i < 4; i++) {
        const struct json *r = json_at(around, (size_t)i);
        c->around_row[i] = json_int(r, "r", i);
        const struct json *notes = json_get(r, "notes");
        for (int t = 0; t < TRACKS; t++) {
            const struct json *n = json_at(notes, (size_t)t);
            snprintf(c->around[i][t], sizeof c->around[i][t], "%s",
                     (n && n->type == JSON_STR) ? n->str : "---");
        }
    }

    const char *mutes = json_str(j, "mutes", "00000000");
    for (int t = 0; t < TRACKS; t++)
        c->mutes[t] = (int)(strlen(mutes) > (size_t)t && mutes[t] == '1');

    const struct json *pads = json_get(j, "padleds");
    for (int i = 0; i < PADS; i++) {
        const struct json *v = json_at(pads, (size_t)i);
        int r = 0, g = 0, b = 0;
        if (v && v->type == JSON_STR) hex_rgb(v->str, &r, &g, &b);
        if (c->pad_rgb[i][0] != r || c->pad_rgb[i][1] != g ||
            c->pad_rgb[i][2] != b) {
            c->pad_rgb[i][0] = r; c->pad_rgb[i][1] = g; c->pad_rgb[i][2] = b;
            c->pad_dirty[i] = 1;
        }
    }

    update_leds(c);

    const int sig = oled_signature(c);
    const int64_t t = now_ms();
    if (sig != c->oled_sig || t - c->last_full_ms > OLED_REFRESH_MS) {
        c->oled_sig = sig;
        render_oled(c);
    }
}

/* The firmware's text face: 80 columns by up to 24 lines of the screen.  The
 * OLED is 21 characters of 6 px, so a line is shown from its start. */
static void on_text_state(struct client *c, const struct json *j) {
    const struct json *lines = json_get(j, "lines");
    const size_t n = json_count(lines);
    c->ntext = 0;
    for (size_t i = 0; i < n && c->ntext < 24; i++) {
        const struct json *l = json_at(lines, i);
        if (!l || l->type != JSON_STR) continue;
        snprintf(c->text[c->ntext], sizeof c->text[0], "%.21s", l->str);
        c->ntext++;
    }
    const int sig = text_signature(c);
    const int64_t t = now_ms();
    if (sig != c->oled_sig || t - c->last_full_ms > OLED_REFRESH_MS) {
        c->oled_sig = sig;
        render_oled_fw(c);
    }
}

static void on_line(const char *line, void *ud) {
    struct client *c = ud;
    struct json *j = json_parse(line);
    if (!j) return;
    const char *t = json_str(j, "t", "");
    if (!strcmp(t, "fire")) {
        on_fire_state(c, j);
        if (c->verbose) fprintf(stderr, "fire: %s row %02d\n", c->screen, c->row);
    } else if (!strcmp(t, "text")) {
        on_text_state(c, j);
    }
    json_free(j);
}

/* --------------------------------------------------------------------------
 * Output pump
 * ------------------------------------------------------------------------ */

static void pump(struct client *c) {
    uint8_t msg[FIRE_MSG_MAX];

    int pad_budget = 4;
    for (int i = 0; i < PADS && pad_budget > 0; i++) {
        if (!c->pad_dirty[i]) continue;
        const int cell = pad_to_cell(i);
        if (cell < 0) { c->pad_dirty[i] = 0; continue; }
        int len = fire_output_rgb_led(msg, cell, c->pad_rgb[i][0],
                                      c->pad_rgb[i][1], c->pad_rgb[i][2]);
        if (len > 0) midi_send(c->midi, msg, len);
        c->pad_dirty[i] = 0;
        c->pad_sent[i][0] = c->pad_rgb[i][0];
        c->pad_sent[i][1] = c->pad_rgb[i][1];
        c->pad_sent[i][2] = c->pad_rgb[i][2];
        pad_budget--;
    }

    for (int n = 0; n < 64; n++) {
        if (!c->btn_dirty[n]) continue;
        int len = fire_output_button_led(msg, n, c->btn_led[n]);
        if (len > 0) midi_send(c->midi, msg, len);
        c->btn_dirty[n] = 0;
    }

    if (c->oled_pending) {
        /* All eight stripes in one pump: the panel's event arrives every
         * 100 ms, and one stripe per loop could not finish inside that. */
        uint8_t sysex[256];
        for (int i = 0; i < 8; i++) {
            int len = fire_oled_get_sysex(c->oled, sysex, i);
            if (len > 0) midi_send(c->midi, sysex, len);
        }
        c->oled_pending = 0;
        c->oled_stripe = 0;
        c->last_full_ms = now_ms();
    }
}

/* --------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------ */

static void on_midi(const uint8_t *bytes, int len, void *ud) {
    struct client *c = ud;
    if (c->verbose) {
        fprintf(stderr, "midi:");
        for (int i = 0; i < len; i++) fprintf(stderr, " %02X", bytes[i]);
        fputc('\n', stderr);
    }
    FireInputEvent ev;
    if (!fire_input_parse_midi(bytes, len, &ev)) return;
    if (ev.type == FIRE_INPUT_PAD_PRESS || ev.type == FIRE_INPUT_PAD_RELEASE)
        on_pad(ev.index, ev.type == FIRE_INPUT_PAD_PRESS, ev.value, c);
    else if (ev.type == FIRE_INPUT_BUTTON_PRESS ||
             ev.type == FIRE_INPUT_BUTTON_RELEASE)
        on_button(ev.index, ev.type == FIRE_INPUT_BUTTON_PRESS, c);
    else if (ev.type == FIRE_INPUT_ENCODER_TURN)
        on_encoder(ev.index, ev.value, c);
}

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void usage(FILE *f, const char *argv0) {    fprintf(f,
"usage: %s [options]\n"
"\n"
"  --door HOST:PORT   the line-JSON door (default %s:%d)\n"
"  --firmware         talk to the firmware host's door (emulator/firmware),\n"
"                     not the panel's: same Fire mapping, button ids instead\n"
"                     of panel requests, and the text face on the OLED\n"
"  --out SPEC         where the Fire's MIDI goes:\n"
"                       raw:PATH    a USB MIDI endpoint (raw: picks the first)\n"
"                       alsa:NAME   a sequencer port (if built with ALSA)\n"
"                       file:PATH   a transcript of the messages\n"
"                       stdout      the same, on standard output\n"
"                       none        nowhere (default)\n"
"  --in SPEC          where Fire input comes from; the same forms, plus\n"
"                       file:PATH   replay a transcript (default: the --out port)\n"
"  --list             list the MIDI ports this build can see\n"
"  -v, --verbose      print what arrives\n"
"  -h, --help\n",
            argv0, DEFAULT_HOST, DEFAULT_PORT);
}

int main(int argc, char **argv) {
    const char *door_spec = NULL;
    const char *out_spec = "none";
    const char *in_spec = NULL;
    int verbose = 0, list = 0, firmware = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout, argv[0]); return 0; }
        else if (!strcmp(a, "--list")) list = 1;
        else if (!strcmp(a, "--firmware")) firmware = 1;
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) verbose = 1;
        else if (!strcmp(a, "--door") && val) { door_spec = val; i++; }
        else if (!strcmp(a, "--out") && val) { out_spec = val; i++; }
        else if (!strcmp(a, "--in") && val) { in_spec = val; i++; }
        else { fprintf(stderr, "tracker-fire: unknown option %s\n", a); return 2; }
    }

    if (list) return midi_list_ports();

    char host[256] = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    if (door_spec) {
        const char *colon = strrchr(door_spec, ':');
        if (colon) {
            const size_t n = (size_t)(colon - door_spec);
            if (n >= sizeof host) { fprintf(stderr, "tracker-fire: host too long\n"); return 2; }
            memcpy(host, door_spec, n); host[n] = '\0';
            port = atoi(colon + 1);
        } else {
            snprintf(host, sizeof host, "%s", door_spec);
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    struct client c;
    memset(&c, 0, sizeof c);
    c.verbose = verbose;
    c.firmware = firmware;
    c.track = 0;
    snprintf(c.screen, sizeof c.screen, "pattern");
    snprintf(c.field, sizeof c.field, "note");
    c.bpm = 120;
    c.master = 81;
    c.steps = STEPS;

    /* One duplex port: the Fire's presses arrive on the port its LEDs go out
     * on, so this is opened both ways (alsa) or not at all (none/stdout).
     * --in separates the two, which is what makes a recorded transcript
     * replayable without the hardware. */
    c.midi = midi_open(out_spec, MIDI_SEND | (in_spec ? 0 : MIDI_RECEIVE));
    if (!c.midi) {
        fprintf(stderr, "tracker-fire: --out %s: %s\n", out_spec, midi_error());
        return 1;
    }
    if (in_spec) {
        c.inmidi = midi_open(in_spec, MIDI_RECEIVE);
        if (!c.inmidi) {
            fprintf(stderr, "tracker-fire: --in %s: %s\n", in_spec, midi_error());
            return 1;
        }
    }
    struct midi_info info;
    midi_describe(c.midi, &info);
    fprintf(stderr, "tracker-fire: out %s (%s), door %s:%d\n",
            info.kind, info.detail ? info.detail : "", host, port);

    c.oled = fire_oled_create();
    if (!c.oled) { fprintf(stderr, "tracker-fire: no OLED buffer\n"); return 1; }
    c.door = door_open(host, port);

    /* The Fire holds its last picture after a client exits; clearing it once
     * makes a relaunch start from a known panel rather than the old one. */
    if (info.writable) {
        uint8_t msg[FIRE_MSG_MAX];
        for (int i = 0; i < 64; i++) {
            int len = fire_output_rgb_led(msg, i, 0, 0, 0);
            if (len > 0) midi_send(c.midi, msg, len);
        }
        for (int n = 0; n < 64; n++) {
            int len = fire_output_button_led(msg, n, FIRE_LED_OFF);
            if (len > 0) midi_send(c.midi, msg, len);
        }
    }

    int64_t next_pump = 0;
    int64_t next_text = 0;
    while (!g_stop && !c.stopping) {
        struct pollfd fds[2];
        int n = 0;
        int door_i = -1, midi_i = -1;

        door_reconnect(c.door);
        if (door_connected(c.door) && !c.hellod) {
            /* Ask for the compact event, not a full state per request.  If the
             * door drops, the next connect says it again.  The firmware host
             * answers a hello with its frame; the text face is asked for. */
            door_send(c.door, "{\"t\":\"hello\",\"role\":\"fire\"}");
            c.hellod = 1;
            if (c.firmware) {
                door_send(c.door, "{\"t\":\"text\"}");
                next_text = now_ms() + 150;
            }
            /* A transcript is replayed once, after the first connect, so its
             * requests have a door to go to. */
            if (c.inmidi) midi_replay(c.inmidi, on_midi, &c);
        } else if (!door_connected(c.door)) {
            c.hellod = 0;
        }
        /* The firmware streams frames, not panel state; its text face is the
         * view the OLED can use, so ask for it at the panel event rate. */
        if (c.firmware && door_connected(c.door) && now_ms() >= next_text) {
            door_send(c.door, "{\"t\":\"text\"}");
            next_text = now_ms() + 150;
        }
        const int dfd = door_fd(c.door);
        if (dfd >= 0) {
            door_i = n;
            fds[n].fd = dfd; fds[n].events = POLLIN; fds[n].revents = 0; n++;
        }
        /* A --in transcript is replayed in full after connect; polling it
         * again would only spin on end-of-file. */
        struct midi *inm = c.inmidi ? NULL : c.midi;
        const int mfd = midi_fd(inm);
        if (mfd >= 0) {
            midi_i = n;
            fds[n].fd = mfd; fds[n].events = POLLIN; fds[n].revents = 0; n++;
        }

        int rc = n ? poll(fds, (nfds_t)n, 20) : (usleep(20000), 0);
        if (rc < 0 && errno != EINTR) break;

        if (door_i >= 0 && (fds[door_i].revents & (POLLIN | POLLHUP))) {
            door_poll(c.door, on_line, &c);
        }
        if (midi_i >= 0 && (fds[midi_i].revents & POLLIN)) {
            midi_take_input(inm, on_midi, &c);
        }

        const int64_t t = now_ms();
        if (t >= next_pump) {
            pump(&c);
            next_pump = t + 5;
        }
    }

    door_close(c.door);
    if (c.inmidi) midi_close(c.inmidi);
    midi_close(c.midi);
    fire_oled_destroy(c.oled);
    return 0;
}
