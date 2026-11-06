// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/gr2port/device.h"

namespace Gr2Port {
class Memory;
}

namespace Gr2Port::Meshes {
struct Mesh;
}

namespace Gr2Port::Scene {
struct Header;
}

// The game's deformed instances (SObj kind 0x19, 0x180 bytes): a copy of a shape's vertex buffer
// that the game's compute deformers rewrite each frame. The renderer keeps its own copy of each
// and runs its own deformers on it. An instance holds its handle at +0x13c, which no game code
// reads or writes; the capture registers an instance the first time it meets it.
namespace Gr2Port::Deformers {

inline constexpr u32 Capacity = 4096;

/// The renderer's copy of a deformed instance: the device addresses of its vertex data and, when
/// its shape has cloth, of its particles' state (a u32 the write pass sets once the particles
/// hold positions, padding to 32 bytes, then a float4 position and previous position each).
struct Copy {
    u64 vertices = 0;
    u64 state = 0;
};

/// Capture thread: the renderer's copy of the deformed instance whose vertex buffer object is at
/// vertex_buffer, a copy of mesh's shape, usable from the frame being captured; zeros when the
/// buffer has no room or the instance's layout is not its shape's. The capture's frame marks the
/// instance used.
Copy Get(VAddr vertex_buffer, VAddr shape, const Meshes::Mesh& mesh, u64 frame);

/// Render thread: creates the deformer pipelines, the buffer the copies share and the effector
/// grid.
bool Start(const Device& device, Memory& memory);

/// Render thread, before the frame's draws: records the copies that fill new instances' copies,
/// and frees the copies of instances unused for a while once the frames that may draw them are
/// done. Frames up to completed are done on the GPU; game_frame is the packet's frame.
void Update(vk::CommandBuffer cmd, u64 frame, u64 completed, u64 game_frame);

/// Render thread, outside rendering: runs the packet's effector grid, skin jobs and SoftBody jobs,
/// in the game's order, and makes the copies visible to vertex shaders. The packet is at address.
void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address);

struct Stats {
    /// Instances with a copy, and the bytes of the copies.
    u32 instances;
    u64 resident_bytes;
    /// Captures of instances that found no room in the buffer the copies share.
    u32 pending;
    /// Instances whose layout is not their shape's, and copies freed after going unused.
    u32 unsupported;
    u32 retired;
};
Stats TakeStats();

} // namespace Gr2Port::Deformers
