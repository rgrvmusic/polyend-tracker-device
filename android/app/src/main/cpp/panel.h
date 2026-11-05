/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

// The Mini's front panel: button geometry (device-information, Mini Essentials
// p.7) and drawing. `top` is the height the top bar takes above it.
#pragma once

#include <vector>

#include "imgui.h"

struct PanelRect
{
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    float width() const { return x1 - x0; }
    float height() const { return y1 - y0; }
    bool contains(ImVec2 p) const { return p.x >= x0 && p.x < x1 && p.y >= y0 && p.y < y1; }
};

struct PanelButton
{
    int id = -1; // the firmware's switch id
    const char* label = "";
    PanelRect rect;
};

struct PanelLayout
{
    PanelRect screen; // the 800x480 draw list goes here
    std::vector<PanelButton> buttons;
};

// Surface pixels. `top` is the height reserved for the top bar.
PanelLayout layoutPanel(float w, float h, float top = 0);

// The button under a point, or -1.
int hitTest(const PanelLayout& layout, ImVec2 p);

// held: switch ids currently down, for the pressed look.
void drawPanel(ImDrawList* dl, const PanelLayout& layout, ImFont* font,
               const std::vector<int>& held);

// The band the top bar takes: 48dp plus a 2dp shadow, scaled.
float bannerHeight(float scale);

// Draws the bar: red fill, folder control at the left, title beside it. `mark`
// is the control's hit box.
void drawBanner(ImDrawList* dl, ImFont* font, float width, float scale, const char* title,
                PanelRect* mark, bool pressed);
