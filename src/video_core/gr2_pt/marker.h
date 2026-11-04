// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// Marker packets: the hooks' messages to the command processor, placed in the game's command
// buffer at the point of the stream they refer to.
namespace Gr2Pt::Marker {

enum class Kind : u32 {
    PassBegin = 1,
    PassEnd = 2,
    // The caster batch of a pass, drawn by the renderer at this point of the stream.
    CasterBatch = 3,
    // A compare frame's copy of the shadow map after the pass's first caster.
    CasterSnapshot = 4,
};

/// CasterBatch flags.
enum BatchFlags : u32 {
    // The game's own draws of the batch's casters ran too: the renderer draws into a copy.
    CompareBatch = 1,
};

/// The words of a marker packet after the payload type.
struct Words {
    Kind kind;
    u32 pass;
    u32 frame;
    // PassEnd: the draws the game issued inside the pass and its draw-thread time. CasterBatch:
    // the casters after the first that the game's callback drew.
    u32 draws;
    u32 game_ns;
    // CasterBatch: the batch's first record, its record count and BatchFlags.
    u32 first;
    u32 count;
    u32 flags;
};

inline constexpr u32 WordCount = sizeof(Words) / sizeof(u32);

/// The graphics context of the calling game thread.
VAddr Context();

/// Writes a marker packet at the command buffer cursor of the calling game thread's context.
void Write(const Words& words);

} // namespace Gr2Pt::Marker
