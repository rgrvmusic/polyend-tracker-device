#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
#
# The Tracker is bare-metal Cortex-M firmware, so this build compiles core/
# against a host implementation of core/include/Platform.h instead of running
# a device binary.
#
# ARCH=armhf (default when .toolchain is present) produces Thumb-2 ARM code
# for the device's ARM cores, which execute AArch32 natively. ARCH=x86_64 is
# a development fallback.
#
# The vendor checkout is not modified; these files are replaced or patched in
# the build tree:
#   core/source/Profiler.cpp        -> host/profiler_host.cpp
#   core/effector/ptfx_loader.cpp   -> patched copy (cache sync), armhf only
#   core/effector/ptfx_loader.cpp   -> host/ptfx_loader_host.cpp, x86_64 only
# One header is force-included ahead of the original (its include guard matches,
# so the vendor header is skipped) because it contains privileged instructions:
#   core/config/config.h            -> host/include/host_config.h
# On x86_64 two more headers are replaced for the same reason:
#   core/libs/Audio/utility/dspinst.h  -> host/include/host_dspinst.h
#   core/libs/Audio/Polyverb/util.h    -> host/include/polyverb_util.h
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="${TRACKER_SRC:-$ROOT/tracker-ce}"
TC="${TRACKER_TOOLCHAIN:-$ROOT/.toolchain}"
ARCH="${ARCH:-auto}"

if [[ "$ARCH" == auto ]]; then
    if [[ -x "$TC/bin/arm-linux-gnueabihf-g++" ]]; then ARCH=armhf; else ARCH=x86_64; fi
fi

BUILD="$HERE/build/$ARCH"
OUT="$HERE/tracker-firmware"
JOBS="${JOBS:-$(nproc)}"
[[ "$JOBS" -gt 8 ]] && JOBS=8

# TRACKER_MINI=1 builds the Tracker Mini profile: the D-pad/function-button
# control set instead of pads, 16 tracks, and the synth engines.  It gets its
# own build directory and binary so the two profiles never share objects.
if [[ "${TRACKER_MINI:-0}" != 0 ]]; then
    BUILD="$BUILD-mini"
    OUT="$HERE/tracker-firmware-mini"
fi

# TRACKER_ANDROID=1 links dynamically against the glibc 2.24 runtime staged by
# android/tools/build-runtime.sh. Android 10's seccomp policy traps
# set_robust_list (and statx from 2.28 on); 2.24 predates both, and its
# libpthread can be patched at the call site.
ANDROID="${TRACKER_ANDROID:-0}"
SYSROOT="${TRACKER_SYSROOT:-$ROOT/sysroot}"
if [[ "$ANDROID" != 0 ]]; then
    BUILD="$BUILD-android"
    OUT="$OUT-android"
fi

if [[ ! -d "$SRC/core" ]]; then
    echo "firmware source not found: $SRC" >&2
    echo "set TRACKER_SRC to a polyend-tracker-ce checkout" >&2
    exit 1
fi

INC=(
    -I"$SRC/core/include" -I"$SRC/core/config" -I"$SRC/core/source"
    -I"$SRC/core/synth" -I"$SRC/core/libs" -I"$SRC/core/libs/Audio"
    -I"$SRC/core/libs/Audio/utility" -I"$SRC/core/libs/ElapsedMillis"
    -I"$SRC/core/libs/FT812" -I"$SRC/core/libs/fonts"
    -I"$SRC/core/libs/NewDisplay" -I"$SRC/core/interface"
    -I"$SRC/core/effector" -I"$SRC/core/FileManager"
    -I"$SRC/core/Exporter" -I"$SRC/core/AudioEngine"
    -I"$SRC/core/libs/Audio/Polyverb"
    -I"$SRC/core/AudioEngine/ConstantValues"
    -I"$SRC/core/AudioEngine/MasterEnhancer"
    -I"$SRC/util/include" -I"$SRC/util/3rdparty"
    -I"$SRC/synth/include" -I"$SRC/platforms/mk66/config"
    -I"$ROOT/emulator/panel/fire" -I"$HERE/host"
)
DEF=(
    -DKINETISK -DTRACKER_HOST -D__NEWLIB__ -DF_CPU=180000000
    -DTEENSYDUINO=142 -DARDUINO=10805 -DLAYOUT_US_ENGLISH
    -DCPU_MK66FX1M0VMD18 -DUSB_MIDI
    -DAUDIO_PERFORMANCE_INFO_ENABLED -DMEMORY_USAGE_INFO_ENABLED
)
if [[ "${TRACKER_MINI:-0}" != 0 ]]; then DEF+=(-DTRACKER_MINI); fi
# -fpermissive: the firmware casts pointers to uint32_t for EEPROM offsets and
# stores string literals in char*; see emulator/firmware/README.md.
FLAGS=(-std=gnu++20 -O2 -g -fno-exceptions -fno-rtti -fpermissive
       -include cstdint -include cassert -include algorithm)
