// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>

#include "video_core/gr2_pt/device.h"

namespace Xbyak {
class CodeGenerator;
}

namespace Gr2Pt {
class Memory;
}

// The game's mesh shapes (SObj kind 6, built by the .gfx builder 0x1447990) copied into the
// renderer's memory: vertex data in a buffer per shape, indices in one shared index buffer. Each
// shape holds its handle at +0x184, which no game code reads or writes.
namespace Gr2Pt::Meshes {

inline constexpr u32 Capacity = 1u << 16;

struct Mesh {
    std::atomic<VAddr> shape{0};
    std::atomic<bool> ready{false};
    /// The device address of the vertex data.
    u64 vertices = 0;
    /// The first index of the shape's index data in the shared index buffer, and its count.
    u32 first_index = 0;
    u32 indices = 0;
    u32 stride = 0;
    /// The position element: byte offset in the low 8 bits, format index above.
    u32 position = 0;
    /// The texture coordinate element, encoded as position; format 0 when the shape has none.
    u32 texcoord = 0;
};

/// Installs the hooks on the builder and on the model unload.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// The ready mesh of shape, or null. Any thread.
const Mesh* Get(VAddr shape);

/// Render thread: creates the shared index buffer.
bool Start(const Device& device, Memory& memory);

/// Render thread, before the frame's draws: records the copies of up to a frame's budget of new
/// shapes into cmd, and frees the meshes of unloaded models once the frames that may draw them
/// are done. Frames up to completed are done on the GPU.
void Update(vk::CommandBuffer cmd, u64 frame, u64 completed);

/// The buffer holding every mesh's 16-bit indices.
vk::Buffer IndexBuffer();

struct Stats {
    u32 meshes;
    u32 pending;
    /// Shapes the shaders cannot draw, or with no room.
    u32 skipped;
    /// Of those, shapes with indices past their vertex data.
    u32 bad_indices;
    u32 uploads_logged;
    /// Shapes built again at the address of a shape that was never unloaded.
    u32 missed_unloads;
    /// Unloaded shapes whose handle field does not hold their handle.
    u32 lost_handles;
    /// Shapes with no texture coordinates the shaders decode.
    u32 no_texcoord;
    u64 uploaded_bytes;
    u64 resident_bytes;
};
Stats TakeStats();

} // namespace Gr2Pt::Meshes
