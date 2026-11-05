/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

#include "panel.h"

#include <cfloat>
#include <cmath>

#include "imgui_internal.h" // ImMin/ImMax

namespace
{
// Switch ids, from core/interface/core/interfaceDefs.h.
enum : int
{
    kNote = 18,
    kInstr = 19,
    kFX2 = 20,
    kFX1 = 21,
    kPlay = 23,
    kCopy = 24,
    kInsert = 25,
    kUp = 26,
    kDelete = 27,
    kRec = 28,
    kShift = 29,
    kLeft = 30,
    kDown = 31,
    kRight = 32,
    kEnter = 33, // MiniMiddle
    kMenu = 34,
    kFn1 = 35,
    kFn2 = 36,
    kFn3 = 37,
    kFn4 = 38,
    kPlus10 = 39,
    kPlus1 = 40,
    kMinus1 = 41,
    kMinus10 = 42,
    kGame = 43
};

struct Builder
{
    PanelLayout* layout;
    float gap;

    void add(int id, const char* label, float x, float y, float w, float h)
    {
        layout->buttons.push_back({id, label, {x, y, x + w, y + h}});
    }

    // rows[c] = {id, label, colspan}
    struct Cell
    {
        int id;
        const char* label;
        int span;
    };

    void grid(const PanelRect& r, const std::vector<std::vector<Cell>>& rows)
    {
        const int cols = 4;
        const float colW = (r.width() - (cols - 1) * gap) / cols;
        const float rowH = (r.height() - (rows.size() - 1) * gap) / (float)rows.size();
        for (size_t y = 0; y < rows.size(); y++)
        {
            int c = 0;
            for (const Cell& cell : rows[y])
            {
                const float x = r.x0 + c * (colW + gap);
                const float w = cell.span * colW + (cell.span - 1) * gap;
                add(cell.id, cell.label, x, r.y0 + y * (rowH + gap), w, rowH);
                c += cell.span;
            }
        }
    }

    // The cross: Up, Left, Enter, Right, Down; the corners stay empty.
    void dpad(const PanelRect& square)
    {
        const float cell = (square.width() - 2 * gap) / 3;
        const float cellH = (square.height() - 2 * gap) / 3;
        auto at = [&](int col, int row) {
            return PanelRect{square.x0 + col * (cell + gap), square.y0 + row * (cellH + gap),
                             square.x0 + col * (cell + gap) + cell,
                             square.y0 + row * (cellH + gap) + cellH};
        };
        PanelRect up = at(1, 0), left = at(0, 1), mid = at(1, 1), right = at(2, 1), down = at(1, 2);
        add(kUp, "^", up.x0, up.y0, up.width(), up.height());
        add(kLeft, "<", left.x0, left.y0, left.width(), left.height());
        add(kEnter, "ENT", mid.x0, mid.y0, mid.width(), mid.height());
        add(kRight, ">", right.x0, right.y0, right.width(), right.height());
        add(kDown, "v", down.x0, down.y0, down.width(), down.height());
    }

    void master(const PanelRect& column, int count)
    {
        static const int ids[] = {kPlus10, kPlus1, kMinus1, kMinus10, kGame};
        static const char* labels[] = {"+10", "+1", "-1", "-10", "GAME"};
        const float h = (column.height() - (count - 1) * gap) / count;
        for (int i = 0; i < count; i++)
            add(ids[i], labels[i], column.x0, column.y0 + i * (h + gap), column.width(), h);
    }

    void masterRow(const PanelRect& row)
    {
        static const int ids[] = {kPlus10, kPlus1, kMinus1, kMinus10, kGame};
        static const char* labels[] = {"+10", "+1", "-1", "-10", "GAME"};
        const float w = (row.width() - 4 * gap) / 5;
        for (int i = 0; i < 5; i++)
            add(ids[i], labels[i], row.x0 + i * (w + gap), row.y0, w, row.height());
    }

    void softRow(const PanelRect& row)
    {
        const float w = (row.width() - 7 * gap) / 8;
        for (int i = 0; i < 8; i++)
            add(i, "", row.x0 + i * (w + gap), row.y0, w, row.height());
    }

