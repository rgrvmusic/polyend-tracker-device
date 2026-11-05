/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
// Bits the firmware supplies from its platform layer (MK66/IMX), or from
// plain C files the probe build did not pick up.
#include <cmath>
#include <cstdint>

int16_t sin_q15(int16_t value)
{
    // arm_sin_q15: q15 angle, full circle = 0x10000, q15 sine output.
    double angle = (double)value * (2.0 * M_PI / 65536.0);
    long v = lround(std::sin(angle) * 32767.0);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static bool outputEnabled = false;
void muteAudioOutByKey() { outputEnabled = false; }
void unmuteAudioOutByKey() { outputEnabled = true; }
uint8_t isAudioOutMuted() { return outputEnabled ? 0 : 1; }

// The audio interrupt is the core's block clock; the host audio thread is the
// only caller, so these are bookkeeping no-ops.
void enableAudioInterrupt() {}
void disableAudioInterrupt() {}
