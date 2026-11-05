#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
# build.sh -- build the text face.  No curses, no ALSA.
set -eu

SELF=$(cd "$(dirname "$0")" && pwd)
FIREDIR=$SELF/../fire
OUT=$(cd "$SELF/../.." && pwd)/build

mkdir -p "$OUT"

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -std=c11 -Wall -Wextra}

"$CC" $CFLAGS -I"$FIREDIR" -o "$OUT/tracker-tui" \
    "$SELF/tracker-tui.c" "$FIREDIR/door.c" "$FIREDIR/json.c" -lm

echo "built $OUT/tracker-tui"
