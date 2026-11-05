/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
// Audio output for the host. The firmware renders 16-bit stereo at the MK66
// I2S rate (44117.647 Hz); this sink resamples to the playback device's rate
// and writes through the kernel PCM character device, so no ALSA library is
// linked.
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

class AudioOut
{
  public:
    // TRACKER_AUDIO:
    //   unset / "auto"     first playback PCM device
    //   "none"             discard (the firmware still renders)
    //   "wav:PATH"         write the same samples to a WAV file
    //   "fd:N"             write interleaved S16 stereo at 44.1 kHz to an
    //                      inherited file descriptor (the Android wrapper
    //                      passes one end of a socketpair, and its reader
    //                      paces the firmware)
    //   "/dev/snd/pcm..."  that device
    bool open(double sourceRate);
    // Resamples and writes one block; a device sink blocks on room in the
    // buffer, so this call paces the audio thread.
    void feed(const int16_t* left, const int16_t* right, int frames);
    void close();

    bool isOpen() const { return opened_; }
    // A PCM device blocks on its buffer and so paces the audio thread; a WAV
    // file does not.
    bool paces() const { return fd_ >= 0; }
    const char* describe() const { return what_; }

  private:
    bool openDevice(const char* path);
    bool openWav(const char* path);
    bool openStream(int fd, unsigned rate);
    bool writeFrames(const int16_t* frames, int count);
    bool writeStream(const int16_t* frames, int count);
    void writeWav(const int16_t* frames, int count);

    int fd_ = -1;
    bool opened_ = false;
    // fd_ is a pipe/socket rather than a PCM device: write() instead of the
    // PCM ioctl, and the reader's backpressure is what paces the thread.
    bool stream_ = false;
    double sourceRate_ = 44117.6470588;
    unsigned rate_ = 44100;
    double ratio_ = 1.0;
    std::vector<int16_t> srcL_, srcR_;
    double pos_ = 0.0;
    std::vector<int16_t> out_;
    FILE* wav_ = nullptr;
    uint32_t wavBytes_ = 0;
    char what_[160] = "none";
};
