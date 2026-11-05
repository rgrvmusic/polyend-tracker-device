/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
// The two doors of the firmware host: an HTTP face (:8894) and a line-JSON
// door (:8895) on one poll() loop.
//
// The HTTP face serves the draw list EveDisplay publishes and accepts the same
// request objects as the line door. Nothing is rasterised in the firmware.
#ifndef TRACKER_HOST_SERVICE_H
#define TRACKER_HOST_SERVICE_H

#include <cstdint>

class EveDisplay;

class InputSink
{
  public:
    virtual ~InputSink() = default;
    virtual void button(int idx, bool down) = 0;
    virtual void pad(int idx, bool down) = 0;
    virtual void encoder(int delta) = 0;
    // Clears anything the firmware still holds, e.g. a key pressed when the
    // page went away; without it a stuck Shift or Rec changes what Play does.
    virtual void releaseAll() = 0;
    // Re-runs the firmware with the same arguments (the power cycle).
    virtual void restart() = 0;
};

struct ServiceOptions
{
    int httpPort = 8894;
    int linePort = 8895;
    const char* page = nullptr; // defaults to ../web/index.html next to the binary
};

bool service_start(EveDisplay* display, InputSink* input, const ServiceOptions& opts);
void service_stop();

// Renders the current frame as plain text: a fixed grid of characters taken
// from the draw list's text runs, for the text face and for diagnosis.
void service_frame_text(EveDisplay* display, char* out, int outSize, int cols, int rows);

#endif
