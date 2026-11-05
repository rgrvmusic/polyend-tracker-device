/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
#include "eve.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
constexpr uint32_t kRamGSize = 1024 * 1024;
constexpr uint32_t kRamDlSize = 8 * 1024;
constexpr uint32_t kRamDlBase = 0x300000;

bool eveDebug()
{
    static const bool on = getenv("TRACKER_EVE_DEBUG") != nullptr;
    return on;
}

// Display-list opcodes (the high byte of a display-list word).
constexpr uint8_t DL_DISPLAY = 0x00;
constexpr uint8_t DL_BITMAP_SOURCE = 0x01;
constexpr uint8_t DL_CLEAR_COLOR_RGB = 0x02;
constexpr uint8_t DL_COLOR_RGB = 0x04;
constexpr uint8_t DL_BITMAP_HANDLE = 0x05;
constexpr uint8_t DL_CELL = 0x06;
constexpr uint8_t DL_BITMAP_LAYOUT = 0x07;
constexpr uint8_t DL_BITMAP_SIZE = 0x08;
constexpr uint8_t DL_ALPHA_FUNC = 0x09;
constexpr uint8_t DL_BLEND_FUNC = 0x0B;
constexpr uint8_t DL_POINT_SIZE = 0x0D;
constexpr uint8_t DL_LINE_WIDTH = 0x0E;
constexpr uint8_t DL_CLEAR_COLOR_A = 0x0F;
constexpr uint8_t DL_COLOR_A = 0x10;
constexpr uint8_t DL_CLEAR = 0x26;
constexpr uint8_t DL_SCISSOR_XY = 0x1B;
constexpr uint8_t DL_SCISSOR_SIZE = 0x1C;
constexpr uint8_t DL_BEGIN = 0x1F;
constexpr uint8_t DL_END = 0x21;
constexpr uint8_t DL_SAVE_CONTEXT = 0x22;
constexpr uint8_t DL_RESTORE_CONTEXT = 0x23;
constexpr uint8_t DL_VERTEX_FORMAT = 0x27;

// Coprocessor commands, low byte of 0xFFFFFFxx.
constexpr uint8_t CMD_GRADIENT = 0x0B;
constexpr uint8_t CMD_TEXT = 0x0C;
constexpr uint8_t CMD_SETMATRIX = 0x1A;
constexpr uint8_t CMD_MEMSET = 0x1B;
constexpr uint8_t CMD_MEMZERO = 0x1C;
constexpr uint8_t CMD_MEMCPY = 0x1D;
constexpr uint8_t CMD_APPEND = 0x1E;
constexpr uint8_t CMD_LOADIMAGE = 0x24;
constexpr uint8_t CMD_GETPROPS = 0x25;
constexpr uint8_t CMD_LOADIDENTITY = 0x26;
constexpr uint8_t CMD_TRANSLATE = 0x27;
constexpr uint8_t CMD_SCALE = 0x28;
constexpr uint8_t CMD_ROTATE = 0x29;
constexpr uint8_t CMD_NUMBER = 0x2E;
constexpr uint8_t CMD_SNAPSHOT2 = 0x37;
constexpr uint8_t CMD_SETBASE = 0x38;
constexpr uint8_t CMD_SETFONT2 = 0x3B;
constexpr uint8_t CMD_SETBITMAP = 0x43;

constexpr uint16_t OPT_MONO = 1;
constexpr uint16_t OPT_CENTERX = 512;
constexpr uint16_t OPT_CENTERY = 1024;
constexpr uint16_t OPT_RIGHTX = 2048;

// Number of parameter words that follow a co-processor command, or -1 when the
// command is not known (the replay then stops rather than desynchronising).
int paramCount(uint8_t cmd)
{
    switch (cmd)
    {
        case CMD_GRADIENT: return 4;
        case CMD_NUMBER: return 3;
        case CMD_SETBITMAP: return 3;
        case CMD_SETFONT2: return 3;
        case CMD_LOADIMAGE: return 2;
        case CMD_GETPROPS: return 3;
        case CMD_LOADIDENTITY: return 0;
        case CMD_SETMATRIX: return 0;
        case CMD_TRANSLATE: return 2;
        case CMD_SCALE: return 2;
        case CMD_ROTATE: return 1;
        case CMD_SNAPSHOT2: return 4;
        case CMD_SETBASE: return 1;
        default: return -1;
    }
}
} // namespace

