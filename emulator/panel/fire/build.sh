#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
# build.sh -- compile the Fire client.
#
# Output goes in emulator/build/.  A static link is tried first because
# glibc-static is not installed everywhere.
#
# libfeuer is a checkout of its own.  It is found by $LIBFEUER, then as a
# sibling of this repo, then under emulator/.  Only feuer_oled.c is linked;
# everything else used here is header-only.
#
# Usage:  build.sh [--clean] [TARGET...]      (default: tracker-fire)
# Env:    CC, CFLAGS, STATIC, LIBFEUER

set -eu

SELF=$(cd "$(dirname "$0")" && pwd)
EMU=$(cd "$SELF/../.." && pwd)
REPO=$(cd "$EMU/.." && pwd)
OUT=$EMU/build

CC=${CC:-cc}
EXTRA_CFLAGS=${CFLAGS:-}
WANT_STATIC=${STATIC:-auto}

CFLAGS="-O2 -Wall -Wextra -std=gnu99 ${EXTRA_CFLAGS}"

clean=0
targets=()
for arg in "$@"; do
    case "$arg" in
        --clean) clean=1 ;;
        -*) echo "build.sh: unknown option $arg" >&2; exit 2 ;;
        *) targets+=("$arg") ;;
    esac
done
[ ${#targets[@]} -eq 0 ] && targets=(tracker-fire)

command -v "$CC" >/dev/null || {
    echo "build.sh: no $CC -- install a C compiler, or set CC" >&2
    exit 1
}

if [ -z "${LIBFEUER:-}" ]; then
    for cand in "$REPO/../libfeuer" "$EMU/libfeuer" "$REPO/libfeuer"; do
        if [ -f "$cand/include/feuer_oled.h" ]; then LIBFEUER=$cand; break; fi
    done
fi
if [ -z "${LIBFEUER:-}" ] || [ ! -f "$LIBFEUER/include/feuer_oled.h" ]; then
    cat >&2 <<'EOF'
build.sh: cannot find libfeuer.

  Set LIBFEUER=/path/to/libfeuer, or put a checkout where one of these would be:
      a sibling of this repo
      emulator/libfeuer
EOF
    exit 1
fi
echo "libfeuer: $LIBFEUER"

ALSA_CFLAGS=
ALSA_LIBS=
HAVE_ALSA=0
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists alsa 2>/dev/null; then
    ALSA_CFLAGS=$(pkg-config --cflags alsa)
    ALSA_LIBS=$(pkg-config --libs alsa)
    HAVE_ALSA=1
    echo "alsa:     $(pkg-config --modversion alsa)"
else
    echo "alsa:     not found -- use --out raw:/dev/snd/midiC?D? for a Fire"
fi

mkdir -p "$OUT"

target_sources() {
    case $1 in
        tracker-fire) echo "tracker-fire.c door.c json.c midi.c" ;;
        *)            echo "$1.c" ;;
    esac
}

HAVE_ALSA_DEFINE=
[ "$HAVE_ALSA" = "1" ] && HAVE_ALSA_DEFINE="-DHAVE_ALSA"

compile() {
    t=$1; mode=$2
    srcs=
    for s in $(target_sources "$t"); do srcs="$srcs $SELF/$s"; done
    [ "$HAVE_ALSA" = "1" ] && srcs="$srcs $SELF/midi_alsa.c"
    # shellcheck disable=SC2086
    $CC $CFLAGS -I"$SELF" -I"$LIBFEUER/include" $ALSA_CFLAGS $HAVE_ALSA_DEFINE \
        $mode -o "$OUT/$t" $srcs "$LIBFEUER/src/feuer_oled.c" $ALSA_LIBS -lm \
        2>"$OUT/$t.build.log"
}

link_mode=dynamic
if [ "$clean" = "1" ]; then
    for t in "${targets[@]}"; do rm -f "$OUT/$t"; done
fi

if [ "$WANT_STATIC" != "0" ]; then
    first=${targets[0]}
    if compile "$first" "-static"; then
        link_mode=static
    elif [ "$WANT_STATIC" = "1" ]; then
        echo "build.sh: a static link was asked for and this host cannot do it:" >&2
        sed 's/^/    /' "$OUT/$first.build.log" >&2
        exit 1
    else
        echo "Note: no static libc here ($(grep -m1 -i 'error\|undefined' \
            "$OUT/$first.build.log" || sed -n '1p' "$OUT/$first.build.log"))"
        echo "      building dynamic; STATIC=1 to make that an error"
    fi
fi

for t in "${targets[@]}"; do
    [ -f "$SELF/$(target_sources "$t" | awk '{print $1}')" ] || {
        echo "build.sh: no source for $t" >&2; exit 1; }
    if [ "$link_mode" = "dynamic" ] || [ "$t" != "${targets[0]}" ]; then
        compile "$t" "" || {
            echo "build.sh: $CC failed on $t:" >&2
            sed 's/^/    /' "$OUT/$t.build.log" >&2
            exit 1
        }
    fi
    rm -f "$OUT/$t.build.log"
    echo
    echo "--- artifact: $t ($link_mode) ---"
    ls -l "$OUT/$t"
done

echo
echo "Hardware:  $OUT/tracker-fire --out raw: --door 127.0.0.1:8895"
echo "No Fire:   $OUT/tracker-fire --out stdout --door 127.0.0.1:8895 -v"
echo "Ports:     $OUT/tracker-fire --out raw: --list"
