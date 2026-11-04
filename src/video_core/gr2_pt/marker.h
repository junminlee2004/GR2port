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
};

/// The words of a marker packet after the payload type.
struct Words {
    Kind kind;
    u32 pass;
    u32 frame;
    // PassEnd: the draws the game issued inside the pass and its draw-thread time.
    u32 draws;
    u32 game_ns;
};

inline constexpr u32 WordCount = sizeof(Words) / sizeof(u32);

/// The graphics context of the calling game thread.
VAddr Context();

/// Writes a marker packet at the command buffer cursor of the calling game thread's context.
void Write(const Words& words);

} // namespace Gr2Pt::Marker
