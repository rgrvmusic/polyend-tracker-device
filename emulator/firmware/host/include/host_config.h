/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
#ifndef CONFIG_CONFIG_H_
#define CONFIG_CONFIG_H_

/* Host copy of core/config/config.h.  Only the interrupt mask is different:
 * a Linux process has no PRIMASK, and the host audio thread is the only place
 * audio blocks are updated.  The guard is the original's, so force-including
 * this file makes the vendor header a no-op. */

#define DISABLE_GLOBAL_INTERRUPTS() ((void)0)
#define ENABLE_GLOBAL_INTERRUPTS()  ((void)0)

#define DMAMEM            __attribute__((section(".dmabuffers"), used))
#define AUDIO_CORE_FLASH  __attribute__((section(".audio_flash_functions"), used))
#define AUDIO_CORE_RAM    __attribute__((section(".audio_ram_functions"), used))
#define OCRAM_BSS_SECTION __attribute__((section(".bss.$RAM3"), used))

static constexpr int kMaxFilesListLength = 256;
static constexpr int kMaxFilePathLen = 255;
static constexpr int kMaxFileNameLen = 80;

// !!! important: this value is supposed to be 0 if we do not include Synth
// if you need "independent" number of synth instruments use "kSynthInstrumentsCount" from
// mtStruth.h witch is always (3), this allows this constant to be used instead of the whole
// definition
#ifdef FEATURE_SYNTH
static constexpr int kSynthParts = 3;
#else
static constexpr int kSynthParts = 0;
#endif

static constexpr int kRedFrameWidth = 24;

static constexpr int kMidiChannelsPerPort = 16;
static constexpr int kSynthMaxVoices = 8;
static constexpr int kSynthCCs = 8;
static constexpr int kSends = 2;

const char* const kPatchesFolder = "Patches";
const char* const kWorkspaceFolder = "Workspace";

#endif /* CONFIG_CONFIG_H_ */
