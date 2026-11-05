Polyend Tracker Native / Device emulator
========================================


Two targets share one protocol.

- `firmware/`: the Tracker Community Edition firmware compiled for a host
  behind a host `Platform` implementation. This is the real sequencer, audio
  engine and display list; it is what the Android app runs. See
  `firmware/README.md`.
- `panel/`: a small standalone tracker with the same doors and face layout,
  useful for front-end work without a firmware build.


## How to run

```sh
./run.sh                          # silent, web on :8894, line-JSON on :8895
./run.sh --audio aplay            # sound through the default ALSA device
./run.sh --audio wav:/tmp/t.wav   # record the live output
./run.sh --tui                    # the text face in this terminal
./run.sh --fire                   # plus the Akai Fire front end (uses libfeuer)
./run.sh --render-wav /tmp/p.wav  # render the pattern once and exit
```

`run.sh` builds what is missing. On a headless machine, open
`http://HOST:8894/`; the face is a static file served by the service.

The Fire client needs libfeuer (`LIBFEUER=/path/to/libfeuer`):

```sh
panel/fire/build.sh
./build/tracker-fire --out raw: --door 127.0.0.1:8895
```

`raw:` opens the first `/dev/snd/midi*` endpoint directly, so no ALSA library
or headers are needed. `raw:/dev/snd/midiC1D0` names one; a build with ALSA
also takes `alsa:FIRE`.


### Run the firmware

```sh
firmware/build.sh
./firmware/tracker-firmware        # web on :8894, line-JSON on :8895
```

## Layout

```
emulator/
  run.sh
  panel/
    c/tracker-paneld.c       engine, HTTP+SSE door, line-JSON door, audio
    tui/tracker-tui.c        text interface
    fire/tracker-fire.c      Akai Fire front end (uses libfeuer)
    web/index.html           browser interface
    protocol.md              requests, state, control ids, Fire mapping
  firmware/                  the real firmware built for a host
    host/main.cpp            Platform implementation
    host/eve.cpp             FT812 display model
    host/service.cpp         HTTP and line doors
    host/fs.cpp              the SD card directory
    host/audio.cpp           PCM and descriptor sinks
    host/midi.cpp            rawmidi and descriptor transport
```

## License

MIT. Copyright (c) 2026 Gerard Braad (Regroove | RGRV).