    void softBlock(const PanelRect& block)
    {
        const float w = (block.width() - 3 * gap) / 4;
        const float h = (block.height() - gap) / 2;
        for (int i = 0; i < 8; i++)
            add(i, "", block.x0 + (i % 4) * (w + gap), block.y0 + (i / 4) * (h + gap), w, h);
    }
};

const std::vector<std::vector<Builder::Cell>>& transportRows()
{
    static const std::vector<std::vector<Builder::Cell>> rows = {
        {{kNote, "NOTE", 1}, {kInstr, "INSTR", 1}, {kFX1, "FX1", 1}, {kFX2, "FX2", 1}},
        {{kPlay, "PLAY", 2}, {kRec, "REC", 2}},
        {{kInsert, "INS", 2}, {kMenu, "MENU", 2}},
        {{kDelete, "DEL", 2}, {kCopy, "COPY", 2}},
        {{kFn1, "FN1", 1}, {kFn2, "FN2", 1}, {kFn3, "FN3", 1}, {kFn4, "FN4", 1}},
        {{kShift, "SHIFT", 4}},
    };
    return rows;
}
} // namespace

PanelLayout layoutPanel(float w, float h, float top)
{
    PanelLayout layout;
    const bool landscape = w > h;
    const float pad = ImMax(8.0f, ImMin(w, h) * 0.014f);
    const float gap = ImMax(4.0f, ImMin(w, h) * 0.007f);
    const float areaH = h - top; // what is left under the top bar

    Builder b{&layout, gap};

    float ax, ay, aw, ah; // where the controls other than the screen buttons live
    float softH;

    // The eight screen buttons are two cells wide and one high in both
    // orientations: the row's height is half a button's width.
    auto softRowHeight = [&](float rowWidth) {
        return ImMax(18.0f, ((rowWidth - 7 * gap) / 8.0f) * 0.5f);
    };

    if (landscape)
    {
        // The screen row sits under the display, as on the hardware. Its
        // height follows the display width, so two passes settle the pair.
        float sw = ImMin((areaH - 2 * pad - gap) * 800.0f / 480.0f, w * 0.72f);
        for (int i = 0; i < 2; i++)
        {
            softH = softRowHeight(sw);
            sw = ImMin((areaH - 2 * pad - gap - softH) * 800.0f / 480.0f, w * 0.72f);
        }
        softH = softRowHeight(sw);
        const float sh = ImMin(areaH - 2 * pad - gap - softH, sw * 480.0f / 800.0f);
        sw = sh * 800.0f / 480.0f;
        const float leftH = sh + gap + softH;
        const float sy = top + (areaH - leftH) * 0.5f;
        layout.screen = {pad, sy, pad + sw, sy + sh};
        b.softRow({pad, sy + sh + gap, pad + sw, sy + sh + gap + softH});
        ax = layout.screen.x1 + pad;
        ay = top + pad;
        aw = w - ax - pad;
        ah = areaH - 2 * pad;
    }
    else
    {
        float sw = w - 2 * pad;
        float sh = sw * 480.0f / 800.0f;
        if (sh > areaH * 0.40f)
        {
            sh = areaH * 0.40f;
            sw = sh * 800.0f / 480.0f;
        }
        const float sx = (w - sw) * 0.5f;
        layout.screen = {sx, top + pad, sx + sw, top + pad + sh};
        ax = pad;
        ay = layout.screen.y1 + pad;
        aw = w - 2 * pad;
        ah = h - ay - pad;
        softH = softRowHeight(aw);
        b.softRow({ax, ay, ax + aw, ay + softH});
    }

    // Portrait stacks the controls under the function row. Landscape uses the
    // whole right strip, from the top bar to the bottom edge.
    const float cy = landscape ? ay : ay + softH + gap;
    const float ch = landscape ? ah : ah - softH - gap;

    if (landscape)
    {
        // Strip: D-pad and the edit cluster side by side, master row at the
        // foot of the strip.
        const float masterH = ImMax(40.0f, ImMin(ch * 0.10f, 120.0f));
        const float clusterH = ch - masterH - gap;
        const float dpadW = ImMin(aw * 0.34f, clusterH * 0.55f);
        b.dpad({ax, cy, ax + dpadW, cy + ImMin(clusterH, dpadW)});
        const PanelRect grid{ax + dpadW + gap, cy, ax + aw, cy + clusterH};
        b.grid(grid, transportRows());
        b.masterRow({ax, cy + clusterH + gap, ax + aw, cy + clusterH + gap + masterH});
    }
    else
    {
        const float dpadW = aw * 0.30f;
        const float masterW = aw * 0.20f;
        const float dpadSide = ImMin(ImMin(dpadW, ch), ch * 0.55f);
        const float dy = cy + (ch - dpadSide) * 0.5f;
        b.dpad({ax, dy, ax + dpadSide, dy + dpadSide});
        b.grid({ax + dpadW + gap, cy, ax + aw - masterW - gap, cy + ch}, transportRows());
        b.master({ax + aw - masterW, cy, ax + aw, cy + ch}, 5);
    }

    return layout;
}