EveDisplay::EveDisplay()
  : ram_g_(kRamGSize, 0)
  , ram_dl_(kRamDlSize, 0)
{
}

uint32_t EveDisplay::rd32(const std::vector<uint8_t>& b, size_t off) const
{
    if (off + 4 > b.size()) return 0;
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) | ((uint32_t)b[off + 2] << 16) |
           ((uint32_t)b[off + 3] << 24);
}

void EveDisplay::wr32(std::vector<uint8_t>& b, size_t off, uint32_t v)
{
    if (off + 4 > b.size()) return;
    b[off] = (uint8_t)v;
    b[off + 1] = (uint8_t)(v >> 8);
    b[off + 2] = (uint8_t)(v >> 16);
    b[off + 3] = (uint8_t)(v >> 24);
}

int16_t EveDisplay::sx15(uint32_t v) const
{
    v &= 0x7FFF;
    return (int16_t)((v & 0x4000) ? (v | 0x8000) : v);
}

int16_t EveDisplay::sx16(uint32_t v) const
{
    return (int16_t)(v & 0xFFFF);
}

void EveDisplay::dlAppendBytes(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (dl_ptr_ >= kRamDlSize) return;
        ram_dl_[dl_ptr_++] = p[i];
    }
}

void EveDisplay::dlAppendWord(uint32_t w)
{
    uint8_t b[4] = {(uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24)};
    dlAppendBytes(b, 4);
}

void EveDisplay::dlStart()
{
    in_dl_ = true;
    dl_ptr_ = 0;
}

void EveDisplay::cmdMemcpy(uint32_t dest, uint32_t src, uint32_t num)
{
    if (num == 0 || dest >= kRamGSize) return;
    size_t n = std::min<size_t>(num, kRamGSize - dest);
    if (src == kRamDlBase)
    {
        size_t avail = std::min<size_t>(n, dl_ptr_);
        memcpy(&ram_g_[dest], ram_dl_.data(), avail);
        if (n > avail) memset(&ram_g_[dest + avail], 0, n - avail);
    }
    else if (src < kRamGSize)
    {
        n = std::min<size_t>(n, kRamGSize - src);
        memmove(&ram_g_[dest], &ram_g_[src], n);
    }
}

void EveDisplay::cmdAppend(uint32_t ptr, uint32_t num)
{
    if (ptr >= kRamGSize) return;
    size_t n = std::min<size_t>(num, kRamGSize - ptr);
    dlAppendBytes(&ram_g_[ptr], n);
}

