/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

// Java-to-native MIDI and stop hooks.
//
// Everything here only touches atomics, because Java calls these from its own
// thread while the panel runs on SDL's thread.
#pragma once

// The firmware's end of the MIDI socketpair, or -1 when there is no child.
void midiBridgeSetFd(int fd);

// Set by MainActivity.nativeStop(); the panel loop is what acts on it.
void requestStop();
bool stopRequested();
