// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include "common/types.h"

// The audio engine behind libSceNgs2: systems, racks and voices in host memory, driven by the
// parameter lists and the render calls of the game. It knows nothing of guest handles: the caller
// turns a handle into a voice.
namespace Libraries::Ngs2::Mixer {

struct System;
struct Rack;
struct Voice;

struct RenderBuffer {
    void* data;
    size_t size;
    u32 waveform_type;
    u32 channels;
};

// Turns the destination handle of a patch parameter into its voice, or nullptr.
using Resolver = Voice* (*)(u64 handle);

System* CreateSystem(u32 sample_rate, u32 grain_samples);
void DestroySystem(System* system);

// option is the rack option of the game (OrbisNgs2RackOption and what follows it), or nullptr.
Rack* CreateRack(System* system, u32 rack_id, const void* option);
void DestroyRack(Rack* rack);
u32 NumVoices(const Rack* rack);
Voice* GetVoice(Rack* rack, u32 index);

// params is a list of voice parameters (OrbisNgs2VoiceParamHeader first in each).
s32 Control(Voice* voice, const void* params, Resolver resolve);
s32 GetState(Voice* voice, void* out, size_t size);
s32 Render(System* system, const RenderBuffer* buffers, u32 count);

} // namespace Libraries::Ngs2::Mixer