void EveDisplay::executeFifo()
{
    size_t i = 0;
    while (i + 4 <= fifo_.size())
    {
        uint32_t w = rd32(fifo_, i);
        i += 4;
        if ((w >> 24) != 0xFF)
        {
            // A display-list word (or a vertex): accumulate it while a list is
            // being built.  DLSTART resets the list offset, as REG_CMD_DL does.
            if (in_dl_) dlAppendWord(w);
            continue;
        }
        uint8_t cmd = (uint8_t)(w & 0xFF);
        switch (cmd)
        {
            case 0x00: // CMD_DLSTART
                dlStart();
                break;
            case 0x01: // CMD_SWAP
                if (in_dl_) renderList();
                in_dl_ = false;
                if (eveDebug()) fprintf(stderr, "eve swap dl=%zu ops=%zu\n", (size_t)dl_ptr_, last_ops_);
                if (getenv("TRACKER_EVE_DLDUMP") != nullptr)
                {
                    FILE* f = fopen("/tmp/eve_dl.bin", "wb");
                    if (f != nullptr)
                    {
                        fwrite(ram_dl_.data(), 1, dl_ptr_, f);
                        fclose(f);
                    }
                }
                break;
            case CMD_MEMCPY:
            {
                uint32_t dest = rd32(fifo_, i);
                uint32_t src = rd32(fifo_, i + 4);
                uint32_t num = rd32(fifo_, i + 8);
                i += 12;
                if (eveDebug()) fprintf(stderr, "eve memcpy dest=%u src=%u num=%u\n", dest, src, num);
                cmdMemcpy(dest, src, num);
                break;
            }
            case CMD_APPEND:
            {
                uint32_t ptr = rd32(fifo_, i);
                uint32_t num = rd32(fifo_, i + 4);
                i += 8;
                if (eveDebug()) fprintf(stderr, "eve append ptr=%u num=%u\n", ptr, num);
                cmdAppend(ptr, num);
                break;
            }
            case CMD_MEMSET:
            {
                uint32_t ptr = rd32(fifo_, i);
                uint32_t val = rd32(fifo_, i + 4);
                uint32_t num = rd32(fifo_, i + 8);
                i += 12;
                if (ptr < kRamGSize)
                {
                    size_t e = std::min<size_t>(kRamGSize, (size_t)ptr + num);
                    memset(&ram_g_[ptr], (int)(val & 0xFF), e - ptr);
                }
                break;
            }
            case CMD_MEMZERO:
            {
                uint32_t ptr = rd32(fifo_, i);
                uint32_t num = rd32(fifo_, i + 4);
                i += 8;
                if (ptr < kRamGSize)
                {
                    size_t e = std::min<size_t>(kRamGSize, (size_t)ptr + num);
                    memset(&ram_g_[ptr], 0, e - ptr);
                }
                break;
            }
            case CMD_TEXT:
            {
                uint32_t p0 = rd32(fifo_, i);
                uint32_t p1 = rd32(fifo_, i + 4);
                size_t s = i + 8;
                size_t e = s;
                while (e < fifo_.size() && fifo_[e] != 0) e++;
                if (e < fifo_.size()) e++; // include the NUL
                size_t len = e - s;
                size_t padded = (len + 3) & ~(size_t)3;
                if (eveDebug())
                    fprintf(stderr, "eve text len=%zu padded=%zu fifo=%zu dl=%zu\n", len,
                            padded, fifo_.size(), (size_t)dl_ptr_);
                if (in_dl_)
                {
                    dlAppendWord(w);
                    dlAppendWord(p0);
                    dlAppendWord(p1);
                    dlAppendBytes(&fifo_[s], len);
                    // Pad with bytes, not words: the list is byte-addressed.
                    static const uint8_t zero = 0;
                    for (size_t k = len; k < padded; k++) dlAppendBytes(&zero, 1);
                }
                i = s + padded;
                break;
            }
            default:
            {
                int n = paramCount(cmd);
                if (n < 0)
                {
                    // Unknown command: do not guess at parameter boundaries.
                    i = fifo_.size();
                    break;
                }
                if (in_dl_)
                {
                    dlAppendWord(w);
                    for (int k = 0; k < n; k++) dlAppendWord(rd32(fifo_, i + 4 * (size_t)k));
                }
                i += 4 * (size_t)n;
                break;
            }
        }
    }
    fifo_.clear();
}

void EveDisplay::resetRenderState(RenderState& st)
{
    st = RenderState{};
}

bool EveDisplay::fontGlyph(int handle, uint8_t ch, uint8_t& advance, uint8_t& height) const
{
    if (handle < 0 || handle >= 32 || !fonts_[handle].valid) return false;
    const uint32_t ptr = fonts_[handle].ptr;
    if (ptr + 148 > kRamGSize) return false;
    if (ch >= 128) return false;
    advance = ram_g_[ptr + ch];
    height = ram_g_[ptr + 140];
    return height != 0;
}

int EveDisplay::fontStringWidth(int handle, const std::string& s, bool mono) const
{
    int width = 0;
    if (handle < 0 || handle >= 32 || !fonts_[handle].valid) return 0;
    const uint32_t ptr = fonts_[handle].ptr;
    if (ptr + 148 > kRamGSize) return 0;
    const uint8_t monoAdvance = ram_g_[ptr + 136];
    for (unsigned char ch : s)
    {
        if (mono) width += monoAdvance;
        else if (ch < 128) width += ram_g_[ptr + ch];
    }
    return width;
}

uint32_t EveDisplay::spriteFor(uint32_t source, uint32_t fmt, uint32_t stride, uint32_t width,
                               uint32_t height, uint32_t cell)
{
    uint8_t bpp = 0;
    switch (fmt)
    {
        case 1: bpp = 1; break;  // L1
        case 17: bpp = 2; break; // L2
        case 2: bpp = 4; break;  // L4
        case 3: bpp = 8; break;  // L8
        default: return 0;       // images and paletted formats not modelled
    }
    if (width == 0 || height == 0 || stride == 0) return 0;
    uint64_t off = (uint64_t)source + (uint64_t)cell * stride * height;
    if (off >= kRamGSize) return 0;
    size_t len = (size_t)stride * height;
    if ((size_t)off + len > kRamGSize) len = kRamGSize - (size_t)off;

    std::lock_guard<std::mutex> lock(sprite_mu_);
    for (const auto& sp : sprites_)
    {
        if (sp.address == (uint32_t)off && sp.format == fmt && sp.stride == stride &&
            sp.w == width && sp.h == height)
            return sp.id;
    }
    EveSprite sp;
    sp.id = (uint32_t)sprites_.size() + 1;
    sp.address = (uint32_t)off;
    sp.w = (uint16_t)width;
    sp.h = (uint16_t)height;
    sp.stride = (uint16_t)stride;
    sp.bpp = bpp;
    sp.format = (uint8_t)fmt;
    sp.data.assign(ram_g_.begin() + off, ram_g_.begin() + off + len);
    sprites_.push_back(std::move(sp));
    return sprites_.back().id;
}

