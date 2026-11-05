Panel protocol
==============

The headless Tracker has two doors into one panel. They carry the same
vocabulary, and either can be used alone or at the same time.

| door | address | shape |
|---|---|---|
| web | `http://HOST:8894/` | HTML face, `GET /events` (SSE), `POST /req`, `GET /state` |
| line | `HOST:8895` | one JSON object per line, both directions |

`tracker-paneld` (C, `panel/c/`) is the single writer. The web face, the text
face and the Fire client send requests; the panel applies them between reads
and pushes state back out.

The defaults are 8894 and 8895. `--http` and `--line` override them.


## HTTP

```
GET  /            the face (emulator/panel/web/index.html)
GET  /state       full state, JSON
GET  /events      SSE; `hello` first, then a full state event at 10 Hz
POST /req         one request object; reply {"ok":true}
GET  /audio.wav   current pattern rendered once, offline
GET  /health      {"ok":true,"version":...}
```

An SSE `data:` line is either `{"t":"hello",...}` or a state object (which has
no `t`).


## Line-JSON

On connect the panel sends a `hello` and one full state line. A client that
sends

```json
{"t":"hello","role":"fire"}
```

then receives a compact `fire` event every 100 ms instead of a state per
request. Any other client receives a state after each request, and one initial
state.

Requests are the objects below. Responses are `{"t":"error","error":"..."}` on
a malformed or failed request; ordinary requests have no reply of their own.


## Requests

