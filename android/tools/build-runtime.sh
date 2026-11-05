#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
#
#   android/tools/build-runtime.sh [--check]
#
# Writes android/runtime/ and copies the set to app/src/main/assets/runtime/,
# which the APK carries and RuntimeStager copies out at run time.
#
# The set is:
#   ld-linux-armhf.so.3   glibc 2.24 loader, exec'd by name
#   libc.so.6, libm.so.6  glibc 2.24
#   libpthread.so.0       glibc 2.24 with set_robust_list patched out
#   tracker-firmware-mini firmware linked dynamic, with static libstdc++/libgcc
#
# A staged runtime is needed because Android 10's seccomp policy traps unknown
# syscalls instead of returning ENOSYS. glibc 2.24 predates both
# set_robust_list (338) and statx (397); the one call it makes is patched out
# of libpthread at its call site.
#
# libstdc++ and libgcc are static: gcc 12's shared libstdc++ needs symbols
# newer than 2.24. host/compat_glibc.cpp supplies what the 2.36 headers
# expect, and host/main.cpp passes the constructors 2.24's __libc_start_main
# requires.
set -euo pipefail

SELF="$(cd "$(dirname "$0")" && pwd)"
ANDROID="$(cd "$SELF/.." && pwd)"
REPO="$(cd "$ANDROID/.." && pwd)"
FW="$REPO/emulator/firmware"
OUT="$ANDROID/runtime"
ASSETS="$ANDROID/app/src/main/assets/runtime"
BUILD="$ANDROID/build"
SYSROOT="${TRACKER_SYSROOT:-$REPO/sysroot}"
TOOLCHAIN="${TRACKER_TOOLCHAIN:-$REPO/.toolchain}"
STRIP="$TOOLCHAIN/bin/arm-linux-gnueabihf-strip"

check_only=0
for arg in "$@"; do
    case "$arg" in
        --check) check_only=1 ;;
        -h|--help) sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "$0: unknown option: $arg" >&2; exit 2 ;;
    esac
done

for f in "$SYSROOT/lib/ld-2.24.so" "$SYSROOT/lib/libc-2.24.so" \
         "$SYSROOT/lib/libm-2.24.so" "$SYSROOT/lib/libpthread-2.24.so"; do
    [[ -f "$f" ]] || { echo "$0: missing glibc 2.24 object: $f" >&2; exit 2; }
done

mkdir -p "$OUT" "$ASSETS" "$BUILD"

if [[ "$check_only" == 0 ]]; then
    # ---- the firmware ------------------------------------------------------
    echo "== building the firmware against glibc 2.24"
    ( cd "$FW" && TRACKER_MINI=1 TRACKER_ANDROID=1 ./build.sh )
    BIN="$FW/tracker-firmware-mini-android"
else
    BIN="$FW/tracker-firmware-mini-android"
    [[ -f "$BIN" ]] || { echo "$0: no firmware build at $BIN" >&2; exit 2; }
fi

# ---- the seccomp patch -----------------------------------------------------
echo "== building patch-set-robust"
cc -O2 -Wall -o "$BUILD/patch-set-robust" "$SELF/patch-set-robust.c"

if [[ "$check_only" == 1 ]]; then
    "$BUILD/patch-set-robust" --check "$OUT/libpthread.so.0"
    exit 0
fi

# ---- the runtime -----------------------------------------------------------
echo "== staging the runtime"
install -m 0644 "$SYSROOT/lib/libc-2.24.so" "$OUT/libc.so.6"
install -m 0644 "$SYSROOT/lib/libm-2.24.so" "$OUT/libm.so.6"
install -m 0644 "$SYSROOT/lib/libpthread-2.24.so" "$OUT/libpthread.so.0"
"$BUILD/patch-set-robust" "$OUT/libpthread.so.0"
# The sysroot copies carry debug symbols (13 MB for libc); the APK needs only
# the dynamic symbol table.
if [[ -x "$STRIP" ]]; then
    LD_LIBRARY_PATH="$TOOLCHAIN/hostlib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$STRIP" --strip-unneeded "$OUT/libc.so.6" "$OUT/libm.so.6" "$OUT/libpthread.so.0"
fi
# The loader is exec'd at this name; an asset cannot be a symlink, so the
# ld-2.24.so content is staged under the name the firmware's PT_INTERP and the
# other libraries' NEEDED entries use.
install -m 0755 "$SYSROOT/lib/ld-2.24.so" "$OUT/ld-linux-armhf.so.3"
if [[ -x "$STRIP" ]]; then
    # The cross binutils need their own libbfd and libisl, from hostlib.
    LD_LIBRARY_PATH="$TOOLCHAIN/hostlib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$STRIP" -o "$OUT/tracker-firmware-mini" "$BIN"
else
    install -m 0755 "$BIN" "$OUT/tracker-firmware-mini"
fi
chmod 0755 "$OUT/tracker-firmware-mini" "$OUT/ld-linux-armhf.so.3"

# ---- the checks ------------------------------------------------------------
#
# A library that is not ELF32 ARM would be silently skipped, and a NEEDED name
# with no file beside it makes the loader stop with "cannot open shared
# object"; the app shows that as "waiting for the firmware".
for f in ld-linux-armhf.so.3 libc.so.6 libm.so.6 libpthread.so.0 tracker-firmware-mini; do
    readelf -h "$OUT/$f" | grep -q 'Class:.*ELF32' || {
        echo "$0: $f is not ELF32" >&2; exit 1; }
    readelf -h "$OUT/$f" | grep -q 'Machine:.*ARM' || {
        echo "$0: $f is not ARM" >&2; exit 1; }
done
for need in $(readelf -d "$OUT/tracker-firmware-mini" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
    [[ "$need" == ld-linux-armhf.so.3 || -f "$OUT/$need" ]] || {
        echo "$0: the firmware needs $need and it is not staged" >&2; exit 1; }
done
"$BUILD/patch-set-robust" --check "$OUT/libpthread.so.0"

# ---- stage into the APK ----------------------------------------------------
for f in ld-linux-armhf.so.3 libc.so.6 libm.so.6 libpthread.so.0 tracker-firmware-mini; do
    cp "$OUT/$f" "$ASSETS/$f"
done
chmod 0755 "$ASSETS/ld-linux-armhf.so.3" "$ASSETS/tracker-firmware-mini"

echo "---- staged 5 files into $ASSETS ----"
ls -l "$ASSETS"
