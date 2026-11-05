Polyend Tracker device emulator
===============================


The Polyend Tracker firmware built for a host, and an Android app


## Available versions

### Polyend Tracker Native
- `emulator/`: the Community Edition firmware compiled for ARMv7 running 
  with a host `Platform` implementation. Screen, audio, MIDI and the card
  become host services, and the result is reachable from a browser, a
  terminal or an Akai Fire (using libfeuer).

### Polyend Tracker Pocket
- `android/`: PTPocket, the Android app. It runs the Mini firmware as a
  child process, draws its display with SDL3 and Dear ImGui, and connects
  audio, MIDI, the buttons and the card over pipes and a local socket.


## Requirements

- Polyend Tracker Community Edition source, carried as the `tracker-ce/`
  submodule (`git submodule update --init`). `TRACKER_SRC` points the build at
  a different checkout.
- armhf GCC toolchain in `.toolchain/` at the repo root (or set
  `TRACKER_TOOLCHAIN`).
- A glibc 2.24 sysroot for the app runtime: `android/tools/fetch-sysroot.sh`
  populates `sysroot/` from Debian 9 (or set `TRACKER_SYSROOT`).
- SDL3 and Dear ImGui sources, for the app. Put them in `third_party/SDL3`
  and `third_party/imgui`, or pass `-DSDL3_DIR` and `-DIMGUI_DIR` to CMake.
- For the Android build: Android SDK, NDK 23.1.7779620 and Gradle 8.5.


## Host emulator

```sh
emulator/firmware/build.sh            # add TRACKER_MINI=1 for the Mini
./emulator/firmware/tracker-firmware  # web on :8894, line-JSON on :8895

./emulator/run.sh                     # standalone panel engine, web on :8894
```

The firmware build writes `tracker-firmware` next to its script. See
`emulator/firmware/README.md` for the audio and MIDI sinks and the SD card
directory.


## Android app

```sh
android/stage.sh                      # build and stage the armhf runtime
cd android && gradle assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

See `android/README.md`.

> [!NOTE]
> Verified a 32-bit ARM phone (SM-N9600, Android 10) and on an ARMv7 host


## License
This project is pulled out of my experiments for RFX (Regreoove Effects), hence you will see this namespace used. It has been refactored in to a separate project and licensed as MIT.

The firmware source is the
[Polyend Tracker Community Edition](https://github.com/polyend/tracker-ce),
used unchanged from the `tracker-ce/` submodule, under the Polyend Tracker
Community Edition License (MIT with a Wholesale Commercial Clone Restriction),
Copyright (c) 2020-2026 Polyend Sp. z o.o. and contributors. Its precompiled
synth libraries are licensed separately. Neither is covered by this
repository's MIT license.
