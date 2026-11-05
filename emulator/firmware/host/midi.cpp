/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
#include "midi.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <strings.h>
#include <unistd.h>

namespace
{
// The /proc name of a rawmidi node, e.g. "U2MIDI Pro".
bool portName(int card, int dev, char* out, size_t len)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/asound/card%d/midi%d", card, dev);
    FILE* f = fopen(path, "r");
    if (f == nullptr) return false;
    out[0] = 0;
    if (fgets(out, (int)len, f) == nullptr) out[0] = 0;
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return true;
}

int openAuto(char* chosen, size_t len)
{
    char node[64], name[128];
    for (int card = 0; card < 8; card++)
        for (int dev = 0; dev < 4; dev++)
        {
            snprintf(node, sizeof(node), "/dev/snd/midiC%dD%d", card, dev);
            if (access(node, R_OK | W_OK) != 0) continue;
            portName(card, dev, name, sizeof(name));
            // The emulator panel has its own MIDI client; skip its node.
            if (strcasestr(name, "fire") != nullptr) continue;
            int fd = ::open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            snprintf(chosen, len, "%s", node);
            return fd;
        }
    return -1;
}

int needFor(uint8_t status)
{
    switch (status & 0xF0)
    {
        case 0x80: return 2;
        case 0x90: return 2;
        case 0xA0: return 2;
        case 0xB0: return 2;
        case 0xC0: return 1;
        case 0xD0: return 1;
        case 0xE0: return 2;
        default: return 0;
    }
}
} // namespace

bool MidiPort::open(const char* path)
{
    if (path == nullptr || !strcmp(path, "none")) return false;
    if (*path == 0 || !strcmp(path, "auto"))
    {
        snprintf(path_, sizeof(path_), "auto");
        fd_ = openAuto(path_, sizeof(path_));
        if (fd_ < 0) snprintf(path_, sizeof(path_), "none");
        return fd_ >= 0;
    }
    // "fd:N" is an inherited descriptor already carrying raw MIDI bytes in
    // both directions; the Android wrapper passes one end of a socketpair.
    if (!strncmp(path, "fd:", 3))
    {
        snprintf(path_, sizeof(path_), "%s", path);
        fd_ = atoi(path + 3);
        return fd_ >= 0;
    }
    snprintf(path_, sizeof(path_), "%s", path);
    fd_ = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    return fd_ >= 0;
}

bool MidiPort::readMore()
{
    uint8_t buf[256];
    bool any = false;
    for (;;)
    {
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0)
        {
            bytes_.insert(bytes_.end(), buf, buf + n);
            any = true;
            continue;
        }
        break;
    }
    return any;
}

bool MidiPort::isReadAvailable()
{
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ < 0) return false;
    if (bytes_.empty()) readMore();
    return !bytes_.empty();
}

int MidiPort::read(uint8_t* message)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ < 0) return 0;
    if (bytes_.empty()) readMore();
    if (bytes_.empty()) return 0;

    uint8_t b = bytes_.front();
    bytes_.erase(bytes_.begin());

    // Real-time bytes may appear between any two other bytes.
    if (b >= 0xF8)
    {
        message[0] = b;
        return 1;
    }
    if (b == 0xF7)
    {
        sysex_ = false;
        status_ = 0;
        dataLen_ = 0;
        return 0;
    }
    if (b >= 0xF0)
    {
        sysex_ = (b == 0xF0);
        status_ = b;
        dataLen_ = 0;
        dataNeed_ = (b == 0xF1 || b == 0xF3) ? 1 : (b == 0xF2 ? 2 : 0);
        return 0; // system common is not used by the tracker
    }
    if (b & 0x80)
    {
        status_ = b;
        sysex_ = false;
        dataLen_ = 0;
        dataNeed_ = needFor(b);
        return 0;
    }
    if (sysex_ || status_ == 0 || dataNeed_ == 0) return 0;

    data_[dataLen_++] = b;
    if (dataLen_ < dataNeed_) return 0;

    message[0] = status_;
    message[1] = data_[0];
    message[2] = dataNeed_ > 1 ? data_[1] : 0;
    int len = 1 + dataNeed_;
    dataLen_ = 0;
    if (status_ >= 0xF0) status_ = 0; // no running status for system common
    return len;
}

void MidiPort::send(const uint8_t* data, int len)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ < 0 || len <= 0) return;
    for (int tries = 0; tries < 20; tries++)
    {
        ssize_t n = ::write(fd_, data, (size_t)len);
        if (n == len) return;
        if (n > 0)
        {
            data += n;
            len -= (int)n;
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) return;
        usleep(500);
    }
}
