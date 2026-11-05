#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
# build the Tracker service
set -eu

SELF=$(cd "$(dirname "$0")" && pwd)
EMU=$(cd "$SELF/../.." && pwd)
OUT=$EMU/build
JSON=$SELF/../fire

mkdir -p "$OUT"

CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -std=c11 -Wall -Wextra}

"$CC" $CFLAGS -I"$JSON" -pthread -o "$OUT/tracker-paneld" \
    "$SELF/tracker-paneld.c" "$JSON/json.c" -lm

echo "built $OUT/tracker-paneld"
