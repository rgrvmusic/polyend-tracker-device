/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

#include "midi_bridge.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <jni.h>
#include <unistd.h>

namespace
{
std::atomic<int> gMidiFd{-1};
std::atomic<bool> gStop{false};
} // namespace

void midiBridgeSetFd(int fd)
{
    gMidiFd = fd;
}

void requestStop()
{
    gStop = true;
}

bool stopRequested()
{
    return gStop.load();
}

extern "C" {

// MidiHandler.nativeMidiIn(byte[]): bytes received from the controller.
JNIEXPORT void JNICALL Java_nl_rgrv_ptpocket_MidiHandler_nativeMidiIn(JNIEnv* env, jclass,
                                                                      jbyteArray data)
{
    const int fd = gMidiFd.load();
    if (fd < 0 || data == nullptr) return;
    const jsize n = env->GetArrayLength(data);
    if (n <= 0) return;
    jbyte buf[1024];
    if (n > (jsize)sizeof buf) return; // a chunk larger than this is not MIDI
    env->GetByteArrayRegion(data, 0, n, buf);
    for (int tries = 0; tries < 4; tries++)
    {
        ssize_t w = ::write(fd, buf, (size_t)n);
        if (w == n) return;
        if (w >= 0 || (errno != EAGAIN && errno != EINTR)) return;
    }
}

// MidiHandler.nativeMidiOut(): whatever the firmware sent since the last call.
JNIEXPORT jbyteArray JNICALL Java_nl_rgrv_ptpocket_MidiHandler_nativeMidiOut(JNIEnv* env, jclass)
{
    const int fd = gMidiFd.load();
    if (fd < 0) return nullptr;
    uint8_t buf[1024];
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n <= 0) return nullptr;
    jbyteArray out = env->NewByteArray((jsize)n);
    if (out == nullptr) return nullptr;
    env->SetByteArrayRegion(out, 0, (jsize)n, (const jbyte*)buf);
    return out;
}

JNIEXPORT void JNICALL Java_nl_rgrv_ptpocket_MainActivity_nativeStop(JNIEnv*, jclass)
{
    requestStop();
}
}
