/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* Host implementation of core/source/Profiler.cpp.
 *
 * The firmware version allocates and frees the whole heap so newlib's
 * mallinfo() covers it. A Linux process overcommits, so that loop never
 * stops; the measurements are kept and the heap claim uses host statistics. */
#include "Profiler.h"
#include "Tracker.h"
#include "Platform.h"

#include <algorithm>
#include <climits>
#include <malloc.h>

Profiler* Profiler::instance = nullptr;

void Profiler::init()
{
    resetMeasuringAudio();
}

size_t Profiler::freeRamBytes()
{
// mallinfo2 is glibc 2.33; the Android runtime stages glibc 2.24.
#if defined(__GLIBC__) && !defined(TRACKER_GLIBC224) && __GLIBC_PREREQ(2, 33)
    struct mallinfo2 mi = mallinfo2();
    return mi.fordblks;
#elif defined(__GLIBC__)
    struct mallinfo mi = mallinfo();
    return mi.fordblks;
#else
    return 0;
#endif
}

size_t Profiler::maxStackBytes()
{
    /* No watermark is placed on the host stack. */
    return 0;
}

void Profiler::resetMeasuringAudio()
{
    lastSampleUs = 0;
    sumOfSamples = 0;
    minSampleUs = INT_MAX;
    maxSampleUs = 0;
    samples = 0;
    tempMaxSampleUs = 0;
}

void Profiler::startMeasuringAudio()
{
    if (Tracker::getInstance()->getPlatform() == nullptr)
    {
        return;
    }
    audioUpdateTimer = Tracker::getInstance()->getPlatform()->getTimer()->clockCycles();
}

void Profiler::stopMeasuringAudio()
{
    if (Tracker::getInstance()->getPlatform() == nullptr)
    {
        return;
    }

    lastSampleUs =
      std::max<int>(
        Tracker::getInstance()->getPlatform()->getTimer()->clockCycles() - audioUpdateTimer, 0) /
      (float)Tracker::getInstance()->getPlatform()->getTimer()->clockCyclesPerUs();

    minSampleUs = std::min(minSampleUs, lastSampleUs);
    maxSampleUs = std::max(maxSampleUs, lastSampleUs);
    tempMaxSampleUs = std::max(tempMaxSampleUs, lastSampleUs);

    sumOfSamples += lastSampleUs;
    samples++;
}

void Profiler::getAudioProcessingUsageUs(AudioProcessorUsage& usage, bool resetTempMaximum)
{
    usage.average = samples > 0 ? sumOfSamples / samples : 0;
    usage.last = lastSampleUs;
    usage.maximum = maxSampleUs;
    usage.minimum = minSampleUs;
    usage.tempMaximum = tempMaxSampleUs;

    if (resetTempMaximum)
    {
        tempMaxSampleUs = 0;
    }
}

void Profiler::getAudioProcessingUsagePercent(AudioProcessorUsage& usage, bool resetTempMaximum)
{
    getAudioProcessingUsageUs(usage, resetTempMaximum);

    usage.average = calculatePercentProcessorUsage(usage.average);
    usage.last = calculatePercentProcessorUsage(usage.last);
    usage.maximum = calculatePercentProcessorUsage(usage.maximum);
    usage.minimum = calculatePercentProcessorUsage(usage.minimum);
    usage.tempMaximum = calculatePercentProcessorUsage(usage.tempMaximum);
}

int Profiler::calculatePercentProcessorUsage(int processorUsageUs)
{
    return (100 * processorUsageUs) / (int)kMaxAudioUpdateTime;
}