Input (the Tracker's own button and pad numbering):

```json
{"t":"input","kind":"pad","id":0..47,"state":1|0,"vel":0..127}
{"t":"input","kind":"button","id":8..34,"state":1|0}
{"t":"input","kind":"encoder","delta":-64..63}
{"t":"input","kind":"note","note":0..119,"on":true,"vel":0..127}
```

Transport and screens:

```json
{"t":"transport","action":"play"|"stop"|"toggle"|"rec"}
{"t":"screen","name":"pattern"|"song"|"instrument"|"sample"|
    "master"|"performance"|"file"|"config"}
```

State changes:

```json
{"t":"set","path":"tempo","value":20..300}
{"t":"set","path":"master","value":0..100}
{"t":"set","path":"selected_instrument","value":1..48}
{"t":"set","path":"edit_step","value":0..32}
{"t":"set","path":"pads_root","value":0..108}
{"t":"set","path":"track_mute","track":0..7,"value":true|false}
{"t":"set","path":"track_vol","track":0..7,"value":0..100}
{"t":"set","path":"song_row","value":0..n}
{"t":"set_cursor","row":0..63,"track":0..7}
{"t":"focus","field":0..3}          # note, instr, vol, fx
{"t":"cell","row":0..63,"track":0..7,"note":0..119|null,
            "instr":0..47,"vol":0..64,"fx":0..127|null}
{"t":"mark","text":"..."}           # a line in the panel log
{"t":"ping"}
```


## State

The state object is the machine-readable project. The fields a front end uses:

| field | meaning |
|---|---|
| `screen` | current screen name |
| `cursor` | `{row, track, field}`; field is 0..3 |
| `pattern` | 64 rows × 8 tracks; a cell is `[note, instr, vol, fx]` |
| `playing`, `rec`, `play_row` | transport |
| `tempo`, `master` | BPM 20..300, master 0..100 |
| `selected_instrument` | 1..48 |
| `instruments` | 48 slots, `name/wave/volume/tune/filter/length/peaks` |
| `pads` | `root`, `page`, `first_note`, `count` |
| `track_mute`, `track_vol` | 8 each |
| `song` | pattern index per song row (one pattern in this build) |
| `leds` | `pads` (48 CSS colours), `buttons` (id → colour) |
| `audio` | `sink`, `error`, `frames` |
| `log` | last 20 log lines |

A cell's note is `null` for empty, otherwise MIDI note 0..119; `instr` 0 means
none; `vol` defaults to 64; `fx` is `null` when empty.


## The compact `fire` event

Sent only to line clients that asked for role `fire`:

```json
{"t":"fire","screen":"pattern","playing":1,"rec":0,
 "row":5,"play_row":12,"track":0,"field":"note","bpm":120,
 "steps":64,"cells":["C-4 01 64 --", ...8],
 "around":[{"r":4,"notes":["---",...]}, ...4 rows],
 "mutes":"00000000","padleds":["#rrggbb",...48],
 "btnleds":{"23":"#2f8f4e",...},"master":81,"shift":0}
```


## Controls

Buttons follow the Tracker CE ids (`mtInterfaceButtonsDef`). The front ends
send these numbers; the labels are the panel's.

| id | control | id | control |
|---|---|---|---|
| 8 | Params | 21 | Fx |
| 9 | Performance | 22 | Pattern |
| 10 | File | 23 | Play |
| 11 | Config | 24 | Copy |
| 12 | Master | 25 | Insert |
| 13 | Sample load | 26 | Up |
| 14 | Sample play | 27 | Delete |
| 15 | Sample edit | 28 | Record |
| 16 | Sample record | 29 | Shift |
| 17 | Song | 30 | Left |
| 18 | Note | 31 | Down |
| 19 | Instrument | 32 | Right |
| 20 | Volume | 33 / 34 | Enter / Back (panel additions) |

Pads are 0..47 in four rows of 12. Pad 0 is the bottom-left of the face and
`pads.first_note` (default 48, C-4) is its MIDI note; a row is 12 semitones, so
pad 12 is C-5. In the pattern screen, a pad press writes its note at the cursor
and advances one row; Shift+pad clears the cell. On other screens a pad
previews the note.


## Akai Fire mapping (`fire/tracker-fire.c`)

The Fire's 4×16 grid carries the Tracker's 4×12 in columns 0–11, with the rows
turned over so the panel's bottom row is at the bottom; columns 12–15 are dark.

| Fire | Tracker |
|---|---|
| pad cell | pad 0..47 (`cell_to_pad`) |
| PLAY | button 23 |
| STOP | transport stop |
| RECORD | button 28 |
| SHIFT | button 29 (held) |
| GRID LEFT / RIGHT | button 30 / 32 |
| PATTERN UP / DOWN | button 26 / 31 |
| STEP, PATTERN | screen pattern |
| NOTE | button 18 (note field) |
| DRUM | button 19 (instrument) |
| PERFORM | screen performance |
| BROWSER | screen file |
| CHANNEL MODE | next screen |
| ALT | transport stop |
| SOLO 1–4 | cursor track 1–4 |
| MUTE 1–4 | track mute 1–4 |
| knobs 1–4 | encoder delta |
| SELECT turn | cursor up/down |
| SELECT press | not bound in this build |

The client's `--out` takes `raw:PATH` (a USB MIDI endpoint, `raw:` picks the
first `/dev/snd/midi*`), `alsa:NAME` where alsa-lib was available at build
time, `file:PATH` for a transcript, or `stdout`. No ALSA library is needed for
`raw:`.

The Fire OLED shows the screen, transport, BPM, four rows around the cursor,
and the cursor row. Only tracks 1–4 have Fire track buttons; tracks 5–8 are
reachable from the web face and the door.


## Audio

The panel renders at 44100 Hz, stereo, int16. `--audio` takes:

```
none                 silent (default)
cmd:aplay -q -t raw -f S16_LE -c 2 -r 44100
wav:/tmp/out.wav     record live output
aplay                shorthand for the full `cmd:` line above, in run.sh only
```

The command receives headerless stereo int16 PCM at 44100 Hz, so `aplay` needs
the format flags; without them it assumes 8 kHz mono.

`GET /audio.wav` is an offline render of the current pattern, independent of
the live sink.


## License

MIT. Copyright (c) 2026 Gerard Braad (Regroove | RGRV).