FORCE=(-include "$HERE/host/include/host_config.h")

if [[ "$ARCH" == armhf ]]; then
    CXX="$TC/bin/arm-linux-gnueabihf-g++"
    CC="$TC/bin/arm-linux-gnueabihf-gcc"
    SYS=("--sysroot=$TC/arm-linux-gnueabihf")
    LINKFLAGS=(-static
        # host/main.cpp wraps __libc_start_main to install its SIGSYS handler
        # before glibc's own start-up: Android's seccomp filter traps
        # set_robust_list, which glibc issues before main.
        -Wl,--wrap=__libc_start_main)
    if [[ "$ANDROID" != 0 ]]; then
        # Headers stay the toolchain's; only the libraries come from 2.24.
        # Compiling against the 2.24 headers is not possible: gcc 12's
        # libstdc++ headers call pthread_cond_clockwait (glibc 2.30).  The
        # sysroot's lib/ is not on the search path: it holds a gcc 6
        # libstdc++.a with none of gcc 12's symbols.
        SYS=("--sysroot=$SYSROOT" -L"$SYSROOT/usr/lib")
        DEF+=(-DTRACKER_GLIBC224)
        LINKFLAGS=(-Wl,--wrap=__libc_start_main)
    fi
    ARCHFLAGS=(
        "${SYS[@]}"
        -B"$TC/lib/gcc-cross/arm-linux-gnueabihf/12/"
        -mcpu=cortex-a72 -mfpu=neon-fp-armv8 -mfloat-abi=hard -mthumb
        # The synth's Patch.h stores parameter limits as __fp16.  GCC only
        # names that type on ARM once a half-precision format is chosen, and
        # without this the Mini profile's TsLoaderVoice.cpp does not compile.
        -mfp16-format=ieee
    )
    # The Android link is after the objects: only glibc and the loader are
    # shared.  libstdc++ and libgcc are static because the runtime ships glibc
    # 2.24 and gcc 12's shared libstdc++ requires newer symbols.  libstdc++
    # declares pthread_create and friends weak, so --as-needed would drop
    # libpthread and the calls would be null at run time.
    if [[ "$ANDROID" != 0 ]]; then
        LIBS=(-static-libstdc++ -static-libgcc
              -Wl,--no-as-needed -lpthread -Wl,--as-needed)
    else LIBS=(-lpthread); fi
    export LD_LIBRARY_PATH="$TC/hostlib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
else
    CXX="${CXX:-g++}"
    CC="${CC:-gcc}"
    ARCHFLAGS=()
    LINKFLAGS=()
    LIBS=(-lpthread)
    FORCE+=(-include "$HERE/host/include/host_dspinst.h")
fi

