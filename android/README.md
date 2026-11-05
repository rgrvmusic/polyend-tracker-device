PTPOCKET for Android
====================


Polyend Tracker Mini hosted on Android


## Description
An Android app that runs the Tracker Mini firmware as a child process and draws
its display with SDL3 and Dear ImGui. Audio, MIDI, the eight screen buttons and
the card are wired over pipes and a local socket.

The app is 32-bit ARM only (armeabi-v7a): the firmware is armhf, and an app
process is the ABI of the objects in the APK.

> [!NOTE]
> Sorry, this won't run on Aarch64 only devices like Google Pixel. This can be
> fixed by compiling the firmware differently.


## Stack

| piece | version |
| --- | --- |
| AGP / Gradle | 7.4.2 / 8.5 |
| compileSdk / minSdk | 33 / 26 |
| targetSdk | 28 |
| NDK / CMake | 23.1.7779620 / 3.22.1 |
| SDL3 | 3.5.0 |
| Dear ImGui | 1.92.3 |
| GL | OpenGL ES 3.0 |


> [!NOTE]
> `targetSdk 28` is required: Android 10 (API 29) removed `execve` from the app
> domain for apps targeting 29 or higher, and the app starts the firmware from
> its own data directory.


## How it works

`RuntimeStager.java` copies `assets/runtime/` into `filesDir/runtime/` on
first run and marks it executable. The set is the firmware plus a small glibc
2.24 runtime:

| file | what it is |
| --- | --- |
| `ld-linux-armhf.so.3` | glibc 2.24 loader |
| `libc.so.6`, `libm.so.6` | glibc 2.24, stripped |
| `libpthread.so.0` | glibc 2.24 with `set_robust_list` patched out |
| `tracker-firmware-mini` | the Mini firmware |

glibc 2.24 is used because Android's app seccomp filter kills the newer
syscalls (`set_robust_list`, `statx`) that later glibc issues before `main`.

It also stages the monospace font and creates `filesDir/sdcard/` as the card.

`child.cpp` starts the firmware through the loader, because the firmware's
`PT_INTERP` does not exist on Android:

    filesDir/runtime/ld-linux-armhf.so.3 --library-path filesDir/runtime \
        filesDir/runtime/tracker-firmware-mini

| variable | value |
| --- | --- |
| `TRACKER_SD` | `filesDir/sdcard` |
| `TRACKER_AUDIO` | `fd:<n>`: S16 stereo, 44100 Hz |
| `TRACKER_MIDI` | `fd:<m>`: MIDI bytes both ways, non-blocking |
| `TRACKER_BIND` | `127.0.0.1` |
| `TRACKER_SECCOMP_SKIP` | `1` |
| `TRACKER_RESTART` | `0`: the app owns restarts |
| `TMPDIR` | `filesDir/sdcard` |

The child is `setsid`'d, watched every 500 ms and restarted if it exits; the
app kills it on the way out. Its output goes to `filesDir/tracker.log`.

The audio socket's send buffer is about 8 KB. `main.cpp` reads it on its own
thread into a one-second ring and hands it to SDL from the audio callback,
reading only below a 100 ms target: the full socket paces the firmware's
blocking write.

`screen.cpp` holds one line connection to `127.0.0.1:8895` and sends
`{"t":"watch","on":1}`; a frame then arrives on every display swap. Sprites
come from `GET /sprite?id=N` and are cached as GL textures. `render.cpp` maps
the draw list onto ImGui's, and `panel.cpp` draws the buttons.


## Panel

Buttons send `{"t":"button","button":N,"down":0|1}` with the firmware's ids
(0-7 screen keys, 18 Note, 19 Instr, 20 FX2, 21 FX1, 23 Play, 24 Copy,
25 Insert, 26 Up, 27 Delete, 28 Rec, 29 Shift, 30 Left, 31 Down, 32 Right,
33 Enter, 34 Menu, 35-38 Fn1-4, 39 +10, 40 +1, 41 -1, 42 -10, 43 Game).
Touches are tracked per finger, so chords work. The top bar's folder opens
the card screen (`UserFsActivity`), where files are imported into the card
directory.


## MIDI

`MidiHandler.java` registers with `MidiManager`, opens the first device's
output and input ports, and bridges both through `midi_bridge.cpp` to the
child's descriptor. The firmware's routing is off by default: enable Notes
in/out in **Config → MIDI**.


## Build

Build the runtime first; the assets directory is what the APK packages:

```sh
./stage.sh                       # builds and stages the armhf runtime
```

`stage.sh` calls `tools/build-runtime.sh`, which builds the firmware with
`TRACKER_MINI=1 TRACKER_ANDROID=1`, patches `libpthread.so.0`, strips the
libraries and copies the five files into `app/src/main/assets/runtime/`. It
needs `TRACKER_SRC` (the Community Edition checkout) and `TRACKER_TOOLCHAIN`
(default `.toolchain/` at the repo root), plus `TRACKER_SYSROOT` for glibc
2.24.


### Runtime sysroot

Only four files are taken from the sysroot: `lib/ld-2.24.so`,
`lib/libc-2.24.so`, `lib/libm-2.24.so` and `lib/libpthread-2.24.so`. Fetch
them from Debian 9 (stretch), the last release with glibc 2.24:

```sh
tools/fetch-sysroot.sh            # writes sysroot/ at the repo root
```

The script downloads `libc6` and `libc6-dev` from `archive.debian.org` and
unpacks them without root; `libc6-dev` supplies the crt objects and linker
scripts the firmware link needs. `TRACKER_SYSROOT` may point at any armhf
glibc 2.24 sysroot instead. Newer glibc cannot be used (see the seccomp note
in `tools/build-runtime.sh`). `tools/patch-set-robust.c` removes the three
`set_robust_list` calls for whichever build is used: the crosstool-NG form
(`movw`/`svc`) and the Debian form (`mov.w ip`/`bl __libc_do_syscall`).

Then the APK:

```sh
gradle assembleDebug
```

The APK lands in `app/build/outputs/apk/debug/app-debug.apk`. `local.properties`
must point `sdk.dir` at the Android SDK. CMake needs SDL3 and Dear ImGui; put
them in `third_party/` at the repo root or pass `-DSDL3_DIR` / `-DIMGUI_DIR`.

## Run
```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n nl.rgrv.ptpocket/.MainActivity
adb shell run-as nl.rgrv.ptpocket cat files/tracker.log
```

The first start stages the runtime into `filesDir`, so the display appears a
moment later.


## Status

Verified on a 32-bit ARM phone (SM-N9600, Android 10).
As mentioned above, this might not work on other devices that are Aarch64 only.


## License

MIT. Copyright (c) 2026 Gerard Braad (Regroove | RGRV). See
[NOTICE.md](../NOTICE.md) for third-party components. The Java files under
`app/src/main/java/org/libsdl/app/` are SDL's and keep their upstream license.
The firmware the app runs is the
[Polyend Tracker Community Edition](https://github.com/polyend/tracker-ce),
Copyright (c) 2020-2026 Polyend Sp. z o.o. and contributors, under the
Polyend Tracker Community Edition License.
