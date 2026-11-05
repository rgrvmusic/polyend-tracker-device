#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
#
# Usage:  run.sh [--audio SPEC] [--fire] [--tui] [--render-wav PATH]
#
#   run.sh                       silent panel on 8894 (web) and 8895 (line)
#   run.sh --audio aplay         sound through the default ALSA device
#   run.sh --audio 'cmd:aplay -q -t raw -f S16_LE -c 2 -r 44100'
#   run.sh --audio wav:/tmp/x.wav  record the live output
#   run.sh --fire                also start the Fire client against the door
#   run.sh --tui                 the text face in this terminal
#   run.sh --render-wav x.wav    render the pattern once and exit
#
# Env: PANEL_AUDIO  default sink (none | cmd:... | wav:PATH)
#      PANEL_HTTP   HTTP/SSE port (default 8894)
#      PANEL_LINE   line-JSON port (default 8895)
#      LIBFEUER     libfeuer checkout, if not a sibling of this repo
#      FIRE_OUT     the Fire client's --out spec (default raw:)
set -eu

SELF=$(cd "$(dirname "$0")" && pwd)          # emulator/
PANEL=$SELF/panel
BUILD=$SELF/build

AUDIO=${PANEL_AUDIO:-none}
HTTP_PORT=${PANEL_HTTP:-8894}
LINE_PORT=${PANEL_LINE:-8895}
FIRE=0
TUI=0
RENDER=
PASSTHRU=()

while [ $# -gt 0 ]; do
    case "$1" in
        --audio)      AUDIO=$2; shift 2 ;;
        --audio=*)    AUDIO=${1#*=}; shift ;;
        --fire)       FIRE=1; shift ;;
        --tui)        TUI=1; shift ;;
        --render-wav) RENDER=$2; shift 2 ;;
        --http)       HTTP_PORT=$2; shift 2 ;;
        --line)       LINE_PORT=$2; shift 2 ;;
        -h|--help)
            sed -n '4,28p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)            PASSTHRU+=("$1"); shift ;;
    esac
done

# Build what is missing, so a changed source is not a stale binary.
[ -x "$BUILD/tracker-paneld" ] || bash "$PANEL/c/build.sh"
if [ "$TUI" = "1" ] && [ ! -x "$BUILD/tracker-tui" ]; then
    bash "$PANEL/tui/build.sh"
fi

if [ -n "$RENDER" ]; then
    exec env PANEL_PAGE="$PANEL/web/index.html" \
        "$BUILD/tracker-paneld" --audio none --render-wav "$RENDER"
fi

# The audio sinks are the panel's own names.  `aplay` is shorthand for the
# command form, expanded only when the player exists.  The panel writes
# headerless PCM, so the format must be stated or aplay assumes 8 kHz mono.
case "$AUDIO" in
    aplay) AUDIO="cmd:aplay -q -t raw -f S16_LE -c 2 -r 44100" ;;
esac
if [ "$AUDIO" != "none" ] && [ "${AUDIO#cmd:}" != "$AUDIO" ]; then
    cmd=${AUDIO#cmd:}
    first=${cmd%% *}
    command -v "$first" >/dev/null || {
        echo "run.sh: audio command '$first' not found; use --audio none" >&2
        exit 1
    }
fi

PANEL_PAGE="$PANEL/web/index.html" "$BUILD/tracker-paneld" \
    --http "$HTTP_PORT" --line "$LINE_PORT" \
    --audio "$AUDIO" "${PASSTHRU[@]}" &
PANEL_PID=$!

cleanup() {
    [ -n "${FIRE_PID:-}" ] && kill "$FIRE_PID" 2>/dev/null || true
    kill "$PANEL_PID" 2>/dev/null || true
    wait "$PANEL_PID" 2>/dev/null || true
}
trap cleanup INT TERM EXIT

sleep 0.4
if [ "$FIRE" = "1" ]; then
    if [ ! -x "$BUILD/tracker-fire" ]; then
        echo "run.sh: no $BUILD/tracker-fire; building it" >&2
        bash "$PANEL/fire/build.sh" || exit 1
    fi
    "$BUILD/tracker-fire" --out "${FIRE_OUT:-raw:}" \
        --door "127.0.0.1:$LINE_PORT" &
    FIRE_PID=$!
fi

if [ "$TUI" = "1" ]; then
    "$BUILD/tracker-tui" --door "127.0.0.1:$LINE_PORT" || true
    exit 0
fi

echo
echo "web   http://$(hostname):$HTTP_PORT/"
echo "door  $(hostname):$LINE_PORT"
echo "audio $AUDIO"
[ "$FIRE" = "1" ] && echo "fire  ${FIRE_OUT:-raw:} (pid $FIRE_PID)"
echo "stop  Ctrl-C"
echo

wait "$PANEL_PID"

