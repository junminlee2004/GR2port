// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/gr2_pt/device.h"

namespace Gr2Pt {
class Memory;
}

namespace Gr2Pt::Meshes {
struct Mesh;
}

namespace Gr2Pt::Scene {
struct Header;
}

// The game's deformed instances (SObj kind 0x19, 0x180 bytes): a copy of a shape's vertex buffer
// that the game's compute deformers rewrite each frame. The renderer keeps its own copy of each
// and runs its own deformers on it. An instance holds its handle at +0x13c, which no game code
// reads or writes; the capture registers an instance the first time it meets it.
namespace Gr2Pt::Deformers {

inline constexpr u32 Capacity = 4096;

/// Capture thread: the device address of the renderer's copy of the deformed instance whose
/// vertex buffer object is at vertex_buffer, a copy of mesh's shape; 0 while the copy is not
/// ready or when the instance's layout is not its shape's. The capture's frame marks the
/// instance used.
u64 Get(VAddr vertex_buffer, VAddr shape, const Meshes::Mesh& mesh, u64 frame);

/// Render thread: creates the skinning pipeline and the buffer the copies share.
bool Start(const Device& device, Memory& memory);

/// Render thread, before the frame's draws: records the copies that fill new instances' copies,
/// and frees the copies of instances unused for a while once the frames that may draw them are
/// done. Frames up to completed are done on the GPU; game_frame is the packet's frame.
void Update(vk::CommandBuffer cmd, u64 frame, u64 completed, u64 game_frame);

/// Render thread, outside rendering: runs the skin jobs of the packet at address and makes their
/// copies visible to vertex shaders.
void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address);

struct Stats {
    /// Instances with a copy, and the bytes of the copies.
    u32 instances;
    u64 resident_bytes;
    /// Instances waiting for room in the buffer the copies share.
    u32 pending;
    /// Instances whose layout is not their shape's, and copies freed after going unused.
    u32 unsupported;
    u32 retired;
};
Stats TakeStats();

} // namespace Gr2Pt::Deformers