void EveDisplay::emitGlyph(const RenderState& st, std::vector<EveOp>& ops, int handle, int x,
                           int y, uint8_t ch)
{
    uint8_t advance = 0, height = 0;
    if (!fontGlyph(handle, ch, advance, height)) return;
    if (advance == 0 && ch != ' ') return; // glyph not present in the font

    if (!ops.empty())
    {
        EveOp& last = ops.back();
        if (last.kind == EveOp::kText && last.y == y && last.color == st.color &&
            last.alpha == st.alpha && last.blend == st.blend && last.font_px == height &&
            !last.xs.empty() && x == (int)last.xs.back() + last.advance)
        {
            last.text.push_back((char)ch);
            last.xs.push_back((uint16_t)x);
            last.advance = advance;
            return;
        }
    }

    EveOp op;
    op.kind = EveOp::kText;
    op.x = (int16_t)x;
    op.y = (int16_t)y;
    op.color = st.color;
    op.alpha = st.alpha;
    op.blend = st.blend;
    op.font_px = height;
    op.advance = advance;
    op.text.assign(1, (char)ch);
    op.xs.push_back((uint16_t)x);
    ops.push_back(std::move(op));
}

void EveDisplay::emitText(const RenderState& st, std::vector<EveOp>& ops, int handle, int x,
                          int y, uint16_t options, const std::string& s)
{
    if (s.empty()) return;
    if (handle < 0 || handle >= 32 || !fonts_[handle].valid) return;
    const uint32_t ptr = fonts_[handle].ptr;
    if (ptr + 148 > kRamGSize) return;
    const uint8_t height = ram_g_[ptr + 140];
    const uint8_t monoAdvance = ram_g_[ptr + 136];
    const bool mono = (options & OPT_MONO) != 0;

    int width = 0;
    for (unsigned char ch : s)
    {
        if (mono) width += monoAdvance;
        else if (ch < 128) width += ram_g_[ptr + ch];
    }

    if (options & OPT_RIGHTX) x -= width;
    else if (options & OPT_CENTERX) x -= width / 2;
    if (options & OPT_CENTERY) y -= height / 2;

    EveOp op;
    op.kind = EveOp::kText;
    op.x = (int16_t)x;
    op.y = (int16_t)y;
    op.color = st.color;
    op.alpha = st.alpha;
    op.blend = st.blend;
    op.font_px = height;
    op.advance = mono ? monoAdvance : (unsigned char)s[0] < 128 ? ram_g_[ptr + (unsigned char)s[0]] : 0;
    op.text = s;
    for (unsigned char ch : s)
    {
        op.xs.push_back((uint16_t)x);
        if (mono) x += monoAdvance;
        else if (ch < 128) x += ram_g_[ptr + ch];
    }
    ops.push_back(std::move(op));
}

void EveDisplay::emitNumber(const RenderState& st, std::vector<EveOp>& ops, int handle, int x,
                            int y, uint16_t options, int32_t n)
{
    char buf[16];
    if ((options & 0x100) && n >= 0) snprintf(buf, sizeof(buf), "+%d", n);
    else snprintf(buf, sizeof(buf), "%d", n);
    emitText(st, ops, handle, x, y, options, buf);
}

void EveDisplay::emitIcon(const RenderState& st, std::vector<EveOp>& ops, int handle, int x,
                          int y, uint8_t cell)
{
    if (handle < 0 || handle >= 32 || !bitmaps_[handle].valid) return;
    const BitmapProps& bp = bitmaps_[handle];
    uint32_t id = spriteFor(bp.source, bp.format, bp.linestride, bp.width, bp.height, cell);
    if (id == 0) return;
    EveOp op;
    op.kind = EveOp::kIcon;
    op.x = (int16_t)x;
    op.y = (int16_t)y;
    op.color = st.color;
    op.alpha = st.alpha;
    op.blend = st.blend;
    op.sprite = id;
    ops.push_back(std::move(op));
}

