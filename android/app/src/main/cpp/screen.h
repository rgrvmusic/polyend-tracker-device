/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

// The FT812 draw list, as the line door publishes it, parsed into a plain list
// for the renderer. One connection carries both frames and button requests.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum FrameOpKind
{
    OP_CLEAR,
    OP_SCISSOR,
    OP_RECT,
    OP_LINE,
    OP_POINT,
    OP_TEXT,
    OP_ICON,
    OP_GRADIENT
};

struct FrameOp
{
    FrameOpKind kind = OP_CLEAR;
    int x = 0, y = 0, w = 0, h = 0, x2 = 0, y2 = 0;
    int px = 0;        // text: glyph size
    int sprite = 0;    // icon
    int size = 0;      // point diameter / line width
    int gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
    uint32_t color = 0xFFFFFF, color2 = 0xFFFFFF;
    unsigned char alpha = 255;
    unsigned char blend = 0; // 1 = additive blend
    std::string text;
    std::vector<int> xs; // per-character pen positions
};

struct SpriteMeta
{
    int id = 0, w = 0, h = 0, stride = 0, bpp = 0, fmt = 0;
};

struct Frame
{
    uint32_t seq = 0;
    std::vector<FrameOp> ops;
    std::vector<SpriteMeta> sprites;
};

// A sprite mask as /sprite?id=N serves it: an 8-byte LE header, then the bits.
struct SpriteImage
{
    int w = 0, h = 0, stride = 0, bpp = 0, fmt = 0;
    std::vector<uint8_t> data;
};

bool fetchSprite(int id, SpriteImage& out);

class ControlLink
{
  public:
    // 127.0.0.1:8895, the line door.  Retries are the caller's business.
    bool connectDoor();
    void close();
    bool connected() const { return fd_ >= 0; }

    // Subscribes once and then drains whatever arrived: returns true when a
    // frame newer than the last one was parsed.
    bool pollFrame(Frame& out);

    void sendButton(int id, bool down);

  private:
    int fd_ = -1;
    std::string in_;
    uint32_t lastSeq_ = 0;
    bool haveSeq_ = false;
};
