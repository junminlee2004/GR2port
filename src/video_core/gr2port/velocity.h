// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"

namespace Gr2Port {
class Memory;
}

// The game's Velocity step, on the render thread: how far each pixel moved on the screen since the
// last frame, in clip space.
namespace Gr2Port::Velocity {

/// Creates the pipeline.
bool Start(const Device& device, Memory& memory);

/// With the GPU idle: creates the image at extent, to fill from the scene's and the characters'
/// depth.
bool Resize(vk::Extent2D extent, vk::ImageView depth, vk::ImageView character_depth);

/// Outside rendering: a drawn frame, with both depths readable, gets its motion, any other frame
/// holds still. Compute shaders can then read the image in the general layout.
void Record(vk::CommandBuffer cmd, bool drawn, u64 packet_address);

vk::ImageView View();

} // namespace Gr2Port::Velocity