void EveDisplay::emitScissor(const RenderState& st, std::vector<EveOp>& ops)
{
    EveOp op;
    op.kind = EveOp::kScissor;
    op.x = (int16_t)st.scissor_x;
    op.y = (int16_t)st.scissor_y;
    op.w = (int16_t)st.scissor_w;
    op.h = (int16_t)st.scissor_h;
    ops.push_back(std::move(op));
}

void EveDisplay::flushVertices(const RenderState& st, std::vector<EveOp>& ops,
                               std::vector<std::pair<int, int>>& verts)
{
    auto rect = [&](int x0, int y0, int x1, int y1)
    {
        EveOp op;
        op.kind = EveOp::kRect;
        op.x = (int16_t)std::min(x0, x1);
        op.y = (int16_t)std::min(y0, y1);
        op.w = (int16_t)std::abs(x1 - x0);
        op.h = (int16_t)std::abs(y1 - y0);
        op.color = st.color;
        op.alpha = st.alpha;
        op.blend = st.blend;
        ops.push_back(std::move(op));
    };
    auto line = [&](int x0, int y0, int x1, int y1)
    {
        EveOp op;
        op.kind = EveOp::kLine;
        op.x = (int16_t)x0;
        op.y = (int16_t)y0;
        op.w = (int16_t)x1;
        op.h = (int16_t)y1;
        op.color = st.color;
        op.alpha = st.alpha;
        op.blend = st.blend;
        op.size = (uint16_t)std::max(1, (int)st.line_width / 16);
        ops.push_back(std::move(op));
    };

    switch (st.begin)
    {
        case 9: // RECTS
            for (size_t i = 0; i + 1 < verts.size(); i += 2)
                rect(verts[i].first, verts[i].second, verts[i + 1].first, verts[i + 1].second);
            break;
        case 3: // LINES
            for (size_t i = 0; i + 1 < verts.size(); i += 2)
                line(verts[i].first, verts[i].second, verts[i + 1].first, verts[i + 1].second);
            break;
        case 4: // LINE_STRIP
            for (size_t i = 0; i + 1 < verts.size(); i++)
                line(verts[i].first, verts[i].second, verts[i + 1].first, verts[i + 1].second);
            break;
        case 2: // POINTS
            for (auto& v : verts)
            {
                EveOp op;
                op.kind = EveOp::kPoint;
                op.x = (int16_t)v.first;
                op.y = (int16_t)v.second;
                op.color = st.color;
                op.alpha = st.alpha;
                op.blend = st.blend;
                op.size = (uint16_t)std::max(1, (int)st.point_size / 16);
                ops.push_back(std::move(op));
            }
            break;
        default: break;
    }
    verts.clear();
}