int hitTest(const PanelLayout& layout, ImVec2 p)
{
    for (const PanelButton& b : layout.buttons)
        if (b.rect.contains(p)) return b.id;
    return -1;
}

namespace
{
void label(ImDrawList* dl, ImFont* font, const PanelRect& r, const char* text, ImU32 col,
           float maxH)
{
    if (text == nullptr || *text == 0) return;
    float size = ImMin(r.height() * 0.42f, maxH);
    float tw = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    const float avail = r.width() * 0.88f;
    if (tw > avail && tw > 0.0f) size *= avail / tw;
    if (size < 6.0f) return;
    const ImFontBaked* baked = font->GetFontBaked(size);
    const float x = (r.x0 + r.x1 - font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x) * 0.5f;
    const float y = (r.y0 + r.y1) * 0.5f - baked->Ascent * 0.5f;
    dl->AddText(font, size, ImVec2(x, y), col, text);
}
} // namespace

void drawPanel(ImDrawList* dl, const PanelLayout& layout, ImFont* font,
               const std::vector<int>& held)
{
    for (const PanelButton& b : layout.buttons)
    {
        bool down = false;
        for (int id : held)
            if (id == b.id)
            {
                down = true;
                break;
            }
        const float round = ImMin(b.rect.width(), b.rect.height()) * 0.16f;
        const ImU32 fill = down ? IM_COL32(70, 84, 100, 255) : IM_COL32(38, 40, 44, 255);
        const ImU32 edge = down ? IM_COL32(130, 160, 200, 255) : IM_COL32(72, 76, 84, 255);
        dl->AddRectFilled(ImVec2(b.rect.x0, b.rect.y0), ImVec2(b.rect.x1, b.rect.y1), fill, round);
        dl->AddRect(ImVec2(b.rect.x0, b.rect.y0), ImVec2(b.rect.x1, b.rect.y1), edge, round, 0,
                    2.0f);
        label(dl, font, b.rect, b.label, IM_COL32(220, 224, 230, 255), 40.0f);
    }
}

// ---- the top bar ---------------------------------------------------------
//
// A 48dp bar, a 2dp shadow, and a 48x32dp control inset 8dp from the left
// with the title starting 68dp from the edge.

float bannerHeight(float scale)
{
    if (scale < 1.0f) scale = 1.0f;
    return 50.0f * scale; // the 48dp bar and its 2dp shadow
}

void drawBanner(ImDrawList* dl, ImFont* font, float width, float scale, const char* title,
                PanelRect* mark, bool pressed)
{
    if (scale < 1.0f) scale = 1.0f;

    const float bar = 48.0f * scale;
    const float shadow = 2.0f * scale;
    const float mx = 8.0f * scale, my = 8.0f * scale;
    const float mw = 48.0f * scale, mh = 32.0f * scale;

    // The bar and the fade under it.
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(width, bar), IM_COL32(207, 26, 55, 255));
    dl->AddRectFilledMultiColor(ImVec2(0, bar), ImVec2(width, bar + shadow),
                                IM_COL32(0, 0, 0, 128), IM_COL32(0, 0, 0, 128),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));

    if (mark != nullptr) *mark = PanelRect{mx, my, mx + mw, my + mh};
    if (pressed)
        dl->AddRectFilled(ImVec2(mx, my), ImVec2(mx + mw, my + mh),
                          IM_COL32(255, 255, 255, 30), 4.0f * scale);

    // A folder, outlined in the arrow's own stroke: the control opens the
    // screen that holds the card's files.
    const float pen = 2.5f * scale;
    const ImU32 ink = IM_COL32(255, 255, 255, 255);
    const ImVec2 folder[] = {
        {mx + 11.0f * scale, my + 12.0f * scale}, {mx + 11.0f * scale, my + 8.0f * scale},
        {mx + 19.0f * scale, my + 8.0f * scale},  {mx + 22.0f * scale, my + 11.0f * scale},
        {mx + 37.0f * scale, my + 11.0f * scale}, {mx + 37.0f * scale, my + 25.0f * scale},
        {mx + 11.0f * scale, my + 25.0f * scale}, {mx + 11.0f * scale, my + 12.0f * scale},
    };
    dl->AddPolyline(folder, (int)(sizeof folder / sizeof folder[0]), ink, 0, pen);

    if (font != nullptr && title != nullptr)
    {
        const float size = 15.0f * scale;
        const ImFontBaked* baked = font->GetFontBaked(size);
        const float x = mx + mw + 12.0f * scale;
        const float y = (bar - baked->Ascent) * 0.5f;
        dl->AddText(font, size, ImVec2(x, y), IM_COL32(255, 255, 255, 255), title);
    }
}
