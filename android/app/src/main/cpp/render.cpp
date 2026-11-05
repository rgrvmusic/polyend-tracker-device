/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

#include "render.h"

#include <GLES3/gl3.h>

#include <chrono>

#include "imgui_internal.h" // ImMin/ImMax

namespace
{
constexpr int kScreenW = 800;
constexpr int kScreenH = 480;

double nowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Additive blend is used for highlighted rows. ImGui sets its own blend state
// once per frame, so switching needs a draw callback and a callback back.
void additiveBlend(const ImDrawList*, const ImDrawCmd*)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
}

void normalBlend(const ImDrawList*, const ImDrawCmd*)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

ImU32 colorOf(uint32_t rgb, unsigned char alpha)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

unsigned int uploadTexture(const std::vector<uint8_t>& rgba, int w, int h)
{
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}
} // namespace

bool SpriteCache::get(int sprite, uint32_t color, SpriteTexture& out)
{
    const uint64_t key = ((uint64_t)(uint32_t)sprite << 24) | (color & 0xFFFFFF);
    auto it = entries_.find(key);
    if (it != entries_.end())
    {
        out.tex = it->second.tex;
        out.w = it->second.w;
        out.h = it->second.h;
        if (it->second.tex != 0) return true;
        if (nowSeconds() < it->second.nextTry) return false;
    }

    SpriteImage img;
    if (!fetchSprite(sprite, img) || img.w <= 0 || img.h <= 0 || img.bpp <= 0)
    {
        // Not in the firmware's table (yet), or the door is not up: do not ask
        // again for a while, or every frame would open a socket per icon.
        Entry e;
        e.nextTry = nowSeconds() + 1.0;
        entries_[key] = e;
        return false;
    }

    const int r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    std::vector<uint8_t> rgba((size_t)img.w * img.h * 4, 0);
    const int perByte = 8 / img.bpp;
    for (int y = 0; y < img.h; y++)
    {
        for (int x = 0; x < img.w; x++)
        {
            size_t i = (size_t)y * img.stride + (size_t)(x / perByte);
            if (i >= img.data.size()) continue;
            const unsigned b0 = img.data[i];
            int level;
            if (img.bpp == 1) level = (b0 >> (7 - (x % 8))) & 1;
            else if (img.bpp == 2) level = (b0 >> (6 - 2 * (x % 4))) & 3;
            else if (img.bpp == 4) level = (b0 >> (4 - 4 * (x % 2))) & 0xF;
            else level = b0;
            if (!level) continue;
            const size_t o = ((size_t)y * img.w + x) * 4;
            rgba[o] = (uint8_t)r;
            rgba[o + 1] = (uint8_t)g;
            rgba[o + 2] = (uint8_t)b;
            rgba[o + 3] = img.bpp == 8 ? (uint8_t)level
                                       : (uint8_t)((level * 255) / ((1 << img.bpp) - 1));
        }
    }

    Entry e;
    e.tex = uploadTexture(rgba, img.w, img.h);
    e.w = img.w;
    e.h = img.h;
    entries_[key] = e;
    out.tex = e.tex;
    out.w = e.w;
    out.h = e.h;
    return e.tex != 0;
}

void SpriteCache::release()
{
    for (auto& kv : entries_)
        if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
    entries_.clear();
}

void drawFrame(ImDrawList* dl, const Frame& f, ImFont* font, float scale, ImVec2 origin,
               SpriteCache& cache)
{
    auto X = [&](float v) { return origin.x + v * scale; };
    auto Y = [&](float v) { return origin.y + v * scale; };

    int clipDepth = 0;
    unsigned char blend = 0;

    for (const FrameOp& op : f.ops)
    {
        if (op.blend != blend)
        {
            blend = op.blend;
            dl->AddCallback(blend ? additiveBlend : normalBlend, nullptr);
        }

        switch (op.kind)
        {
            case OP_CLEAR:
                dl->AddRectFilled(ImVec2(X(0), Y(0)), ImVec2(X(kScreenW), Y(kScreenH)),
                                  colorOf(op.color, op.alpha));
                break;
            case OP_SCISSOR:
                // Every scissor op restores and re-clips.
                if (clipDepth > 0)
                {
                    dl->PopClipRect();
                    clipDepth--;
                }
                dl->PushClipRect(ImVec2(X(op.x), Y(op.y)), ImVec2(X(op.x + op.w), Y(op.y + op.h)),
                                 true);
                clipDepth++;
                break;
            case OP_RECT:
                dl->AddRectFilled(ImVec2(X(op.x), Y(op.y)), ImVec2(X(op.x + op.w), Y(op.y + op.h)),
                                  colorOf(op.color, op.alpha));
                break;
            case OP_GRADIENT:
            {
                const ImU32 c1 = colorOf(op.color, op.alpha);
                const ImU32 c2 = colorOf(op.color2, op.alpha);
                ImU32 tl, tr, bl, br;
                if (op.gx0 == op.gx1 && op.gy0 == op.gy1)
                {
                    tl = tr = bl = br = c1;
                }
                else if (op.gy0 == op.gy1) // horizontal
                {
                    tl = bl = c1;
                    tr = br = c2;
                }
                else // vertical (and diagonals, approximated)
                {
                    tl = tr = c1;
                    bl = br = c2;
                }
                dl->AddRectFilledMultiColor(ImVec2(X(op.x), Y(op.y)),
                                            ImVec2(X(op.x + op.w), Y(op.y + op.h)), tl, tr, br, bl);
                break;
            }
            case OP_LINE:
                dl->AddLine(ImVec2(X(op.x), Y(op.y)), ImVec2(X(op.x2), Y(op.y2)),
                            colorOf(op.color, op.alpha), ImMax(1.0f, op.size * scale));
                break;
            case OP_POINT:
            {
                const float d = ImMax(1.0f, op.size * scale);
                const float cx = X(op.x), cy = Y(op.y);
                dl->AddRectFilled(ImVec2(cx - d * 0.5f, cy - d * 0.5f),
                                  ImVec2(cx + d * 0.5f, cy + d * 0.5f),
                                  colorOf(op.color, op.alpha));
                break;
            }
            case OP_TEXT:
            {
                const float px = op.px * scale;
                if (px < 1.0f) break;
                const ImFontBaked* baked = font->GetFontBaked(px);
                const float baseline = Y(op.y + op.px - 3);
                const float top = baseline - baked->Ascent;
                const ImU32 col = colorOf(op.color, op.alpha);
                for (size_t i = 0; i < op.text.size(); i++)
                {
                    unsigned char ch = (unsigned char)op.text[i];
                    if (ch < 32 || ch == 127) ch = ' ';
                    else if (ch > 126) ch = '?';
                    char s[2] = {(char)ch, 0};
                    float x = (i < op.xs.size() ? (float)op.xs[i] : (float)op.x) * scale + origin.x;
                    dl->AddText(font, px, ImVec2(x, top), col, s);
                }
                break;
            }
            case OP_ICON:
            {
                SpriteTexture t;
                if (!cache.get(op.sprite, op.color, t)) break;
                dl->AddImage((ImTextureID)t.tex, ImVec2(X(op.x), Y(op.y)),
                             ImVec2(X(op.x + t.w), Y(op.y + t.h)), ImVec2(0, 0), ImVec2(1, 1),
                             IM_COL32(255, 255, 255, op.alpha));
                break;
            }
        }
    }

    while (clipDepth-- > 0) dl->PopClipRect();
    if (blend) dl->AddCallback(normalBlend, nullptr);
}