void EveDisplay::renderList()
{
    std::vector<EveOp> ops;
    RenderState st;
    resetRenderState(st);
    std::vector<RenderState> stack;
    std::vector<std::pair<int, int>> verts;

    size_t i = 0;
    bool done = false;
    while (i + 4 <= dl_ptr_ && !done)
    {
        uint32_t w = rd32(ram_dl_, i);
        i += 4;

        if ((w >> 30) == 1) // VERTEX2F
        {
            int x = sx15((w >> 15) & 0x7FFF);
            int y = sx15(w & 0x7FFF);
            if (st.vertex_frac) { x >>= st.vertex_frac; y >>= st.vertex_frac; }
            if (st.begin == 1 && (st.handle < 1 || !fonts_[st.handle].valid))
                emitIcon(st, ops, st.handle, x, y, st.cell);
            else if (st.begin == 1)
                emitGlyph(st, ops, st.handle, x, y, st.cell);
            else
                verts.push_back({x, y});
            continue;
        }
        if ((w >> 30) == 2) // VERTEX2II
        {
            int x = (int)((w >> 21) & 0x1FF);
            int y = (int)((w >> 12) & 0x1FF);
            uint8_t h = (uint8_t)((w >> 7) & 0x1F);
            uint8_t cell = (uint8_t)(w & 0x7F);
            if (st.begin == 1 && (h < 1 || !fonts_[h].valid))
                emitIcon(st, ops, h, x, y, cell);
            else if (st.begin == 1)
                emitGlyph(st, ops, h, x, y, cell);
            else
                verts.push_back({x, y});
            continue;
        }

        uint8_t op = (uint8_t)(w >> 24);
        if (op == 0xFF)
        {
            uint8_t cmd = (uint8_t)(w & 0xFF);
            switch (cmd)
            {
                case CMD_TEXT:
                {
                    uint32_t p0 = rd32(ram_dl_, i);
                    uint32_t p1 = rd32(ram_dl_, i + 4);
                    i += 8;
                    size_t s = i;
                    size_t e = s;
                    while (e < dl_ptr_ && ram_dl_[e] != 0) e++;
                    if (e < dl_ptr_) e++;
                    std::string text((const char*)&ram_dl_[s], e - s);
                    if (!text.empty() && text.back() == '\0') text.pop_back();
                    i = (s + ((text.size() + 1 + 3) & ~(size_t)3));
                    emitText(st, ops, (int)(p1 & 0xFFFF), sx16(p0), (int)(p0 >> 16),
                             (uint16_t)(p1 >> 16), text);
                    break;
                }
                case CMD_NUMBER:
                {
                    uint32_t p0 = rd32(ram_dl_, i);
                    uint32_t p1 = rd32(ram_dl_, i + 4);
                    int32_t n = (int32_t)rd32(ram_dl_, i + 8);
                    i += 12;
                    emitNumber(st, ops, (int)(p1 & 0xFFFF), sx16(p0), (int)(p0 >> 16),
                               (uint16_t)(p1 >> 16), n);
                    break;
                }
                case CMD_GRADIENT:
                {
                    uint32_t p0 = rd32(ram_dl_, i);
                    uint32_t c0 = rd32(ram_dl_, i + 4);
                    uint32_t p1 = rd32(ram_dl_, i + 8);
                    uint32_t c1 = rd32(ram_dl_, i + 12);
                    i += 16;
                    EveOp gop;
                    gop.kind = EveOp::kGradient;
                    gop.x = (int16_t)st.scissor_x;
                    gop.y = (int16_t)st.scissor_y;
                    gop.w = (int16_t)st.scissor_w;
                    gop.h = (int16_t)st.scissor_h;
                    gop.color = c0 & 0xFFFFFF;
                    gop.color2 = c1 & 0xFFFFFF;
                    gop.alpha = st.alpha;
                    gop.blend = st.blend;
                    gop.gx0 = sx16(p0);
                    gop.gy0 = (int16_t)(p0 >> 16);
                    gop.gx1 = sx16(p1);
                    gop.gy1 = (int16_t)(p1 >> 16);
                    ops.push_back(std::move(gop));
                    break;
                }
                case CMD_SETBITMAP:
                {
                    uint32_t source = rd32(ram_dl_, i);
                    uint32_t fw = rd32(ram_dl_, i + 4);
                    uint32_t h = rd32(ram_dl_, i + 8);
                    i += 12;
                    bitmaps_[0].source = source & 0x3FFFFF;
                    bitmaps_[0].format = fw & 0xFFFF;
                    bitmaps_[0].width = (fw >> 16) & 0x1FF;
                    bitmaps_[0].height = h & 0x1FF;
                    bitmaps_[0].valid = true;
                    st.handle = 0;
                    break;
                }
                case CMD_SETFONT2:
                {
                    uint32_t font = rd32(ram_dl_, i);
                    uint32_t ptr = rd32(ram_dl_, i + 4);
                    uint32_t first = rd32(ram_dl_, i + 8);
                    i += 12;
                    if (font < 32)
                    {
                        fonts_[font].ptr = ptr;
                        fonts_[font].first_char = (uint16_t)first;
                        fonts_[font].valid = true;
                    }
                    break;
                }
                default:
                {
                    // A display-list command we do not model (image transforms,
                    // snapshots): skip its parameters but keep the frame.
                    int n = paramCount(cmd);
                    if (n < 0) { done = true; break; }
                    i += 4 * (size_t)n;
                    break;
                }
            }
            continue;
        }

        switch (op)
        {
            case DL_DISPLAY:
                flushVertices(st, ops, verts);
                done = true;
                break;
            case DL_CLEAR_COLOR_RGB:
                st.clear_color = w & 0xFFFFFF;
                break;
            case DL_CLEAR_COLOR_A:
                st.clear_alpha = (uint8_t)(w & 0xFF);
                break;
            case DL_CLEAR:
            {
                EveOp cop;
                cop.kind = EveOp::kClear;
                cop.color = st.clear_color;
                cop.alpha = st.clear_alpha;
                ops.push_back(std::move(cop));
                break;
            }
            case DL_COLOR_RGB:
                st.color = w & 0xFFFFFF;
                break;
            case DL_COLOR_A:
                st.alpha = (uint8_t)(w & 0xFF);
                break;
            case DL_BLEND_FUNC:
            {
                uint8_t src = (uint8_t)((w >> 3) & 7);
                uint8_t dst = (uint8_t)(w & 7);
                if (src == 2 && dst == 1) st.blend = 1;       // additive
                else if (src == 2 && dst == 0) st.blend = 2;  // source only
                else st.blend = 0;
                break;
            }
            case DL_LINE_WIDTH:
                st.line_width = (uint16_t)(w & 0xFFF);
                break;
            case DL_POINT_SIZE:
                st.point_size = (uint16_t)(w & 0x1FFF);
                break;
            case DL_BEGIN:
                flushVertices(st, ops, verts);
                st.begin = (int)(w & 0xF);
                break;
            case DL_END:
                flushVertices(st, ops, verts);
                st.begin = -1;
                break;
            case DL_SAVE_CONTEXT:
                flushVertices(st, ops, verts);
                stack.push_back(st);
                break;
            case DL_RESTORE_CONTEXT:
                flushVertices(st, ops, verts);
                if (!stack.empty())
                {
                    st = stack.back();
                    stack.pop_back();
                    emitScissor(st, ops);
                }
                break;
            case DL_SCISSOR_XY:
                st.scissor_x = (int)((w >> 11) & 0x7FF);
                st.scissor_y = (int)(w & 0x7FF);
                emitScissor(st, ops);
                break;
            case DL_SCISSOR_SIZE:
                st.scissor_w = (int)((w >> 12) & 0xFFF);
                st.scissor_h = (int)(w & 0xFFF);
                emitScissor(st, ops);
                break;
            case DL_VERTEX_FORMAT:
                st.vertex_frac = (uint8_t)(w & 7);
                break;
            case DL_BITMAP_HANDLE:
                st.handle = (uint8_t)(w & 0x1F);
                break;
            case DL_CELL:
                st.cell = (uint8_t)(w & 0x7F);
                break;
            case DL_BITMAP_SOURCE:
                bitmaps_[st.handle].source = w & 0x3FFFFF;
                bitmaps_[st.handle].valid = true;
                break;
            case DL_BITMAP_LAYOUT:
                bitmaps_[st.handle].format = (w >> 19) & 0x1F;
                bitmaps_[st.handle].linestride = (w >> 9) & 0x3FF;
                bitmaps_[st.handle].height = w & 0x1FF;
                bitmaps_[st.handle].valid = true;
                break;
            case DL_BITMAP_SIZE:
                bitmaps_[st.handle].width = (w >> 9) & 0x1FF;
                bitmaps_[st.handle].height = w & 0x1FF;
                bitmaps_[st.handle].valid = true;
                break;
            case DL_ALPHA_FUNC:
                break;
            default:
                break;
        }
    }
    flushVertices(st, ops, verts);
    last_ops_ = ops.size();

    EveFrame f;
    f.seq = ++seq_;
    f.ops = std::move(ops);
    {
        std::lock_guard<std::mutex> lock(mu_);
        frame_ = std::move(f);
    }
}