KEY=$(cat "$HERE"/host/include/*.h "$HERE"/host/*.h "$HERE"/host/patches/* \
       "$HERE"/host/*.cpp 2>/dev/null \
       | sha1sum | cut -d' ' -f1)"-$ARCH-${TRACKER_MINI:-0}"
if [[ "${1:-}" == "--refresh" || ! -f "$BUILD/obj/.key" || "$(cat "$BUILD/obj/.key")" != "$KEY" ]]; then
    rm -rf "$BUILD/obj"
fi
mkdir -p "$BUILD/obj" "$BUILD/patched"
echo "$KEY" >"$BUILD/obj/.key"

# The plugin loader writes Thumb code into the scratch buffer and then calls
# cache maintenance on memory-mapped controller registers.  Compile a copy
# whose syncInstructionCache uses the host's cache flush instead.
PATCHED="$BUILD/patched/ptfx_loader.cpp"
rm -f "$PATCHED"
patch -s -f -p0 -o "$PATCHED" "$SRC/core/effector/ptfx_loader.cpp" \
      "$HERE/host/patches/ptfx-loader-host.patch"

compile_cpp() {
    local src="$1" extra=()
    if [[ "$ARCH" == x86_64 ]] && grep -qE 'Polyverb|polyverb' "$src"; then
        extra=(-include "$HERE/host/include/polyverb_util.h")
    fi
    local name
    name=$(realpath --relative-to="$SRC" "$src" 2>/dev/null || basename "$src")
    local obj="$BUILD/obj/$(echo "$name" | tr / _).o"
    [[ -f "$obj" && "$obj" -nt "$src" && "$obj" -nt "$HERE/build.sh" ]] && return
    "$CXX" "${ARCHFLAGS[@]}" "${FLAGS[@]}" "${FORCE[@]}" "${extra[@]}" "${DEF[@]}" "${INC[@]}" -c -o "$obj" "$src"
}

objs=()
n=0
while IFS= read -r -d '' src; do
    case "$src" in
        */core/source/Profiler.cpp) continue ;;
        # The TS synth engine needs FEATURE_SYNTH and external SDRAM, which
        # this host does not provide; skip the TU as for Profiler.cpp.
        */core/synth/engines/ts_loader/TsLoaderVoice.cpp)
            rm -f "$BUILD/obj/core_synth_engines_ts_loader_TsLoaderVoice.cpp.o"
            continue ;;
        */core/effector/ptfx_loader.cpp)
            if [[ "$ARCH" == x86_64 ]]; then continue; fi
            src="$PATCHED"
            ;;
    esac
    compile_cpp "$src" &
    n=$((n + 1))
    if ((n % JOBS == 0)); then wait; fi
done < <(find "$SRC/core" -name '*.cpp' -print0)
wait

# Plain C tables the audio library links against, and the shared JSON reader.
for src in "$SRC/core/libs/Audio/utility/sqrt_integer.c" \
           "$SRC/core/libs/Audio/data_waveforms.c" \
           "$ROOT/emulator/panel/fire/json.c"; do
    obj="$BUILD/obj/$(basename "$src").o"
    if [[ ! -f "$obj" || "$obj" -ot "$src" ]]; then
        "$CC" "${ARCHFLAGS[@]}" -O2 -g -c -o "$obj" "$src"
    fi
    objs+=("$obj")
done

for src in "$HERE"/host/*.cpp; do
    [[ "$ARCH" == armhf && "$src" == */ptfx_loader_host.cpp ]] && continue
    obj="$BUILD/obj/host_$(basename "$src").o"
    if [[ ! -f "$obj" || "$obj" -ot "$src" ]]; then
        "$CXX" "${ARCHFLAGS[@]}" "${FLAGS[@]}" "${FORCE[@]}" "${DEF[@]}" "${INC[@]}" \
               -c -o "$obj" "$src"
    fi
    objs+=("$obj")
done

# Embed the web face so a deployed binary serves it without a web/ directory.
PAGE_SRC="$HERE/web/index.html"
PAGE_CPP="$BUILD/web_page.cpp"
if [[ ! -f "$PAGE_CPP" || "$PAGE_SRC" -nt "$PAGE_CPP" ]]; then
    {
        echo '// Generated from web/index.html by build.sh; do not edit.'
        xxd -i -n kWebPage "$PAGE_SRC"
    } >"$PAGE_CPP"
fi
obj="$BUILD/obj/web_page.cpp.o"
if [[ ! -f "$obj" || "$obj" -ot "$PAGE_CPP" ]]; then
    "$CXX" "${ARCHFLAGS[@]}" "${FLAGS[@]}" -c -o "$obj" "$PAGE_CPP"
fi
objs+=("$obj")

"$CXX" "${ARCHFLAGS[@]}" "${LINKFLAGS[@]}" -o "$OUT" \
       $(find "$BUILD/obj" -name '*.o') "${LIBS[@]}"
echo "built $OUT ($ARCH)"
