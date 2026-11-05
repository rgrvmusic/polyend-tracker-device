/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
// EVE/FT812 display model for the Polyend Tracker core.
//
// The core keeps retained display lists in RAM_G and builds a frame in RAM_DL.
// This model keeps the same memory and translates the assembled frame into a
// structured draw list: rectangles, lines, points, text runs and icon masks.
#ifndef TRACKER_EVE_H
#define TRACKER_EVE_H

#include "DisplayDriver.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

struct EveOp
{
    enum Kind : uint8_t
    {
        kClear = 0,
        kScissor,
        kRect,
        kLine,
        kPoint,
        kText,
        kIcon,
        kGradient,
    };

    uint8_t kind = kClear;
    int16_t x = 0, y = 0, w = 0, h = 0;
    uint32_t color = 0xFFFFFF;
    uint32_t color2 = 0; // gradient end colour
    uint8_t alpha = 255;
    uint8_t blend = 0; // 0 source-over, 1 additive, 2 source-only
    uint16_t size = 1; // line width / point size

    // Text: one op per run.  xs holds the absolute x of each character.
    std::string text;
    std::vector<uint16_t> xs;
    uint8_t font_px = 0;
    uint8_t advance = 0;

    // Icon mask
    uint32_t sprite = 0;

    // Gradient axis, in absolute screen coordinates.
    int16_t gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
};

struct EveSprite
{
    uint32_t id = 0;
    uint32_t address = 0;
    uint16_t w = 0, h = 0;
    uint16_t stride = 0;
    uint8_t bpp = 0;    // 2, 4 or 8
    uint8_t format = 0; // EVE format code
    std::vector<uint8_t> data;
};

struct EveFrame
{
    uint32_t seq = 0;
    std::vector<EveOp> ops;
    std::vector<EveSprite> sprite_meta; // no data; ids for /sprite
};

class EveDisplay : public DisplayDriver
{
  public:
    static constexpr int kWidth = 800;
    static constexpr int kHeight = 480;

    EveDisplay();

    // DisplayDriver
    void setPowerState(bool state) override;
    void setBrightness(int value) override;
    void setRotation(int value) override;
    void getImgProps(uint32_t& addr, uint32_t& w, uint32_t& h) override;
    uint32_t getDisplayListOffset() override;
    void IncCMDOffset(uint16_t value) override;
    void WriteData32(uint32_t data) override;
    void beginCoProList() override;
    void beginCoProListNoCheck() override;
    void endCoProList() override;
    void awaitCoProEmpty() override;
    uint8_t isCoProEmpty() override;
    void writeDataRAMG(const uint8_t* imgData, uint32_t dataSize, uint32_t destAddress) override;
    void writeFontRAMG(const uint8_t* imgData, uint32_t dataSize, uint32_t destAddress) override;
    void readDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t sourceAddress) override;
    void readBackDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t sourceAddress) override;
    uint8_t sendString(const char* string, uint32_t length) override;
    void writeDataToCMD(const uint8_t* imgData, uint32_t totalDataSize) override;
    void initStreamDma(uint16_t byteResolution, uint16_t streamWidth, uint16_t streamHeight,
                       uint8_t* streamBuf, uint32_t destMemAddress) override;
    bool updateStreamDma(bool lastFrame) override;
    void stopStreamDma() override;

    // Frame access for the service.  Copies the last assembled frame.
    bool copyFrame(EveFrame& out);
    bool getSprite(uint32_t id, EveSprite& out);
    void dumpFrameText(FILE* out);

  private:
    struct BitmapProps
    {
        uint32_t source = 0;
        uint32_t format = 0;
        uint32_t linestride = 0;
        uint32_t height = 0;
        uint32_t width = 0;
        bool valid = false;
    };

    struct Font
    {
        uint32_t ptr = 0;
        uint16_t first_char = 0;
        bool valid = false;
    };

    struct RenderState
    {
        uint32_t color = 0xFFFFFF;
        uint32_t clear_color = 0;
        uint8_t clear_alpha = 255;
        uint8_t alpha = 255;
        uint8_t blend = 0;
        uint16_t line_width = 1;
        uint16_t point_size = 1;
        int scissor_x = 0, scissor_y = 0;
        int scissor_w = kWidth, scissor_h = kHeight;
        uint8_t vertex_frac = 4;
        int begin = -1; // -1 none
        uint8_t cell = 0;
        uint8_t handle = 0;
    };

    // memory
    std::vector<uint8_t> ram_g_;
    std::vector<uint8_t> ram_dl_;
    uint32_t dl_ptr_ = 0;
    bool in_dl_ = false;
    std::vector<uint8_t> fifo_;
    BitmapProps bitmaps_[32];
    Font fonts_[32];

    int brightness_ = 255;
    size_t last_ops_ = 0;
    int rotation_ = 0;

    // published frame
    std::mutex mu_;
    EveFrame frame_;
    uint32_t seq_ = 0;

    // sprites
    std::mutex sprite_mu_;
    std::vector<EveSprite> sprites_;

    // execution
    void executeFifo();
    void cmdMemcpy(uint32_t dest, uint32_t src, uint32_t num);
    void cmdAppend(uint32_t ptr, uint32_t num);
    void dlStart();
    void renderList();
    void resetRenderState(RenderState& st);

    // helpers
    uint32_t rd32(const std::vector<uint8_t>& b, size_t off) const;
    void wr32(std::vector<uint8_t>& b, size_t off, uint32_t v);
    void dlAppendWord(uint32_t w);
    void dlAppendBytes(const uint8_t* p, size_t n);
    int16_t sx15(uint32_t v) const;
    int16_t sx16(uint32_t v) const;
    bool fontGlyph(int handle, uint8_t ch, uint8_t& advance, uint8_t& height) const;
    int fontStringWidth(int handle, const std::string& s, bool mono) const;
    uint32_t spriteFor(uint32_t source, uint32_t fmt, uint32_t stride, uint32_t width,
                       uint32_t height, uint32_t cell);
    void emitGlyph(const RenderState& st, std::vector<EveOp>& ops, int handle, int x, int y,
                   uint8_t ch);
    void emitText(const RenderState& st, std::vector<EveOp>& ops, int handle, int x, int y,
                  uint16_t options, const std::string& s);
    void emitNumber(const RenderState& st, std::vector<EveOp>& ops, int handle, int x, int y,
                    uint16_t options, int32_t n);
    void emitIcon(const RenderState& st, std::vector<EveOp>& ops, int handle, int x, int y,
                  uint8_t cell);
    void emitScissor(const RenderState& st, std::vector<EveOp>& ops);
    void flushVertices(const RenderState& st, std::vector<EveOp>& ops,
                       std::vector<std::pair<int, int>>& verts);
};

#endif
