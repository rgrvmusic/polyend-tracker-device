/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
#include "audio.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Kernel PCM interface (the UAPI alsa-lib itself uses).
#include <sound/asound.h>

namespace
{
void maskAny(snd_mask* m)
{
    for (size_t i = 0; i < sizeof(m->bits) / sizeof(m->bits[0]); i++) m->bits[i] = ~0u;
}

void maskNone(snd_mask* m)
{
    memset(m->bits, 0, sizeof(m->bits));
}

void maskSet(snd_mask* m, unsigned v)
{
    m->bits[v / 32] |= 1u << (v % 32);
}

void intervalAny(snd_interval* i)
{
    i->min = 0;
    i->max = ~0u;
    i->openmin = 0;
    i->openmax = 0;
    i->integer = 0;
    i->empty = 0;
}

void intervalSet(snd_interval* i, unsigned v)
{
    i->min = i->max = v;
    i->openmin = i->openmax = 0;
    i->integer = 1;
    i->empty = 0;
}

void intervalRange(snd_interval* i, unsigned lo, unsigned hi)
{
    i->min = lo;
    i->max = hi;
    i->openmin = i->openmax = 0;
    i->integer = 0;
    i->empty = 0;
}

snd_mask* masks(snd_pcm_hw_params* p, int param)
{
    return &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

snd_interval* intervals(snd_pcm_hw_params* p, int param)
{
    return &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

// Two channels, S16_LE, interleaved, at an exact rate.  Period and buffer are
// left as ranges so the driver can pick what it supports.
void fillParams(snd_pcm_hw_params* p, unsigned rate)
{
    memset(p, 0, sizeof(*p));
    p->flags = 0;

    for (int i = SNDRV_PCM_HW_PARAM_FIRST_MASK; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; i++)
        maskAny(masks(p, i));
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; i++)
        intervalAny(intervals(p, i));

    maskNone(masks(p, SNDRV_PCM_HW_PARAM_ACCESS));
    maskSet(masks(p, SNDRV_PCM_HW_PARAM_ACCESS), SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    maskNone(masks(p, SNDRV_PCM_HW_PARAM_FORMAT));
    maskSet(masks(p, SNDRV_PCM_HW_PARAM_FORMAT), SNDRV_PCM_FORMAT_S16_LE);
    maskNone(masks(p, SNDRV_PCM_HW_PARAM_SUBFORMAT));
    maskSet(masks(p, SNDRV_PCM_HW_PARAM_SUBFORMAT), SNDRV_PCM_SUBFORMAT_STD);

    intervalSet(intervals(p, SNDRV_PCM_HW_PARAM_SAMPLE_BITS), 16);
    intervalSet(intervals(p, SNDRV_PCM_HW_PARAM_FRAME_BITS), 32);
    intervalSet(intervals(p, SNDRV_PCM_HW_PARAM_CHANNELS), 2);
    intervalSet(intervals(p, SNDRV_PCM_HW_PARAM_RATE), rate);
    intervalRange(intervals(p, SNDRV_PCM_HW_PARAM_PERIOD_TIME), 5000, 100000);
    intervalRange(intervals(p, SNDRV_PCM_HW_PARAM_BUFFER_TIME), 20000, 400000);
}

// The first card that has a playback node.
int autoDevice()
{
    char path[64];
    for (int card = 0; card < 8; card++)
        for (int dev = 0; dev < 4; dev++)
        {
            snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%dp", card, dev);
            if (access(path, W_OK) == 0) return card * 4 + dev;
        }
    return -1;
}

int autoDevicePath(char* out, size_t len)
{
    int id = autoDevice();
    if (id < 0) return -1;
    snprintf(out, len, "/dev/snd/pcmC%dD%dp", id / 4, id % 4);
    return 0;
}
} // namespace

bool AudioOut::open(double sourceRate)
{
    sourceRate_ = sourceRate;
    const char* spec = getenv("TRACKER_AUDIO");
    if (spec == nullptr || *spec == 0 || !strcmp(spec, "auto"))
    {
        char path[64];
        if (autoDevicePath(path, sizeof(path)) == 0) return openDevice(path);
        snprintf(what_, sizeof(what_), "none (no PCM device found)");
        return false;
    }
    if (!strcmp(spec, "none"))
    {
        opened_ = false;
        return false;
    }
    if (!strncmp(spec, "wav:", 4)) return openWav(spec + 4);
    if (!strncmp(spec, "fd:", 3)) return openStream(atoi(spec + 3), 44100);
    return openDevice(spec);
}

// An inherited descriptor, as the Android wrapper's socketpair passes.  The
// parent reads at 44.1 kHz; write() blocking on a full socket buffer paces the
// firmware's audio thread, as the PCM buffer does for a device.
bool AudioOut::openStream(int fd, unsigned rate)
{
    if (fd < 0)
    {
        snprintf(what_, sizeof(what_), "none (fd %d is not valid)", fd);
        return false;
    }
    fd_ = fd;
    stream_ = true;
    rate_ = rate;
    ratio_ = sourceRate_ / (double)rate_;
    opened_ = true;
    snprintf(what_, sizeof(what_), "fd %d (%u Hz)", fd, rate_);
    return true;
}

bool AudioOut::openWav(const char* path)
{
    wav_ = fopen(path, "wb");
    if (wav_ == nullptr)
    {
        snprintf(what_, sizeof(what_), "none (wav %s: %s)", path, strerror(errno));
        return false;
    }
    // Capture at the firmware's own rate: no resampling, exact loop points.
    rate_ = (unsigned)lround(sourceRate_);
    // A 44-byte header; the two sizes are patched when the file is closed.
    const uint32_t zero = 0, sixteen = 16, fmt = 1, channels = 2, bits = 16;
    uint32_t rate = rate_;
    uint32_t blockAlign = channels * bits / 8;
    uint32_t byteRate = rate * blockAlign;
    fwrite("RIFF", 1, 4, wav_);
    fwrite(&zero, 4, 1, wav_);
    fwrite("WAVEfmt ", 1, 8, wav_);
    fwrite(&sixteen, 4, 1, wav_);
    fwrite(&fmt, 2, 1, wav_);
    fwrite(&channels, 2, 1, wav_);
    fwrite(&rate, 4, 1, wav_);
    fwrite(&byteRate, 4, 1, wav_);
    fwrite(&blockAlign, 2, 1, wav_);
    fwrite(&bits, 2, 1, wav_);
    fwrite("data", 1, 4, wav_);
    fwrite(&zero, 4, 1, wav_);
    wavBytes_ = 0;
    ratio_ = sourceRate_ / (double)rate_;
    opened_ = true;
    snprintf(what_, sizeof(what_), "wav %s (%u Hz)", path, rate_);
    return true;
}

bool AudioOut::openDevice(const char* path)
{
    fd_ = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd_ < 0)
    {
        snprintf(what_, sizeof(what_), "none (%s: %s)", path, strerror(errno));
        return false;
    }

    static const unsigned rates[] = {44100, 48000, 96000, 32000};
    snd_pcm_hw_params params;
    bool ok = false;
    for (unsigned rate : rates)
    {
        fillParams(&params, rate);
        if (ioctl(fd_, SNDRV_PCM_IOCTL_HW_PARAMS, &params) == 0)
        {
            ok = true;
            break;
        }
    }
    if (!ok)
    {
        snprintf(what_, sizeof(what_), "none (%s: no S16_LE stereo rate)", path);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    // The driver refines the parameters in place; take the rate it chose.
    snd_interval* r = intervals(&params, SNDRV_PCM_HW_PARAM_RATE);
    rate_ = r->min;
    if (ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE, 0) < 0)
    {
        snprintf(what_, sizeof(what_), "none (%s: prepare: %s)", path, strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    ratio_ = sourceRate_ / (double)rate_;
    opened_ = true;
    snprintf(what_, sizeof(what_), "%s (%u Hz)", path, rate_);
    return true;
}

bool AudioOut::writeFrames(const int16_t* frames, int count)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        snd_xferi xfer;
        xfer.buf = (void*)frames;
        xfer.frames = (snd_pcm_uframes_t)count;
        xfer.result = 0;
        if (ioctl(fd_, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer) == 0) return true;
        if (errno != EPIPE) return false;
        // Underrun: drop the broken stream state and continue.
        ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE, 0);
    }
    return false;
}

