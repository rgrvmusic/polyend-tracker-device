# Third-party components

The MIT license in `LICENSE` covers the source in this repository. The
following components are used but not covered by it:

- SDL3  
  zlib license. Linked into the Android app; the Java activity in
  `android/app/src/main/java/org/libsdl/app/` is SDL's own file set and keeps
  its upstream license.
- Dear ImGui  
  MIT license. Linked into the Android app and the Emulator panel.
- libfeuer  
  MIT license. Used by the Akai Fire client. Regroove | RGRV
- Noto Sans Mono (`android/app/src/main/assets/fonts/NotoSansMono.ttf`)  
  Copyright Google, SIL Open Font License 1.1, redistributed unmodified.
- glibc 2.24  
  LGPL-2.1. The Android runtime staged by `android/tools/fetch-sysroot.sh` is
  the Debian 9 (stretch) armhf libc; it is downloaded at build time, not
  included in this repository.
- Polyend Tracker Community Edition https://github.com/polyend/tracker-ce  
  Copyright (c) 2020-2026 Polyend Sp. z o.o. and contributors. The firmware is
  built from this source, carried as the `tracker-ce/` submodule, under the
  Polyend Tracker Community Edition License: MIT for the source with a
  Wholesale Commercial Clone Restriction on the complete integrated firmware,
  and a separate license for the precompiled synth libraries
  (`synth/lib/`). No firmware source or binary is included in this repository;
  see the build instructions.
