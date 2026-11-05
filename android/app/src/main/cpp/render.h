/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */

// Drawing the firmware's draw list with ImGui, and the sprite masks.
#pragma once

#include <cstdint>
#include <unordered_map>

#include "imgui.h"
#include "screen.h"

struct SpriteTexture
{
    unsigned int tex = 0;
    int w = 0;
    int h = 0;
};

class SpriteCache
{
  public:
    // The mask tinted with the icon op's colour, uploaded once per (id,colour).
    bool get(int sprite, uint32_t color, SpriteTexture& out);
    void release();

  private:
    struct Entry
    {
        unsigned int tex = 0;
        int w = 0, h = 0;
        double nextTry = 0; // a missing sprite is not asked for again before this
    };
    std::unordered_map<uint64_t, Entry> entries_;
};

// scale maps the firmware's 800x480 pixels onto the surface; origin is the
// top-left of the screen block.
void drawFrame(ImDrawList* dl, const Frame& f, ImFont* font, float scale, ImVec2 origin,
               SpriteCache& cache);
