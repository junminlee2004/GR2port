// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "video_core/gr2port/device.h"

namespace Xbyak {
class CodeGenerator;
}

namespace Gr2Port {
class Memory;
}

// The game's GNF textures (InitTextureFromGnf 0x1618ca0) converted into the renderer's images once,
// when the game builds them: detiled on the renderer's GPU, block-compressed formats kept. Each
// texture object holds its handle at +0x6c, which no game code reads or writes. Shaders sample
// every texture through one array indexed by handle, as a 2D texture or a cube map by the texture's
// kind; handle 0 is a white 2D texture.
namespace Gr2Port::Textures {

inline constexpr u32 Capacity = 1u << 15;

/// Installs the hooks on the GNF texture init and on the texture destroy.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// The handle of texture, the game's texture object, when it is a 2D texture and ready, else 0.
/// Any thread.
u32 Get(VAddr texture);

/// As Get, for a cube map.
u32 GetCube(VAddr texture);

/// Render thread: creates the texture array, its sampler and the detile pipelines.
bool Start(const Device& device, Memory& memory);

/// Render thread, before the frame's draws: records the conversion of up to a frame's budget of
/// new textures into cmd, and frees destroyed textures once the frames that may sample them are
/// done. Frames up to completed are done on the GPU.
void Update(vk::CommandBuffer cmd, u64 frame, u64 completed);

/// The set with the texture array at binding 0, a repeating sampler at binding 1 and a clamping one
/// at binding 2, for fragment shaders.
vk::DescriptorSetLayout SetLayout();
vk::DescriptorSet Set();

struct Stats {
    u32 textures;
    u32 pending;
    /// Textures not converted: not a single 2D image or cube map, a tiling or format the renderer
    /// does not convert, a layout larger than the GNF data, or no memory.
    u32 not_2d;
    u32 unsupported;
    u32 too_large;
    u32 failed;
    /// Textures initialised again at the address of one never destroyed.
    u32 missed_destroys;
    /// Bytes converted, of the textures' whole data.
    u64 uploaded_bytes;
    u64 uploaded_data_bytes;
    u64 resident_bytes;
};
Stats TakeStats();

} // namespace Gr2Port::Textures
