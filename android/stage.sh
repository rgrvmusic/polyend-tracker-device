#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)
# Builds the firmware runtime and stages it into the APK assets.
# The work is in tools/build-runtime.sh. Run this before the Gradle build.
set -euo pipefail
exec "$(cd "$(dirname "$0")" && pwd)/tools/build-runtime.sh" "$@"
