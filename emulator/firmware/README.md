# Firmware host target


Builds the Polyend Tracker Community Edition firmware for a Linux host. The
firmware's own `core/` is recompiled behind a host implementation of
`core/include/Platform.h`: no device binary is emulated, and the vendor source
is not modified.


## Why armhf

The firmware casts pointers to 32-bit values and serializes structures by
`sizeof`, and it contains ARM DSP assembly and Thumb-2 `.ptfx` plugins. Only a
32-bit ARM build keeps those assumptions. The default target is ARMv7
hard-float (`arm-linux-gnueabihf`, `-mcpu=cortex-a72 -mthumb`), built with the
repo's `.toolchain`. `ARCH=x86_64` exists as a development fallback, with more
headers replaced and without plugins.


## What is replaced

| vendor | host |
| --- | --- |
| `core/source/Profiler.cpp` | `host/profiler_host.cpp` |
| `core/effector/ptfx_loader.cpp` (armhf) | patched copy: host cache flush |
| `core/effector/ptfx_loader.cpp` (x86_64) | `host/ptfx_loader_host.cpp` |
| `core/config/config.h` | `host/include/host_config.h` |
| `core/libs/Audio/utility/dspinst.h` (x86_64) | `host/include/host_dspinst.h` |
| `core/libs/Audio/Polyverb/util.h` (x86_64) | `host/include/polyverb_util.h` |

`host/main.cpp` implements every `Platform` interface: file system, config,
display timer, buttons, pads, LEDs, codec, battery, MIDI and USB.
`host/eve.cpp` models the FT812, `host/service.cpp` serves the doors,
`host/fs.cpp` maps the SD card, `host/audio.cpp` sinks PCM and `host/midi.cpp`
carries MIDI.


## Build and run

```sh
./build.sh                     # TRACKER_SRC defaults to tracker-ce/ at the repo root
TRACKER_MINI=1 ./build.sh      # Mini profile: no pads, 16 tracks
./tracker-firmware             # 4 s of updates
./tracker-firmware --frames -1 --http 8894 --line 8895
```

`--frames N` bounds the run (`-1` is forever), `--dump-every N` prints the
draw list. Service defaults: HTTP `8894`, line-JSON `8895`, page served from
`../web/index.html`; override with `--http`, `--line`, `--page` or
`--no-serve`.

### Environment

| variable | meaning |
| --- | --- |
| `TRACKER_SRC` | Community Edition checkout (default `tracker-ce/` at the repo root) |
| `TRACKER_TOOLCHAIN` | armhf toolchain (default `.toolchain/`) |
| `TRACKER_SYSROOT` | glibc 2.24 sysroot (default `sysroot/`) |
| `TRACKER_SD` | card directory (default `./sdcard`) |
| `TRACKER_AUDIO` | sink: node, `wav:PATH`, `fd:N`, `none`, `auto` |
| `TRACKER_MIDI` | port: node, `fd:N`, `none`, `auto` |
| `TRACKER_BIND` | door interfaces (default all) |

The host code matches `Platform.h` on the Community Edition `main` branch,
which is what the submodule carries. A branch that changes an interface (for
example `FileSystem::readFile` returning `size_t`) needs matching host code.


## SD card

Every firmware file call is mapped into `TRACKER_SD` by `host/fs.cpp`, with
`..` rejected. Projects, `.wav` samples, themes, fonts and the workspace are
ordinary files there. Importing a ProTracker module: **File → Import Mod**
opens a browser rooted at `Mod/`; `.mod` and `.it` files convert into an
ordinary project, with the sample data written to `Workspace/instruments/`.
Playback is from the saved project, not from the module file.


## Audio and MIDI

`host/audio.cpp` takes the firmware's 16-bit stereo blocks at 44117.647 Hz and
resamples to the sink rate (44100 first). A blocking write paces the firmware's
audio thread. `TRACKER_AUDIO=fd:N` writes S16 stereo at 44100 Hz into an
inherited descriptor: the app reads it continuously, and the full socket
buffer is what keeps the firmware at real time.

`host/midi.cpp` reads and writes a rawmidi node as a byte stream and maps it
onto the firmware's USB MIDI driver; `fd:N` carries bytes both ways and is
polled, so that descriptor must be non-blocking. The firmware's MIDI routing is
off by default: enable Notes in/out in **Config → MIDI**.


## Display

The core writes FT812 display lists (`CMD_TEXT`, `BEGIN`/`VERTEX2F`,
`CMD_MEMCPY`, RAM_G and RAM_DL). `host/eve.cpp` keeps the chip's memory model
and executes the coprocessor FIFO, including the `CMD_APPEND` assembly the
renderer depends on. It does not rasterise; the assembled frame becomes a
structured draw list:

| op | from |
| --- | --- |
| `clear`, `scissor`, `rect`, `gradient` | `CMD_CLEAR`, `CMD_SCISSOR`, `BEGIN RECTS`, gradient command |
| `line`, `point` | `BEGIN LINES`/`POINTS` |
| `text` | `CMD_TEXT`, `CMD_NUMBER` |
| `icon` | `BEGIN BITMAPS`, mask bytes served as a sprite |

Frames are published on `CMD_SWAP`. Icons are fetched once by address through
`/sprite?id=N`.


## How to access (doors)

Both doors carry the firmware's input vocabulary (button and pad ids, encoder
delta) and the assembled frame.


### HTTP `:8894`

    GET  /                     the web interface
    GET  /state, /frame        current frame as JSON
    GET  /sprite?id=N          mask blob with header and bytes
    GET  /text?cols=&rows=     screen as a character grid
    GET  /health
    GET  /events               SSE, one frame per swap
    POST /req                  {"t":"button","button":N,"down":1|0}
                               {"t":"pad","pad":N,"down":1|0}
                               {"t":"encoder","delta":N}
                               {"t":"release-all"}
    GET  /input?button=N&down=1


### Line-JSON `:8895`

One JSON object per line, both directions. On connect the door sends
`{"t":"hello","role":"firmware"}` and a frame. Requests are
`{"t":"button",...}`, `{"t":"pad",...}`, `{"t":"encoder",...}`,
`{"t":"release-all"}` and `{"t":"restart"}`. `{"t":"watch","on":1}` pushes a
frame on every swap; the Android panel uses this, one connection for frames and
buttons. `{"t":"text"}` answers with the screen as text lines.


## Interfaces

- `web/index.html`: the instrument layout (screen with eight screen buttons,
  pads below, keys and jog at the right) with a switch to the draw list or a
  text grid. Controls post to `/req`; SSE pushes frames.
- `panel/fire`: the Akai Fire client: OLED, pads, buttons, encoders, lamps.


## License

MIT. Copyright (c) 2026 Gerard Braad (Regroove | RGRV). The firmware source
itself carries the Polyend Tracker Community Edition license; see
[NOTICE.md](../../NOTICE.md).
