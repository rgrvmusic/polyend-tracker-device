#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
#
# Downloads libc6 and libc6-dev from the Debian archive and unpacks them into
# DIR (default: sysroot/ at the repo root).
#
# libc6 holds the glibc 2.24 the app stages;
# libc6-dev holds the crt objects and linker scripts the firmware links against.
#
# The four runtime files are also copied flat into DIR/lib,
# the names build-runtime.sh reads:
#
#   ld-2.24.so  libc-2.24.so  libm-2.24.so  libpthread-2.24.so
#
# Usage:  android/tools/fetch-sysroot.sh [DIR]
# Env:    MIRROR, SUITE, ARCH
set -euo pipefail

SELF="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SELF/../.." && pwd)"
OUT="${1:-$REPO/sysroot}"
MIRROR="${MIRROR:-https://archive.debian.org/debian}"
SUITE="${SUITE:-stretch}"
ARCH="${ARCH:-armhf}"
CURL="curl -fsSL --retry 3 --retry-delay 2 --connect-timeout 20 --max-time 900"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== index: $SUITE/$ARCH"
$CURL -o "$WORK/Packages.gz" \
    "$MIRROR/dists/$SUITE/main/binary-$ARCH/Packages.gz"
gunzip -c "$WORK/Packages.gz" >"$WORK/Packages"

mkdir -p "$OUT"
for pkg in libc6 libc6-dev; do
    file="$(awk -v pkg="$pkg" '$1 == "Package:" { in_pkg = ($2 == pkg) }
         in_pkg && $1 == "Filename:" { print $2; exit }' "$WORK/Packages")"
    [[ -n "$file" ]] || { echo "$0: no $pkg in the index" >&2; exit 1; }
    echo "== $pkg: $file"
    $CURL -o "$WORK/$pkg.deb" "$MIRROR/$file"
    mkdir "$WORK/$pkg"
    ( cd "$WORK/$pkg" && ar x "../$pkg.deb" )
    data="$(ls "$WORK/$pkg"/data.tar.* | head -1)"
    tar -xpf "$data" -C "$OUT"
done

# Debian installs armhf libraries under lib/arm-linux-gnueabihf and
# usr/lib/arm-linux-gnueabihf.  gcc's start files and the -L in
# build-runtime.sh use the flat dirs, so mirror the subtree there.  The
# linker scripts keep their absolute paths, which ld resolves under --sysroot.
for d in lib usr/lib; do
    [[ -d "$OUT/$d/arm-linux-gnueabihf" ]] &&
        cp -a "$OUT/$d/arm-linux-gnueabihf/." "$OUT/$d/"
done

# Debian's dev symlinks are absolute (/lib/arm-linux-gnueabihf/libm.so.6).
# --sysroot does not redirect symlinks the linker opens, so make them relative
# to their own directory.
while IFS= read -r l; do
    t="$(readlink "$l")"
    case "$t" in
        /*) ln -sfn "$(realpath -m --relative-to="$(dirname "$l")" "$OUT$t")" "$l" ;;
    esac
done < <(find "$OUT" -type l)

# The flat copies: build-runtime.sh checks DIR/lib/<name> before staging.
mkdir -p "$OUT/lib"
for f in ld-2.24.so libc-2.24.so libm-2.24.so libpthread-2.24.so; do
    src="$(find "$OUT/lib" "$OUT/usr/lib" -name "$f" -type f | head -1)"
    [[ -n "$src" ]] || { echo "$0: $f is not in libc6" >&2; exit 1; }
    [[ "$src" == "$OUT/lib/$f" ]] || install -m 0755 "$src" "$OUT/lib/$f"
done

echo "== installed into $OUT"
ls -l "$OUT/lib"
