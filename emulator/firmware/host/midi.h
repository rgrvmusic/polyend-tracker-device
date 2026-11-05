/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
// A raw MIDI port for the host.
//
// The kernel exposes each MIDI interface as a rawmidi character device
// (/dev/snd/midiC*D*). Reading and writing those nodes directly gives the
// firmware the same byte streams its UART and USB MIDI drivers see, without
// linking an ALSA library.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

class MidiPort
{
  public:
    // path == nullptr disables the port.  "auto" (or an empty string) picks
    // the first rawmidi node that is not the emulator panel's; "none" disables.
    bool open(const char* path);
    bool isOpen() const { return fd_ >= 0; }
    const char* path() const { return path_; }

    // True when at least one byte is waiting (the firmware calls read() until
    // this turns false).
    bool isReadAvailable();
    // Consumes one byte; returns the message length when that byte completed a
    // message (1..3), 0 otherwise.
    int read(uint8_t* message);
    void send(const uint8_t* data, int len);

  private:
    bool readMore();

    int fd_ = -1;
    char path_[64] = "-";
    std::mutex mu_;
    std::vector<uint8_t> bytes_;

    // Running-status parser.
    uint8_t status_ = 0;
    uint8_t data_[2] = {0, 0};
    int dataLen_ = 0;
    int dataNeed_ = 0;
    bool sysex_ = false;
};
