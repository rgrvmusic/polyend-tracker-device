/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* Host implementation of core/effector/ptfx_loader.cpp.
 *
 * Tracker effect plugins are ARM Cortex-M Thumb-2 binaries, which cannot run
 * on x86-64, so no plugin loads here. The buffers and accessors remain
 * because the editor reads parameter metadata from the code buffer. */
#include "ptfx_loader.h"

#include <cstring>

extern uint8_t externalRamScratchBuff[];

static constexpr uint32_t kHostMaxCodeSize = 32 * 1024;

uint8_t* PtfxLoader::getCodeBuffer()
{
    return externalRamScratchBuff;
}

uint32_t PtfxLoader::getMaxCodeSize()
{
    return kHostMaxCodeSize;
}

void* PtfxLoader::getScratchBuffer()
{
    return externalRamScratchBuff + kPtfxScratchOffset;
}

uint32_t PtfxLoader::getMaxScratchSize()
{
    return kPtfxScratchTopOffset - kPtfxScratchOffset;
}

const PtfxHostApi* PtfxLoader::getHostApi()
{
    return nullptr;
}

void PtfxLoader::syncInstructionCache(void*, uint32_t)
{
    /* Host caches are coherent. */
}

bool PtfxLoader::readHeader(const char*, PtfxHeader&)
{
    return false;
}

bool PtfxLoader::loadPlugin(const char*,
                            PtfxHeader&,
                            PtfxInitFunc& outInit,
                            PtfxSetParamFunc& outSetParam,
                            PtfxProcessBlockFunc& outProcessBlock)
{
    outInit = nullptr;
    outSetParam = nullptr;
    outProcessBlock = nullptr;
    return false;
}
