// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

namespace Gr2Port {
class Memory;
}

// The key light's shadow cascades, drawn each frame from the scene packet's casters into the
// renderer's own depth maps with the game's cascade matrices and depth bias.
namespace Gr2Port::Shadows {

/// Render thread: creates the cascade maps, their sampler and set, and the caster pipelines.
bool Start(const Device& device, Memory& memory);

/// Render thread, outside rendering: draws the casters of the packet at address into the
/// cascades and leaves them for fragment shaders to sample. The packet is at offset in buffer.
/// With a query pool, writes a timestamp after each cascade from first_query on.
void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
            u64 offset, vk::QueryPool timestamps, u32 first_query);

/// The set with the cascades for fragment shaders: the first two cascades at binding 0 as a
/// sampler2DArrayShadow and at 1 as a sampler2DArray of their depth; the last cascade at binding 2
/// as a sampler2DShadow and at 3 as a sampler2D.
vk::DescriptorSetLayout SetLayout();
vk::DescriptorSet Set();

} // namespace Gr2Port::Shadows