// ---------------------------------------------------------------------------
// DisplayDriver

void EveDisplay::setPowerState(bool) {}
void EveDisplay::setBrightness(int value) { brightness_ = value; }
void EveDisplay::setRotation(int value) { rotation_ = value; }

void EveDisplay::getImgProps(uint32_t& addr, uint32_t& w, uint32_t& h)
{
    addr = w = h = 0;
}

uint32_t EveDisplay::getDisplayListOffset()
{
    return dl_ptr_;
}

void EveDisplay::IncCMDOffset(uint16_t) {}
void EveDisplay::beginCoProList() { fifo_.clear(); }
void EveDisplay::beginCoProListNoCheck() { fifo_.clear(); }
void EveDisplay::endCoProList() { executeFifo(); }
void EveDisplay::awaitCoProEmpty() {}
uint8_t EveDisplay::isCoProEmpty() { return 1; }

void EveDisplay::WriteData32(uint32_t data)
{
    fifo_.push_back((uint8_t)data);
    fifo_.push_back((uint8_t)(data >> 8));
    fifo_.push_back((uint8_t)(data >> 16));
    fifo_.push_back((uint8_t)(data >> 24));
}

void EveDisplay::writeDataRAMG(const uint8_t* imgData, uint32_t dataSize, uint32_t destAddress)
{
    if (destAddress >= kRamGSize) return;
    size_t n = std::min<size_t>(dataSize, kRamGSize - destAddress);
    memcpy(&ram_g_[destAddress], imgData, n);
}

