// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

namespace Gr2Port {
class Memory;
} // namespace Gr2Port

// The game's SelfTranslucent pass: the backs of characters and the effects inside them, in targets
// of their own, which G2_CH_Main shows through the skin. The stencil tells the characters apart.
namespace Gr2Port::SelfTranslucent {

inline constexpr vk::Format ColorFormat = vk::Format::eR8G8B8A8Unorm;
inline constexpr vk::Format DepthFormat = vk::Format::eD32SfloatS8Uint;

/// Creates the set and the pipeline.
bool Start(const Device& device, Memory& memory);

/// With the GPU idle: makes the targets at extent.
bool Resize(vk::Extent2D extent);

/// The targets for fragment shaders to read after Record: binding 0 colour, 1 depth.
vk::DescriptorSetLayout SetLayout();
vk::DescriptorSet Set();

/// Outside rendering, after the particle simulation, with the scene's last depth readable: draws
/// the packet at address, which is at offset in buffer, into the targets.
void Record(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
            vk::Buffer buffer, u64 offset);

} // namespace Gr2Port::SelfTranslucent
