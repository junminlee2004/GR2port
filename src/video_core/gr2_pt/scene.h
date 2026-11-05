// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// The layout of a frame's scene packet in the scene ring; shaders/scene.glsl mirrors it.
namespace Gr2Pt::Scene {

inline constexpr u32 MaxDraws = 32768;

/// The start of each packet.
struct Header {
    u64 frame;
    /// The index count of all draws, and of the largest.
    u64 indices;
    u32 max_indices;
    u32 draws;
    /// Items with no drawable mesh, or past MaxDraws.
    u32 dropped;
    /// The capture's time on the game's draw thread.
    u32 capture_ns;
};

/// A draw of a mesh, read by the vertex shader at gl_DrawID. Matrices are the game's: row-major,
/// positions multiply from the left, OpenGL clip space.
struct Draw {
    float world_view[16];
    float world_view_projection[16];
    u64 vertices;
    u32 stride;
    /// The position element: byte offset in the low 8 bits, format index above.
    u32 position;
};
static_assert(sizeof(Draw) == 144);

/// The indirect command of a draw, at the same index as the draw.
struct Command {
    u32 index_count;
    u32 instance_count;
    u32 first_index;
    s32 vertex_offset;
    u32 first_instance;
};
static_assert(sizeof(Command) == 20);

inline constexpr u64 DrawsOffset = 256;
inline constexpr u64 CommandsOffset = DrawsOffset + MaxDraws * sizeof(Draw);
inline constexpr u64 PacketBytes = CommandsOffset + MaxDraws * sizeof(Command);

} // namespace Gr2Pt::Scene