void EveDisplay::writeFontRAMG(const uint8_t* imgData, uint32_t dataSize, uint32_t destAddress)
{
    writeDataRAMG(imgData, dataSize, destAddress);
}

void EveDisplay::readDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t sourceAddress)
{
    if (sourceAddress >= kRamGSize) return;
    size_t n = std::min<size_t>(dataSize, kRamGSize - sourceAddress);
    memcpy(imgData, &ram_g_[sourceAddress], n);
}

void EveDisplay::readBackDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t sourceAddress)
{
    readDataRAMG(imgData, dataSize, sourceAddress);
}

uint8_t EveDisplay::sendString(const char* string, uint32_t length)
{
    size_t n = length ? length : strlen(string);
    for (size_t i = 0; i < n; i++) fifo_.push_back((uint8_t)string[i]);
    fifo_.push_back(0);
    while (fifo_.size() % 4) fifo_.push_back(0);
    return (uint8_t)std::min<size_t>(255, (n + 4) & ~(size_t)3);
}

void EveDisplay::writeDataToCMD(const uint8_t* imgData, uint32_t totalDataSize)
{
    for (uint32_t i = 0; i < totalDataSize; i++) fifo_.push_back(imgData[i]);
}

void EveDisplay::initStreamDma(uint16_t, uint16_t, uint16_t, uint8_t*, uint32_t) {}
bool EveDisplay::updateStreamDma(bool) { return true; }
void EveDisplay::stopStreamDma() {}

bool EveDisplay::copyFrame(EveFrame& out)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (frame_.ops.empty()) return false;
    out = frame_;
    std::lock_guard<std::mutex> slock(sprite_mu_);
    out.sprite_meta.clear();
    for (const auto& sp : sprites_)
    {
        EveSprite meta;
        meta.id = sp.id;
        meta.address = sp.address;
        meta.w = sp.w;
        meta.h = sp.h;
        meta.stride = sp.stride;
        meta.bpp = sp.bpp;
        meta.format = sp.format;
        out.sprite_meta.push_back(std::move(meta));
    }
    return true;
}

bool EveDisplay::getSprite(uint32_t id, EveSprite& out)
{
    std::lock_guard<std::mutex> lock(sprite_mu_);
    for (const auto& sp : sprites_)
    {
        if (sp.id == id)
        {
            out = sp;
            return true;
        }
    }
    return false;
}

void EveDisplay::dumpFrameText(FILE* out)
{
    EveFrame f;
    if (!copyFrame(f)) return;
    fprintf(out, "frame %u: %zu ops\n", f.seq, f.ops.size());
    for (const auto& op : f.ops)
    {
        switch (op.kind)
        {
            case EveOp::kClear:
                fprintf(out, "  clear #%06x a%d\n", op.color, op.alpha);
                break;
            case EveOp::kScissor:
                fprintf(out, "  scissor %d,%d %dx%d\n", op.x, op.y, op.w, op.h);
                break;
            case EveOp::kRect:
                fprintf(out, "  rect %d,%d %dx%d #%06x a%d\n", op.x, op.y, op.w, op.h,
                        op.color, op.alpha);
                break;
            case EveOp::kLine:
                fprintf(out, "  line %d,%d -> %d,%d #%06x w%d\n", op.x, op.y, op.w, op.h,
                        op.color, op.size);
                break;
            case EveOp::kPoint:
                fprintf(out, "  point %d,%d #%06x d%d\n", op.x, op.y, op.color, op.size);
                break;
            case EveOp::kText:
                fprintf(out, "  text %d,%d px%d #%06x \"%s\"\n", op.x, op.y, op.font_px,
                        op.color, op.text.c_str());
                break;
            case EveOp::kIcon:
                fprintf(out, "  icon %d,%d sprite%u #%06x\n", op.x, op.y, op.sprite, op.color);
                break;
            case EveOp::kGradient:
                fprintf(out, "  gradient %d,%d %dx%d #%06x -> #%06x axis %d,%d -> %d,%d\n",
                        op.x, op.y, op.w, op.h, op.color, op.color2, op.gx0, op.gy0, op.gx1,
                        op.gy1);
                break;
        }
    }
}