bool AudioOut::writeStream(const int16_t* frames, int count)
{
    const char* p = (const char*)frames;
    size_t left = (size_t)count * sizeof(int16_t) * 2;
    while (left > 0)
    {
        ssize_t n = ::write(fd_, p, left);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            // EPIPE: the reader is gone; stop feeding rather than spinning.
            ::close(fd_);
            fd_ = -1;
            opened_ = false;
            return false;
        }
        if (n == 0) return false;
        p += n;
        left -= (size_t)n;
    }
    return true;
}

void AudioOut::writeWav(const int16_t* frames, int count)
{
    size_t n = fwrite(frames, sizeof(int16_t) * 2, (size_t)count, wav_);
    wavBytes_ += (uint32_t)(n * sizeof(int16_t) * 2);
}

void AudioOut::feed(const int16_t* left, const int16_t* right, int frames)
{
    if (!opened_ || frames <= 0) return;

    srcL_.insert(srcL_.end(), left, left + frames);
    srcR_.insert(srcR_.end(), right, right + frames);

    // Linear interpolation; the ratio is 1.0 for a WAV at the source rate.
    if (out_.capacity() < (size_t)frames * 2 + 2) out_.reserve((size_t)frames * 2 + 2);
    out_.clear();
    while (pos_ + 1.0 < (double)srcL_.size())
    {
        size_t i = (size_t)pos_;
        double f = pos_ - (double)i;
        double l = srcL_[i] + (srcL_[i + 1] - srcL_[i]) * f;
        double r = srcR_[i] + (srcR_[i + 1] - srcR_[i]) * f;
        out_.push_back((int16_t)lround(l));
        out_.push_back((int16_t)lround(r));
        pos_ += ratio_;
    }
    size_t drop = (size_t)pos_;
    if (drop > 0)
    {
        srcL_.erase(srcL_.begin(), srcL_.begin() + drop);
        srcR_.erase(srcR_.begin(), srcR_.begin() + drop);
        pos_ -= (double)drop;
    }
    if (out_.empty()) return;
    int outFrames = (int)(out_.size() / 2);
    if (wav_) writeWav(out_.data(), outFrames);
    else if (stream_) writeStream(out_.data(), outFrames);
    else writeFrames(out_.data(), outFrames);
}

void AudioOut::close()
{
    if (fd_ >= 0)
    {
        if (!stream_) ioctl(fd_, SNDRV_PCM_IOCTL_DRAIN, 0);
        ::close(fd_);
        fd_ = -1;
    }
    stream_ = false;
    if (wav_ != nullptr)
    {
        uint32_t riff = 36 + wavBytes_;
        fseek(wav_, 4, SEEK_SET);
        fwrite(&riff, 4, 1, wav_);
        fseek(wav_, 40, SEEK_SET);
        fwrite(&wavBytes_, 4, 1, wav_);
        fclose(wav_);
        wav_ = nullptr;
    }
    opened_ = false;
}
