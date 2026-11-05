/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

/* Pins the process ABI to armeabi-v7a. The firmware is armhf and the app is
 * forked 32-bit only when a real armeabi-v7a object is present in the APK;
 * abiFilters alone filters an empty set of libraries. */

const char *tracker_abi_anchor(void);

const char *tracker_abi_anchor(void)
{
    return "tracker pins its process ABI to armeabi-v7a; see abi_anchor.c";
}
